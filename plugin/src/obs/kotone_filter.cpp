#include "kotone/dsp_engine.hpp"
#include "kotone/profile_loader.hpp"

#include <obs-module.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace kotone::obs_adapter {
namespace {

constexpr size_t kHotkeySlotCount = 10;

constexpr const char* kFilterId = "kotone_voice_filter";
constexpr const char* kPropProfileSelect = "profile_select";
constexpr const char* kPropRefreshBtn = "refresh_profiles_btn";
constexpr const char* kPropMixPercent = "mix_percent";
constexpr const char* kPropMakeupTrimDb = "output_makeup_trim_db";
constexpr const char* kPropLimiterCeiling = "limiter_ceiling_dbfs";
constexpr const char* kPropDeclickAmount = "declick_amount_percent";
constexpr const char* kPropBreathAmount = "breath_amount_percent";
constexpr const char* kPropBypass = "bypass";
constexpr const char* kPropSaveGroup = "profile_save_group";
constexpr const char* kPropNewProfileName = "new_profile_name";
constexpr const char* kPropSaveNewBtn = "save_new_profile_btn";
constexpr const char* kPropOverwriteBtn = "overwrite_profile_btn";
constexpr const char* kPropProfilesDir = "profiles_dir";

constexpr const char* kDefaultProfileId = "builtin:neutral_safe";

constexpr const char* kHotkeyNames[kHotkeySlotCount] = {
    "kotone.switch_profile_1",  "kotone.switch_profile_2", "kotone.switch_profile_3",
    "kotone.switch_profile_4",  "kotone.switch_profile_5", "kotone.switch_profile_6",
    "kotone.switch_profile_7",  "kotone.switch_profile_8", "kotone.switch_profile_9",
    "kotone.switch_profile_10",
};

constexpr const char* kHotkeyLocaleKeys[kHotkeySlotCount] = {
    "Kotone.Hotkey.SwitchProfile1",  "Kotone.Hotkey.SwitchProfile2", "Kotone.Hotkey.SwitchProfile3",
    "Kotone.Hotkey.SwitchProfile4",  "Kotone.Hotkey.SwitchProfile5", "Kotone.Hotkey.SwitchProfile6",
    "Kotone.Hotkey.SwitchProfile7",  "Kotone.Hotkey.SwitchProfile8", "Kotone.Hotkey.SwitchProfile9",
    "Kotone.Hotkey.SwitchProfile10",
};

constexpr const char* kHotkeySaveKeys[kHotkeySlotCount] = {
    "kotone_hotkey_1", "kotone_hotkey_2", "kotone_hotkey_3", "kotone_hotkey_4", "kotone_hotkey_5",
    "kotone_hotkey_6", "kotone_hotkey_7", "kotone_hotkey_8", "kotone_hotkey_9", "kotone_hotkey_10",
};

std::string get_default_profiles_dir() {
    char* raw_path = obs_module_config_path("profiles");
    if (raw_path == nullptr) {
        return "";
    }
    std::string result(raw_path);
    bfree(raw_path);
    return result;
}

void ensure_directory_exists(const std::string& dir_utf8) {
    if (dir_utf8.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(utf8_to_path(dir_utf8), ec);
    if (ec) {
        blog(LOG_WARNING, "[kotone] Failed to create profiles directory '%s': %s", dir_utf8.c_str(),
             ec.message().c_str());
    }
}

uint32_t query_obs_sample_rate() {
    const audio_t* audio = obs_get_audio();
    if (audio != nullptr) {
        return sanitize_sample_rate(audio_output_get_sample_rate(audio));
    }
    return kDefaultSampleRate;
}

struct KotoneFilterContext;

struct HotkeySlotBinding {
    KotoneFilterContext* ctx = nullptr;
    size_t slot_index = 0;
};

struct KotoneFilterContext {
    obs_source_t* source = nullptr;
    DspEngine engine{kDefaultSampleRate};

    // Guarded strictly on OBS UI/control threads; NEVER locked inside filter_audio().
    std::mutex ui_mutex;
    std::string profiles_dir;
    std::string selected_profile_id = kDefaultProfileId;
    VoiceProfile active_profile = make_default_neutral_profile();
    UiOverrides active_overrides{};
    bool bypass = false;
    bool has_valid_profile = false;
    std::vector<ProfileEntry> user_profiles;

    std::array<obs_hotkey_id, kHotkeySlotCount> hotkey_ids{};
    std::array<HotkeySlotBinding, kHotkeySlotCount> hotkey_bindings{};

    KotoneFilterContext() { hotkey_ids.fill(OBS_INVALID_HOTKEY_ID); }
};

// Populates the profile_select combo box with 3 built-in presets + unlimited user profiles.
void populate_profile_dropdown(obs_property_t* list_prop,
                               const std::vector<ProfileEntry>& user_profiles) {
    if (list_prop == nullptr) {
        return;
    }
    obs_property_list_clear(list_prop);

    obs_property_list_add_string(list_prop, obs_module_text("Kotone.Preset.NeutralSafe"),
                                 "builtin:neutral_safe");
    obs_property_list_add_string(list_prop, obs_module_text("Kotone.Preset.ClearBroadcast"),
                                 "builtin:clear_broadcast");
    obs_property_list_add_string(list_prop, obs_module_text("Kotone.Preset.WarmPodcast"),
                                 "builtin:warm_podcast");

    for (const auto& entry : user_profiles) {
        obs_property_list_add_string(list_prop, entry.display_name.c_str(), entry.id.c_str());
    }
}

// Rescans profiles_dir (caller must hold ctx->ui_mutex).
void rescan_user_profiles_locked(KotoneFilterContext* ctx) {
    ensure_directory_exists(ctx->profiles_dir);
    ctx->user_profiles = list_profiles(ctx->profiles_dir, false);
}

// Loads profile_id into ctx->active_profile and pushes a lock-free snapshot to DspEngine.
// On error, retains the previous valid profile (or falls back to neutral_safe on first load).
// Caller must hold ctx->ui_mutex.
bool load_and_apply_profile_locked(KotoneFilterContext* ctx, const std::string& profile_id,
                                   const UiOverrides& overrides, bool bypass,
                                   bool use_loaded_overrides) {

    const uint32_t fs = query_obs_sample_rate();
    if (ctx->engine.sample_rate() != fs) {
        ctx->engine.prepare(fs);
    }

    const ProfileLoadResult res = load_profile_by_id(profile_id, ctx->profiles_dir, fs);
    if (!res) {
        blog(LOG_WARNING, "[kotone] Failed to load profile '%s' (%s: %s); keeping safe fallback",
             profile_id.c_str(), error_code_to_string(res.error), res.error_message.c_str());

        if (!ctx->has_valid_profile) {
            ctx->active_profile = make_default_neutral_profile();
            ctx->active_overrides = overrides;
            clamp_ui_overrides(ctx->active_overrides);
            ctx->engine.apply_profile_with_overrides(ctx->active_profile, ctx->active_overrides,
                                                     bypass);
        }
        ctx->engine.set_state(bypass ? EngineState::Bypassed : EngineState::FallbackSafe);
        return false;
    }

    ctx->selected_profile_id = profile_id;
    ctx->active_profile = res.profile;
    ctx->has_valid_profile = true;
    ctx->active_overrides = use_loaded_overrides ? res.profile.ui_overrides : overrides;
    clamp_ui_overrides(ctx->active_overrides);
    ctx->bypass = bypass;

    ctx->engine.apply_profile_with_overrides(ctx->active_profile, ctx->active_overrides,
                                             ctx->bypass);
    return true;
}

UiOverrides read_overrides_from_settings(obs_data_t* settings) {
    UiOverrides ov;
    ov.mix_percent = static_cast<float>(obs_data_get_double(settings, kPropMixPercent));
    ov.output_makeup_trim_db = static_cast<float>(obs_data_get_double(settings, kPropMakeupTrimDb));
    ov.limiter_ceiling_dbfs =
        static_cast<float>(obs_data_get_double(settings, kPropLimiterCeiling));
    ov.declick_amount_percent =
        static_cast<float>(obs_data_get_double(settings, kPropDeclickAmount));
    ov.breath_amount_percent = static_cast<float>(obs_data_get_double(settings, kPropBreathAmount));
    clamp_ui_overrides(ov);
    return ov;
}

void write_overrides_to_settings(obs_data_t* settings, const UiOverrides& ov) {
    obs_data_set_double(settings, kPropMixPercent, static_cast<double>(ov.mix_percent));
    obs_data_set_double(settings, kPropMakeupTrimDb, static_cast<double>(ov.output_makeup_trim_db));
    obs_data_set_double(settings, kPropLimiterCeiling,
                        static_cast<double>(ov.limiter_ceiling_dbfs));
    obs_data_set_double(settings, kPropDeclickAmount,
                        static_cast<double>(ov.declick_amount_percent));
    obs_data_set_double(settings, kPropBreathAmount, static_cast<double>(ov.breath_amount_percent));
}

// ============================================================================
// OBS UI Property Callbacks (UI Thread Only)
// ============================================================================

bool on_profile_selected_cb(void* priv, obs_properties_t* props, obs_property_t* property,
                            obs_data_t* settings) {
    (void)props;
    (void)property;
    auto* ctx = static_cast<KotoneFilterContext*>(priv);
    if (ctx == nullptr || settings == nullptr) {
        return false;
    }

    const char* id_cstr = obs_data_get_string(settings, kPropProfileSelect);
    const std::string chosen_id =
        (id_cstr != nullptr && id_cstr[0] != '\0') ? id_cstr : kDefaultProfileId;
    const bool bypass = obs_data_get_bool(settings, kPropBypass);

    std::lock_guard<std::mutex> lock(ctx->ui_mutex);
    if (chosen_id == ctx->selected_profile_id && ctx->has_valid_profile) {
        return false;
    }

    if (load_and_apply_profile_locked(ctx, chosen_id, read_overrides_from_settings(settings),
                                      bypass, true)) {
        write_overrides_to_settings(settings, ctx->active_overrides);
        blog(LOG_INFO, "[kotone] Switched active profile to '%s' (%s)",
             ctx->active_profile.profile_name.c_str(), chosen_id.c_str());
        return true;
    }
    return false;
}

bool on_profiles_dir_changed_cb(void* priv, obs_properties_t* props, obs_property_t* property,
                                obs_data_t* settings) {
    (void)property;
    auto* ctx = static_cast<KotoneFilterContext*>(priv);
    if (ctx == nullptr || props == nullptr || settings == nullptr) {
        return false;
    }

    const char* dir_cstr = obs_data_get_string(settings, kPropProfilesDir);
    const std::string new_dir = (dir_cstr != nullptr && dir_cstr[0] != '\0')
                                    ? std::string(dir_cstr)
                                    : get_default_profiles_dir();

    std::lock_guard<std::mutex> lock(ctx->ui_mutex);
    if (new_dir == ctx->profiles_dir) {
        return false;
    }
    ctx->profiles_dir = new_dir;
    rescan_user_profiles_locked(ctx);
    populate_profile_dropdown(obs_properties_get(props, kPropProfileSelect), ctx->user_profiles);
    return true;
}

bool on_refresh_profiles_clicked_cb(obs_properties_t* props, obs_property_t* property, void* data) {
    (void)property;
    auto* ctx = static_cast<KotoneFilterContext*>(data);
    if (ctx == nullptr || props == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(ctx->ui_mutex);
    rescan_user_profiles_locked(ctx);
    populate_profile_dropdown(obs_properties_get(props, kPropProfileSelect), ctx->user_profiles);
    blog(LOG_INFO, "[kotone] Rescanned profiles directory '%s': found %zu user profile(s)",
         ctx->profiles_dir.c_str(), ctx->user_profiles.size());
    return true;
}

bool save_as_new_locked(KotoneFilterContext* ctx, obs_properties_t* props, obs_data_t* settings) {
    const char* name_cstr = obs_data_get_string(settings, kPropNewProfileName);
    std::string requested_name = (name_cstr != nullptr) ? std::string(name_cstr) : "";
    if (requested_name.empty()) {
        requested_name = ctx->active_profile.profile_name;
    }

    VoiceProfile to_save = ctx->active_profile;
    to_save.ui_overrides = read_overrides_from_settings(settings);
    to_save.dsp_params.limiter.ceiling_dbfs = to_save.ui_overrides.limiter_ceiling_dbfs;

    const ProfileSaveResult saved = save_profile_new(ctx->profiles_dir, requested_name, to_save);
    if (!saved) {
        blog(LOG_ERROR, "[kotone] %s: %s", error_code_to_string(saved.error),
             saved.error_message.c_str());
        return false;
    }

    rescan_user_profiles_locked(ctx);
    populate_profile_dropdown(obs_properties_get(props, kPropProfileSelect), ctx->user_profiles);

    obs_data_set_string(settings, kPropProfileSelect, saved.saved_id.c_str());
    obs_data_set_string(settings, kPropNewProfileName, "");

    const bool bypass = obs_data_get_bool(settings, kPropBypass);
    load_and_apply_profile_locked(ctx, saved.saved_id, to_save.ui_overrides, bypass, true);
    blog(LOG_INFO, "[kotone] Saved new profile '%s' -> %s",
         ctx->active_profile.profile_name.c_str(), saved.saved_file_path.c_str());
    return true;
}

bool on_save_new_clicked_cb(obs_properties_t* props, obs_property_t* property, void* data) {
    (void)property;
    auto* ctx = static_cast<KotoneFilterContext*>(data);
    if (ctx == nullptr || ctx->source == nullptr || props == nullptr) {
        return false;
    }

    obs_data_t* settings = obs_source_get_settings(ctx->source);
    if (settings == nullptr) {
        return false;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(ctx->ui_mutex);
        changed = save_as_new_locked(ctx, props, settings);
    }
    obs_data_release(settings);
    return changed;
}

bool on_overwrite_clicked_cb(obs_properties_t* props, obs_property_t* property, void* data) {
    (void)property;
    auto* ctx = static_cast<KotoneFilterContext*>(data);
    if (ctx == nullptr || ctx->source == nullptr || props == nullptr) {
        return false;
    }

    obs_data_t* settings = obs_source_get_settings(ctx->source);
    if (settings == nullptr) {
        return false;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(ctx->ui_mutex);
        // Protect built-in presets by falling back to Save As New Profile
        if (ctx->selected_profile_id.rfind("file:", 0) != 0) {
            blog(LOG_INFO,
                 "[kotone] Built-in preset selected; falling back to Save As New Profile");
            changed = save_as_new_locked(ctx, props, settings);
        } else {
            const std::string fname = ctx->selected_profile_id.substr(5);
            if (fname.empty() || fname.find('/') != std::string::npos ||
                fname.find('\\') != std::string::npos || fname.find("..") != std::string::npos) {
                blog(LOG_ERROR, "[kotone] Invalid selected file ID for overwrite: '%s'",
                     ctx->selected_profile_id.c_str());
            } else {
                const std::string target_utf8 =
                    path_to_utf8(utf8_to_path(ctx->profiles_dir) / utf8_to_path(fname));
                VoiceProfile to_save = ctx->active_profile;
                to_save.ui_overrides = read_overrides_from_settings(settings);
                to_save.dsp_params.limiter.ceiling_dbfs = to_save.ui_overrides.limiter_ceiling_dbfs;

                const ProfileSaveResult saved = save_profile_overwrite(target_utf8, to_save);
                if (!saved) {
                    blog(LOG_ERROR, "[kotone] %s: %s", error_code_to_string(saved.error),
                         saved.error_message.c_str());
                } else {
                    ctx->active_profile = to_save;
                    ctx->active_overrides = to_save.ui_overrides;
                    const bool bypass = obs_data_get_bool(settings, kPropBypass);
                    ctx->engine.apply_profile_with_overrides(ctx->active_profile,
                                                             ctx->active_overrides, bypass);
                    rescan_user_profiles_locked(ctx);
                    populate_profile_dropdown(obs_properties_get(props, kPropProfileSelect),
                                              ctx->user_profiles);
                    blog(LOG_INFO, "[kotone] Overwrote profile '%s' -> %s",
                         ctx->active_profile.profile_name.c_str(), target_utf8.c_str());
                    changed = true;
                }
            }
        }
    }

    obs_data_release(settings);
    return changed;
}

// ============================================================================
// OBS Hotkey Callback (Profile 1..10 Quick Switch)
// ============================================================================

void on_profile_hotkey_cb(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    (void)id;
    (void)hotkey;
    if (!pressed || data == nullptr) {
        return;
    }

    auto* binding = static_cast<HotkeySlotBinding*>(data);
    KotoneFilterContext* ctx = binding->ctx;
    if (ctx == nullptr || ctx->source == nullptr) {
        return;
    }

    std::string target_id;
    std::string target_name;
    UiOverrides new_overrides{};
    bool switched = false;

    {
        std::lock_guard<std::mutex> lock(ctx->ui_mutex);
        rescan_user_profiles_locked(ctx);
        if (binding->slot_index >= ctx->user_profiles.size()) {
            blog(LOG_INFO,
                 "[kotone] Hotkey slot %zu pressed, but only %zu user profile(s) available",
                 binding->slot_index + 1, ctx->user_profiles.size());
            return;
        }
        const ProfileEntry& entry = ctx->user_profiles[binding->slot_index];
        target_id = entry.id;
        if (load_and_apply_profile_locked(ctx, target_id, ctx->active_overrides, ctx->bypass,
                                          true)) {
            target_name = ctx->active_profile.profile_name;
            new_overrides = ctx->active_overrides;
            switched = true;
        }
    }

    if (switched) {
        obs_data_t* settings = obs_source_get_settings(ctx->source);
        if (settings != nullptr) {
            obs_data_set_string(settings, kPropProfileSelect, target_id.c_str());
            write_overrides_to_settings(settings, new_overrides);
            obs_data_release(settings);
        }
        obs_source_update_properties(ctx->source);
        blog(LOG_INFO, "[kotone] Hotkey slot %zu switched profile to '%s' (%s)",
             binding->slot_index + 1, target_name.c_str(), target_id.c_str());
    }
}

// ============================================================================
// obs_source_info Callbacks
// ============================================================================

const char* kotone_get_name(void* unused) {
    (void)unused;
    return obs_module_text("Kotone.FilterName");
}

void kotone_get_defaults(obs_data_t* settings) {
    const std::string default_dir = get_default_profiles_dir();
    obs_data_set_default_string(settings, kPropProfileSelect, kDefaultProfileId);
    obs_data_set_default_double(settings, kPropMixPercent, 100.0);
    obs_data_set_default_double(settings, kPropMakeupTrimDb, 0.0);
    obs_data_set_default_double(settings, kPropDeclickAmount, 100.0);
    obs_data_set_default_double(settings, kPropBreathAmount, 100.0);
    obs_data_set_default_double(settings, kPropLimiterCeiling, -1.0);
    obs_data_set_default_bool(settings, kPropBypass, false);
    obs_data_set_default_string(settings, kPropNewProfileName, "");
    obs_data_set_default_string(settings, kPropProfilesDir, default_dir.c_str());
}

void kotone_update(void* data, obs_data_t* settings) {
    auto* ctx = static_cast<KotoneFilterContext*>(data);
    if (ctx == nullptr || settings == nullptr) {
        return;
    }

    const char* dir_cstr = obs_data_get_string(settings, kPropProfilesDir);
    const std::string dir = (dir_cstr != nullptr && dir_cstr[0] != '\0')
                                ? std::string(dir_cstr)
                                : get_default_profiles_dir();

    const char* id_cstr = obs_data_get_string(settings, kPropProfileSelect);
    const std::string profile_id = (id_cstr != nullptr && id_cstr[0] != '\0')
                                       ? std::string(id_cstr)
                                       : std::string(kDefaultProfileId);

    const UiOverrides overrides = read_overrides_from_settings(settings);
    const bool bypass = obs_data_get_bool(settings, kPropBypass);

    std::lock_guard<std::mutex> lock(ctx->ui_mutex);
    if (dir != ctx->profiles_dir) {
        ctx->profiles_dir = dir;
        rescan_user_profiles_locked(ctx);
    }

    if (!ctx->has_valid_profile || profile_id != ctx->selected_profile_id) {
        load_and_apply_profile_locked(ctx, profile_id, overrides, bypass, false);
    } else {
        const uint32_t fs = query_obs_sample_rate();
        if (ctx->engine.sample_rate() != fs) {
            ctx->engine.prepare(fs);
        }
        ctx->active_overrides = overrides;
        ctx->bypass = bypass;
        ctx->engine.apply_profile_with_overrides(ctx->active_profile, ctx->active_overrides,
                                                 ctx->bypass);
    }
}

void* kotone_create(obs_data_t* settings, obs_source_t* source) {
    auto* ctx = new KotoneFilterContext();
    ctx->source = source;
    ctx->engine.prepare(query_obs_sample_rate());

    {
        std::lock_guard<std::mutex> lock(ctx->ui_mutex);
        const char* dir_cstr =
            settings != nullptr ? obs_data_get_string(settings, kPropProfilesDir) : nullptr;
        ctx->profiles_dir = (dir_cstr != nullptr && dir_cstr[0] != '\0')
                                ? std::string(dir_cstr)
                                : get_default_profiles_dir();
        rescan_user_profiles_locked(ctx);
    }

    // Register 10 quick-switch hotkeys bound to this audio filter source
    for (size_t i = 0; i < kHotkeySlotCount; ++i) {
        ctx->hotkey_bindings[i] = HotkeySlotBinding{ctx, i};
        ctx->hotkey_ids[i] = obs_hotkey_register_source(
            source, kHotkeyNames[i], obs_module_text(kHotkeyLocaleKeys[i]), on_profile_hotkey_cb,
            &ctx->hotkey_bindings[i]);
    }

    if (settings != nullptr) {
        kotone_update(ctx, settings);
    }
    return ctx;
}

void kotone_destroy(void* data) {
    auto* ctx = static_cast<KotoneFilterContext*>(data);
    if (ctx == nullptr) {
        return;
    }

    for (size_t i = 0; i < kHotkeySlotCount; ++i) {
        if (ctx->hotkey_ids[i] != OBS_INVALID_HOTKEY_ID) {
            obs_hotkey_unregister(ctx->hotkey_ids[i]);
            ctx->hotkey_ids[i] = OBS_INVALID_HOTKEY_ID;
        }
    }
    delete ctx;
}

void kotone_save(void* data, obs_data_t* settings) {
    auto* ctx = static_cast<KotoneFilterContext*>(data);
    if (ctx == nullptr || settings == nullptr) {
        return;
    }
    for (size_t i = 0; i < kHotkeySlotCount; ++i) {
        if (ctx->hotkey_ids[i] != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_hotkey_save(ctx->hotkey_ids[i]);
            if (arr != nullptr) {
                obs_data_set_array(settings, kHotkeySaveKeys[i], arr);
                obs_data_array_release(arr);
            }
        }
    }
}

void kotone_load(void* data, obs_data_t* settings) {
    auto* ctx = static_cast<KotoneFilterContext*>(data);
    if (ctx == nullptr || settings == nullptr) {
        return;
    }
    for (size_t i = 0; i < kHotkeySlotCount; ++i) {
        if (ctx->hotkey_ids[i] != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t* arr = obs_data_get_array(settings, kHotkeySaveKeys[i]);
            if (arr != nullptr) {
                obs_hotkey_load(ctx->hotkey_ids[i], arr);
                obs_data_array_release(arr);
            }
        }
    }
    kotone_update(ctx, settings);
}

obs_properties_t* kotone_get_properties(void* data) {
    auto* ctx = static_cast<KotoneFilterContext*>(data);
    obs_properties_t* props = obs_properties_create();

    // 1. Profile Selection Dropdown (3 built-ins + unlimited user profiles)
    obs_property_t* profile_list = obs_properties_add_list(
        props, kPropProfileSelect, obs_module_text("Kotone.Prop.ProfileSelect"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

    std::string current_dir;
    if (ctx != nullptr) {
        std::lock_guard<std::mutex> lock(ctx->ui_mutex);
        rescan_user_profiles_locked(ctx);
        populate_profile_dropdown(profile_list, ctx->user_profiles);
        current_dir = ctx->profiles_dir;
        obs_property_set_modified_callback2(profile_list, on_profile_selected_cb, ctx);
    } else {
        populate_profile_dropdown(profile_list, {});
        current_dir = get_default_profiles_dir();
    }

    // 2. Refresh Profiles Button
    obs_properties_add_button2(props, kPropRefreshBtn,
                               obs_module_text("Kotone.Prop.RefreshProfiles"),
                               on_refresh_profiles_clicked_cb, ctx);

    // 3. Real-Time Fine-Tuning Sliders & Bypass
    obs_property_t* mix_slider = obs_properties_add_float_slider(
        props, kPropMixPercent, obs_module_text("Kotone.Prop.MixPercent"), 0.0, 100.0, 1.0);
    obs_property_float_set_suffix(mix_slider, " %");

    obs_property_t* makeup_slider = obs_properties_add_float_slider(
        props, kPropMakeupTrimDb, obs_module_text("Kotone.Prop.MakeupTrimDb"), -12.0, 12.0, 0.1);
    obs_property_float_set_suffix(makeup_slider, " dB");

    obs_property_t* ceiling_slider = obs_properties_add_float_slider(
        props, kPropLimiterCeiling, obs_module_text("Kotone.Prop.LimiterCeilingDbfs"), -12.0, -0.1,
        0.1);
    obs_property_float_set_suffix(ceiling_slider, " dBFS");

    obs_property_t* declick_slider = obs_properties_add_float_slider(
        props, kPropDeclickAmount, obs_module_text("Kotone.Prop.DeclickAmount"), 0.0, 100.0, 1.0);
    obs_property_float_set_suffix(declick_slider, " %");

    obs_property_t* breath_slider = obs_properties_add_float_slider(
        props, kPropBreathAmount, obs_module_text("Kotone.Prop.BreathAmount"), 0.0, 100.0, 1.0);
    obs_property_float_set_suffix(breath_slider, " %");

    obs_properties_add_bool(props, kPropBypass, obs_module_text("Kotone.Prop.Bypass"));

    // 4. Named Profile Save / Management Group
    obs_properties_t* save_group = obs_properties_create();
    obs_properties_add_text(save_group, kPropNewProfileName,
                            obs_module_text("Kotone.Prop.NewProfileName"), OBS_TEXT_DEFAULT);

    obs_properties_add_button2(save_group, kPropSaveNewBtn,
                               obs_module_text("Kotone.Prop.SaveNewProfile"),
                               on_save_new_clicked_cb, ctx);

    obs_properties_add_button2(save_group, kPropOverwriteBtn,
                               obs_module_text("Kotone.Prop.OverwriteProfile"),
                               on_overwrite_clicked_cb, ctx);

    obs_property_t* dir_prop = obs_properties_add_path(
        save_group, kPropProfilesDir, obs_module_text("Kotone.Prop.ProfilesDir"),
        OBS_PATH_DIRECTORY, nullptr, current_dir.c_str());
    if (ctx != nullptr) {
        obs_property_set_modified_callback2(dir_prop, on_profiles_dir_changed_cb, ctx);
    }

    obs_properties_add_group(props, kPropSaveGroup, obs_module_text("Kotone.Group.ProfileSave"),
                             OBS_GROUP_NORMAL, save_group);

    return props;
}

// Strictly real-time safe audio callback: zero heap allocation, zero mutex lock, zero I/O.
struct obs_audio_data* kotone_filter_audio(void* data, struct obs_audio_data* audio) {
    if (data == nullptr || audio == nullptr || audio->frames == 0) {
        return audio;
    }

    auto* ctx = static_cast<KotoneFilterContext*>(data);

    float* planes[kMaxChannels]{};
    uint32_t num_channels = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(MAX_AV_PLANES) && num_channels < kMaxChannels;
         ++i) {
        if (audio->data[i] != nullptr) {
            planes[num_channels++] = reinterpret_cast<float*>(audio->data[i]);
        }
    }

    if (num_channels > 0) {
        ctx->engine.process(planes, num_channels, audio->frames);
    }
    return audio;
}

} // namespace

void register_kotone_filter() {
    struct obs_source_info info = {};
    info.id = kFilterId;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = kotone_get_name;
    info.create = kotone_create;
    info.destroy = kotone_destroy;
    info.get_defaults = kotone_get_defaults;
    info.get_properties = kotone_get_properties;
    info.update = kotone_update;
    info.filter_audio = kotone_filter_audio;
    info.save = kotone_save;
    info.load = kotone_load;

    obs_register_source(&info);
}

} // namespace kotone::obs_adapter
