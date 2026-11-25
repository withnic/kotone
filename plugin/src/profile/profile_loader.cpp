#include "kotone/profile_loader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kotone {
namespace {

// ============================================================================
// Portable thread-safe UTC ISO-8601 timestamp (pure C++17, zero OS headers)
// ============================================================================

std::string current_utc_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const int64_t total_sec = duration_cast<seconds>(now.time_since_epoch()).count();

    int64_t days = total_sec / 86400;
    int64_t rem = total_sec % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }

    const int64_t hour = rem / 3600;
    rem %= 3600;
    const int64_t minute = rem / 60;
    const int64_t second = rem % 60;

    // Howard Hinnant's civil_from_days algorithm (exact proleptic Gregorian calendar)
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp = (5 * doy + 2) / 153;
    const uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint64_t m = mp + (mp < 10 ? 3 : -9);
    const int64_t year = y + (m <= 2 ? 1 : 0);

    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2) << m << '-'
        << std::setw(2) << d << 'T' << std::setw(2) << hour << ':' << std::setw(2) << minute << ':'
        << std::setw(2) << second << 'Z';
    return oss.str();
}

// ============================================================================
// Pure C++17 Recursive-Descent JSON Parser (RFC 8259)
// ============================================================================

struct JsonValue {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool bool_val = false;
    double number_val = 0.0;
    std::string string_val;
    std::vector<JsonValue> array_val;
    std::vector<std::pair<std::string, JsonValue>> object_val;

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        for (const auto& kv : object_val) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }

    void set_member(std::string key, JsonValue val) {
        for (auto& kv : object_val) {
            if (kv.first == key) {
                kv.second = std::move(val);
                return;
            }
        }
        object_val.emplace_back(std::move(key), std::move(val));
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src) {}

    bool parse(JsonValue& out, std::string& err) {
        // Strip optional UTF-8 BOM
        if (src_.size() >= 3 && static_cast<unsigned char>(src_[0]) == 0xEF &&
            static_cast<unsigned char>(src_[1]) == 0xBB &&
            static_cast<unsigned char>(src_[2]) == 0xBF) {
            pos_ = 3;
        }

        skip_ws();
        if (!parse_value(out, 0, err)) {
            return false;
        }
        skip_ws();
        if (pos_ != src_.size()) {
            err = "Unexpected trailing characters after JSON root value";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    void skip_ws() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool parse_value(JsonValue& out, int depth, std::string& err) {
        if (depth > kMaxDepth) {
            err = "JSON nesting depth exceeded";
            return false;
        }
        skip_ws();
        if (pos_ >= src_.size()) {
            err = "Unexpected end of JSON input";
            return false;
        }

        const char c = src_[pos_];
        if (c == '{') {
            return parse_object(out, depth + 1, err);
        }
        if (c == '[') {
            return parse_array(out, depth + 1, err);
        }
        if (c == '"') {
            out = JsonValue{};
            out.type = JsonValue::Type::String;
            return parse_string(out.string_val, err);
        }
        if (c == 't') {
            if (src_.compare(pos_, 4, "true") == 0) {
                pos_ += 4;
                out = JsonValue{};
                out.type = JsonValue::Type::Bool;
                out.bool_val = true;
                return true;
            }
            err = "Invalid token starting with 't'";
            return false;
        }
        if (c == 'f') {
            if (src_.compare(pos_, 5, "false") == 0) {
                pos_ += 5;
                out = JsonValue{};
                out.type = JsonValue::Type::Bool;
                out.bool_val = false;
                return true;
            }
            err = "Invalid token starting with 'f'";
            return false;
        }
        if (c == 'n') {
            if (src_.compare(pos_, 4, "null") == 0) {
                pos_ += 4;
                out = JsonValue{};
                out.type = JsonValue::Type::Null;
                return true;
            }
            err = "Invalid token starting with 'n'";
            return false;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(out, err);
        }

        err = std::string("Unexpected character in JSON: '") + c + "'";
        return false;
    }

    bool parse_object(JsonValue& out, int depth, std::string& err) {
        out = JsonValue{};
        out.type = JsonValue::Type::Object;
        ++pos_; // consume '{'
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }

        for (;;) {
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != '"') {
                err = "Expected string key in JSON object";
                return false;
            }
            std::string key;
            if (!parse_string(key, err)) {
                return false;
            }
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                err = "Expected ':' after key in JSON object";
                return false;
            }
            ++pos_; // consume ':'

            JsonValue val;
            if (!parse_value(val, depth, err)) {
                return false;
            }
            out.set_member(std::move(key), std::move(val));

            skip_ws();
            if (pos_ >= src_.size()) {
                err = "Unterminated JSON object";
                return false;
            }
            if (src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            err = "Expected ',' or '}' in JSON object";
            return false;
        }
    }

    bool parse_array(JsonValue& out, int depth, std::string& err) {
        out = JsonValue{};
        out.type = JsonValue::Type::Array;
        ++pos_; // consume '['
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return true;
        }

        for (;;) {
            JsonValue elem;
            if (!parse_value(elem, depth, err)) {
                return false;
            }
            out.array_val.push_back(std::move(elem));

            skip_ws();
            if (pos_ >= src_.size()) {
                err = "Unterminated JSON array";
                return false;
            }
            if (src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            err = "Expected ',' or ']' in JSON array";
            return false;
        }
    }

    bool parse_hex4(uint32_t& out_val) {
        if (pos_ + 4 > src_.size()) {
            return false;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            v <<= 4u;
            if (c >= '0' && c <= '9') {
                v |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<uint32_t>(10 + (c - 'a'));
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<uint32_t>(10 + (c - 'A'));
            } else {
                return false;
            }
        }
        out_val = v;
        return true;
    }

    static void append_utf8(std::string& dst, uint32_t cp) {
        if (cp <= 0x7Fu) {
            dst.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            dst.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
            dst.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            dst.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
            dst.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
            dst.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            dst.push_back(static_cast<char>(0xF0u | (cp >> 18u)));
            dst.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
            dst.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
            dst.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    bool parse_string(std::string& out, std::string& err) {
        out.clear();
        ++pos_; // consume opening '"'
        while (pos_ < src_.size()) {
            const unsigned char uc = static_cast<unsigned char>(src_[pos_++]);
            if (uc == '"') {
                return true;
            }
            if (uc < 0x20u) {
                err = "Unescaped control character in JSON string";
                return false;
            }
            if (uc != '\\') {
                out.push_back(static_cast<char>(uc));
                continue;
            }
            if (pos_ >= src_.size()) {
                err = "Unterminated escape sequence in JSON string";
                return false;
            }
            const char esc = src_[pos_++];
            switch (esc) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t cp = 0;
                if (!parse_hex4(cp)) {
                    err = "Invalid \\uXXXX escape in JSON string";
                    return false;
                }
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    // Surrogate pair
                    if (pos_ + 2 > src_.size() || src_[pos_] != '\\' || src_[pos_ + 1] != 'u') {
                        err = "Missing low surrogate in JSON string";
                        return false;
                    }
                    pos_ += 2;
                    uint32_t low = 0;
                    if (!parse_hex4(low) || low < 0xDC00u || low > 0xDFFFu) {
                        err = "Invalid low surrogate in JSON string";
                        return false;
                    }
                    cp = 0x10000u + (((cp - 0xD800u) << 10u) | (low - 0xDC00u));
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    err = "Unexpected lone low surrogate in JSON string";
                    return false;
                }
                append_utf8(out, cp);
                break;
            }
            default:
                err = "Invalid escape sequence in JSON string";
                return false;
            }
        }
        err = "Unterminated JSON string";
        return false;
    }

    bool parse_number(JsonValue& out, std::string& err) {
        const size_t start = pos_;
        if (src_[pos_] == '-') {
            ++pos_;
            if (pos_ >= src_.size()) {
                err = "Invalid number in JSON";
                return false;
            }
        }
        if (src_[pos_] == '0') {
            ++pos_;
            if (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                err = "Leading zeros not allowed in JSON number";
                return false;
            }
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        } else {
            err = "Invalid number format in JSON";
            return false;
        }

        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                err = "Invalid fractional part in JSON number";
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }

        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                err = "Invalid exponent in JSON number";
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }

        const std::string token = src_.substr(start, pos_ - start);
        std::istringstream iss(token);
        iss.imbue(std::locale::classic());
        double val = 0.0;
        iss >> val;
        if (iss.fail() || !iss.eof() || !std::isfinite(val)) {
            err = "Non-finite or out-of-range JSON number: " + token;
            return false;
        }

        out = JsonValue{};
        out.type = JsonValue::Type::Number;
        out.number_val = val;
        return true;
    }

    const std::string& src_;
    size_t pos_ = 0;
};

