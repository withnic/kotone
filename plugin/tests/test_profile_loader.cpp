#include "test_framework.hpp"

#include "kotone/profile_loader.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace kotone::test {
namespace {

struct ScopedTempDir {
    std::filesystem::path dir;

    explicit ScopedTempDir(const std::string& tag) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = std::filesystem::temp_directory_path() /
              utf8_to_path("kotone_test_" + tag + "_" + std::to_string(tick));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }

    ~ScopedTempDir() noexcept {
        try {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        } catch (const std::exception& ex) {
            std::cerr << "Warning: failed to clean temp dir: " << ex.what() << "\n";
        } catch (...) {
            std::cerr << "Warning: unknown error cleaning temp dir\n";
        }
    }

    std::string utf8() const { return path_to_utf8(dir); }
};

void test_multiprofile_save_list_japanese_and_dedup() {
    ScopedTempDir tmp("multi");

    const std::vector<std::string> names = {u8"普段トーク", u8"ゲーム配信", u8"囁き声配信",
                                            u8"深夜雑談",   u8"歌枠用",     u8"検証用6"};

    for (size_t i = 0; i < names.size(); ++i) {
        VoiceProfile p = make_default_neutral_profile();
        p.dsp_params.input_gain_db = static_cast<float>(i) - 2.0f;
        p.dsp_params.hpf.cutoff_hz = 70.0f + static_cast<float>(i) * 5.0f;
        p.ui_overrides.mix_percent = 80.0f + static_cast<float>(i) * 2.0f;

        const ProfileSaveResult saved = save_profile_new(tmp.utf8(), names[i], p);
        KOTONE_EXPECT_TRUE(static_cast<bool>(saved));
        KOTONE_EXPECT_EQ(saved.saved_file_name, names[i] + ".json");
        KOTONE_EXPECT_EQ(saved.saved_id, "file:" + names[i] + ".json");
    }

    // List all user profiles and verify all 6 exist without omission
    const std::vector<ProfileEntry> entries = list_profiles(tmp.utf8(), false);
    KOTONE_EXPECT_EQ(entries.size(), names.size());

    std::set<std::string> found_names;
    for (const auto& e : entries) {
        found_names.insert(e.profile_name);
        const ProfileLoadResult loaded = load_profile_by_id(e.id, tmp.utf8());
        KOTONE_EXPECT_TRUE(static_cast<bool>(loaded));
        KOTONE_EXPECT_EQ(loaded.profile.profile_name, e.profile_name);
    }
    for (const auto& expected_name : names) {
        KOTONE_EXPECT_TRUE(found_names.count(expected_name) == 1);
    }

    // Duplicate name saves must auto-append _2, _3 without overwriting original
    VoiceProfile dup_profile = make_default_neutral_profile();
    dup_profile.dsp_params.input_gain_db = 5.5f;
    const ProfileSaveResult dup2 = save_profile_new(tmp.utf8(), u8"普段トーク", dup_profile);
    KOTONE_EXPECT_TRUE(static_cast<bool>(dup2));
    KOTONE_EXPECT_EQ(dup2.saved_file_name, std::string(u8"普段トーク_2.json"));

    const ProfileSaveResult dup3 = save_profile_new(tmp.utf8(), u8"普段トーク", dup_profile);
    KOTONE_EXPECT_TRUE(static_cast<bool>(dup3));
    KOTONE_EXPECT_EQ(dup3.saved_file_name, std::string(u8"普段トーク_3.json"));

    // Listing should now contain 8 entries and disambiguate duplicate profile_name labels
    const std::vector<ProfileEntry> after_dup = list_profiles(tmp.utf8(), false);
    KOTONE_EXPECT_EQ(after_dup.size(), static_cast<size_t>(8));
    int disambiguated = 0;
    for (const auto& e : after_dup) {
        if (e.profile_name == u8"普段トーク") {
            KOTONE_EXPECT_TRUE(e.display_name.find(e.file_name) != std::string::npos);
            ++disambiguated;
        }
    }
    KOTONE_EXPECT_EQ(disambiguated, 3);
}

