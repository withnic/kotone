#pragma once

#include "kotone/biquad.hpp"
#include "kotone/dsp_types.hpp"

#include <array>
#include <cstdint>

namespace kotone {

// Split-band De-Esser using 0 dB constant-peak bandpass sidechain detection.
// Attenuates only the sibilance band (4kHz-10kHz) without coloring lower voice frequencies.
class DeEsser {
public:
    void reset() noexcept;

    void process_frame(double* frame, uint32_t num_channels, const DeEsserParams& params,
                       const BiquadCoeffs& sidechain_coeffs, uint32_t sample_rate) noexcept;

    double last_gain_reduction_db() const noexcept { return last_gr_db_; }

private:
    std::array<BiquadState, kMaxChannels> sc_filters_{};
    double sc_ms_ = 0.0;
    double sc_peak_ = 0.0;
    double smooth_gr_db_ = 0.0;
    double last_gr_db_ = 0.0;
};

// Stereo-linked RMS/Peak hybrid feed-forward compressor with soft-knee transfer function.
class Compressor {
public:
    void reset() noexcept;

    void process_frame(double* frame, uint32_t num_channels, const CompressorParams& params,
                       uint32_t sample_rate) noexcept;

    double last_gain_reduction_db() const noexcept { return last_gr_db_; }

private:
    double rms_ms_ = 0.0;
    double peak_env_ = 0.0;
    double smooth_gr_db_ = 0.0;
    double last_gr_db_ = 0.0;
};

// Pre-allocated fixed-size lookahead brickwall limiter with O(1) sliding-window peak detection,
// smooth lookahead ramp, guaranteed hard ceiling clamp, and NaN/Inf safety guard.
class LookaheadLimiter {
public:
    static constexpr uint32_t kRingSize = 1024; // Power of 2, > kMaxLookaheadSamples (960)
    static constexpr uint32_t kRingMask = kRingSize - 1;

    LookaheadLimiter() noexcept;
    void reset() noexcept;

    // Returns true if any non-finite sample was detected and muted.
    bool process_frame(double* frame, uint32_t num_channels, const LimiterParams& params,
                       uint32_t sample_rate) noexcept;

    double last_gain_reduction_db() const noexcept { return last_gr_db_; }

private:
    void rebuild_lookahead_window(uint32_t new_delay_samples) noexcept;
    double push_window_peak(double peak_val, uint32_t window_len) noexcept;

    std::array<std::array<float, kRingSize>, kMaxChannels> delay_ring_{};
    std::array<double, kRingSize> peak_ring_{};

    // Fixed-size monotonic deque for O(1) sliding window maximum
    std::array<uint64_t, kRingSize> deque_idx_{};
    std::array<double, kRingSize> deque_val_{};
    uint32_t deque_head_ = 0;
    uint32_t deque_tail_ = 0;
    uint32_t deque_count_ = 0;

    // Fixed-size moving average buffer for click-free lookahead attack smoothing
    std::array<double, kRingSize> ma_ring_{};
    double ma_sum_ = 0.0;
    uint32_t ma_pos_ = 0;
    uint32_t ma_len_ = 0;

    uint32_t write_pos_ = 0;
    uint32_t current_delay_samples_ = 0;
    uint64_t sample_counter_ = 0;
    double release_gain_ = 1.0;
    double last_gr_db_ = 0.0;
};

} // namespace kotone