// ============================================================================
// JSON Serialization Helpers
// ============================================================================

std::string json_escape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20u) {
                static const char kHex[] = "0123456789abcdef";
                const uint32_t uc = static_cast<uint32_t>(c);
                out += "\\u00";
                out.push_back(kHex[(uc >> 4u) & 0xFu]);
                out.push_back(kHex[uc & 0xFu]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string format_number(double v, int max_decimals = 4) {
    if (!std::isfinite(v)) {
        return "0.0";
    }
    if (std::abs(v) < 1e-9) {
        v = 0.0;
    }
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::fixed << std::setprecision(max_decimals) << v;
    std::string s = oss.str();
    // Trim trailing zeros while preserving at least one decimal digit (e.g. "1.0")
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        while (s.size() > dot + 2 && s.back() == '0') {
            s.pop_back();
        }
    }
    return s;
}

// ============================================================================
// Field Extraction Helpers (Strict Type Validation)
// ============================================================================

bool read_opt_float(const JsonValue& obj, const std::string& key, float& dst, std::string& err,
                    bool* out_present = nullptr) {
    const JsonValue* v = obj.find(key);
    if (!v) {
        if (out_present) {
            *out_present = false;
        }
        return true;
    }
    if (v->type != JsonValue::Type::Number || !std::isfinite(v->number_val)) {
        err = "Field '" + key + "' must be a finite number";
        return false;
    }
    dst = static_cast<float>(v->number_val);
    if (out_present) {
        *out_present = true;
    }
    return true;
}

bool read_opt_int(const JsonValue& obj, const std::string& key, int& dst, std::string& err) {
    const JsonValue* v = obj.find(key);
    if (!v) {
        return true;
    }
    if (v->type != JsonValue::Type::Number || !std::isfinite(v->number_val)) {
        err = "Field '" + key + "' must be an integer number";
        return false;
    }
    dst = static_cast<int>(std::llround(v->number_val));
    return true;
}

bool read_opt_bool(const JsonValue& obj, const std::string& key, bool& dst, std::string& err) {
    const JsonValue* v = obj.find(key);
    if (!v) {
        return true;
    }
    if (v->type != JsonValue::Type::Bool) {
        err = "Field '" + key + "' must be a boolean";
        return false;
    }
    dst = v->bool_val;
    return true;
}

bool read_opt_string(const JsonValue& obj, const std::string& key, std::string& dst,
                     std::string& err) {
    const JsonValue* v = obj.find(key);
    if (!v) {
        return true;
    }
    if (v->type != JsonValue::Type::String) {
        err = "Field '" + key + "' must be a string";
        return false;
    }
    dst = v->string_val;
    return true;
}

bool check_schema_major_v1(const std::string& version) {
    if (version.empty()) {
        return false;
    }
    const size_t dot = version.find('.');
    const std::string major_str = (dot == std::string::npos) ? version : version.substr(0, dot);
    return major_str == "1";
}

std::string trim_ascii_ws(const std::string& s) {
    size_t start = 0;
    while (start < s.size() &&
           (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start &&
           (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(start, end - start);
}

bool is_windows_reserved_name(const std::string& stem) {
    std::string upper;
    upper.reserve(stem.size());
    for (unsigned char c : stem) {
        upper.push_back(static_cast<char>(std::toupper(c)));
    }
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return true;
    }
    if (upper.size() == 4 && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) &&
        upper[3] >= '1' && upper[3] <= '9') {
        return true;
    }
    return false;
}

bool ends_with_json_ci(const std::string& filename) {
    if (filename.size() < 5) {
        return false;
    }
    const std::string ext = filename.substr(filename.size() - 5);
    std::string lower;
    lower.reserve(5);
    for (unsigned char c : ext) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower == ".json";
}

std::string strip_json_ext(const std::string& filename) {
    if (ends_with_json_ci(filename)) {
        return filename.substr(0, filename.size() - 5);
    }
    return filename;
}

bool atomic_write_utf8_file(const std::filesystem::path& target_path, const std::string& content,
                            std::string& err) {
    std::filesystem::path tmp_path = target_path;
    tmp_path += std::filesystem::u8path(".tmp");

    {
        std::ofstream ofs(tmp_path, std::ios::binary);
        if (!ofs.is_open()) {
            err = "Failed to open temporary profile file for writing";
            return false;
        }
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        if (!ofs.good()) {
            err = "Failed while writing temporary profile file";
            ofs.close();
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, target_path, ec);
    if (ec) {
        // On some Windows implementations, rename over an existing file may fail;
        // try safe backup-swap if target exists, without losing data.
        std::error_code exists_ec;
        if (std::filesystem::exists(target_path, exists_ec)) {
            std::filesystem::path bak_path = target_path;
            bak_path += std::filesystem::u8path(".bak");
            std::error_code ec1;
            std::filesystem::remove(bak_path, ec1);
            std::filesystem::rename(target_path, bak_path, ec1);
            if (!ec1) {
                std::error_code ec2;
                std::filesystem::rename(tmp_path, target_path, ec2);
                if (!ec2) {
                    std::error_code ec3;
                    std::filesystem::remove(bak_path, ec3);
                    return true;
                }
                // Restore backup on failure
                std::error_code restore_ec;
                std::filesystem::rename(bak_path, target_path, restore_ec);
            }
        }
        err = "Failed to atomically rename temporary profile file: " + ec.message();
        std::error_code rm_ec;
        std::filesystem::remove(tmp_path, rm_ec);
        return false;
    }

    return true;
}

VoiceProfile make_clear_broadcast_preset() {
    VoiceProfile p = make_default_neutral_profile();
    p.profile_name = "Clear Broadcast";
    p.target_preset = "clear_broadcast";

    // Lip-noise removal works without learned thresholds; breath suppression needs a profile.
    p.dsp_params.declick.enabled = true;
    p.dsp_params.declick.max_reduction_db = 12.0f;

    p.dsp_params.input_gain_db = 0.0f;
    p.dsp_params.hpf.enabled = true;
    p.dsp_params.hpf.cutoff_hz = 85.0f;
    p.dsp_params.hpf.slope_db_per_oct = 24;

    p.dsp_params.eq_bands[0] = {0, true, EqBandType::LowShelf, 120.0f, -1.0f, 0.707f};
    p.dsp_params.eq_bands[1] = {1, true, EqBandType::Peaking, 250.0f, -2.0f, 1.8f};
    p.dsp_params.eq_bands[2] = {2, true, EqBandType::Peaking, 500.0f, -1.0f, 1.6f};
    p.dsp_params.eq_bands[3] = {3, false, EqBandType::Peaking, 1500.0f, 0.0f, 1.4f};
    p.dsp_params.eq_bands[4] = {4, true, EqBandType::Peaking, 3200.0f, 2.0f, 1.5f};
    p.dsp_params.eq_bands[5] = {5, false, EqBandType::Peaking, 5000.0f, 0.0f, 2.0f};
    p.dsp_params.eq_bands[6] = {6, true, EqBandType::Peaking, 7200.0f, -1.5f, 3.0f};
    p.dsp_params.eq_bands[7] = {7, true, EqBandType::HighShelf, 10000.0f, 1.5f, 0.707f};

    p.dsp_params.deesser.enabled = true;
    p.dsp_params.deesser.freq_hz = 6800.0f;
    p.dsp_params.deesser.bandwidth_q = 2.5f;
    p.dsp_params.deesser.threshold_dbfs = -24.0f;
    p.dsp_params.deesser.max_reduction_db = 6.0f;
    p.dsp_params.deesser.attack_ms = 1.0f;
    p.dsp_params.deesser.release_ms = 40.0f;

    p.dsp_params.compressor.enabled = true;
    p.dsp_params.compressor.threshold_dbfs = -22.0f;
    p.dsp_params.compressor.ratio = 3.2f;
    p.dsp_params.compressor.knee_db = 6.0f;
    p.dsp_params.compressor.attack_ms = 8.0f;
    p.dsp_params.compressor.release_ms = 100.0f;
    p.dsp_params.compressor.makeup_gain_db = 4.5f;

    p.dsp_params.limiter.enabled = true;
    p.dsp_params.limiter.ceiling_dbfs = -1.0f;
    p.dsp_params.limiter.lookahead_ms = 2.0f;
    p.dsp_params.limiter.release_ms = 50.0f;

    p.ui_overrides = {100.0f, 0.0f, -1.0f, 100.0f, 100.0f};
    clamp_voice_profile(p, kDefaultSampleRate);
    return p;
}

VoiceProfile make_warm_podcast_preset() {
    VoiceProfile p = make_default_neutral_profile();
    p.profile_name = "Warm Podcast";
    p.target_preset = "warm_podcast";

    // Gentler lip-noise removal: whisper / ASMR content may contain intentional mouth sounds.
    p.dsp_params.declick.enabled = true;
    p.dsp_params.declick.max_reduction_db = 6.0f;

    p.dsp_params.input_gain_db = 0.0f;
    p.dsp_params.hpf.enabled = true;
    p.dsp_params.hpf.cutoff_hz = 70.0f;
    p.dsp_params.hpf.slope_db_per_oct = 24;

    p.dsp_params.eq_bands[0] = {0, true, EqBandType::LowShelf, 120.0f, 2.0f, 0.707f};
    p.dsp_params.eq_bands[1] = {1, true, EqBandType::Peaking, 320.0f, -1.5f, 1.8f};
    p.dsp_params.eq_bands[2] = {2, false, EqBandType::Peaking, 500.0f, 0.0f, 1.6f};
    p.dsp_params.eq_bands[3] = {3, false, EqBandType::Peaking, 1500.0f, 0.0f, 1.4f};
    p.dsp_params.eq_bands[4] = {4, true, EqBandType::Peaking, 2800.0f, 1.2f, 1.4f};
    p.dsp_params.eq_bands[5] = {5, false, EqBandType::Peaking, 5000.0f, 0.0f, 2.0f};
    p.dsp_params.eq_bands[6] = {6, true, EqBandType::Peaking, 7200.0f, -2.0f, 3.2f};
    p.dsp_params.eq_bands[7] = {7, true, EqBandType::HighShelf, 11000.0f, 1.0f, 0.707f};

    p.dsp_params.deesser.enabled = true;
    p.dsp_params.deesser.freq_hz = 6500.0f;
    p.dsp_params.deesser.bandwidth_q = 2.5f;
    p.dsp_params.deesser.threshold_dbfs = -25.0f;
    p.dsp_params.deesser.max_reduction_db = 6.0f;
    p.dsp_params.deesser.attack_ms = 1.0f;
    p.dsp_params.deesser.release_ms = 40.0f;

    p.dsp_params.compressor.enabled = true;
    p.dsp_params.compressor.threshold_dbfs = -24.0f;
    p.dsp_params.compressor.ratio = 3.5f;
    p.dsp_params.compressor.knee_db = 8.0f;
    p.dsp_params.compressor.attack_ms = 8.0f;
    p.dsp_params.compressor.release_ms = 120.0f;
    p.dsp_params.compressor.makeup_gain_db = 5.5f;

    p.dsp_params.limiter.enabled = true;
    p.dsp_params.limiter.ceiling_dbfs = -1.0f;
    p.dsp_params.limiter.lookahead_ms = 2.0f;
    p.dsp_params.limiter.release_ms = 50.0f;

    p.ui_overrides = {100.0f, 0.0f, -1.0f, 100.0f, 100.0f};
    clamp_voice_profile(p, kDefaultSampleRate);
    return p;
}

} // namespace

// ============================================================================
// Public API
// ============================================================================

std::filesystem::path utf8_to_path(const std::string& utf8_str) {
    return std::filesystem::u8path(utf8_str);
}

std::string path_to_utf8(const std::filesystem::path& path) {
    return path.u8string();
}

std::string sanitize_profile_filename(const std::string& requested_name) {
    std::string cleaned;
    cleaned.reserve(requested_name.size());

    for (unsigned char c : requested_name) {
        if (c < 0x20u || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            cleaned.push_back('_');
        } else {
            cleaned.push_back(static_cast<char>(c));
        }
    }

    // Trim leading/trailing ASCII spaces and dots
    size_t start = 0;
    while (start < cleaned.size() &&
           (cleaned[start] == ' ' || cleaned[start] == '\t' || cleaned[start] == '.')) {
        ++start;
    }
    size_t end = cleaned.size();
    while (end > start &&
           (cleaned[end - 1] == ' ' || cleaned[end - 1] == '\t' || cleaned[end - 1] == '.')) {
        --end;
    }
    cleaned = cleaned.substr(start, end - start);

    // Truncate at UTF-8 codepoint boundary
    cleaned = truncate_utf8(cleaned, kMaxProfileNameChars);

    // Re-trim trailing spaces/dots after truncation
    while (!cleaned.empty() &&
           (cleaned.back() == ' ' || cleaned.back() == '\t' || cleaned.back() == '.')) {
        cleaned.pop_back();
    }

    if (cleaned.empty()) {
        return "custom_profile";
    }
    if (is_windows_reserved_name(cleaned)) {
        cleaned = "_" + cleaned;
    }
    return cleaned;
}

ProfileLoadResult parse_profile_json(const std::string& json_text, const std::string& fallback_name,
                                     uint32_t sample_rate) {

    ProfileLoadResult res;
    if (json_text.size() > kMaxProfileFileBytes) {
        res.error = ErrorCode::ErrFileTooLarge;
        res.error_message = "Profile JSON exceeds 1MB limit";
        return res;
    }

    JsonValue root;
    std::string parse_err;
    JsonParser parser(json_text);
    if (!parser.parse(root, parse_err)) {
        res.error = ErrorCode::ErrJsonParseFailed;
        res.error_message = parse_err;
        return res;
    }

    if (root.type != JsonValue::Type::Object) {
        res.error = ErrorCode::ErrJsonParseFailed;
        res.error_message = "Profile JSON root must be an object";
        return res;
    }

    // Validate required schema_version
    const JsonValue* schema_v = root.find("schema_version");
    if (!schema_v || schema_v->type != JsonValue::Type::String) {
        res.error = ErrorCode::ErrJsonParseFailed;
        res.error_message = "Missing or invalid 'schema_version'";
        return res;
    }
    if (!check_schema_major_v1(schema_v->string_val)) {
        res.error = ErrorCode::ErrSchemaUnsupported;
        res.error_message = "Unsupported schema_version: " + schema_v->string_val;
        return res;
    }

    // Validate required dsp_params object
    const JsonValue* dsp_obj = root.find("dsp_params");
    if (!dsp_obj || dsp_obj->type != JsonValue::Type::Object) {
        res.error = ErrorCode::ErrJsonParseFailed;
        res.error_message = "Missing or invalid 'dsp_params' object";
        return res;
    }

    VoiceProfile profile = make_default_neutral_profile();
    profile.schema_version = schema_v->string_val;
    profile.profile_name.clear();
    profile.target_preset.clear();

    std::string field_err;
    if (!read_opt_string(root, "profile_name", profile.profile_name, field_err) ||
        !read_opt_string(root, "created_at", profile.created_at, field_err) ||
        !read_opt_string(root, "updated_at", profile.updated_at, field_err) ||
        !read_opt_string(root, "target_preset", profile.target_preset, field_err)) {
        res.error = ErrorCode::ErrJsonParseFailed;
        res.error_message = field_err;
        return res;
    }

    profile.profile_name = trim_ascii_ws(profile.profile_name);
    if (profile.profile_name.empty()) {
        profile.profile_name = !fallback_name.empty() ? fallback_name : "Unnamed Profile";
    }
    if (profile.target_preset.empty()) {
        profile.target_preset = "custom";
    }

    // Optional analysis_metrics
    if (const JsonValue* metrics_v = root.find("analysis_metrics")) {
        if (metrics_v->type != JsonValue::Type::Object) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'analysis_metrics' must be an object";
            return res;
        }
        profile.analysis_metrics.present = true;
        int sr_int = static_cast<int>(kDefaultSampleRate);
        if (!read_opt_int(*metrics_v, "sample_rate_hz", sr_int, field_err) ||
            !read_opt_float(*metrics_v, "voiced_duration_sec",
                            profile.analysis_metrics.voiced_duration_sec, field_err) ||
            !read_opt_float(*metrics_v, "noise_floor_dbfs",
                            profile.analysis_metrics.noise_floor_dbfs, field_err) ||
            !read_opt_float(*metrics_v, "mean_rms_dbfs", profile.analysis_metrics.mean_rms_dbfs,
                            field_err) ||
            !read_opt_float(*metrics_v, "peak_dbfs", profile.analysis_metrics.peak_dbfs,
                            field_err) ||
            !read_opt_float(*metrics_v, "crest_factor_db", profile.analysis_metrics.crest_factor_db,
                            field_err) ||
            !read_opt_float(*metrics_v, "fundamental_freq_median_hz",
                            profile.analysis_metrics.fundamental_freq_median_hz, field_err) ||
            !read_opt_float(*metrics_v, "sibilance_ratio", profile.analysis_metrics.sibilance_ratio,
                            field_err)) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = field_err;
            return res;
        }
        profile.analysis_metrics.sample_rate_hz =
            sanitize_sample_rate(sr_int > 0 ? static_cast<uint32_t>(sr_int) : kDefaultSampleRate);
    }

    // dsp_params fields
    if (!read_opt_float(*dsp_obj, "input_gain_db", profile.dsp_params.input_gain_db, field_err)) {
        res.error = ErrorCode::ErrJsonParseFailed;
        res.error_message = field_err;
        return res;
    }

    if (const JsonValue* hpf_v = dsp_obj->find("hpf")) {
        if (hpf_v->type != JsonValue::Type::Object) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'dsp_params.hpf' must be an object";
            return res;
        }
        if (!read_opt_bool(*hpf_v, "enabled", profile.dsp_params.hpf.enabled, field_err) ||
            !read_opt_float(*hpf_v, "cutoff_hz", profile.dsp_params.hpf.cutoff_hz, field_err) ||
            !read_opt_int(*hpf_v, "slope_db_per_oct", profile.dsp_params.hpf.slope_db_per_oct,
                          field_err)) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = field_err;
            return res;
        }
    }

    if (const JsonValue* eq_v = dsp_obj->find("eq_bands")) {
        if (eq_v->type != JsonValue::Type::Array) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'dsp_params.eq_bands' must be an array";
            return res;
        }
        // Reset all 8 bands to disabled defaults; populate up to first 8 items
        for (uint32_t i = 0; i < kNumEqBands; ++i) {
            profile.dsp_params.eq_bands[i] = EqBandParams{};
            profile.dsp_params.eq_bands[i].index = static_cast<int>(i);
            profile.dsp_params.eq_bands[i].enabled = false;
        }
        const size_t count = std::min<size_t>(eq_v->array_val.size(), kNumEqBands);
        for (size_t i = 0; i < count; ++i) {
            const JsonValue& band_v = eq_v->array_val[i];
            if (band_v.type != JsonValue::Type::Object) {
                res.error = ErrorCode::ErrJsonParseFailed;
                res.error_message = "Each entry in 'eq_bands' must be an object";
                return res;
            }
            EqBandParams& band = profile.dsp_params.eq_bands[i];
            band.index = static_cast<int>(i);
            band.enabled = true;
            std::string type_str = "peaking";

            if (!read_opt_bool(band_v, "enabled", band.enabled, field_err) ||
                !read_opt_string(band_v, "type", type_str, field_err) ||
                !read_opt_float(band_v, "freq_hz", band.freq_hz, field_err) ||
                !read_opt_float(band_v, "gain_db", band.gain_db, field_err) ||
                !read_opt_float(band_v, "q", band.q, field_err)) {
                res.error = ErrorCode::ErrJsonParseFailed;
                res.error_message = field_err;
                return res;
            }
            if (!parse_eq_band_type(type_str, band.type)) {
                res.error = ErrorCode::ErrJsonParseFailed;
                res.error_message = "Invalid EQ band type: " + type_str;
                return res;
            }
        }
    }

    if (const JsonValue* dc_v = dsp_obj->find("declick")) {
        if (dc_v->type != JsonValue::Type::Object) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'dsp_params.declick' must be an object";
            return res;
        }
        DeclickParams& dc = profile.dsp_params.declick;
        if (!read_opt_bool(*dc_v, "enabled", dc.enabled, field_err) ||
            !read_opt_float(*dc_v, "sensitivity_db", dc.sensitivity_db, field_err) ||
            !read_opt_float(*dc_v, "max_reduction_db", dc.max_reduction_db, field_err) ||
            !read_opt_float(*dc_v, "crossover_hz", dc.crossover_hz, field_err)) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = field_err;
            return res;
        }
    }

    if (const JsonValue* br_v = dsp_obj->find("breath")) {
        if (br_v->type != JsonValue::Type::Object) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'dsp_params.breath' must be an object";
            return res;
        }
        BreathParams& br = profile.dsp_params.breath;
        if (!read_opt_bool(*br_v, "enabled", br.enabled, field_err) ||
            !read_opt_float(*br_v, "max_reduction_db", br.max_reduction_db, field_err) ||
            !read_opt_float(*br_v, "level_ceiling_dbfs", br.level_ceiling_dbfs, field_err) ||
            !read_opt_float(*br_v, "tilt_threshold_db", br.tilt_threshold_db, field_err) ||
            !read_opt_float(*br_v, "min_duration_ms", br.min_duration_ms, field_err) ||
            !read_opt_float(*br_v, "hangover_ms", br.hangover_ms, field_err) ||
            !read_opt_float(*br_v, "attack_ms", br.attack_ms, field_err) ||
            !read_opt_float(*br_v, "release_ms", br.release_ms, field_err)) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = field_err;
            return res;
        }
    }

    if (const JsonValue* de_v = dsp_obj->find("deesser")) {
        if (de_v->type != JsonValue::Type::Object) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'dsp_params.deesser' must be an object";
            return res;
        }
        if (!read_opt_bool(*de_v, "enabled", profile.dsp_params.deesser.enabled, field_err) ||
            !read_opt_float(*de_v, "freq_hz", profile.dsp_params.deesser.freq_hz, field_err) ||
            !read_opt_float(*de_v, "bandwidth_q", profile.dsp_params.deesser.bandwidth_q,
                            field_err) ||
            !read_opt_float(*de_v, "threshold_dbfs", profile.dsp_params.deesser.threshold_dbfs,
                            field_err) ||
            !read_opt_float(*de_v, "max_reduction_db", profile.dsp_params.deesser.max_reduction_db,
                            field_err) ||
            !read_opt_float(*de_v, "attack_ms", profile.dsp_params.deesser.attack_ms, field_err) ||
            !read_opt_float(*de_v, "release_ms", profile.dsp_params.deesser.release_ms,
                            field_err)) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = field_err;
            return res;
        }
    }

    if (const JsonValue* comp_v = dsp_obj->find("compressor")) {
        if (comp_v->type != JsonValue::Type::Object) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'dsp_params.compressor' must be an object";
            return res;
        }
        if (!read_opt_bool(*comp_v, "enabled", profile.dsp_params.compressor.enabled, field_err) ||
            !read_opt_float(*comp_v, "threshold_dbfs", profile.dsp_params.compressor.threshold_dbfs,
                            field_err) ||
            !read_opt_float(*comp_v, "ratio", profile.dsp_params.compressor.ratio, field_err) ||
            !read_opt_float(*comp_v, "knee_db", profile.dsp_params.compressor.knee_db, field_err) ||
            !read_opt_float(*comp_v, "attack_ms", profile.dsp_params.compressor.attack_ms,
                            field_err) ||
            !read_opt_float(*comp_v, "release_ms", profile.dsp_params.compressor.release_ms,
                            field_err) ||
            !read_opt_float(*comp_v, "makeup_gain_db", profile.dsp_params.compressor.makeup_gain_db,
                            field_err)) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = field_err;
            return res;
        }
    }

    if (const JsonValue* lim_v = dsp_obj->find("limiter")) {
        if (lim_v->type != JsonValue::Type::Object) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'dsp_params.limiter' must be an object";
            return res;
        }
        if (!read_opt_bool(*lim_v, "enabled", profile.dsp_params.limiter.enabled, field_err) ||
            !read_opt_float(*lim_v, "ceiling_dbfs", profile.dsp_params.limiter.ceiling_dbfs,
                            field_err) ||
            !read_opt_float(*lim_v, "lookahead_ms", profile.dsp_params.limiter.lookahead_ms,
                            field_err) ||
            !read_opt_float(*lim_v, "release_ms", profile.dsp_params.limiter.release_ms,
                            field_err)) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = field_err;
            return res;
        }
    }

    // Optional ui_overrides: default limiter_ceiling_dbfs to dsp_params.limiter.ceiling_dbfs
    profile.ui_overrides.mix_percent = 100.0f;
    profile.ui_overrides.output_makeup_trim_db = 0.0f;
    profile.ui_overrides.limiter_ceiling_dbfs = profile.dsp_params.limiter.ceiling_dbfs;

    if (const JsonValue* ov_v = root.find("ui_overrides")) {
        if (ov_v->type != JsonValue::Type::Object) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "'ui_overrides' must be an object";
            return res;
        }
        bool has_ceiling = false;
        if (!read_opt_float(*ov_v, "mix_percent", profile.ui_overrides.mix_percent, field_err) ||
            !read_opt_float(*ov_v, "output_makeup_trim_db",
                            profile.ui_overrides.output_makeup_trim_db, field_err) ||
            !read_opt_float(*ov_v, "limiter_ceiling_dbfs",
                            profile.ui_overrides.limiter_ceiling_dbfs, field_err, &has_ceiling) ||
            !read_opt_float(*ov_v, "declick_amount_percent",
                            profile.ui_overrides.declick_amount_percent, field_err) ||
            !read_opt_float(*ov_v, "breath_amount_percent",
                            profile.ui_overrides.breath_amount_percent, field_err)) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = field_err;
            return res;
        }
        if (!has_ceiling) {
            profile.ui_overrides.limiter_ceiling_dbfs = profile.dsp_params.limiter.ceiling_dbfs;
        }
    }

    clamp_voice_profile(profile, sample_rate);
    res.error = ErrorCode::Ok;
    res.profile = std::move(profile);
    return res;
}

