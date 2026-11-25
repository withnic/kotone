#include "kotone/voice_cleaner.hpp"

#include <algorithm>
#include <cmath>

namespace kotone {
namespace {

uint32_t ms_to_samples(double ms, double fs, uint32_t max_samples) noexcept {
    const double v = std::round(ms * 0.001 * fs);
    if (!std::isfinite(v) || v < 1.0) {
        return 1;
    }
    return std::min(static_cast<uint32_t>(v), max_samples);
}

double tc_coeff(double ms, double fs) noexcept {
    if (!std::isfinite(ms) || ms <= 0.0) {
        return 0.0;
    }
    return std::exp(-1.0 / (fs * ms * 0.001));
}

double clamp_scale(double s) noexcept {
    if (!std::isfinite(s)) {
        return 1.0;
    }
    return std::clamp(s, 0.0, 1.0);
}

double non_negative(double v) noexcept {
    return (std::isfinite(v) && v > 0.0) ? v : 0.0;
}

} // namespace

VoiceCleanerConfig make_voice_cleaner_config(const DeclickParams& declick,
                                             const BreathParams& breath, double input_gain_db,
                                             double declick_scale, double breath_scale,
                                             uint32_t sample_rate) noexcept {
    const double fs = static_cast<double>(sanitize_sample_rate(sample_rate));
    const double gain_db = std::isfinite(input_gain_db) ? input_gain_db : 0.0;
    VoiceCleanerConfig c;

    c.lookahead_samples = ms_to_samples(kVoiceCleanerLookaheadMs, fs, VoiceCleaner::kShortRing - 1);

    // ---- Lip-noise click detector ----
    c.click_fast_samples = ms_to_samples(kClickFastMs, fs, 256);
    c.click_slow_samples = ms_to_samples(kClickSlowMs, fs, 4096);
    c.click_decay_samples = ms_to_samples(kClickDecayMs, fs, 256);
    // Detection happens at c = n - decay; the click reaches the output (delayed by lookahead)
    // lookahead - decay samples later. Keep the gain down for kClickHoldMs after that.
    const uint32_t hold = ms_to_samples(kClickHoldMs, fs, 1024);
    c.click_hold_samples = hold + (c.lookahead_samples > c.click_decay_samples
                                       ? c.lookahead_samples - c.click_decay_samples
                                       : 0);
    c.click_sensitivity_lin = std::pow(10.0, static_cast<double>(declick.sensitivity_db) / 10.0);
    const double broad_db = kClickBroadbandDbAt48k - 40.0 * std::log10(fs / 48000.0);
    c.click_broadband_lin = std::pow(10.0, broad_db / 10.0);
    c.click_decay_lin = std::pow(10.0, -kClickDecayDb / 10.0);
    const double click_red_db =
        static_cast<double>(declick.max_reduction_db) * clamp_scale(declick_scale);
    c.click_floor_gain = db_to_linear(-click_red_db);
    c.declick_enabled = declick.enabled && click_red_db > 1e-3;
    c.click_attack_coeff = tc_coeff(0.05, fs);
    c.click_release_coeff = tc_coeff(1.0, fs);
    c.crossover_hpf =
        BiquadCoeffs::highpass(fs, static_cast<double>(declick.crossover_hz), 0.7071067811865476);

    // ---- Breath suppressor ----
    c.breath_window_samples = ms_to_samples(kBreathWindowMs, fs, VoiceCleaner::kBreathRing - 1);
    c.breath_min_samples = ms_to_samples(static_cast<double>(breath.min_duration_ms), fs, 1048576u);
    c.breath_hangover_samples =
        ms_to_samples(static_cast<double>(breath.hangover_ms), fs, 1048576u);
    const double ceiling_db = static_cast<double>(breath.level_ceiling_dbfs) + gain_db;
    c.breath_ceiling_pow = std::pow(10.0, std::clamp(ceiling_db, -140.0, 20.0) / 10.0);
    // Below (ceiling - 30 dB) nothing is considered voice for the hangover logic.
    c.breath_voice_floor_pow = c.breath_ceiling_pow * 1e-3;
    c.breath_tilt_lin = std::pow(
        10.0, std::clamp(static_cast<double>(breath.tilt_threshold_db), -40.0, 40.0) / 10.0);
    const double breath_red_db =
        static_cast<double>(breath.max_reduction_db) * clamp_scale(breath_scale);
    c.breath_floor_gain = db_to_linear(-breath_red_db);
    c.breath_enabled = breath.enabled && breath_red_db > 1e-3;
    c.breath_attack_coeff = tc_coeff(static_cast<double>(breath.attack_ms), fs);
    c.breath_release_coeff = tc_coeff(static_cast<double>(breath.release_ms), fs);
    c.breath_low_bp = BiquadCoeffs::bandpass_constant_peak(fs, kBreathLowHz, kBreathBandQ);
    c.breath_high_bp =
        BiquadCoeffs::bandpass_constant_peak(fs, std::min(kBreathHighHz, fs * 0.3), kBreathBandQ);
    return c;
}

VoiceCleaner::VoiceCleaner() noexcept {
    reset();
}

void VoiceCleaner::reset() noexcept {
    for (auto& ch : delay_) {
        ch.fill(0.0f);
    }
    delay_pos_ = 0;
    for (auto& f : crossover_) {
        f.reset();
    }
    e_ring_.fill(0.0f);
    x2_ring_.fill(0.0f);
    fe_hist_.fill(0.0);
    fx_hist_.fill(0.0);
    n_ = 0;
    m1_ = 0.0;
    m2_ = 0.0;
    fast_e_sum_ = 0.0;
    fast_x_sum_ = 0.0;
    slow_e_sum_ = 0.0;
    click_hold_ = 0;
    last_hit_n_ = 0;
    has_hit_ = false;
    click_events_ = 0;
    click_gain_ = 1.0;

    breath_low_.reset();
    breath_high_.reset();
    bx_ring_.fill(0.0f);
    bl_ring_.fill(0.0f);
    bh_ring_.fill(0.0f);
    bx_sum_ = 0.0;
    bl_sum_ = 0.0;
    bh_sum_ = 0.0;
    breath_hang_ = 0;
    breath_persist_ = 0;
    breath_active_ = false;
    breath_gain_ = 1.0;
}

void VoiceCleaner::detect_click(double mono, const VoiceCleanerConfig& cfg) noexcept {
    const uint32_t lf = cfg.click_fast_samples;
    const uint32_t ls = cfg.click_slow_samples;
    const uint32_t ld = cfg.click_decay_samples;

    const double d = mono - 2.0 * m1_ + m2_;
    m2_ = m1_;
    m1_ = mono;
    const double e = (n_ >= 2) ? d * d : 0.0;
    const double x2 = mono * mono;

    const uint32_t pos = static_cast<uint32_t>(n_ % kClickRing);
    const uint32_t spos = static_cast<uint32_t>(n_ % kShortRing);
    e_ring_[pos] = static_cast<float>(e);
    x2_ring_[spos] = static_cast<float>(x2);

    // Fast windows ending at n: [n - lf + 1, n]
    fast_e_sum_ += e;
    fast_x_sum_ += x2;
    if (n_ >= lf) {
        fast_e_sum_ -= static_cast<double>(e_ring_[(n_ - lf) % kClickRing]);
        fast_x_sum_ -= static_cast<double>(x2_ring_[(n_ - lf) % kShortRing]);
    }
    fast_e_sum_ = non_negative(fast_e_sum_);
    fast_x_sum_ = non_negative(fast_x_sum_);
    fe_hist_[spos] = fast_e_sum_;
    fx_hist_[spos] = fast_x_sum_;

    // Slow window ends right before the fast window of the centre c = n - ld:
    // [c - lf - ls + 1, c - lf]
    const uint64_t lag = static_cast<uint64_t>(ld) + lf;
    if (n_ >= lag) {
        slow_e_sum_ += static_cast<double>(e_ring_[(n_ - lag) % kClickRing]);
        if (n_ >= lag + ls) {
            slow_e_sum_ -= static_cast<double>(e_ring_[(n_ - lag - ls) % kClickRing]);
        }
        slow_e_sum_ = non_negative(slow_e_sum_);
    }

    bool hit = false;
    if (n_ >= lag + ls) {
        const double fe_c = fe_hist_[(n_ - ld) % kShortRing];
        const double fx_c = fx_hist_[(n_ - ld) % kShortRing];
        const double fast_c = fe_c / static_cast<double>(lf);
        const double slow_c = slow_e_sum_ / static_cast<double>(ls);
        hit = fast_c > 1e-14 && fast_c > slow_c * cfg.click_sensitivity_lin &&
              fe_c > fx_c * cfg.click_broadband_lin && fast_e_sum_ < fe_c * cfg.click_decay_lin;
    }

    if (hit) {
        const uint64_t c = n_ - ld;
        if (!has_hit_ || c - last_hit_n_ > cfg.click_hold_samples) {
            ++click_events_;
        }
        has_hit_ = true;
        last_hit_n_ = c;
        click_hold_ = cfg.click_hold_samples;
    } else if (click_hold_ > 0) {
        --click_hold_;
    }
}

void VoiceCleaner::detect_breath(double mono, const VoiceCleanerConfig& cfg) noexcept {
    const uint32_t w = cfg.breath_window_samples;
    const double lo = breath_low_.process(mono, cfg.breath_low_bp);
    const double hi = breath_high_.process(mono, cfg.breath_high_bp);

    const uint32_t pos = static_cast<uint32_t>(n_ % kBreathRing);
    const double x2 = mono * mono;
    const double l2 = lo * lo;
    const double h2 = hi * hi;
    if (n_ >= w) {
        const uint32_t old = static_cast<uint32_t>((n_ - w) % kBreathRing);
        bx_sum_ -= static_cast<double>(bx_ring_[old]);
        bl_sum_ -= static_cast<double>(bl_ring_[old]);
        bh_sum_ -= static_cast<double>(bh_ring_[old]);
    }
    bx_ring_[pos] = static_cast<float>(x2);
    bl_ring_[pos] = static_cast<float>(l2);
    bh_ring_[pos] = static_cast<float>(h2);
    bx_sum_ = non_negative(bx_sum_ + static_cast<double>(bx_ring_[pos]));
    bl_sum_ = non_negative(bl_sum_ + static_cast<double>(bl_ring_[pos]));
    bh_sum_ = non_negative(bh_sum_ + static_cast<double>(bh_ring_[pos]));

    const double inv_w = 1.0 / static_cast<double>(w);
    const double level = bx_sum_ * inv_w;
    const bool breathy_tilt = bh_sum_ > bl_sum_ * cfg.breath_tilt_lin;
    const bool breathy = breathy_tilt && level < cfg.breath_ceiling_pow;
    const bool voice_like = !breathy && level >= cfg.breath_voice_floor_pow &&
                            (!breathy_tilt || level >= cfg.breath_ceiling_pow);

    if (voice_like) {
        breath_hang_ = cfg.breath_hangover_samples;
    } else if (breath_hang_ > 0) {
        --breath_hang_;
    }

    if (breathy && breath_hang_ == 0) {
        if (breath_persist_ < cfg.breath_min_samples) {
            ++breath_persist_;
        }
    } else {
        breath_persist_ = 0;
    }
    breath_active_ = breathy && breath_hang_ == 0 && breath_persist_ >= cfg.breath_min_samples;
}

void VoiceCleaner::process_frame(double* frame, uint32_t num_channels,
                                 const VoiceCleanerConfig& cfg) noexcept {
    const uint32_t ch_count = std::min(num_channels, kMaxChannels);
    if (ch_count == 0) {
        return;
    }

    double mono = 0.0;
    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        mono += frame[ch];
    }
    mono /= static_cast<double>(ch_count);
    if (!std::isfinite(mono)) {
        mono = 0.0;
    }