void test_filename_sanitization_and_overwrite() {
    ScopedTempDir tmp("sanitize");

    // Forbidden Windows/POSIX characters
    const std::string dirty = u8"ゲーム/配信:用*?\"<>|";
    const std::string sanitized = sanitize_profile_filename(dirty);
    KOTONE_EXPECT_TRUE(sanitized.find('/') == std::string::npos);
    KOTONE_EXPECT_TRUE(sanitized.find(':') == std::string::npos);
    KOTONE_EXPECT_TRUE(sanitized.find('*') == std::string::npos);
    KOTONE_EXPECT_TRUE(sanitized.find('?') == std::string::npos);
    KOTONE_EXPECT_TRUE(sanitized.find('"') == std::string::npos);
    KOTONE_EXPECT_TRUE(sanitized.find('<') == std::string::npos);
    KOTONE_EXPECT_TRUE(sanitized.find('>') == std::string::npos);
    KOTONE_EXPECT_TRUE(sanitized.find('|') == std::string::npos);
    KOTONE_EXPECT_TRUE(sanitized.find(u8"ゲーム") != std::string::npos);

    // Empty / dots-only fallback & reserved Windows device name
    KOTONE_EXPECT_EQ(sanitize_profile_filename("   ...  "), std::string("custom_profile"));
    KOTONE_EXPECT_EQ(sanitize_profile_filename("CON"), std::string("_CON"));

    VoiceProfile p = make_default_neutral_profile();
    p.ui_overrides.mix_percent = 75.0f;
    p.ui_overrides.output_makeup_trim_db = 2.5f;
    p.ui_overrides.limiter_ceiling_dbfs = -1.5f;

    const ProfileSaveResult saved = save_profile_new(tmp.utf8(), dirty, p);
    KOTONE_EXPECT_TRUE(static_cast<bool>(saved));

    // Overwrite save with modified overrides
    p.ui_overrides.mix_percent = 55.0f;
    p.ui_overrides.output_makeup_trim_db = -3.0f;
    p.ui_overrides.limiter_ceiling_dbfs = -2.0f;
    const ProfileSaveResult overwritten = save_profile_overwrite(saved.saved_file_path, p);
    KOTONE_EXPECT_TRUE(static_cast<bool>(overwritten));

    // Ensure no .tmp artifact left behind
    std::error_code ec;
    KOTONE_EXPECT_FALSE(std::filesystem::exists(utf8_to_path(saved.saved_file_path + ".tmp"), ec));

    const ProfileLoadResult reloaded = load_profile_file(saved.saved_file_path);
    KOTONE_EXPECT_TRUE(static_cast<bool>(reloaded));
    KOTONE_EXPECT_NEAR(reloaded.profile.ui_overrides.mix_percent, 55.0, 0.01);
    KOTONE_EXPECT_NEAR(reloaded.profile.ui_overrides.output_makeup_trim_db, -3.0, 0.01);
    KOTONE_EXPECT_NEAR(reloaded.profile.ui_overrides.limiter_ceiling_dbfs, -2.0, 0.01);
}