std::string serialize_profile_json(const VoiceProfile& raw_profile) {
    VoiceProfile p = raw_profile;
    clamp_voice_profile(p, kDefaultSampleRate);

    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "{\n";
    oss << "  \"schema_version\": " << json_escape_string(p.schema_version) << ",\n";
    oss << "  \"profile_name\": " << json_escape_string(p.profile_name) << ",\n";
    if (!p.created_at.empty()) {
        oss << "  \"created_at\": " << json_escape_string(p.created_at) << ",\n";
    }
    if (!p.updated_at.empty()) {
        oss << "  \"updated_at\": " << json_escape_string(p.updated_at) << ",\n";
    }
    oss << "  \"target_preset\": " << json_escape_string(p.target_preset) << ",\n";

    oss << "  \"ui_overrides\": {\n";
    oss << "    \"mix_percent\": " << format_number(p.ui_overrides.mix_percent, 2) << ",\n";
    oss << "    \"output_makeup_trim_db\": "
        << format_number(p.ui_overrides.output_makeup_trim_db, 2) << ",\n";
    oss << "    \"limiter_ceiling_dbfs\": " << format_number(p.ui_overrides.limiter_ceiling_dbfs, 2)
        << ",\n";
    oss << "    \"declick_amount_percent\": "
        << format_number(p.ui_overrides.declick_amount_percent, 2) << ",\n";
    oss << "    \"breath_amount_percent\": "
        << format_number(p.ui_overrides.breath_amount_percent, 2) << "\n";
    oss << "  },\n";

    if (p.analysis_metrics.present) {
        oss << "  \"analysis_metrics\": {\n";
        oss << "    \"sample_rate_hz\": " << p.analysis_metrics.sample_rate_hz << ",\n";
        oss << "    \"voiced_duration_sec\": "
            << format_number(p.analysis_metrics.voiced_duration_sec, 2) << ",\n";
        oss << "    \"noise_floor_dbfs\": " << format_number(p.analysis_metrics.noise_floor_dbfs, 2)
            << ",\n";
        oss << "    \"mean_rms_dbfs\": " << format_number(p.analysis_metrics.mean_rms_dbfs, 2)
            << ",\n";
        oss << "    \"peak_dbfs\": " << format_number(p.analysis_metrics.peak_dbfs, 2) << ",\n";
        oss << "    \"crest_factor_db\": " << format_number(p.analysis_metrics.crest_factor_db, 2)
            << ",\n";
        oss << "    \"fundamental_freq_median_hz\": "
            << format_number(p.analysis_metrics.fundamental_freq_median_hz, 2) << ",\n";
        oss << "    \"sibilance_ratio\": " << format_number(p.analysis_metrics.sibilance_ratio, 4)
            << "\n";
        oss << "  },\n";
    }

    oss << "  \"dsp_params\": {\n";
    oss << "    \"input_gain_db\": " << format_number(p.dsp_params.input_gain_db, 2) << ",\n";

    oss << "    \"hpf\": {\n";
    oss << "      \"enabled\": " << (p.dsp_params.hpf.enabled ? "true" : "false") << ",\n";
    oss << "      \"cutoff_hz\": " << format_number(p.dsp_params.hpf.cutoff_hz, 2) << ",\n";
    oss << "      \"slope_db_per_oct\": " << p.dsp_params.hpf.slope_db_per_oct << "\n";
    oss << "    },\n";

    const DeclickParams& dc = p.dsp_params.declick;
    oss << "    \"declick\": {\n";
    oss << "      \"enabled\": " << (dc.enabled ? "true" : "false") << ",\n";
    oss << "      \"sensitivity_db\": " << format_number(dc.sensitivity_db, 2) << ",\n";
    oss << "      \"max_reduction_db\": " << format_number(dc.max_reduction_db, 2) << ",\n";
    oss << "      \"crossover_hz\": " << format_number(dc.crossover_hz, 1) << "\n";
    oss << "    },\n";

    const BreathParams& br = p.dsp_params.breath;
    oss << "    \"breath\": {\n";
    oss << "      \"enabled\": " << (br.enabled ? "true" : "false") << ",\n";
    oss << "      \"max_reduction_db\": " << format_number(br.max_reduction_db, 2) << ",\n";
    oss << "      \"level_ceiling_dbfs\": " << format_number(br.level_ceiling_dbfs, 2) << ",\n";
    oss << "      \"tilt_threshold_db\": " << format_number(br.tilt_threshold_db, 2) << ",\n";
    oss << "      \"min_duration_ms\": " << format_number(br.min_duration_ms, 2) << ",\n";
    oss << "      \"hangover_ms\": " << format_number(br.hangover_ms, 2) << ",\n";
    oss << "      \"attack_ms\": " << format_number(br.attack_ms, 2) << ",\n";
    oss << "      \"release_ms\": " << format_number(br.release_ms, 2) << "\n";
    oss << "    },\n";

    oss << "    \"eq_bands\": [\n";
    for (uint32_t i = 0; i < kNumEqBands; ++i) {
        const EqBandParams& b = p.dsp_params.eq_bands[i];
        oss << "      { \"index\": " << i << ", \"enabled\": " << (b.enabled ? "true" : "false")
            << ", \"type\": " << json_escape_string(eq_band_type_to_string(b.type))
            << ", \"freq_hz\": " << format_number(b.freq_hz, 2)
            << ", \"gain_db\": " << format_number(b.gain_db, 2)
            << ", \"q\": " << format_number(b.q, 3) << " }" << (i + 1 < kNumEqBands ? "," : "")
            << "\n";
    }
    oss << "    ],\n";

    oss << "    \"deesser\": {\n";
    oss << "      \"enabled\": " << (p.dsp_params.deesser.enabled ? "true" : "false") << ",\n";
    oss << "      \"freq_hz\": " << format_number(p.dsp_params.deesser.freq_hz, 2) << ",\n";
    oss << "      \"bandwidth_q\": " << format_number(p.dsp_params.deesser.bandwidth_q, 3) << ",\n";
    oss << "      \"threshold_dbfs\": " << format_number(p.dsp_params.deesser.threshold_dbfs, 2)
        << ",\n";
    oss << "      \"max_reduction_db\": " << format_number(p.dsp_params.deesser.max_reduction_db, 2)
        << ",\n";
    oss << "      \"attack_ms\": " << format_number(p.dsp_params.deesser.attack_ms, 2) << ",\n";
    oss << "      \"release_ms\": " << format_number(p.dsp_params.deesser.release_ms, 2) << "\n";
    oss << "    },\n";

    oss << "    \"compressor\": {\n";
    oss << "      \"enabled\": " << (p.dsp_params.compressor.enabled ? "true" : "false") << ",\n";
    oss << "      \"threshold_dbfs\": " << format_number(p.dsp_params.compressor.threshold_dbfs, 2)
        << ",\n";
    oss << "      \"ratio\": " << format_number(p.dsp_params.compressor.ratio, 3) << ",\n";
    oss << "      \"knee_db\": " << format_number(p.dsp_params.compressor.knee_db, 2) << ",\n";
    oss << "      \"attack_ms\": " << format_number(p.dsp_params.compressor.attack_ms, 2) << ",\n";
    oss << "      \"release_ms\": " << format_number(p.dsp_params.compressor.release_ms, 2)
        << ",\n";
    oss << "      \"makeup_gain_db\": " << format_number(p.dsp_params.compressor.makeup_gain_db, 2)
        << "\n";
    oss << "    },\n";

    oss << "    \"limiter\": {\n";
    oss << "      \"enabled\": " << (p.dsp_params.limiter.enabled ? "true" : "false") << ",\n";
    oss << "      \"ceiling_dbfs\": " << format_number(p.dsp_params.limiter.ceiling_dbfs, 2)
        << ",\n";
    oss << "      \"lookahead_ms\": " << format_number(p.dsp_params.limiter.lookahead_ms, 2)
        << ",\n";
    oss << "      \"release_ms\": " << format_number(p.dsp_params.limiter.release_ms, 2) << "\n";
    oss << "    }\n";

    oss << "  }\n";
    oss << "}\n";
    return oss.str();
}

