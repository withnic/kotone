#include "test_framework.hpp"

#include "kotone/dsp_engine.hpp"
#include "kotone/profile_loader.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace kotone::test {
namespace {

void test_brickwall_ceiling_sine_and_pulse() {
    constexpr uint32_t kFs = 48000;
    DspEngine engine(kFs);

    VoiceProfile profile = make_default_neutral_profile();
    profile.dsp_params.hpf.enabled = false;
    profile.dsp_params.compressor.enabled = false;
    profile.dsp_params.deesser.enabled = false;
    profile.dsp_params.limiter.enabled = true;
    profile.dsp_params.limiter.ceiling_dbfs = -1.0f;
    profile.dsp_params.limiter.lookahead_ms = 2.0f;
    profile.dsp_params.limiter.release_ms = 50.0f;
    profile.ui_overrides.limiter_ceiling_dbfs = -1.0f;
    KOTONE_EXPECT_TRUE(engine.apply_profile(profile));

    const float expected_ceiling = db_to_linear_f(-1.0f); // ~0.89125f
    KOTONE_EXPECT_NEAR(expected_ceiling, 0.8912509f, 1e-5);

    // Part 1: Sudden +12 dBFS (amplitude 3.98107f) sine burst after silence
    constexpr uint32_t kNumFrames = 4800; // 100 ms
    std::vector<float> left(kNumFrames, 0.0f);
    std::vector<float> right(kNumFrames, 0.0f);
    for (uint32_t i = 240; i < kNumFrames; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kFs);
        const float s = static_cast<float>(3.98107 * std::sin(2.0 * kPi * 1000.0 * t));
        left[i] = s;
        right[i] = -s;
    }

    float* ptrs[2] = {left.data(), right.data()};
    engine.process(ptrs, 2, kNumFrames);

    float peak_out = 0.0f;
    for (uint32_t i = 0; i < kNumFrames; ++i) {
        KOTONE_EXPECT_TRUE(std::isfinite(left[i]));
        KOTONE_EXPECT_TRUE(std::isfinite(right[i]));
        KOTONE_EXPECT_LE(std::abs(left[i]), expected_ceiling + 1e-5f);
        KOTONE_EXPECT_LE(std::abs(right[i]), expected_ceiling + 1e-5f);
        peak_out = std::max(peak_out, std::max(std::abs(left[i]), std::abs(right[i])));
    }
    // Verify limiter passed strong signal near ceiling rather than muting
    KOTONE_EXPECT_GE(peak_out, expected_ceiling * 0.85f);

    // Part 2: Sharp +12 dBFS single-sample & multi-sample impulses
    engine.reset();
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    left[100] = 3.98107f;
    right[100] = -3.98107f;
    left[500] = -3.98107f;
    left[501] = 3.98107f;
    left[502] = 3.98107f;
    right[1000] = 6.0f;

    engine.process(ptrs, 2, kNumFrames);

    float pulse_peak = 0.0f;
    for (uint32_t i = 0; i < kNumFrames; ++i) {
        KOTONE_EXPECT_TRUE(std::isfinite(left[i]));
        KOTONE_EXPECT_TRUE(std::isfinite(right[i]));
        KOTONE_EXPECT_LE(std::abs(left[i]), expected_ceiling + 1e-5f);
        KOTONE_EXPECT_LE(std::abs(right[i]), expected_ceiling + 1e-5f);
        pulse_peak = std::max(pulse_peak, std::max(std::abs(left[i]), std::abs(right[i])));
    }
    KOTONE_EXPECT_GE(pulse_peak, expected_ceiling * 0.80f);
}

void test_brickwall_various_ceilings_and_lookahead() {
    constexpr uint32_t kFs = 48000;
    const float ceilings[] = {-6.0f, -1.0f, -0.1f};
    const float lookaheads[] = {0.0f, 2.0f, 5.0f};

    for (float ceil_db : ceilings) {
        for (float la_ms : lookaheads) {
            DspEngine engine(kFs);
            VoiceProfile p = make_default_neutral_profile();
            p.dsp_params.hpf.enabled = false;
            p.dsp_params.compressor.enabled = false;
            p.dsp_params.limiter.enabled = true;
            p.dsp_params.limiter.ceiling_dbfs = ceil_db;
            p.dsp_params.limiter.lookahead_ms = la_ms;
            p.ui_overrides.limiter_ceiling_dbfs = ceil_db;
            KOTONE_EXPECT_TRUE(engine.apply_profile(p));

            const float ceil_lin = db_to_linear_f(ceil_db);
            std::vector<float> buf(2048, 0.0f);
            for (size_t i = 0; i < buf.size(); ++i) {
                buf[i] = static_cast<float>(
                    4.5 * std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / kFs));
            }
            float* ptr = buf.data();
            engine.process(&ptr, 1, static_cast<uint32_t>(buf.size()));

            for (float s : buf) {
                KOTONE_EXPECT_TRUE(std::isfinite(s));
                KOTONE_EXPECT_LE(std::abs(s), ceil_lin + 1e-5f);
            }
        }
    }
}

