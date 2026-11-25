#pragma once

#include "kotone/biquad.hpp"
#include "kotone/dsp_types.hpp"

#include <array>
#include <cstdint>

namespace kotone {

// Shared detector constants. These MUST match profiler/kotone_profiler/cleaner.py so that the
// thresholds learned offline behave identically in real time (docs/02_data_modeling.md 4.1).
constexpr double kClickFastMs = 0.5;
constexpr double kClickSlowMs = 20.0;
constexpr double kClickDecayMs = 1.0;
constexpr double kClickDecayDb = 6.0;
constexpr double kClickHoldMs = 2.0;
constexpr double kClickBroadbandDbAt48k = -10.0;
constexpr double kBreathWindowMs = 20.0;
constexpr double kBreathLowHz = 400.0;
constexpr double kBreathHighHz = 4000.0;
constexpr double kBreathBandQ = 0.6;

// Fixed look-ahead of the Voice Cleaner stage (added latency, independent of enable state so
// that profile switches never shift the audio timeline).
constexpr double kVoiceCleanerLookaheadMs = 2.0;

// Precomputed (UI-thread) configuration for VoiceCleaner; trivially copyable, no heap.
struct VoiceCleanerConfig {
    bool declick_enabled = false;
    bool breath_enabled = false;

    // Common timing (samples)
    uint32_t lookahead_samples = 96;

    // Lip-noise click detector / repair
    uint32_t click_fast_samples = 24;
    uint32_t click_slow_samples = 960;
    uint32_t click_decay_samples = 48;
    uint32_t click_hold_samples = 144; // output-side hold after detection
    double click_sensitivity_lin = 15.85;
    double click_broadband_lin = 0.1;
    double click_decay_lin = 0.25;
    double click_floor_gain = 1.0; // linear gain applied to the high band during a click
    double click_attack_coeff = 0.0;
    double click_release_coeff = 0.0;
    BiquadCoeffs crossover_hpf{};

    // Breath detector / suppressor
    uint32_t breath_window_samples = 960;
    uint32_t breath_min_samples = 1440;
    uint32_t breath_hangover_samples = 3840;
    double breath_ceiling_pow = 1e-4; // mean-square level below which breath may be reduced
    double breath_voice_floor_pow = 1e-6;
    double breath_tilt_lin = 1.0;   // high/low band power ratio above which audio is breathy
    double breath_floor_gain = 1.0; // linear gain while suppressing
    double breath_attack_coeff = 0.0;
    double breath_release_coeff = 0.0;
    BiquadCoeffs breath_low_bp{};
    BiquadCoeffs breath_high_bp{};
};

// Builds a VoiceCleanerConfig. reduction_scale_* in [0, 1] scales max_reduction_db (mix and
// UI amount sliders). input_gain_db shifts the learned absolute level thresholds because the
// detector runs after Input Trim.
VoiceCleanerConfig make_voice_cleaner_config(const DeclickParams& declick,
                                             const BreathParams& breath, double input_gain_db,
                                             double declick_scale, double breath_scale,
                                             uint32_t sample_rate) noexcept;

// Lip-noise (click) remover + breath suppressor with a fixed 2 ms look-ahead.
// All buffers are fixed-size members: process_frame() performs no allocation, I/O or locking.
class VoiceCleaner {
public:
    static constexpr uint32_t kClickRing = 8192;  // >= fast + slow + decay at 192 kHz
    static constexpr uint32_t kShortRing = 512;   // >= max(decay, lookahead) at 192 kHz
    static constexpr uint32_t kBreathRing = 4096; // >= 20 ms at 192 kHz

    VoiceCleaner() noexcept;
    void reset() noexcept;

    // Processes one multi-channel frame in place (output is delayed by lookahead_samples).
    void process_frame(double* frame, uint32_t num_channels,
                       const VoiceCleanerConfig& cfg) noexcept;

    double last_click_gain() const noexcept { return click_gain_; }
    double last_breath_gain() const noexcept { return breath_gain_; }
    uint64_t click_events() const noexcept { return click_events_; }

private:
    void detect_click(double mono, const VoiceCleanerConfig& cfg) noexcept;
    void detect_breath(double mono, const VoiceCleanerConfig& cfg) noexcept;

    // Delay line (per channel)
    std::array<std::array<float, kShortRing>, kMaxChannels> delay_{};
    uint32_t delay_pos_ = 0;
    std::array<BiquadState, kMaxChannels> crossover_{};

    // Click detector state
    std::array<float, kClickRing> e_ring_{};   // (2nd difference)^2
    std::array<float, kShortRing> x2_ring_{};  // plain x^2
    std::array<double, kShortRing> fe_hist_{}; // fast 2nd-diff window sum history
    std::array<double, kShortRing> fx_hist_{}; // fast plain window sum history
    uint64_t n_ = 0;
    double m1_ = 0.0;
    double m2_ = 0.0;
    double fast_e_sum_ = 0.0;
    double fast_x_sum_ = 0.0;
    double slow_e_sum_ = 0.0;
    uint32_t click_hold_ = 0;
    uint64_t last_hit_n_ = 0;
    bool has_hit_ = false;
    uint64_t click_events_ = 0;
    double click_gain_ = 1.0;

    // Breath detector state
    BiquadState breath_low_{};
    BiquadState breath_high_{};
    std::array<float, kBreathRing> bx_ring_{};
    std::array<float, kBreathRing> bl_ring_{};
    std::array<float, kBreathRing> bh_ring_{};
    double bx_sum_ = 0.0;
    double bl_sum_ = 0.0;
    double bh_sum_ = 0.0;
    uint32_t breath_hang_ = 0;
    uint32_t breath_persist_ = 0;
    bool breath_active_ = false;
    double breath_gain_ = 1.0;
};

} // namespace kotone