ProfileLoadResult load_profile_file(const std::string& file_path_utf8, uint32_t sample_rate) {
    ProfileLoadResult res;
    const std::filesystem::path path = utf8_to_path(file_path_utf8);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec || !std::filesystem::is_regular_file(path, ec) ||
        ec) {
        res.error = ErrorCode::ErrFileNotFound;
        res.error_message = "Profile file not found: " + file_path_utf8;
        return res;
    }

    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        res.error = ErrorCode::ErrFileNotFound;
        res.error_message = "Unable to query profile file size: " + file_path_utf8;
        return res;
    }
    if (file_size > kMaxProfileFileBytes) {
        res.error = ErrorCode::ErrFileTooLarge;
        res.error_message = "Profile file exceeds 1MB limit: " + file_path_utf8;
        return res;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        res.error = ErrorCode::ErrFileNotFound;
        res.error_message = "Failed to open profile file: " + file_path_utf8;
        return res;
    }

    std::string content(static_cast<size_t>(file_size), '\0');
    if (file_size > 0) {
        ifs.read(content.data(), static_cast<std::streamsize>(file_size));
        if (!ifs) {
            res.error = ErrorCode::ErrJsonParseFailed;
            res.error_message = "Failed to read profile file contents: " + file_path_utf8;
            return res;
        }
    }

    const std::string fallback_name = strip_json_ext(path_to_utf8(path.filename()));
    return parse_profile_json(content, fallback_name, sample_rate);
}

