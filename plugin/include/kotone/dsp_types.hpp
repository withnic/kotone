#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kotone {

constexpr uint32_t kMaxChannels = 8;
constexpr uint32_t kNumEqBands = 8;
constexpr uint32_t kMinSampleRate = 8000;
constexpr uint32_t kMaxSampleRate = 192000;
constexpr uint32_t kDefaultSampleRate = 48000;
constexpr uint32_t kMaxLookaheadSamples = 960; // 5.0 ms @ 192 kHz
constexpr uint32_t kSnapshotSlotCount = 16;
constexpr size_t kMaxProfileFileBytes = 1024u * 1024u; // 1 MB
constexpr size_t kMaxProfileNameChars = 64u;
constexpr double kPi = 3.14159265358979323846;

enum class ErrorCode : uint32_t {
    Ok = 0,
    ErrFileNotFound,
    ErrFileTooLarge,
    ErrProfileSaveFailed,
    ErrJsonParseFailed,
    ErrSchemaUnsupported,
    ErrDspNonFinite
};

const char* error_code_to_string(ErrorCode code) noexcept;

enum class EngineState : uint32_t { Uninitialized = 0, ActiveProfile, FallbackSafe, Bypassed };

enum class EqBandType : uint8_t { Peaking = 0, LowShelf, HighShelf };

const char* eq_band_type_to_string(EqBandType type) noexcept;
bool parse_eq_band_type(const std::string& str, EqBandType& out_type) noexcept;

struct HpfParams {
    bool enabled = true;
    float cutoff_hz = 80.0f;
    int slope_db_per_oct = 24;
};

struct EqBandParams {
    int index = 0;
    bool enabled = false;
    EqBandType type = EqBandType::Peaking;
    float freq_hz = 1000.0f;
    float gain_db = 0.0f;
    float q = 1.0f;
};

struct DeEsserParams {
    bool enabled = false;
    float freq_hz = 6500.0f;
    float bandwidth_q = 2.5f;
    float threshold_dbfs = -22.0f;
    float max_reduction_db = 6.0f;
    float attack_ms = 1.0f;
    float release_ms = 40.0f;
};

struct CompressorParams {
    bool enabled = true;
    float threshold_dbfs = -20.0f;
    float ratio = 3.0f;
    float knee_db = 6.0f;
    float attack_ms = 10.0f;
    float release_ms = 100.0f;
    float makeup_gain_db = 4.0f;
};

struct LimiterParams {
    bool enabled = true;
    float ceiling_dbfs = -1.0f;
    float lookahead_ms = 2.0f;
    float release_ms = 50.0f;
};

struct DspParams {
    float input_gain_db = 0.0f;
    HpfParams hpf{};
    std::array<EqBandParams, kNumEqBands> eq_bands{};
    DeEsserParams deesser{};
    CompressorParams compressor{};
    LimiterParams limiter{};
};

struct UiOverrides {
    float mix_percent = 100.0f;
    float output_makeup_trim_db = 0.0f;
    float limiter_ceiling_dbfs = -1.0f;
};

struct AnalysisMetrics {
    bool present = false;
    uint32_t sample_rate_hz = kDefaultSampleRate;
    float voiced_duration_sec = 0.0f;
    float noise_floor_dbfs = -60.0f;
    float mean_rms_dbfs = -24.0f;
    float peak_dbfs = -6.0f;
    float crest_factor_db = 18.0f;
    float fundamental_freq_median_hz = 150.0f;
    float sibilance_ratio = 0.1f;
};

struct VoiceProfile {
    std::string schema_version = "1.0.0";
    std::string profile_name = "Neutral Safe";
    std::string created_at;
    std::string updated_at;
    std::string target_preset = "neutral_safe";
    UiOverrides ui_overrides{};
    AnalysisMetrics analysis_metrics{};
    DspParams dsp_params{};
};

// Numeric & safety helpers (realtime-safe)
uint32_t sanitize_sample_rate(uint32_t sample_rate) noexcept;
double db_to_linear(double db) noexcept;
float db_to_linear_f(float db) noexcept;
double linear_to_db(double linear) noexcept;
float clamp_float(float value, float min_val, float max_val, float fallback) noexcept;
double flush_denormal(double value) noexcept;

// Boundary clamping per docs/02_data_modeling.md Section 2.2
void clamp_dsp_params(DspParams& params, uint32_t sample_rate = kDefaultSampleRate) noexcept;
void clamp_ui_overrides(UiOverrides& overrides) noexcept;
void clamp_voice_profile(VoiceProfile& profile, uint32_t sample_rate = kDefaultSampleRate);

// Truncates a UTF-8 string to at most max_codepoints without splitting multi-byte sequences.
std::string truncate_utf8(const std::string& input, size_t max_codepoints);

// Returns a default safe neutral VoiceProfile (flat EQ, HPF 80Hz, safe -1.0dBFS limiter).
VoiceProfile make_default_neutral_profile();

} // namespace kotone
