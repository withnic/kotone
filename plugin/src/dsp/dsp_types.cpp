#include "kotone/dsp_types.hpp"

#include <algorithm>
#include <cmath>

namespace kotone {

const char* error_code_to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "OK";
    case ErrorCode::ErrFileNotFound:
        return "ERR_FILE_NOT_FOUND";
    case ErrorCode::ErrFileTooLarge:
        return "ERR_FILE_TOO_LARGE";
    case ErrorCode::ErrProfileSaveFailed:
        return "ERR_PROFILE_SAVE_FAILED";
    case ErrorCode::ErrJsonParseFailed:
        return "ERR_JSON_PARSE_FAILED";
    case ErrorCode::ErrSchemaUnsupported:
        return "ERR_SCHEMA_UNSUPPORTED";
    case ErrorCode::ErrDspNonFinite:
        return "ERR_DSP_NON_FINITE";
    }
    return "ERR_UNKNOWN";
}

const char* eq_band_type_to_string(EqBandType type) noexcept {
    switch (type) {
    case EqBandType::Peaking:
        return "peaking";
    case EqBandType::LowShelf:
        return "low_shelf";
    case EqBandType::HighShelf:
        return "high_shelf";
    }
    return "peaking";
}

bool parse_eq_band_type(const std::string& str, EqBandType& out_type) noexcept {
    if (str == "peaking") {
        out_type = EqBandType::Peaking;
        return true;
    }
    if (str == "low_shelf") {
        out_type = EqBandType::LowShelf;
        return true;
    }
    if (str == "high_shelf") {
        out_type = EqBandType::HighShelf;
        return true;
    }
    return false;
}

uint32_t sanitize_sample_rate(uint32_t sample_rate) noexcept {
    if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate) {
        return kDefaultSampleRate;
    }
    return sample_rate;
}

double db_to_linear(double db) noexcept {
    if (!std::isfinite(db)) {
        return 1.0;
    }
    const double clamped = std::clamp(db, -144.0, 48.0);
    const double lin = std::pow(10.0, clamped / 20.0);
    return std::isfinite(lin) ? lin : 1.0;
}

float db_to_linear_f(float db) noexcept {
    return static_cast<float>(db_to_linear(static_cast<double>(db)));
}

double linear_to_db(double linear) noexcept {
    if (!std::isfinite(linear) || linear <= 1e-12) {
        return -240.0;
    }
    return 20.0 * std::log10(linear);
}

float clamp_float(float value, float min_val, float max_val, float fallback) noexcept {
    if (!std::isfinite(value)) {
        return std::clamp(fallback, min_val, max_val);
    }
    if (min_val > max_val) {
        return min_val;
    }
    return std::clamp(value, min_val, max_val);
}

double flush_denormal(double value) noexcept {
    if (!std::isfinite(value) || std::abs(value) < 1e-15) {
        return 0.0;
    }
    return value;
}