void test_nan_inf_recovery() {
    constexpr uint32_t kFs = 48000;
    DspEngine engine(kFs);

    VoiceProfile p = make_default_neutral_profile();
    KOTONE_EXPECT_TRUE(engine.apply_profile(p));

    constexpr uint32_t kFrames = 2048;
    std::vector<float> left(kFrames, 0.0f);
    std::vector<float> right(kFrames, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i) {
        const float s =
            static_cast<float>(0.25 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
        left[i] = s;
        right[i] = s;
    }

    // Inject NaN, +Inf, -Inf into the audio stream
    left[200] = std::numeric_limits<float>::quiet_NaN();
    right[201] = std::numeric_limits<float>::infinity();
    left[202] = -std::numeric_limits<float>::infinity();

    float* ptrs[2] = {left.data(), right.data()};
    engine.process(ptrs, 2, kFrames);

    KOTONE_EXPECT_GE(engine.non_finite_count(), 3.0);

    for (uint32_t i = 0; i < kFrames; ++i) {
        KOTONE_EXPECT_TRUE(std::isfinite(left[i]));
        KOTONE_EXPECT_TRUE(std::isfinite(right[i]));
    }

    // Verify clean signal recovery in the tail after the corrupted samples
    float tail_peak = 0.0f;
    for (uint32_t i = 1000; i < kFrames; ++i) {
        tail_peak = std::max(tail_peak, std::abs(left[i]));
    }
    KOTONE_EXPECT_GE(tail_peak, 0.1f);
}

void test_bypass_and_reenable() {
    constexpr uint32_t kFs = 48000;
    DspEngine engine(kFs);

    VoiceProfile p = make_default_neutral_profile();
    p.dsp_params.input_gain_db = 12.0f;
    KOTONE_EXPECT_TRUE(engine.apply_profile(p, true));
    KOTONE_EXPECT_TRUE(engine.state() == EngineState::Bypassed);

    std::vector<float> buf(512);
    std::vector<float> orig(512);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] =
            static_cast<float>(0.3 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs));
        orig[i] = buf[i];
    }

    float* ptr = buf.data();
    engine.process(&ptr, 1, static_cast<uint32_t>(buf.size()));

    // Bypass must pass samples through completely unmodified
    for (size_t i = 0; i < buf.size(); ++i) {
        KOTONE_EXPECT_NEAR(buf[i], orig[i], 1e-7);
    }

    // Re-enable processing and verify ceiling protection works cleanly
    KOTONE_EXPECT_TRUE(engine.set_bypass(false));
    KOTONE_EXPECT_TRUE(engine.state() == EngineState::ActiveProfile);

    std::vector<float> loud(1024, 3.0f);
    float* lptr = loud.data();
    engine.process(&lptr, 1, static_cast<uint32_t>(loud.size()));

    const float ceil_lin = db_to_linear_f(p.ui_overrides.limiter_ceiling_dbfs);
    for (float s : loud) {
        KOTONE_EXPECT_TRUE(std::isfinite(s));
        KOTONE_EXPECT_LE(std::abs(s), ceil_lin + 1e-5f);
    }
}

void test_multithread_lockfree_snapshot_switching() {
    constexpr uint32_t kFs = 48000;
    DspEngine engine(kFs);

    VoiceProfile p1;
    VoiceProfile p2;
    VoiceProfile p3;
    KOTONE_EXPECT_TRUE(get_builtin_preset("builtin:neutral_safe", p1));
    KOTONE_EXPECT_TRUE(get_builtin_preset("builtin:clear_broadcast", p2));
    KOTONE_EXPECT_TRUE(get_builtin_preset("builtin:warm_podcast", p3));
    const VoiceProfile profiles[3] = {p1, p2, p3};

    std::atomic<bool> writer_done{false};
    std::atomic<bool> audio_failed{false};
    std::atomic<int> switch_failures{0};

    // Audio thread continuously calling process() on 4 channels
    std::thread audio_thread([&]() {
        constexpr uint32_t kBlock = 128;
        std::array<std::vector<float>, 4> chs;
        float* ptrs[4]{};
        for (uint32_t c = 0; c < 4; ++c) {
            chs[c].resize(kBlock, 0.0f);
            ptrs[c] = chs[c].data();
        }
        const float max_allowed = db_to_linear_f(-1.0f) + 1e-4f;
        uint64_t sample_idx = 0;

        while (!writer_done.load(std::memory_order_acquire)) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                const double t = static_cast<double>(sample_idx++) / static_cast<double>(kFs);
                const float s = static_cast<float>(2.0 * std::sin(2.0 * kPi * 600.0 * t));
                for (uint32_t c = 0; c < 4; ++c) {
                    chs[c][i] = s;
                }
            }
            engine.process(ptrs, 4, kBlock);
            for (uint32_t c = 0; c < 4; ++c) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    if (!std::isfinite(chs[c][i]) || std::abs(chs[c][i]) > max_allowed) {
                        audio_failed.store(true, std::memory_order_relaxed);
                    }
                }
            }
        }
    });

    // UI/control thread performing 1200 consecutive profile switches
    for (int i = 0; i < 1200; ++i) {
        if (!engine.apply_profile(profiles[i % 3])) {
            switch_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    writer_done.store(true, std::memory_order_release);
    audio_thread.join();

    KOTONE_EXPECT_EQ(switch_failures.load(), 0);
    KOTONE_EXPECT_FALSE(audio_failed.load());
}

} // namespace

void run_dsp_limiter_tests(int& passed, int& failed) {
    run_case("dsp_limiter_brickwall_ceiling_sine_and_pulse", test_brickwall_ceiling_sine_and_pulse,
             passed, failed);
    run_case("dsp_limiter_various_ceilings_and_lookahead",
             test_brickwall_various_ceilings_and_lookahead, passed, failed);
    run_case("dsp_limiter_nan_inf_recovery", test_nan_inf_recovery, passed, failed);
    run_case("dsp_limiter_bypass_and_reenable", test_bypass_and_reenable, passed, failed);
    run_case("dsp_limiter_multithread_lockfree_switching",
             test_multithread_lockfree_snapshot_switching, passed, failed);
}

} // namespace kotone::test
