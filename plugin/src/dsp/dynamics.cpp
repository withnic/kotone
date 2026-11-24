#include "kotone/dynamics.hpp"

#include <algorithm>
#include <cmath>

namespace kotone {
namespace {

double time_constant_coeff(double time_ms, double sample_rate) noexcept {
    if (!std::isfinite(time_ms) || time_ms <= 0.0 || !std::isfinite(sample_rate) ||
        sample_rate <= 0.0) {
        return 0.0;
    }
    const double tau_samples = sample_rate * (time_ms * 0.001);
    if (tau_samples <= 1e-6) {
        return 0.0;
    }
    return std::exp(-1.0 / tau_samples);
}

} // namespace

// ============================================================================
// DeEsser
// ============================================================================

void DeEsser::reset() noexcept {
    for (auto& f : sc_filters_) {
        f.reset();
    }
    sc_ms_ = 0.0;
    sc_peak_ = 0.0;
    smooth_gr_db_ = 0.0;
    last_gr_db_ = 0.0;
}

void DeEsser::process_frame(double* frame, uint32_t num_channels, const DeEsserParams& params,
                            const BiquadCoeffs& sidechain_coeffs, uint32_t sample_rate) noexcept {

    const uint32_t ch_count = std::min(num_channels, kMaxChannels);
    if (!params.enabled || params.max_reduction_db <= 1e-4f || ch_count == 0) {
        last_gr_db_ = 0.0;
        return;
    }

    const double fs = static_cast<double>(sanitize_sample_rate(sample_rate));

    double sc_samples[kMaxChannels]{};
    double max_sc_sq = 0.0;
    double max_sc_abs = 0.0;

    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        const double sc = sc_filters_[ch].process(frame[ch], sidechain_coeffs);
        sc_samples[ch] = sc;
        const double abs_sc = std::abs(sc);
        if (abs_sc > max_sc_abs) {
            max_sc_abs = abs_sc;
        }
        const double sq = sc * sc;
        if (sq > max_sc_sq) {
            max_sc_sq = sq;
        }
    }

    // AES17-calibrated RMS + fast peak hybrid sidechain detector
    const double a_rms = time_constant_coeff(5.0, fs);
    sc_ms_ = flush_denormal(a_rms * sc_ms_ + (1.0 - a_rms) * max_sc_sq);
    const double rms_level = std::sqrt(2.0 * std::max(0.0, sc_ms_));

    const double a_pk_rel = time_constant_coeff(5.0, fs);
    if (max_sc_abs >= sc_peak_) {
        sc_peak_ = max_sc_abs;
    } else {
        sc_peak_ = flush_denormal(a_pk_rel * sc_peak_ + (1.0 - a_pk_rel) * max_sc_abs);
    }

    const double det_level = std::max(rms_level, sc_peak_);
    const double det_db = linear_to_db(det_level);

    double target_gr_db = 0.0;
    const double thresh = static_cast<double>(params.threshold_dbfs);
    const double max_red = static_cast<double>(params.max_reduction_db);
    if (det_db > thresh) {
        target_gr_db = std::min(det_db - thresh, max_red);
    }

    const double a_att = time_constant_coeff(static_cast<double>(params.attack_ms), fs);
    const double a_rel = time_constant_coeff(static_cast<double>(params.release_ms), fs);
    if (target_gr_db > smooth_gr_db_) {
        smooth_gr_db_ = a_att * smooth_gr_db_ + (1.0 - a_att) * target_gr_db;
    } else {
        smooth_gr_db_ = a_rel * smooth_gr_db_ + (1.0 - a_rel) * target_gr_db;
    }
    smooth_gr_db_ = flush_denormal(std::clamp(smooth_gr_db_, 0.0, max_red));
    last_gr_db_ = smooth_gr_db_;

    if (smooth_gr_db_ > 1e-6) {
        // Split-band attenuation: out = in - (1 - g) * bandpass(in)
        // Because bandpass has exact 0 dB gain & 0 phase shift at center freq,
        // gain at center freq is g and out-of-band gain remains 1.0 (never boosts).
        const double band_gain = db_to_linear(-smooth_gr_db_);
        const double subtract_factor = 1.0 - band_gain;
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            frame[ch] -= subtract_factor * sc_samples[ch];
        }
    }
}