    if (cfg.declick_enabled) {
        detect_click(mono, cfg);
    } else {
        click_hold_ = 0;
    }
    if (cfg.breath_enabled) {
        detect_breath(mono, cfg);
    } else {
        breath_active_ = false;
    }
    ++n_;

    // Gain envelopes (computed on the undelayed signal, applied to the delayed one)
    const double click_target =
        (cfg.declick_enabled && click_hold_ > 0) ? cfg.click_floor_gain : 1.0;
    const double ca = click_target < click_gain_ ? cfg.click_attack_coeff : cfg.click_release_coeff;
    click_gain_ = ca * click_gain_ + (1.0 - ca) * click_target;

    const double breath_target = breath_active_ ? cfg.breath_floor_gain : 1.0;
    const double ba =
        breath_target < breath_gain_ ? cfg.breath_attack_coeff : cfg.breath_release_coeff;
    breath_gain_ = ba * breath_gain_ + (1.0 - ba) * breath_target;

    // Fixed look-ahead delay line
    const uint32_t d = cfg.lookahead_samples;
    const uint32_t wpos = delay_pos_;
    const uint32_t rpos = (delay_pos_ + kShortRing - d) % kShortRing;
    delay_pos_ = (delay_pos_ + 1) % kShortRing;

    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        delay_[ch][wpos] = static_cast<float>(frame[ch]);
        const double delayed = static_cast<double>(delay_[ch][rpos]);
        // Complementary split: low = x - high, so unity click gain reconstructs x exactly.
        const double high = crossover_[ch].process(delayed, cfg.crossover_hpf);
        const double repaired = delayed - (1.0 - click_gain_) * high;
        frame[ch] = repaired * breath_gain_;
    }
}

} // namespace kotone