void clamp_dsp_params(DspParams& params, uint32_t sample_rate) noexcept {
    const uint32_t fs = sanitize_sample_rate(sample_rate);
    const float hpf_nyquist_max = std::min(300.0f, static_cast<float>(fs) * 0.45f);
    const float eq_nyquist_max = std::min(20000.0f, static_cast<float>(fs) * 0.48f);
    const float deesser_nyquist_max = std::min(12000.0f, static_cast<float>(fs) * 0.48f);

    params.input_gain_db = clamp_float(params.input_gain_db, -24.0f, 24.0f, 0.0f);

    params.hpf.cutoff_hz = clamp_float(params.hpf.cutoff_hz, 20.0f, hpf_nyquist_max, 80.0f);
    if (params.hpf.slope_db_per_oct != 12 && params.hpf.slope_db_per_oct != 24) {
        params.hpf.slope_db_per_oct = 24;
    }

    const float crossover_max = std::min(8000.0f, static_cast<float>(fs) * 0.4f);
    params.declick.sensitivity_db = clamp_float(params.declick.sensitivity_db, 6.0f, 30.0f, 12.0f);
    params.declick.max_reduction_db =
        clamp_float(params.declick.max_reduction_db, 0.0f, 24.0f, 12.0f);
    params.declick.crossover_hz = clamp_float(
        params.declick.crossover_hz, std::min(1000.0f, crossover_max), crossover_max, 2500.0f);

    params.breath.max_reduction_db = clamp_float(params.breath.max_reduction_db, 0.0f, 24.0f, 9.0f);
    params.breath.level_ceiling_dbfs =
        clamp_float(params.breath.level_ceiling_dbfs, -80.0f, -12.0f, -40.0f);
    params.breath.tilt_threshold_db =
        clamp_float(params.breath.tilt_threshold_db, -12.0f, 12.0f, 0.0f);
    params.breath.min_duration_ms = clamp_float(params.breath.min_duration_ms, 0.0f, 200.0f, 30.0f);
    params.breath.hangover_ms = clamp_float(params.breath.hangover_ms, 0.0f, 500.0f, 80.0f);
    params.breath.attack_ms = clamp_float(params.breath.attack_ms, 1.0f, 200.0f, 25.0f);
    params.breath.release_ms = clamp_float(params.breath.release_ms, 0.5f, 100.0f, 4.0f);

    for (uint32_t i = 0; i < kNumEqBands; ++i) {
        EqBandParams& band = params.eq_bands[i];
        band.index = static_cast<int>(i);
        band.freq_hz = clamp_float(band.freq_hz, 20.0f, eq_nyquist_max, 1000.0f);
        band.gain_db = clamp_float(band.gain_db, -18.0f, 18.0f, 0.0f);
        band.q = clamp_float(band.q, 0.1f, 18.0f, 1.0f);
    }

    const float deesser_min = std::min(3000.0f, deesser_nyquist_max);
    params.deesser.freq_hz =
        clamp_float(params.deesser.freq_hz, deesser_min, deesser_nyquist_max, 6500.0f);
    params.deesser.bandwidth_q = clamp_float(params.deesser.bandwidth_q, 0.5f, 18.0f, 2.5f);
    params.deesser.threshold_dbfs =
        clamp_float(params.deesser.threshold_dbfs, -48.0f, 0.0f, -22.0f);
    params.deesser.max_reduction_db =
        clamp_float(params.deesser.max_reduction_db, 0.0f, 18.0f, 6.0f);
    params.deesser.attack_ms = clamp_float(params.deesser.attack_ms, 0.1f, 50.0f, 1.0f);
    params.deesser.release_ms = clamp_float(params.deesser.release_ms, 5.0f, 500.0f, 40.0f);

    params.compressor.threshold_dbfs =
        clamp_float(params.compressor.threshold_dbfs, -60.0f, 0.0f, -20.0f);
    params.compressor.ratio = clamp_float(params.compressor.ratio, 1.0f, 20.0f, 3.0f);
    params.compressor.knee_db = clamp_float(params.compressor.knee_db, 0.0f, 24.0f, 6.0f);
    params.compressor.attack_ms = clamp_float(params.compressor.attack_ms, 0.1f, 200.0f, 10.0f);
    params.compressor.release_ms =
        clamp_float(params.compressor.release_ms, 10.0f, 1000.0f, 100.0f);
    params.compressor.makeup_gain_db =
        clamp_float(params.compressor.makeup_gain_db, -12.0f, 24.0f, 4.0f);

    params.limiter.ceiling_dbfs = clamp_float(params.limiter.ceiling_dbfs, -12.0f, -0.1f, -1.0f);
    params.limiter.lookahead_ms = clamp_float(params.limiter.lookahead_ms, 0.0f, 5.0f, 2.0f);
    params.limiter.release_ms = clamp_float(params.limiter.release_ms, 5.0f, 500.0f, 50.0f);
}

void clamp_ui_overrides(UiOverrides& overrides) noexcept {
    overrides.mix_percent = clamp_float(overrides.mix_percent, 0.0f, 100.0f, 100.0f);
    overrides.output_makeup_trim_db =
        clamp_float(overrides.output_makeup_trim_db, -12.0f, 12.0f, 0.0f);
    overrides.limiter_ceiling_dbfs =
        clamp_float(overrides.limiter_ceiling_dbfs, -12.0f, -0.1f, -1.0f);
    overrides.declick_amount_percent =
        clamp_float(overrides.declick_amount_percent, 0.0f, 100.0f, 100.0f);
    overrides.breath_amount_percent =
        clamp_float(overrides.breath_amount_percent, 0.0f, 100.0f, 100.0f);
}