bool get_builtin_preset(const std::string& id_or_name, VoiceProfile& out_profile) {
    if (id_or_name == "builtin:neutral_safe" || id_or_name == "neutral_safe") {
        out_profile = make_default_neutral_profile();
        return true;
    }
    if (id_or_name == "builtin:clear_broadcast" || id_or_name == "clear_broadcast") {
        out_profile = make_clear_broadcast_preset();
        return true;
    }
    if (id_or_name == "builtin:warm_podcast" || id_or_name == "warm_podcast") {
        out_profile = make_warm_podcast_preset();
        return true;
    }
    return false;
}

std::vector<ProfileEntry> list_builtin_presets() {
    return {
        {"builtin:neutral_safe", "Neutral Safe", "Neutral Safe", "", "", true},
        {"builtin:clear_broadcast", "Clear Broadcast", "Clear Broadcast", "", "", true},
        {"builtin:warm_podcast", "Warm Podcast", "Warm Podcast", "", "", true},
    };
}

std::vector<ProfileEntry> list_profiles(const std::string& dir_path_utf8, bool include_builtins) {
    std::vector<ProfileEntry> result;
    if (include_builtins) {
        result = list_builtin_presets();
    }

    if (dir_path_utf8.empty()) {
        return result;
    }

    const std::filesystem::path dir_path = utf8_to_path(dir_path_utf8);
    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec) || ec ||
        !std::filesystem::is_directory(dir_path, ec) || ec) {
        return result;
    }

    std::vector<ProfileEntry> user_entries;
    for (std::filesystem::directory_iterator it(dir_path, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || entry_ec) {
            continue;
        }
        const std::filesystem::path entry_path = it->path();
        const std::string fname = path_to_utf8(entry_path.filename());
        if (!ends_with_json_ci(fname)) {
            continue;
        }

        const std::string full_utf8 = path_to_utf8(entry_path);
        const ProfileLoadResult loaded = load_profile_file(full_utf8, kDefaultSampleRate);
        if (!loaded) {
            // Skip corrupted / invalid / oversized / unsupported JSON without breaking listing
            continue;
        }

        ProfileEntry entry;
        entry.id = "file:" + fname;
        entry.profile_name = loaded.profile.profile_name;
        entry.display_name = loaded.profile.profile_name;
        entry.file_name = fname;
        entry.file_path = full_utf8;
        entry.is_builtin = false;
        user_entries.push_back(std::move(entry));
    }

    std::sort(
        user_entries.begin(), user_entries.end(),
        [](const ProfileEntry& a, const ProfileEntry& b) { return a.file_name < b.file_name; });

    // Disambiguate duplicate profile_name labels using filename per docs/02_data_modeling.md 3.1
    std::unordered_map<std::string, size_t> name_counts;
    for (const auto& entry : user_entries) {
        ++name_counts[entry.profile_name];
    }
    for (auto& entry : user_entries) {
        if (name_counts[entry.profile_name] > 1) {
            entry.display_name = entry.profile_name + " (" + entry.file_name + ")";
        }
    }

    result.insert(result.end(), std::make_move_iterator(user_entries.begin()),
                  std::make_move_iterator(user_entries.end()));
    return result;
}

