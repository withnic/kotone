#pragma once

#include "kotone/dsp_types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace kotone {

struct ProfileEntry {
    std::string id;           // "builtin:neutral_safe" or "file:<filename>.json"
    std::string display_name; // profile_name, or "profile_name (<filename>)" if duplicate
    std::string profile_name; // raw profile_name from JSON
    std::string file_name;    // UTF-8 filename (empty for builtins)
    std::string file_path;    // full UTF-8 file path (empty for builtins)
    bool is_builtin = false;
};

struct ProfileLoadResult {
    ErrorCode error = ErrorCode::Ok;
    VoiceProfile profile{};
    std::string error_message;

    explicit operator bool() const noexcept { return error == ErrorCode::Ok; }
};

struct ProfileSaveResult {
    ErrorCode error = ErrorCode::Ok;
    std::string saved_id;        // "file:<filename>"
    std::string saved_file_name; // UTF-8 filename
    std::string saved_file_path; // full UTF-8 path
    std::string error_message;

    explicit operator bool() const noexcept { return error == ErrorCode::Ok; }
};

// UTF-8 safe std::filesystem::path conversions (C++17)
std::filesystem::path utf8_to_path(const std::string& utf8_str);
std::string path_to_utf8(const std::filesystem::path& path);

// Sanitizes a user-supplied profile name into a safe cross-platform UTF-8 filename stem.
// Preserves UTF-8 Japanese/Unicode characters; replaces forbidden characters with '_'.
std::string sanitize_profile_filename(const std::string& requested_name);

// Pure C++17 JSON parser & serializer (zero external dependencies)
ProfileLoadResult parse_profile_json(const std::string& json_text,
                                     const std::string& fallback_name = "",
                                     uint32_t sample_rate = kDefaultSampleRate);

std::string serialize_profile_json(const VoiceProfile& profile);

// File loading with 1MB size guard, UTF-8 path support, and strict clamping
ProfileLoadResult load_profile_file(const std::string& file_path_utf8,
                                    uint32_t sample_rate = kDefaultSampleRate);

// Built-in read-only presets ("builtin:neutral_safe", "builtin:clear_broadcast",
// "builtin:warm_podcast")
bool get_builtin_preset(const std::string& id_or_name, VoiceProfile& out_profile);
std::vector<ProfileEntry> list_builtin_presets();

// Unlimited multi-profile repository operations
// Scans all *.json files in dir_path_utf8 (skips corrupted/invalid files without aborting).
std::vector<ProfileEntry> list_profiles(const std::string& dir_path_utf8,
                                        bool include_builtins = false);

ProfileLoadResult load_profile_by_id(const std::string& profile_id,
                                     const std::string& dir_path_utf8,
                                     uint32_t sample_rate = kDefaultSampleRate);

// Saves profile as a new UTF-8 JSON file with auto-numbering (_2, _3...) and atomic .tmp rename.
ProfileSaveResult save_profile_new(const std::string& dir_path_utf8,
                                   const std::string& requested_name, const VoiceProfile& profile);

// Atomically overwrites an existing user profile JSON file via .tmp rename.
ProfileSaveResult save_profile_overwrite(const std::string& file_path_utf8,
                                         const VoiceProfile& profile);

} // namespace kotone