std::string truncate_utf8(const std::string& input, size_t max_codepoints) {
    size_t codepoints = 0;
    size_t i = 0;
    const size_t n = input.size();

    while (i < n && codepoints < max_codepoints) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        size_t char_len = 1;
        if ((c & 0x80u) == 0x00u) {
            char_len = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            char_len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            char_len = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            char_len = 4;
        } else {
            char_len = 1;
        }
        if (i + char_len > n) {
            break;
        }
        i += char_len;
        ++codepoints;
    }
    return input.substr(0, i);
}

void clamp_voice_profile(VoiceProfile& profile, uint32_t sample_rate) {
    if (profile.schema_version.empty()) {
        profile.schema_version = kSchemaVersion;
    }
    profile.profile_name = truncate_utf8(profile.profile_name, kMaxProfileNameChars);
    clamp_dsp_params(profile.dsp_params, sample_rate);
    clamp_ui_overrides(profile.ui_overrides);
}

VoiceProfile make_default_neutral_profile() {
    VoiceProfile p;
    p.schema_version = kSchemaVersion;
    p.profile_name = "Neutral Safe";
    p.target_preset = "neutral_safe";

    p.dsp_params.input_gain_db = 0.0f;
    p.dsp_params.hpf.enabled = true;
    p.dsp_params.hpf.cutoff_hz = 80.0f;
    p.dsp_params.hpf.slope_db_per_oct = 24;

    const float default_freqs[kNumEqBands] = {120.0f,  250.0f,  500.0f,  1500.0f,
                                              3200.0f, 5000.0f, 7200.0f, 11000.0f};
    for (uint32_t i = 0; i < kNumEqBands; ++i) {
        p.dsp_params.eq_bands[i].index = static_cast<int>(i);
        p.dsp_params.eq_bands[i].enabled = false;
        p.dsp_params.eq_bands[i].type =
            (i == 0) ? EqBandType::LowShelf
                     : (i == kNumEqBands - 1 ? EqBandType::HighShelf : EqBandType::Peaking);
        p.dsp_params.eq_bands[i].freq_hz = default_freqs[i];
        p.dsp_params.eq_bands[i].gain_db = 0.0f;
        p.dsp_params.eq_bands[i].q = (i == 0 || i == kNumEqBands - 1) ? 0.707f : 1.4f;
    }

    p.dsp_params.deesser.enabled = false;
    p.dsp_params.deesser.freq_hz = 6500.0f;
    p.dsp_params.deesser.bandwidth_q = 2.5f;
    p.dsp_params.deesser.threshold_dbfs = -22.0f;
    p.dsp_params.deesser.max_reduction_db = 6.0f;
    p.dsp_params.deesser.attack_ms = 1.0f;
    p.dsp_params.deesser.release_ms = 40.0f;

    p.dsp_params.compressor.enabled = true;
    p.dsp_params.compressor.threshold_dbfs = -18.0f;
    p.dsp_params.compressor.ratio = 2.0f;
    p.dsp_params.compressor.knee_db = 6.0f;
    p.dsp_params.compressor.attack_ms = 10.0f;
    p.dsp_params.compressor.release_ms = 120.0f;
    p.dsp_params.compressor.makeup_gain_db = 0.0f;

    p.dsp_params.limiter.enabled = true;
    p.dsp_params.limiter.ceiling_dbfs = -1.0f;
    p.dsp_params.limiter.lookahead_ms = 2.0f;
    p.dsp_params.limiter.release_ms = 50.0f;

    p.ui_overrides.mix_percent = 100.0f;
    p.ui_overrides.output_makeup_trim_db = 0.0f;
    p.ui_overrides.limiter_ceiling_dbfs = -1.0f;

    // Voice Cleaner stays off in the neutral profile (no learned thresholds available).
    p.dsp_params.declick = DeclickParams{};
    p.dsp_params.breath = BreathParams{};

    clamp_voice_profile(p, kDefaultSampleRate);
    return p;
}

} // namespace kotone