ProfileLoadResult load_profile_by_id(const std::string& profile_id,
                                     const std::string& dir_path_utf8, uint32_t sample_rate) {

    ProfileLoadResult res;
    if (profile_id.rfind("builtin:", 0) == 0) {
        VoiceProfile preset;
        if (get_builtin_preset(profile_id, preset)) {
            clamp_voice_profile(preset, sample_rate);
            res.error = ErrorCode::Ok;
            res.profile = std::move(preset);
            return res;
        }
        res.error = ErrorCode::ErrFileNotFound;
        res.error_message = "Unknown builtin preset: " + profile_id;
        return res;
    }

    if (profile_id.rfind("file:", 0) == 0) {
        const std::string fname = profile_id.substr(5);
        if (fname.empty() || fname.find('/') != std::string::npos ||
            fname.find('\\') != std::string::npos || fname.find("..") != std::string::npos) {
            res.error = ErrorCode::ErrFileNotFound;
            res.error_message = "Invalid profile file identifier: " + profile_id;
            return res;
        }
        const std::filesystem::path full_path = utf8_to_path(dir_path_utf8) / utf8_to_path(fname);
        return load_profile_file(path_to_utf8(full_path), sample_rate);
    }

    res.error = ErrorCode::ErrFileNotFound;
    res.error_message = "Unsupported profile identifier format: " + profile_id;
    return res;
}

