#pragma once

#include "kotone/biquad.hpp"
#include "kotone/dsp_types.hpp"
#include "kotone/dynamics.hpp"
#include "kotone/voice_cleaner.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace kotone {

// Pure fixed-size snapshot (zero heap allocations) swapped lock-freely between UI and audio
// threads.
struct DspConfigSnapshot {
    DspParams base_params{};
    DspParams effective_params{};
    UiOverrides ui_overrides{};

    std::array<BiquadCoeffs, 2> hpf_coeffs{};
    std::array<BiquadCoeffs, kNumEqBands> eq_coeffs{};
    BiquadCoeffs deesser_sc_coeffs{};
    VoiceCleanerConfig cleaner{};

    uint32_t sample_rate = kDefaultSampleRate;
    float mix_ratio = 1.0f;
    float output_makeup_trim_db = 0.0f;
    double gain_smooth_coeff = 0.99791883; // ~10ms @ 48kHz
    bool bypass = false;
    uint64_t revision = 0;
};

// Builds a clamped, mix-interpolated DspConfigSnapshot with precomputed Biquad coefficients.
DspConfigSnapshot build_dsp_snapshot(const VoiceProfile& profile, const UiOverrides& overrides,
                                     uint32_t sample_rate, bool bypass = false,
                                     uint64_t revision = 0) noexcept;

DspConfigSnapshot build_dsp_snapshot_from_params(const DspParams& base_params,
                                                 const UiOverrides& overrides, uint32_t sample_rate,
                                                 bool bypass = false,
                                                 uint64_t revision = 0) noexcept;

// Multi-channel (up to 8ch) real-time safe DSP engine with 16-slot lock-free snapshot ring.
// Audio thread (process) performs zero heap allocations, zero I/O, and zero mutex locks.
class DspEngine {
public:
    explicit DspEngine(uint32_t sample_rate = kDefaultSampleRate) noexcept;

    // Non-realtime setup: updates sample rate, rebuilds active snapshot coefficients, and resets
    // states.
    void prepare(uint32_t sample_rate) noexcept;

    // Resets filter states, dynamics envelopes, lookahead buffers, and gain smoothers.
    void reset() noexcept;

    // Lock-free configuration updates (callable from UI/control thread while process() runs).
    bool apply_profile(const VoiceProfile& profile, bool bypass = false) noexcept;
    bool apply_profile_with_overrides(const VoiceProfile& profile, const UiOverrides& overrides,
                                      bool bypass = false) noexcept;
    bool apply_snapshot(const DspConfigSnapshot& snapshot) noexcept;
    bool set_bypass(bool bypass) noexcept;

    void set_state(EngineState state) noexcept { state_.store(state, std::memory_order_release); }
    EngineState state() const noexcept { return state_.load(std::memory_order_acquire); }
    uint32_t sample_rate() const noexcept { return sample_rate_.load(std::memory_order_acquire); }

    // Real-time audio processing callback (strictly zero-alloc, zero-lock, noexcept).
    void process(float* const* channels, uint32_t num_channels, uint32_t num_frames) noexcept;

    // Diagnostics / metering (atomic reads)
    double last_compressor_gr_db() const noexcept {
        return last_comp_gr_db_.load(std::memory_order_relaxed);
    }
    double last_limiter_gr_db() const noexcept {
        return last_lim_gr_db_.load(std::memory_order_relaxed);
    }
    uint64_t click_events() const noexcept { return click_events_.load(std::memory_order_relaxed); }
    uint64_t non_finite_count() const noexcept {
        return non_finite_count_.load(std::memory_order_relaxed);
    }

private:
    struct SnapshotSlot {
        DspConfigSnapshot config{};
        std::atomic<uint32_t> readers{0};
        std::atomic<bool> claimed{false};
    };

    void reset_audio_states() noexcept;
    bool publish_snapshot(const DspConfigSnapshot& snapshot) noexcept;

    std::array<SnapshotSlot, kSnapshotSlotCount> slots_{};
    std::atomic<uint32_t> active_slot_{0};
    std::atomic<uint32_t> write_cursor_{1};
    std::atomic<uint64_t> revision_counter_{1};
    std::atomic<uint32_t> sample_rate_{kDefaultSampleRate};
    std::atomic<EngineState> state_{EngineState::Uninitialized};

    // Audio-thread-exclusive pre-allocated DSP state (no dynamic allocation)
    std::array<BiquadState, kMaxChannels> hpf_stage1_{};
    std::array<BiquadState, kMaxChannels> hpf_stage2_{};
    std::array<std::array<BiquadState, kNumEqBands>, kMaxChannels> eq_states_{};
    VoiceCleaner cleaner_{};
    DeEsser deesser_{};
    Compressor compressor_{};
    LookaheadLimiter limiter_{};

    double current_input_gain_ = 1.0;
    double current_makeup_gain_ = 1.0;
    bool smoothers_initialized_ = false;
    bool was_bypassed_ = false;

    std::atomic<double> last_comp_gr_db_{0.0};
    std::atomic<double> last_lim_gr_db_{0.0};
    std::atomic<uint64_t> click_events_{0};
    std::atomic<uint64_t> non_finite_count_{0};
};

} // namespace kotone
