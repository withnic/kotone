#include "kotone/dsp_engine.hpp"

#include <algorithm>
#include <cmath>

namespace kotone {
namespace {

double compute_gain_smooth_coeff(uint32_t sample_rate) noexcept {
    // 10 ms 1st-order IIR smoothing time constant for click-free profile switching
    const double fs = static_cast<double>(sanitize_sample_rate(sample_rate));
    return std::exp(-1.0 / (fs * 0.010));
}

} // namespace

DspConfigSnapshot build_dsp_snapshot_from_params(const DspParams& base_params,
                                                 const UiOverrides& overrides, uint32_t sample_rate,
                                                 bool bypass, uint64_t revision) noexcept {

    const uint32_t fs = sanitize_sample_rate(sample_rate);

    DspConfigSnapshot snap;
    snap.base_params = base_params;
    clamp_dsp_params(snap.base_params, fs);

    snap.ui_overrides = overrides;
    clamp_ui_overrides(snap.ui_overrides);

    snap.sample_rate = fs;
    snap.mix_ratio = snap.ui_overrides.mix_percent / 100.0f;
    snap.output_makeup_trim_db = snap.ui_overrides.output_makeup_trim_db;
    snap.gain_smooth_coeff = compute_gain_smooth_coeff(fs);
    snap.bypass = bypass;
    snap.revision = revision;

    // Apply mix interpolation per docs/02_data_modeling.md Section 3.3:
    // - EQ gain_db *= mix
    // - Compressor ratio = 1.0 + (ratio - 1.0) * mix
    // - Compressor makeup_gain_db *= mix
    // - De-Esser max_reduction_db *= mix
    // - Input gain *= mix
    // - HPF and Limiter remain 100% active regardless of mix for clipping/rumble protection.
    snap.effective_params = snap.base_params;
    const float mix = snap.mix_ratio;

    snap.effective_params.input_gain_db = snap.base_params.input_gain_db * mix;

    for (uint32_t i = 0; i < kNumEqBands; ++i) {
        snap.effective_params.eq_bands[i].gain_db = snap.base_params.eq_bands[i].gain_db * mix;
    }

    snap.effective_params.deesser.max_reduction_db =
        snap.base_params.deesser.max_reduction_db * mix;

    snap.effective_params.compressor.ratio =
        1.0f + (snap.base_params.compressor.ratio - 1.0f) * mix;
    snap.effective_params.compressor.makeup_gain_db =
        snap.base_params.compressor.makeup_gain_db * mix;

    snap.effective_params.limiter.ceiling_dbfs = snap.ui_overrides.limiter_ceiling_dbfs;

    clamp_dsp_params(snap.effective_params, fs);

    // Precompute all Biquad coefficients off the audio thread
    const double fs_d = static_cast<double>(fs);
    snap.hpf_coeffs = compute_butterworth_hpf(fs_d, snap.effective_params.hpf);
    for (uint32_t i = 0; i < kNumEqBands; ++i) {
        snap.eq_coeffs[i] = BiquadCoeffs::from_eq_band(fs_d, snap.effective_params.eq_bands[i]);
    }
    snap.deesser_sc_coeffs = BiquadCoeffs::bandpass_constant_peak(
        fs_d, static_cast<double>(snap.effective_params.deesser.freq_hz),
        static_cast<double>(snap.effective_params.deesser.bandwidth_q));

    return snap;
}

DspConfigSnapshot build_dsp_snapshot(const VoiceProfile& profile, const UiOverrides& overrides,
                                     uint32_t sample_rate, bool bypass,
                                     uint64_t revision) noexcept {

    return build_dsp_snapshot_from_params(profile.dsp_params, overrides, sample_rate, bypass,
                                          revision);
}

DspEngine::DspEngine(uint32_t sample_rate) noexcept {
    const uint32_t fs = sanitize_sample_rate(sample_rate);
    sample_rate_.store(fs, std::memory_order_release);

    // Initialize slot 0 with default safe neutral profile (flat EQ + -1.0 dBFS limiter)
    const VoiceProfile safe_profile = make_default_neutral_profile();
    slots_[0].config = build_dsp_snapshot(safe_profile, safe_profile.ui_overrides, fs, false, 1);
    active_slot_.store(0, std::memory_order_release);
    state_.store(EngineState::Uninitialized, std::memory_order_release);
    reset_audio_states();
}

void DspEngine::reset_audio_states() noexcept {
    for (auto& f : hpf_stage1_) {
        f.reset();
    }
    for (auto& f : hpf_stage2_) {
        f.reset();
    }
    for (auto& ch_eq : eq_states_) {
        for (auto& band : ch_eq) {
            band.reset();
        }
    }
    deesser_.reset();
    compressor_.reset();
    limiter_.reset();

    current_input_gain_ = 1.0;
    current_makeup_gain_ = 1.0;
    smoothers_initialized_ = false;
    last_comp_gr_db_.store(0.0, std::memory_order_relaxed);
    last_lim_gr_db_.store(0.0, std::memory_order_relaxed);
}

void DspEngine::reset() noexcept {
    reset_audio_states();
    was_bypassed_ = false;
    non_finite_count_.store(0, std::memory_order_relaxed);
}

void DspEngine::prepare(uint32_t sample_rate) noexcept {
    const uint32_t fs = sanitize_sample_rate(sample_rate);
    sample_rate_.store(fs, std::memory_order_release);

    // Rebuild coefficients from the currently active snapshot's base params for the new sample rate
    const uint32_t cur_idx = active_slot_.load(std::memory_order_acquire);
    const DspConfigSnapshot cur_copy = slots_[cur_idx].config;
    const uint64_t rev = revision_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    const DspConfigSnapshot updated = build_dsp_snapshot_from_params(
        cur_copy.base_params, cur_copy.ui_overrides, fs, cur_copy.bypass, rev);
    publish_snapshot(updated);
    reset_audio_states();
}

bool DspEngine::publish_snapshot(const DspConfigSnapshot& snapshot) noexcept {
    const uint32_t start =
        write_cursor_.fetch_add(1, std::memory_order_relaxed) % kSnapshotSlotCount;

    for (uint32_t attempt = 0; attempt < kSnapshotSlotCount * 8; ++attempt) {
        const uint32_t idx = (start + attempt) % kSnapshotSlotCount;
        if (idx == active_slot_.load(std::memory_order_seq_cst)) {
            continue;
        }
        bool expected = false;
        if (!slots_[idx].claimed.compare_exchange_strong(expected, true,
                                                         std::memory_order_seq_cst)) {
            continue;
        }
        if (idx != active_slot_.load(std::memory_order_seq_cst) &&
            slots_[idx].readers.load(std::memory_order_seq_cst) == 0) {
            slots_[idx].config = snapshot;
            active_slot_.store(idx, std::memory_order_seq_cst);
            slots_[idx].claimed.store(false, std::memory_order_release);
            return true;
        }
        slots_[idx].claimed.store(false, std::memory_order_release);
    }
    return false;
}

bool DspEngine::apply_profile(const VoiceProfile& profile, bool bypass) noexcept {
    return apply_profile_with_overrides(profile, profile.ui_overrides, bypass);
}

bool DspEngine::apply_profile_with_overrides(const VoiceProfile& profile,
                                             const UiOverrides& overrides, bool bypass) noexcept {

    const uint32_t fs = sample_rate_.load(std::memory_order_acquire);
    const uint64_t rev = revision_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    const DspConfigSnapshot snap = build_dsp_snapshot(profile, overrides, fs, bypass, rev);
    if (!publish_snapshot(snap)) {
        return false;
    }
    state_.store(bypass ? EngineState::Bypassed : EngineState::ActiveProfile,
                 std::memory_order_release);
    return true;
}

bool DspEngine::apply_snapshot(const DspConfigSnapshot& snapshot) noexcept {
    if (!publish_snapshot(snapshot)) {
        return false;
    }
    state_.store(snapshot.bypass ? EngineState::Bypassed : EngineState::ActiveProfile,
                 std::memory_order_release);
    return true;
}

bool DspEngine::set_bypass(bool bypass) noexcept {
    const uint32_t cur_idx = active_slot_.load(std::memory_order_acquire);
    DspConfigSnapshot copy = slots_[cur_idx].config;
    if (copy.bypass == bypass) {
        return true;
    }
    copy.bypass = bypass;
    copy.revision = revision_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!publish_snapshot(copy)) {
        return false;
    }
    state_.store(bypass ? EngineState::Bypassed : EngineState::ActiveProfile,
                 std::memory_order_release);
    return true;
}