// ============================================================================
// Compressor (Stereo-Linked RMS/Peak Hybrid, Soft-Knee)
// ============================================================================

void Compressor::reset() noexcept {
    rms_ms_ = 0.0;
    peak_env_ = 0.0;
    smooth_gr_db_ = 0.0;
    last_gr_db_ = 0.0;
}

void Compressor::process_frame(double* frame, uint32_t num_channels, const CompressorParams& params,
                               uint32_t sample_rate) noexcept {

    const uint32_t ch_count = std::min(num_channels, kMaxChannels);
    if (!params.enabled || ch_count == 0) {
        last_gr_db_ = 0.0;
        return;
    }

    const double fs = static_cast<double>(sanitize_sample_rate(sample_rate));

    // Stereo-linked detection across all active channels
    double max_sq = 0.0;
    double max_abs = 0.0;
    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        const double s = frame[ch];
        const double abs_s = std::abs(s);
        if (abs_s > max_abs) {
            max_abs = abs_s;
        }
        const double sq = s * s;
        if (sq > max_sq) {
            max_sq = sq;
        }
    }

    // AES17-calibrated RMS (sqrt(2) * RMS == peak for a steady sine wave) + fast peak detector
    const double a_rms = time_constant_coeff(10.0, fs);
    rms_ms_ = flush_denormal(a_rms * rms_ms_ + (1.0 - a_rms) * max_sq);
    const double rms_level = std::sqrt(2.0 * std::max(0.0, rms_ms_));

    const double a_pk_rel = time_constant_coeff(5.0, fs);
    if (max_abs >= peak_env_) {
        peak_env_ = max_abs;
    } else {
        peak_env_ = flush_denormal(a_pk_rel * peak_env_ + (1.0 - a_pk_rel) * max_abs);
    }

    const double det_level = std::max(rms_level, peak_env_);
    const double in_db = linear_to_db(det_level);

    // Soft-knee gain computer (Giannoulis, Massberg & Reiss 2012)
    const double threshold = static_cast<double>(params.threshold_dbfs);
    const double ratio = std::max(1.0, static_cast<double>(params.ratio));
    const double knee = std::max(0.0, static_cast<double>(params.knee_db));
    const double slope = 1.0 - (1.0 / ratio);

    double target_gr_db = 0.0;
    const double over_db = in_db - threshold;
    if (slope > 1e-9) {
        if (2.0 * over_db <= -knee) {
            target_gr_db = 0.0;
        } else if (knee > 1e-6 && 2.0 * std::abs(over_db) < knee) {
            const double d = over_db + knee * 0.5;
            target_gr_db = slope * (d * d) / (2.0 * knee);
        } else {
            target_gr_db = slope * over_db;
        }
    }
    if (target_gr_db < 0.0 || !std::isfinite(target_gr_db)) {
        target_gr_db = 0.0;
    }

    // Smooth ballistics in dB domain
    const double a_att = time_constant_coeff(static_cast<double>(params.attack_ms), fs);
    const double a_rel = time_constant_coeff(static_cast<double>(params.release_ms), fs);
    if (target_gr_db > smooth_gr_db_) {
        smooth_gr_db_ = a_att * smooth_gr_db_ + (1.0 - a_att) * target_gr_db;
    } else {
        smooth_gr_db_ = a_rel * smooth_gr_db_ + (1.0 - a_rel) * target_gr_db;
    }
    smooth_gr_db_ = flush_denormal(std::max(0.0, smooth_gr_db_));
    last_gr_db_ = smooth_gr_db_;

    // Apply common stereo-linked gain reduction to all channels
    const double gain_lin = db_to_linear(-smooth_gr_db_);
    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        frame[ch] *= gain_lin;
    }
}

// ============================================================================
// LookaheadLimiter (Brickwall, Fixed-Size Pre-allocated Ring Buffer)
// ============================================================================