ProfileSaveResult save_profile_new(const std::string& dir_path_utf8,
                                   const std::string& requested_name, const VoiceProfile& profile) {

    ProfileSaveResult res;
    const std::filesystem::path dir_path = utf8_to_path(dir_path_utf8);

    std::error_code ec;
    std::filesystem::create_directories(dir_path, ec);
    if (ec) {
        res.error = ErrorCode::ErrProfileSaveFailed;
        res.error_message = "Failed to create profiles directory: " + ec.message();
        return res;
    }

    VoiceProfile to_save = profile;
    const std::string trimmed_name = trim_ascii_ws(requested_name);
    if (!trimmed_name.empty()) {
        to_save.profile_name = truncate_utf8(trimmed_name, kMaxProfileNameChars);
    } else if (trim_ascii_ws(to_save.profile_name).empty()) {
        to_save.profile_name = "custom_profile";
    }

    const std::string now_ts = current_utc_iso8601();
    if (to_save.created_at.empty()) {
        to_save.created_at = now_ts;
    }
    to_save.updated_at = now_ts;
    clamp_voice_profile(to_save, kDefaultSampleRate);

    const std::string stem =
        sanitize_profile_filename(!trimmed_name.empty() ? trimmed_name : to_save.profile_name);

    // Resolve unique filename with auto-increment suffix (_2, _3, ...) on collision
    std::string candidate_fname = stem + ".json";
    std::filesystem::path target_path = dir_path / utf8_to_path(candidate_fname);
    uint32_t suffix = 2;
    while (std::filesystem::exists(target_path, ec)) {
        candidate_fname = stem + "_" + std::to_string(suffix) + ".json";
        target_path = dir_path / utf8_to_path(candidate_fname);
        ++suffix;
        if (suffix > 100000u) {
            res.error = ErrorCode::ErrProfileSaveFailed;
            res.error_message = "Unable to allocate unique profile filename";
            return res;
        }
    }

    const std::string json_text = serialize_profile_json(to_save);
    std::string write_err;
    if (!atomic_write_utf8_file(target_path, json_text, write_err)) {
        res.error = ErrorCode::ErrProfileSaveFailed;
        res.error_message = write_err;
        return res;
    }

    res.error = ErrorCode::Ok;
    res.saved_file_name = candidate_fname;
    res.saved_id = "file:" + candidate_fname;
    res.saved_file_path = path_to_utf8(target_path);
    return res;
}

