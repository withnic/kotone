#include "test_framework.hpp"

#include "kotone/dsp_engine.hpp"

#include <cmath>
#include <vector>

namespace kotone::test {
namespace {

double measure_steady_peak_db(const std::vector<float>& buf, size_t skip_samples) {
    double max_abs = 0.0;
    for (size_t i = skip_samples; i < buf.size(); ++i) {
        max_abs = std::max(max_abs, static_cast<double>(std::abs(buf[i])));
    }
    return linear_to_db(max_abs);
}

double measure_steady_rms_db(const std::vector<float>& buf, size_t skip_samples) {
    double sum_sq = 0.0;
    const size_t count = buf.size() - skip_samples;
    for (size_t i = skip_samples; i < buf.size(); ++i) {
        const double s = static_cast<double>(buf[i]);
        sum_sq += s * s;
    }
    return linear_to_db(std::sqrt(sum_sq / static_cast<double>(count)));
}

VoiceProfile make_clean_passthrough_profile() {
    VoiceProfile p = make_default_neutral_profile();
    p.dsp_params.input_gain_db = 0.0f;
    p.dsp_params.hpf.enabled = false;
    for (auto& b : p.dsp_params.eq_bands) {
        b.enabled = false;
        b.gain_db = 0.0f;
    }
    p.dsp_params.deesser.enabled = false;
    p.dsp_params.compressor.enabled = false;
    p.dsp_params.compressor.makeup_gain_db = 0.0f;
    p.dsp_params.limiter.enabled = false;
    p.ui_overrides = {100.0f, 0.0f, -1.0f, 100.0f, 100.0f};
    return p;
}

void test_hpf_frequency_response() {
    constexpr uint32_t kFs = 48000;
    constexpr size_t kFrames = 24000; // 0.5 sec
    constexpr size_t kSkip = 12000;

    for (int slope : {24, 12}) {
        VoiceProfile p = make_clean_passthrough_profile();
        p.dsp_params.hpf.enabled = true;
        p.dsp_params.hpf.cutoff_hz = 85.0f;
        p.dsp_params.hpf.slope_db_per_oct = slope;

        // 40 Hz stopband sine
        {
            DspEngine engine(kFs);
            KOTONE_EXPECT_TRUE(engine.apply_profile(p));
            std::vector<float> buf(kFrames);
            const double in_amp = 0.5;
            for (size_t i = 0; i < kFrames; ++i) {
                buf[i] = static_cast<float>(
                    in_amp * std::sin(2.0 * kPi * 40.0 * static_cast<double>(i) / kFs));
            }
            const double in_rms = measure_steady_rms_db(buf, kSkip);
            float* ptr = buf.data();
            engine.process(&ptr, 1, static_cast<uint32_t>(kFrames));
            const double out_rms = measure_steady_rms_db(buf, kSkip);
            const double atten_db = in_rms - out_rms;
            KOTONE_EXPECT_GE(atten_db, 12.0);
        }

        // 1000 Hz passband sine (must be within +/- 0.2 dB)
        {
            DspEngine engine(kFs);
            KOTONE_EXPECT_TRUE(engine.apply_profile(p));
            std::vector<float> buf(kFrames);
            const double in_amp = 0.5;
            for (size_t i = 0; i < kFrames; ++i) {
                buf[i] = static_cast<float>(
                    in_amp * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
            }
            const double in_rms = measure_steady_rms_db(buf, kSkip);
            float* ptr = buf.data();
            engine.process(&ptr, 1, static_cast<uint32_t>(kFrames));
            const double out_rms = measure_steady_rms_db(buf, kSkip);
            KOTONE_EXPECT_NEAR(out_rms - in_rms, 0.0, 0.2);
        }
    }
}

void test_peq_frequency_response() {
    constexpr uint32_t kFs = 48000;
    constexpr size_t kFrames = 16000;
    constexpr size_t kSkip = 8000;

    VoiceProfile p = make_clean_passthrough_profile();
    // Band 0: Low-Shelf +4 dB @ 120 Hz
    p.dsp_params.eq_bands[0] = {0, true, EqBandType::LowShelf, 120.0f, 4.0f, 0.707f};
    // Band 1: Peaking -6 dB @ 250 Hz
    p.dsp_params.eq_bands[1] = {1, true, EqBandType::Peaking, 250.0f, -6.0f, 2.0f};
    // Band 3: Peaking +6 dB @ 1500 Hz
    p.dsp_params.eq_bands[3] = {3, true, EqBandType::Peaking, 1500.0f, 6.0f, 2.0f};
    // Band 7: High-Shelf +4 dB @ 8000 Hz
    p.dsp_params.eq_bands[7] = {7, true, EqBandType::HighShelf, 8000.0f, 4.0f, 0.707f};

    auto measure_gain_at = [&](double freq_hz) -> double {
        DspEngine engine(kFs);
        KOTONE_EXPECT_TRUE(engine.apply_profile(p));
        std::vector<float> buf(kFrames);
        for (size_t i = 0; i < kFrames; ++i) {
            buf[i] = static_cast<float>(
                0.2 * std::sin(2.0 * kPi * freq_hz * static_cast<double>(i) / kFs));
        }
        const double in_rms = measure_steady_rms_db(buf, kSkip);
        float* ptr = buf.data();
        engine.process(&ptr, 1, static_cast<uint32_t>(kFrames));
        const double out_rms = measure_steady_rms_db(buf, kSkip);
        return out_rms - in_rms;
    };

    KOTONE_EXPECT_NEAR(measure_gain_at(35.0), 4.0, 0.5);
    KOTONE_EXPECT_NEAR(measure_gain_at(250.0), -6.0, 0.5);
    KOTONE_EXPECT_NEAR(measure_gain_at(1500.0), 6.0, 0.5);
    KOTONE_EXPECT_NEAR(measure_gain_at(16000.0), 4.0, 0.6);
}

void test_deesser_sibilance_reduction() {
    constexpr uint32_t kFs = 48000;
    constexpr size_t kFrames = 16000;
    constexpr size_t kSkip = 8000;

    VoiceProfile p = make_clean_passthrough_profile();
    p.dsp_params.deesser.enabled = true;
    p.dsp_params.deesser.freq_hz = 6500.0f;
    p.dsp_params.deesser.bandwidth_q = 2.5f;
    p.dsp_params.deesser.threshold_dbfs = -24.0f;
    p.dsp_params.deesser.max_reduction_db = 8.0f;
    p.dsp_params.deesser.attack_ms = 1.0f;
    p.dsp_params.deesser.release_ms = 40.0f;

    // 6500 Hz loud sibilance tone (-10 dBFS) should be attenuated significantly
    {
        DspEngine engine(kFs);
        KOTONE_EXPECT_TRUE(engine.apply_profile(p));
        const double amp = db_to_linear(-10.0);
        std::vector<float> buf(kFrames);
        for (size_t i = 0; i < kFrames; ++i) {
            buf[i] = static_cast<float>(
                amp * std::sin(2.0 * kPi * 6500.0 * static_cast<double>(i) / kFs));
        }
        const double in_rms = measure_steady_rms_db(buf, kSkip);
        float* ptr = buf.data();
        engine.process(&ptr, 1, static_cast<uint32_t>(kFrames));
        const double out_rms = measure_steady_rms_db(buf, kSkip);
        KOTONE_EXPECT_GE(in_rms - out_rms, 6.0);
    }

    // 500 Hz non-sibilant voice fundamental (-10 dBFS) should pass nearly untouched
    {
        DspEngine engine(kFs);
        KOTONE_EXPECT_TRUE(engine.apply_profile(p));
        const double amp = db_to_linear(-10.0);
        std::vector<float> buf(kFrames);
        for (size_t i = 0; i < kFrames; ++i) {
            buf[i] = static_cast<float>(amp *
                                        std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / kFs));
        }
        const double in_rms = measure_steady_rms_db(buf, kSkip);
        float* ptr = buf.data();
        engine.process(&ptr, 1, static_cast<uint32_t>(kFrames));
        const double out_rms = measure_steady_rms_db(buf, kSkip);
        KOTONE_EXPECT_NEAR(out_rms - in_rms, 0.0, 0.2);
    }
}

void test_compressor_ratio_and_stereo_link() {
    constexpr uint32_t kFs = 48000;
    constexpr size_t kFrames = 24000; // 500 ms
    constexpr size_t kSkip = 12000;

    VoiceProfile p = make_clean_passthrough_profile();
    p.dsp_params.compressor.enabled = true;
    p.dsp_params.compressor.threshold_dbfs = -20.0f;
    p.dsp_params.compressor.ratio = 4.0f;
    p.dsp_params.compressor.knee_db = 6.0f;
    p.dsp_params.compressor.attack_ms = 10.0f;
    p.dsp_params.compressor.release_ms = 100.0f;
    p.dsp_params.compressor.makeup_gain_db = 0.0f;

    // Part 1: -8 dBFS steady sine -> theoretical output = -20 + (-8 - (-20)) / 4 = -17.0 dBFS (+/-
    // 0.5 dB)
    {
        DspEngine engine(kFs);
        KOTONE_EXPECT_TRUE(engine.apply_profile(p));

        const double in_amp = db_to_linear(-8.0);
        std::vector<float> left(kFrames);
        std::vector<float> right(kFrames);
        for (size_t i = 0; i < kFrames; ++i) {
            const float s = static_cast<float>(
                in_amp * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
            left[i] = s;
            right[i] = s;
        }
        float* ptrs[2] = {left.data(), right.data()};
        engine.process(ptrs, 2, static_cast<uint32_t>(kFrames));

        const double out_peak_l_db = measure_steady_peak_db(left, kSkip);
        const double out_peak_r_db = measure_steady_peak_db(right, kSkip);
        KOTONE_EXPECT_NEAR(out_peak_l_db, -17.0, 0.5);
        KOTONE_EXPECT_NEAR(out_peak_r_db, -17.0, 0.5);
        KOTONE_EXPECT_NEAR(engine.last_compressor_gr_db(), 9.0, 0.5);
    }

    // Part 2: Stereo-link verification — Lch loud (-8 dBFS), Rch silent (0.0)
    {
        DspEngine engine(kFs);
        KOTONE_EXPECT_TRUE(engine.apply_profile(p));

        const double in_amp_l = db_to_linear(-8.0);
        std::vector<float> left(kFrames);
        std::vector<float> right(kFrames, 0.0f);
        for (size_t i = 0; i < kFrames; ++i) {
            left[i] = static_cast<float>(
                in_amp_l * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
        }
        float* ptrs[2] = {left.data(), right.data()};
        engine.process(ptrs, 2, static_cast<uint32_t>(kFrames));

        KOTONE_EXPECT_NEAR(measure_steady_peak_db(left, kSkip), -17.0, 0.5);
        for (float s : right) {
            KOTONE_EXPECT_NEAR(s, 0.0, 1e-7);
        }
        KOTONE_EXPECT_NEAR(engine.last_compressor_gr_db(), 9.0, 0.5);
    }

    // Part 3: Stereo-link identical GR proof — Lch loud (-8 dBFS), Rch quiet probe (-40 dBFS)
    // Even though Rch is well below threshold (-20 dBFS), it must get the EXACT same -9 dB GR as
    // Lch.
    {
        DspEngine engine(kFs);
        KOTONE_EXPECT_TRUE(engine.apply_profile(p));

        const double in_amp_l = db_to_linear(-8.0);
        const double in_amp_r = db_to_linear(-40.0);
        std::vector<float> left(kFrames);
        std::vector<float> right(kFrames);
        for (size_t i = 0; i < kFrames; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(kFs);
            left[i] = static_cast<float>(in_amp_l * std::sin(2.0 * kPi * 1000.0 * t));
            right[i] = static_cast<float>(in_amp_r * std::sin(2.0 * kPi * 1000.0 * t));
        }
        float* ptrs[2] = {left.data(), right.data()};
        engine.process(ptrs, 2, static_cast<uint32_t>(kFrames));

        const double gr_l = -8.0 - measure_steady_peak_db(left, kSkip);
        const double gr_r = -40.0 - measure_steady_peak_db(right, kSkip);
        KOTONE_EXPECT_NEAR(gr_l, 9.0, 0.5);
        KOTONE_EXPECT_NEAR(gr_r, gr_l, 0.05);
    }
}

void test_mix_interpolation_and_safety_retention() {
    constexpr uint32_t kFs = 48000;
    constexpr size_t kFrames = 16000;
    constexpr size_t kSkip = 8000;

    VoiceProfile p = make_default_neutral_profile();
    p.dsp_params.hpf.enabled = true;
    p.dsp_params.hpf.cutoff_hz = 85.0f;
    p.dsp_params.hpf.slope_db_per_oct = 24;
    p.dsp_params.eq_bands[3] = {3, true, EqBandType::Peaking, 1000.0f, 8.0f, 1.5f};
    p.dsp_params.compressor.enabled = true;
    p.dsp_params.compressor.threshold_dbfs = -20.0f;
    p.dsp_params.compressor.ratio = 5.0f;
    p.dsp_params.compressor.makeup_gain_db = 6.0f;
    p.dsp_params.limiter.enabled = true;
    p.dsp_params.limiter.ceiling_dbfs = -1.0f;

    // Mix = 0%: EQ & Compressor must be neutralized, while HPF and Limiter stay 100% active
    UiOverrides ov_zero{0.0f, 0.0f, -1.0f};
    const DspConfigSnapshot snap_zero = build_dsp_snapshot(p, ov_zero, kFs);
    KOTONE_EXPECT_NEAR(snap_zero.effective_params.eq_bands[3].gain_db, 0.0, 1e-5);
    KOTONE_EXPECT_NEAR(snap_zero.effective_params.compressor.ratio, 1.0, 1e-5);
    KOTONE_EXPECT_NEAR(snap_zero.effective_params.compressor.makeup_gain_db, 0.0, 1e-5);
    KOTONE_EXPECT_TRUE(snap_zero.effective_params.hpf.enabled);
    KOTONE_EXPECT_TRUE(snap_zero.effective_params.limiter.enabled);

    // Verify 40Hz is still attenuated (HPF active) and +12dBFS is still brickwall-limited at mix=0%
    {
        DspEngine engine(kFs);
        KOTONE_EXPECT_TRUE(engine.apply_profile_with_overrides(p, ov_zero));
        std::vector<float> low(kFrames);
        for (size_t i = 0; i < kFrames; ++i) {
            low[i] =
                static_cast<float>(0.3 * std::sin(2.0 * kPi * 40.0 * static_cast<double>(i) / kFs));
        }
        const double in_rms = measure_steady_rms_db(low, kSkip);
        float* ptr = low.data();
        engine.process(&ptr, 1, static_cast<uint32_t>(kFrames));
        const double out_rms = measure_steady_rms_db(low, kSkip);
        KOTONE_EXPECT_GE(in_rms - out_rms, 12.0);
    }

    // Mix = 50%: EQ gain = 4.0 dB, ratio = 1 + (5 - 1) * 0.5 = 3.0, makeup = 3.0 dB
    UiOverrides ov_half{50.0f, 0.0f, -1.0f};
    const DspConfigSnapshot snap_half = build_dsp_snapshot(p, ov_half, kFs);
    KOTONE_EXPECT_NEAR(snap_half.effective_params.eq_bands[3].gain_db, 4.0, 1e-5);
    KOTONE_EXPECT_NEAR(snap_half.effective_params.compressor.ratio, 3.0, 1e-5);
    KOTONE_EXPECT_NEAR(snap_half.effective_params.compressor.makeup_gain_db, 3.0, 1e-5);
}

void test_gain_smoothing_no_click() {
    constexpr uint32_t kFs = 48000;
    DspEngine engine(kFs);

    VoiceProfile p1 = make_clean_passthrough_profile();
    p1.dsp_params.input_gain_db = 0.0f;
    KOTONE_EXPECT_TRUE(engine.apply_profile(p1));

    // Prime engine with DC 0.1 so smoothers initialize at 0 dB (1.0x)
    std::vector<float> prime(64, 0.1f);
    float* ptr = prime.data();
    engine.process(&ptr, 1, static_cast<uint32_t>(prime.size()));

    // Jump input gain to +12 dB (~3.981x) and verify smooth transition over ~10ms (480 samples)
    VoiceProfile p2 = p1;
    p2.dsp_params.input_gain_db = 12.0f;
    KOTONE_EXPECT_TRUE(engine.apply_profile(p2));

    std::vector<float> ramp(2400, 0.1f);
    float* rptr = ramp.data();
    engine.process(&rptr, 1, static_cast<uint32_t>(ramp.size()));

    // First sample must not instantly jump to 0.398f
    KOTONE_EXPECT_LE(ramp[0], 0.12f);
    // Midpoint (~480 samples = 1 tau) should be around 1 - 1/e = 63.2% of transition (~0.288)
    KOTONE_EXPECT_GE(ramp[480], 0.25f);
    KOTONE_EXPECT_LE(ramp[480], 0.33f);
    // Tail (5 tau = 2400 samples) should converge close to 0.3981
    KOTONE_EXPECT_NEAR(ramp.back(), 0.1 * db_to_linear(12.0), 0.01);
}

} // namespace

void run_dsp_eq_comp_tests(int& passed, int& failed) {
    run_case("dsp_hpf_frequency_response", test_hpf_frequency_response, passed, failed);
    run_case("dsp_peq_frequency_response", test_peq_frequency_response, passed, failed);
    run_case("dsp_deesser_sibilance_reduction", test_deesser_sibilance_reduction, passed, failed);
    run_case("dsp_compressor_ratio_and_stereo_link", test_compressor_ratio_and_stereo_link, passed,
             failed);
    run_case("dsp_mix_interpolation_and_safety_retention",
             test_mix_interpolation_and_safety_retention, passed, failed);
    run_case("dsp_gain_smoothing_no_click", test_gain_smoothing_no_click, passed, failed);
}

} // namespace kotone::test