LookaheadLimiter::LookaheadLimiter() noexcept {
    reset();
}

void LookaheadLimiter::reset() noexcept {
    for (auto& ch_buf : delay_ring_) {
        ch_buf.fill(0.0f);
    }
    peak_ring_.fill(0.0);
    deque_idx_.fill(0);
    deque_val_.fill(0.0);
    deque_head_ = 0;
    deque_tail_ = 0;
    deque_count_ = 0;

    ma_ring_.fill(1.0);
    ma_sum_ = 0.0;
    ma_pos_ = 0;
    ma_len_ = 0;

    write_pos_ = 0;
    current_delay_samples_ = 0;
    sample_counter_ = 0;
    release_gain_ = 1.0;
    last_gr_db_ = 0.0;
}

void LookaheadLimiter::rebuild_lookahead_window(uint32_t new_delay_samples) noexcept {
    current_delay_samples_ = std::min(new_delay_samples, kMaxLookaheadSamples);

    // Reset monotonic deque and repopulate from recent peak_ring_ entries
    deque_head_ = 0;
    deque_tail_ = 0;
    deque_count_ = 0;

    const uint32_t available = static_cast<uint32_t>(
        std::min<uint64_t>(sample_counter_, static_cast<uint64_t>(current_delay_samples_)));
    for (uint32_t offset = available; offset > 0; --offset) {
        const uint32_t pos = (write_pos_ - offset) & kRingMask;
        const uint64_t idx = sample_counter_ - offset;
        const double val = peak_ring_[pos];
        while (deque_count_ > 0) {
            const uint32_t back = (deque_tail_ - 1) & kRingMask;
            if (deque_val_[back] <= val) {
                deque_tail_ = back;
                --deque_count_;
            } else {
                break;
            }
        }
        deque_idx_[deque_tail_] = idx;
        deque_val_[deque_tail_] = val;
        deque_tail_ = (deque_tail_ + 1) & kRingMask;
        ++deque_count_;
    }

    // Reinitialize moving average smoother to current release_gain_
    ma_len_ = current_delay_samples_;
    ma_pos_ = 0;
    ma_sum_ = 0.0;
    const double init_g = std::clamp(release_gain_, 0.0, 1.0);
    for (uint32_t i = 0; i < ma_len_; ++i) {
        ma_ring_[i] = init_g;
        ma_sum_ += init_g;
    }
}

double LookaheadLimiter::push_window_peak(double peak_val, uint32_t window_len) noexcept {
    const uint64_t cur_idx = sample_counter_;

    while (deque_count_ > 0) {
        const uint32_t back = (deque_tail_ - 1) & kRingMask;
        if (deque_val_[back] <= peak_val) {
            deque_tail_ = back;
            --deque_count_;
        } else {
            break;
        }
    }

    deque_idx_[deque_tail_] = cur_idx;
    deque_val_[deque_tail_] = peak_val;
    deque_tail_ = (deque_tail_ + 1) & kRingMask;
    ++deque_count_;

    // Evict entries older than cur_idx - window_len (keeping window of length window_len + 1)
    while (deque_count_ > 0 &&
           deque_idx_[deque_head_] + static_cast<uint64_t>(window_len) < cur_idx) {
        deque_head_ = (deque_head_ + 1) & kRingMask;
        --deque_count_;
    }

    return (deque_count_ > 0) ? deque_val_[deque_head_] : peak_val;
}