void DspEngine::process(float* const* channels, uint32_t num_channels,
                        uint32_t num_frames) noexcept {
    if (channels == nullptr || num_channels == 0 || num_frames == 0) {
        return;
    }

    const uint32_t ch_count = std::min(num_channels, kMaxChannels);
    for (uint32_t ch = 0; ch < ch_count; ++ch) {
        if (channels[ch] == nullptr) {
            return;
        }
    }

    // Lock-free wait-free reader acquisition on 16-slot snapshot ring
    uint32_t slot_idx = 0;
    for (;;) {
        slot_idx = active_slot_.load(std::memory_order_seq_cst);
        slots_[slot_idx].readers.fetch_add(1, std::memory_order_seq_cst);
        if (active_slot_.load(std::memory_order_seq_cst) == slot_idx) {
            break;
        }
        slots_[slot_idx].readers.fetch_sub(1, std::memory_order_seq_cst);
    }

    struct SlotReleaseGuard {
        std::atomic<uint32_t>& readers;
        ~SlotReleaseGuard() noexcept { readers.fetch_sub(1, std::memory_order_seq_cst); }
    } guard{slots_[slot_idx].readers};

    const DspConfigSnapshot& cfg = slots_[slot_idx].config;

    // Bypass mode: pass audio through untouched; reset states upon re-enable to avoid clicks
    if (cfg.bypass) {
        was_bypassed_ = true;
        return;
    }
    if (was_bypassed_) {
        reset_audio_states();
        was_bypassed_ = false;
    }

    const double target_input_gain =
        db_to_linear(static_cast<double>(cfg.effective_params.input_gain_db));
    const double comp_makeup_db =
        cfg.effective_params.compressor.enabled
            ? static_cast<double>(cfg.effective_params.compressor.makeup_gain_db)
            : 0.0;
    const double total_makeup_db = comp_makeup_db + static_cast<double>(cfg.output_makeup_trim_db);
    const double target_makeup_gain = db_to_linear(total_makeup_db);
    const double alpha_smooth = cfg.gain_smooth_coeff;

    if (!smoothers_initialized_) {
        current_input_gain_ = target_input_gain;
        current_makeup_gain_ = target_makeup_gain;
        smoothers_initialized_ = true;
    }

    const bool hpf_enabled = cfg.effective_params.hpf.enabled;
    const bool hpf_4th_order = (cfg.effective_params.hpf.slope_db_per_oct == 24);
    const uint32_t fs = cfg.sample_rate;

    double frame[kMaxChannels]{};

    for (uint32_t n = 0; n < num_frames; ++n) {
        // 0. Load input frame with NaN/Inf detection and filter state reset
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            const float raw = channels[ch][n];
            if (!std::isfinite(raw)) {
                frame[ch] = 0.0;
                hpf_stage1_[ch].reset();
                hpf_stage2_[ch].reset();
                for (uint32_t b = 0; b < kNumEqBands; ++b) {
                    eq_states_[ch][b].reset();
                }
                non_finite_count_.fetch_add(1, std::memory_order_relaxed);
            } else {
                frame[ch] = static_cast<double>(raw);
            }
        }

        // 1. Input Trim (10ms smoothed gain)
        current_input_gain_ =
            alpha_smooth * current_input_gain_ + (1.0 - alpha_smooth) * target_input_gain;
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            frame[ch] *= current_input_gain_;
        }

        // 2. High-Pass Filter (2nd / 4th order Butterworth)
        if (hpf_enabled) {
            for (uint32_t ch = 0; ch < ch_count; ++ch) {
                double s = hpf_stage1_[ch].process(frame[ch], cfg.hpf_coeffs[0]);
                if (hpf_4th_order) {
                    s = hpf_stage2_[ch].process(s, cfg.hpf_coeffs[1]);
                }
                frame[ch] = s;
            }
        }

        // 3. 8-Band Parametric EQ
        for (uint32_t b = 0; b < kNumEqBands; ++b) {
            if (!cfg.effective_params.eq_bands[b].enabled) {
                continue;
            }
            const BiquadCoeffs& coeffs = cfg.eq_coeffs[b];
            for (uint32_t ch = 0; ch < ch_count; ++ch) {
                frame[ch] = eq_states_[ch][b].process(frame[ch], coeffs);
            }
        }

        // 4. De-Esser (4kHz-10kHz dynamic split-band reduction)
        if (cfg.effective_params.deesser.enabled) {
            deesser_.process_frame(frame, ch_count, cfg.effective_params.deesser,
                                   cfg.deesser_sc_coeffs, fs);
        }

        // 5. Stereo-Linked RMS/Peak Hybrid Compressor
        if (cfg.effective_params.compressor.enabled) {
            compressor_.process_frame(frame, ch_count, cfg.effective_params.compressor, fs);
        }

        // 6. Auto/Manual Makeup Gain + Output Trim (10ms smoothed gain)
        current_makeup_gain_ =
            alpha_smooth * current_makeup_gain_ + (1.0 - alpha_smooth) * target_makeup_gain;
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            frame[ch] *= current_makeup_gain_;
        }

        // 7. Lookahead Brickwall Limiter (with hard ceiling clamp & NaN guard)
        if (limiter_.process_frame(frame, ch_count, cfg.effective_params.limiter, fs)) {
            non_finite_count_.fetch_add(1, std::memory_order_relaxed);
        }

        // 8. Final output write & safety guard
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            const float out = static_cast<float>(frame[ch]);
            if (!std::isfinite(out)) {
                channels[ch][n] = 0.0f;
                non_finite_count_.fetch_add(1, std::memory_order_relaxed);
            } else {
                channels[ch][n] = out;
            }
        }
    }

    last_comp_gr_db_.store(compressor_.last_gain_reduction_db(), std::memory_order_relaxed);
    last_lim_gr_db_.store(limiter_.last_gain_reduction_db(), std::memory_order_relaxed);
}

} // namespace kotone