ProfileSaveResult save_profile_overwrite(const std::string& file_path_utf8,
                                         const VoiceProfile& profile) {

    ProfileSaveResult res;
    if (file_path_utf8.empty()) {
        res.error = ErrorCode::ErrProfileSaveFailed;
        res.error_message = "Empty target path for overwrite save";
        return res;
    }

    const std::filesystem::path target_path = utf8_to_path(file_path_utf8);
    if (target_path.has_parent_path()) {
        std::error_code dir_ec;
        std::filesystem::create_directories(target_path.parent_path(), dir_ec);
        if (dir_ec) {
            res.error = ErrorCode::ErrProfileSaveFailed;
            res.error_message = "Failed to ensure parent directory exists: " + dir_ec.message();
            return res;
        }
    }

    VoiceProfile to_save = profile;
    const std::string now_ts = current_utc_iso8601();
    if (to_save.created_at.empty()) {
        to_save.created_at = now_ts;
    }
    to_save.updated_at = now_ts;
    clamp_voice_profile(to_save, kDefaultSampleRate);

    const std::string json_text = serialize_profile_json(to_save);
    std::string write_err;
    if (!atomic_write_utf8_file(target_path, json_text, write_err)) {
        res.error = ErrorCode::ErrProfileSaveFailed;
        res.error_message = write_err;
        return res;
    }

    res.error = ErrorCode::Ok;
    res.saved_file_name = path_to_utf8(target_path.filename());
    res.saved_id = "file:" + res.saved_file_name;
    res.saved_file_path = path_to_utf8(target_path);
    return res;
}

} // namespace kotone