bool LookaheadLimiter::process_frame(double* frame, uint32_t num_channels,
                                     const LimiterParams& params, uint32_t sample_rate) noexcept {

    const uint32_t ch_count = std::min(num_channels, kMaxChannels);
    if (ch_count == 0) {
        return false;
    }

    bool non_finite_detected = false;
    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        if (!std::isfinite(frame[ch])) {
            frame[ch] = 0.0;
            non_finite_detected = true;
        }
    }

    if (!params.enabled) {
        last_gr_db_ = 0.0;
        return non_finite_detected;
    }

    const double fs = static_cast<double>(sanitize_sample_rate(sample_rate));
    const double lookahead_ms = std::clamp(static_cast<double>(params.lookahead_ms), 0.0, 5.0);
    const uint32_t target_delay = std::min(
        static_cast<uint32_t>(std::llround(fs * lookahead_ms * 0.001)), kMaxLookaheadSamples);

    if (target_delay != current_delay_samples_) {
        rebuild_lookahead_window(target_delay);
    }

    // Write incoming frame to delay ring and compute incoming peak across all channels
    double in_peak = 0.0;
    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        const double s = frame[ch];
        delay_ring_[ch][write_pos_] = static_cast<float>(s);
        const double abs_s = std::abs(s);
        if (abs_s > in_peak) {
            in_peak = abs_s;
        }
    }
    peak_ring_[write_pos_] = in_peak;

    // Sliding window max over [t - L, t] (L + 1 samples)
    const double window_max = push_window_peak(in_peak, current_delay_samples_);

    // Ceiling in linear amplitude (clamped strictly to <= -0.1 dBFS)
    const double ceiling_db = std::clamp(static_cast<double>(params.ceiling_dbfs), -12.0, -0.1);
    const double ceiling_lin = db_to_linear(ceiling_db);

    const double needed_gain =
        (window_max > ceiling_lin && window_max > 1e-12) ? (ceiling_lin / window_max) : 1.0;

    // Instant drop + smooth release envelope on lookahead-held required gain
    const double a_rel = time_constant_coeff(static_cast<double>(params.release_ms), fs);
    if (needed_gain < release_gain_) {
        release_gain_ = needed_gain;
    } else {
        release_gain_ = a_rel * release_gain_ + (1.0 - a_rel) * needed_gain;
    }
    release_gain_ = std::clamp(release_gain_, 0.0, 1.0);

    // Moving average over L samples creates a smooth linear ramp that reaches needed_gain
    // at the exact sample the peak exits the delay line.
    double smooth_gain = release_gain_;
    if (ma_len_ > 0) {
        ma_sum_ -= ma_ring_[ma_pos_];
        ma_ring_[ma_pos_] = release_gain_;
        ma_sum_ += release_gain_;
        ma_pos_ = (ma_pos_ + 1) % ma_len_;
        if (ma_pos_ == 0) {
            // Periodic exact resync to prevent floating-point drift over long streams
            double exact_sum = 0.0;
            for (uint32_t i = 0; i < ma_len_; ++i) {
                exact_sum += ma_ring_[i];
            }
            ma_sum_ = exact_sum;
        }
        smooth_gain = std::clamp(ma_sum_ / static_cast<double>(ma_len_), 0.0, 1.0);
    }

    // Read delayed audio sample
    const uint32_t read_pos = (write_pos_ - current_delay_samples_) & kRingMask;
    double delayed_peak = 0.0;
    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        const double ds = static_cast<double>(delay_ring_[ch][read_pos]);
        const double abs_ds = std::abs(ds);
        if (abs_ds > delayed_peak) {
            delayed_peak = abs_ds;
        }
    }

    // Safety cap on gain so delayed sample cannot exceed ceiling even during dynamic lookahead
    // changes
    double final_gain = smooth_gain;
    if (delayed_peak > ceiling_lin && delayed_peak > 1e-12) {
        const double exact_cap = ceiling_lin / delayed_peak;
        if (exact_cap < final_gain) {
            final_gain = exact_cap;
        }
    }

    last_gr_db_ = (final_gain < 1.0) ? -linear_to_db(final_gain) : 0.0;

    // Apply gain + final brickwall hard clamp & NaN/Inf safety guard
    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        double out = static_cast<double>(delay_ring_[ch][read_pos]) * final_gain;
        if (!std::isfinite(out)) {
            out = 0.0;
            non_finite_detected = true;
        } else {
            out = std::clamp(out, -ceiling_lin, ceiling_lin);
        }
        frame[ch] = out;
    }

    write_pos_ = (write_pos_ + 1) & kRingMask;
    ++sample_counter_;
    return non_finite_detected;
}

} // namespace kotone