void test_corrupted_json_skip_and_error_codes() {
    ScopedTempDir tmp("corrupt");

    // 1. Valid profile
    VoiceProfile good = make_default_neutral_profile();
    KOTONE_EXPECT_TRUE(static_cast<bool>(save_profile_new(tmp.utf8(), u8"正常プロファイル", good)));

    // 2. Broken JSON syntax
    {
        std::ofstream ofs(tmp.dir / utf8_to_path("broken_syntax.json"), std::ios::binary);
        ofs << "{ \"schema_version\": \"1.0.0\", \"dsp_params\": { broken";
    }

    // 3. Unsupported major schema version (2.0.0)
    const std::string v2_path = path_to_utf8(tmp.dir / utf8_to_path("schema_v2.json"));
    {
        std::ofstream ofs(utf8_to_path(v2_path), std::ios::binary);
        ofs << "{ \"schema_version\": \"2.0.0\", \"dsp_params\": {} }";
    }
    const ProfileLoadResult v2_res = load_profile_file(v2_path);
    KOTONE_EXPECT_TRUE(v2_res.error == ErrorCode::ErrSchemaUnsupported);

    // 4. Oversized file (> 1MB)
    const std::string big_path = path_to_utf8(tmp.dir / utf8_to_path("oversized.json"));
    {
        std::ofstream ofs(utf8_to_path(big_path), std::ios::binary);
        std::string payload(kMaxProfileFileBytes + 128, ' ');
        ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    const ProfileLoadResult big_res = load_profile_file(big_path);
    KOTONE_EXPECT_TRUE(big_res.error == ErrorCode::ErrFileTooLarge);

    // 5. Missing file
    const ProfileLoadResult missing_res =
        load_profile_file(path_to_utf8(tmp.dir / utf8_to_path("does_not_exist.json")));
    KOTONE_EXPECT_TRUE(missing_res.error == ErrorCode::ErrFileNotFound);

    // list_profiles() must skip all invalid files and still list the valid one
    const std::vector<ProfileEntry> listed = list_profiles(tmp.utf8(), false);
    KOTONE_EXPECT_EQ(listed.size(), static_cast<size_t>(1));
    KOTONE_EXPECT_EQ(listed[0].profile_name, std::string(u8"正常プロファイル"));
}

void test_validation_clamping_and_compat() {
    // 70 Japanese characters (> 64 codepoints) to test UTF-8 boundary truncation
    std::string long_jp_name;
    for (int i = 0; i < 70; ++i) {
        long_jp_name += u8"あ";
    }

    const std::string extreme_json =
        "{\n"
        "  \"schema_version\": \"1.2.3\",\n"
        "  \"profile_name\": \"" +
        long_jp_name +
        "\",\n"
        "  \"dsp_params\": {\n"
        "    \"input_gain_db\": 999.0,\n"
        "    \"hpf\": { \"enabled\": true, \"cutoff_hz\": 5000.0, \"slope_db_per_oct\": 18 },\n"
        "    \"eq_bands\": [\n"
        "      { \"index\": 0, \"enabled\": true, \"type\": \"low_shelf\", \"freq_hz\": 5.0, "
        "\"gain_db\": 50.0, \"q\": 0.0 },\n"
        "      { \"index\": 1, \"enabled\": true, \"type\": \"peaking\", \"freq_hz\": 30000.0, "
        "\"gain_db\": -50.0, \"q\": 100.0 }\n"
        "    ],\n"
        "    \"deesser\": { \"enabled\": true, \"freq_hz\": 100.0, \"threshold_dbfs\": 10.0, "
        "\"max_reduction_db\": 99.0 },\n"
        "    \"compressor\": { \"enabled\": true, \"threshold_dbfs\": -120.0, \"ratio\": 0.2, "
        "\"knee_db\": 100.0, \"attack_ms\": 0.0, \"release_ms\": 5000.0, \"makeup_gain_db\": 50.0 "
        "},\n"
        "    \"limiter\": { \"enabled\": true, \"ceiling_dbfs\": 6.0, \"lookahead_ms\": 20.0, "
        "\"release_ms\": 1.0 }\n"
        "  }\n"
        "}\n";

    const ProfileLoadResult res = parse_profile_json(extreme_json, "fallback", 48000);
    KOTONE_EXPECT_TRUE(static_cast<bool>(res));

    const VoiceProfile& p = res.profile;
    // 64 Japanese 3-byte chars = 192 bytes (not split mid-character!)
    KOTONE_EXPECT_EQ(p.profile_name.size(), static_cast<size_t>(64 * 3));
    KOTONE_EXPECT_NEAR(p.dsp_params.input_gain_db, 24.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.hpf.cutoff_hz, 300.0, 1e-5);
    KOTONE_EXPECT_EQ(p.dsp_params.hpf.slope_db_per_oct, 24);

    // Band 0 & 1 clamped; Bands 2..7 padded as disabled
    KOTONE_EXPECT_NEAR(p.dsp_params.eq_bands[0].freq_hz, 20.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.eq_bands[0].gain_db, 18.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.eq_bands[0].q, 0.1, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.eq_bands[1].freq_hz, 20000.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.eq_bands[1].gain_db, -18.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.eq_bands[1].q, 18.0, 1e-5);
    for (uint32_t i = 2; i < kNumEqBands; ++i) {
        KOTONE_EXPECT_FALSE(p.dsp_params.eq_bands[i].enabled);
    }

    KOTONE_EXPECT_NEAR(p.dsp_params.deesser.freq_hz, 3000.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.deesser.threshold_dbfs, 0.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.deesser.max_reduction_db, 18.0, 1e-5);

    KOTONE_EXPECT_NEAR(p.dsp_params.compressor.threshold_dbfs, -60.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.compressor.ratio, 1.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.compressor.knee_db, 24.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.compressor.attack_ms, 0.1, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.compressor.release_ms, 1000.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.compressor.makeup_gain_db, 24.0, 1e-5);

    KOTONE_EXPECT_NEAR(p.dsp_params.limiter.ceiling_dbfs, -0.1, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.limiter.lookahead_ms, 5.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.dsp_params.limiter.release_ms, 5.0, 1e-5);

    // Omitted ui_overrides must default to mix=100, trim=0, ceiling=dsp_params.limiter.ceiling_dbfs
    KOTONE_EXPECT_NEAR(p.ui_overrides.mix_percent, 100.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.ui_overrides.output_makeup_trim_db, 0.0, 1e-5);
    KOTONE_EXPECT_NEAR(p.ui_overrides.limiter_ceiling_dbfs, -0.1, 1e-5);

    // Invalid EQ band type must fail parse
    const std::string bad_eq_json = "{ \"schema_version\": \"1.0.0\", \"dsp_params\": { "
                                    "\"eq_bands\": [ { \"type\": \"notch\" } ] } }";
    const ProfileLoadResult bad_eq = parse_profile_json(bad_eq_json);
    KOTONE_EXPECT_TRUE(bad_eq.error == ErrorCode::ErrJsonParseFailed);

    // Non-numeric field must fail parse
    const std::string bad_num_json =
        "{ \"schema_version\": \"1.0.0\", \"dsp_params\": { \"input_gain_db\": \"loud\" } }";
    const ProfileLoadResult bad_num = parse_profile_json(bad_num_json);
    KOTONE_EXPECT_TRUE(bad_num.error == ErrorCode::ErrJsonParseFailed);
}

void test_builtin_presets() {
    const std::vector<ProfileEntry> builtins = list_builtin_presets();
    KOTONE_EXPECT_EQ(builtins.size(), static_cast<size_t>(3));

    for (const auto& b : builtins) {
        KOTONE_EXPECT_TRUE(b.is_builtin);
        const ProfileLoadResult res = load_profile_by_id(b.id, "");
        KOTONE_EXPECT_TRUE(static_cast<bool>(res));
        KOTONE_EXPECT_TRUE(res.profile.dsp_params.limiter.enabled);
        KOTONE_EXPECT_LE(res.profile.dsp_params.limiter.ceiling_dbfs, -0.1);
    }

    // Path traversal rejection on file: IDs
    const ProfileLoadResult trav = load_profile_by_id("file:../secret.json", ".");
    KOTONE_EXPECT_TRUE(trav.error == ErrorCode::ErrFileNotFound);
}

} // namespace

void run_profile_loader_tests(int& passed, int& failed) {
    run_case("profile_multiprofile_save_list_japanese_and_dedup",
             test_multiprofile_save_list_japanese_and_dedup, passed, failed);
    run_case("profile_filename_sanitization_and_overwrite",
             test_filename_sanitization_and_overwrite, passed, failed);
    run_case("profile_corrupted_json_skip_and_error_codes",
             test_corrupted_json_skip_and_error_codes, passed, failed);
    run_case("profile_validation_clamping_and_compat", test_validation_clamping_and_compat, passed,
             failed);
    run_case("profile_builtin_presets", test_builtin_presets, passed, failed);
}

} // namespace kotone::test
