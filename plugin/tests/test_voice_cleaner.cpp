#include "test_framework.hpp"

#include "kotone/dsp_engine.hpp"
#include "kotone/profile_loader.hpp"
#include "kotone/voice_cleaner.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kotone::test {
namespace {

constexpr uint32_t kFs = 48000;

// Deterministic uniform noise in [-1, 1]
class Lcg {
public:
    explicit Lcg(uint32_t seed) : state_(seed) {}
    double next() {
        state_ = state_ * 1664525u + 1013904223u;
        return (static_cast<double>(state_ >> 8u) / 16777216.0) * 2.0 - 1.0;
    }

private:
    uint32_t state_;
};

// Band-limited voiced-vowel-like tone: harmonics of f0 with 1/k^2 roll-off up to 4 kHz.
double voiced_sample(double t, double f0) {
    double s = 0.0;
    for (int k = 1; static_cast<double>(k) * f0 < 4000.0; ++k) {
        const double kd = static_cast<double>(k);
        s += std::sin(2.0 * kPi * f0 * kd * t + 0.3 * kd) / (kd * kd);
    }
    return s;
}

double rms_db(const std::vector<double>& buf, size_t begin, size_t end) {
    double acc = 0.0;
    for (size_t i = begin; i < end; ++i) {
        acc += buf[i] * buf[i];
    }
    return linear_to_db(std::sqrt(acc / static_cast<double>(end - begin)));
}

// Energy of the 2nd difference (high-frequency content) in a window.
double hf_energy_db(const std::vector<double>& buf, size_t begin, size_t end) {
    double acc = 0.0;
    for (size_t i = std::max<size_t>(begin, 2); i < end; ++i) {
        const double d = buf[i] - 2.0 * buf[i - 1] + buf[i - 2];
        acc += d * d;
    }
    return 10.0 * std::log10(std::max(acc, 1e-30));
}

std::vector<double> run_cleaner(const std::vector<double>& in, const VoiceCleanerConfig& cfg,
                                uint64_t* click_events = nullptr) {
    auto cleaner = std::make_unique<VoiceCleaner>();
    std::vector<double> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        double frame[1] = {in[i]};
        cleaner->process_frame(frame, 1, cfg);
        out[i] = frame[0];
    }
    if (click_events != nullptr) {
        *click_events = cleaner->click_events();
    }
    return out;
}

DeclickParams declick_on(float max_red = 12.0f) {
    DeclickParams p;
    p.enabled = true;
    p.max_reduction_db = max_red;
    return p;
}

BreathParams breath_on(float ceiling_dbfs, float tilt_db, float max_red = 9.0f) {
    BreathParams p;
    p.enabled = true;
    p.level_ceiling_dbfs = ceiling_dbfs;
    p.tilt_threshold_db = tilt_db;
    p.max_reduction_db = max_red;
    return p;
}

void test_fixed_lookahead_and_transparency() {
    constexpr size_t kN = kFs; // 1 s
    std::vector<double> in(kN);
    for (size_t i = 0; i < kN; ++i) {
        in[i] = 0.3 * voiced_sample(static_cast<double>(i) / kFs, 140.0);
    }

    const uint32_t expected_delay = 96; // 2.0 ms @ 48 kHz
    for (bool enabled : {false, true}) {
        DeclickParams dc = enabled ? declick_on() : DeclickParams{};
        BreathParams br = enabled ? breath_on(-40.0f, 0.0f) : BreathParams{};
        const VoiceCleanerConfig cfg = make_voice_cleaner_config(dc, br, 0.0, 1.0, 1.0, kFs);
        KOTONE_EXPECT_EQ(cfg.lookahead_samples, expected_delay);

        uint64_t clicks = 0;
        const std::vector<double> out = run_cleaner(in, cfg, &clicks);
        double max_err = 0.0;
        for (size_t i = expected_delay; i < kN; ++i) {
            max_err = std::max(max_err, std::abs(out[i] - in[i - expected_delay]));
        }
        // Steady voiced speech must pass bit-transparent (float delay line precision only).
        KOTONE_EXPECT_LE(max_err, 1e-6);
        KOTONE_EXPECT_EQ(clicks, static_cast<uint64_t>(0));
    }
}

void test_lip_click_removed_in_high_band_only() {
    constexpr size_t kN = static_cast<size_t>(kFs) * 2;
    constexpr size_t kInterval = 9600; // one click every 200 ms
    Lcg rng(7);
    std::vector<double> in(kN);
    for (size_t i = 0; i < kN; ++i) {
        in[i] = 0.001 * rng.next(); // -60 dBFS room tone
    }
    std::vector<size_t> click_pos;
    for (size_t pos = 4800; pos + 100 < kN; pos += kInterval) {
        click_pos.push_back(pos);
        in[pos] += 0.25;
        in[pos + 1] -= 0.35;
        in[pos + 2] += 0.15;
    }

    const VoiceCleanerConfig cfg =
        make_voice_cleaner_config(declick_on(12.0f), BreathParams{}, 0.0, 1.0, 1.0, kFs);
    uint64_t clicks = 0;
    const std::vector<double> out = run_cleaner(in, cfg, &clicks);
    KOTONE_EXPECT_EQ(clicks, static_cast<uint64_t>(click_pos.size()));

    const size_t d = cfg.lookahead_samples;
    for (size_t pos : click_pos) {
        const double in_hf = hf_energy_db(in, pos - 24, pos + 48);
        const double out_hf = hf_energy_db(out, pos + d - 24, pos + d + 48);
        KOTONE_EXPECT_GE(in_hf - out_hf, 8.0);
    }

    // Away from clicks the room tone passes untouched.
    const size_t quiet_begin = click_pos[2] + 2400;
    double max_err = 0.0;
    for (size_t i = quiet_begin; i < quiet_begin + 2400; ++i) {
        max_err = std::max(max_err, std::abs(out[i + d] - in[i]));
    }
    KOTONE_EXPECT_LE(max_err, 1e-6);
}

void test_no_false_clicks_on_speech_onsets_and_noise() {
    constexpr size_t kN = static_cast<size_t>(kFs) * 2;
    std::vector<double> in(kN, 0.0);
    Lcg rng(11);
    // Voiced syllables with 5 ms raised-cosine attacks / 20 ms releases separated by silence
    // (natural speech has no hard edges), then steady breath-like noise.
    for (size_t i = 0; i < kFs; ++i) {
        const size_t phase = i % 12000;
        double env = 0.0;
        if (phase < 240) {
            env = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(phase) / 240.0);
        } else if (phase < 6240) {
            env = 1.0;
        } else if (phase < 7200) {
            env = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(phase - 6240) / 960.0);
        }
        in[i] = 0.4 * env * voiced_sample(static_cast<double>(i) / kFs, 180.0);
    }
    for (size_t i = kFs; i < kN; ++i) {
        in[i] = 0.02 * rng.next();
    }
    const VoiceCleanerConfig cfg =
        make_voice_cleaner_config(declick_on(), BreathParams{}, 0.0, 1.0, 1.0, kFs);
    uint64_t clicks = 0;
    run_cleaner(in, cfg, &clicks);
    KOTONE_EXPECT_EQ(clicks, static_cast<uint64_t>(0));
}

void test_breath_suppressed_voice_preserved() {
    constexpr size_t kVoice = 14400;  // 300 ms
    constexpr size_t kBreath = 24000; // 500 ms
    constexpr size_t kN = kVoice + kBreath + kVoice;
    Lcg rng(3);
    std::vector<double> in(kN);
    double prev = 0.0;
    for (size_t i = 0; i < kN; ++i) {
        const double t = static_cast<double>(i) / kFs;
        if (i < kVoice || i >= kVoice + kBreath) {
            in[i] = 0.12 * voiced_sample(t, 150.0); // ~ -25 dBFS
        } else {
            // High-tilted (first-difference) noise around -48 dBFS: inhalation-like hiss
            const double w = rng.next();
            in[i] = 0.006 * (w - prev);
            prev = w;
        }
    }

    const VoiceCleanerConfig cfg = make_voice_cleaner_config(
        DeclickParams{}, breath_on(-40.0f, 0.0f, 9.0f), 0.0, 1.0, 1.0, kFs);
    const std::vector<double> out = run_cleaner(in, cfg);
    const size_t d = cfg.lookahead_samples;

    // Middle of the breath (after hangover + min duration + attack) is reduced by ~9 dB.
    const size_t b0 = kVoice + 12000;
    const size_t b1 = kVoice + kBreath - 1200;
    const double breath_red = rms_db(in, b0, b1) - rms_db(out, b0 + d, b1 + d);
    KOTONE_EXPECT_GE(breath_red, 7.0);
    KOTONE_EXPECT_LE(breath_red, 9.5);

    // Voiced segments before and after stay within 0.2 dB (onset release uses the look-ahead).
    const double v_pre = rms_db(out, 2400 + d, kVoice - 2400 + d) - rms_db(in, 2400, kVoice - 2400);
    const size_t v2 = kVoice + kBreath;
    const double v_post = rms_db(out, v2 + 480 + d, kN) - rms_db(in, v2 + 480, kN - d);
    KOTONE_EXPECT_NEAR(v_pre, 0.0, 0.2);
    KOTONE_EXPECT_NEAR(v_post, 0.0, 0.2);
}

void test_breath_ceiling_tracks_input_gain_and_amount() {
    // A learned ceiling of -40 dBFS at the profiler input must follow input gain: with +12 dB
    // input gain the detector threshold is -28 dBFS.
    const VoiceCleanerConfig cfg =
        make_voice_cleaner_config(DeclickParams{}, breath_on(-40.0f, 0.0f), 12.0, 1.0, 1.0, kFs);
    KOTONE_EXPECT_NEAR(10.0 * std::log10(cfg.breath_ceiling_pow), -28.0, 1e-6);

    const VoiceCleanerConfig off =
        make_voice_cleaner_config(declick_on(), breath_on(-40.0f, 0.0f), 0.0, 0.0, 0.0, kFs);
    KOTONE_EXPECT_FALSE(off.breath_enabled);
    KOTONE_EXPECT_FALSE(off.declick_enabled);

    // Engine: UI amount sliders scale max reduction (100% -> full, 0% -> disabled)
    VoiceProfile p = make_default_neutral_profile();
    p.dsp_params.declick = declick_on(12.0f);
    p.dsp_params.breath = breath_on(-40.0f, 0.0f, 10.0f);
    UiOverrides ov = p.ui_overrides;
    ov.breath_amount_percent = 50.0f;
    ov.declick_amount_percent = 0.0f;
    const DspConfigSnapshot snap = build_dsp_snapshot(p, ov, kFs);
    KOTONE_EXPECT_NEAR(snap.effective_params.breath.max_reduction_db, 5.0, 1e-4);
    KOTONE_EXPECT_NEAR(linear_to_db(snap.cleaner.breath_floor_gain), -5.0, 1e-4);
    KOTONE_EXPECT_FALSE(snap.cleaner.declick_enabled);
    KOTONE_EXPECT_TRUE(snap.cleaner.breath_enabled);
}

void test_profile_json_compat_and_roundtrip() {
    // Schema 1.0.0 profiles without the new sections keep the Voice Cleaner disabled.
    const std::string legacy = "{ \"schema_version\": \"1.0.0\", \"profile_name\": \"旧\", "
                               "\"dsp_params\": { \"input_gain_db\": 0.0 } }";
    const ProfileLoadResult old_res = parse_profile_json(legacy, "fallback", kFs);
    KOTONE_EXPECT_TRUE(old_res.error == ErrorCode::Ok);
    KOTONE_EXPECT_FALSE(old_res.profile.dsp_params.declick.enabled);
    KOTONE_EXPECT_FALSE(old_res.profile.dsp_params.breath.enabled);
    KOTONE_EXPECT_NEAR(old_res.profile.ui_overrides.breath_amount_percent, 100.0, 1e-6);

    VoiceProfile p = make_default_neutral_profile();
    p.profile_name = "ブレス対策";
    p.dsp_params.declick = declick_on(10.0f);
    p.dsp_params.declick.sensitivity_db = 14.0f;
    p.dsp_params.breath = breath_on(-43.5f, -2.5f, 7.0f);
    p.dsp_params.breath.hangover_ms = 120.0f;
    p.ui_overrides.breath_amount_percent = 60.0f;
    p.ui_overrides.declick_amount_percent = 80.0f;
    const std::string json = serialize_profile_json(p);
    const ProfileLoadResult res = parse_profile_json(json, "fallback", kFs);
    KOTONE_EXPECT_TRUE(res.error == ErrorCode::Ok);
    const VoiceProfile& q = res.profile;
    KOTONE_EXPECT_EQ(q.schema_version, std::string("1.1.0"));
    KOTONE_EXPECT_TRUE(q.dsp_params.declick.enabled);
    KOTONE_EXPECT_NEAR(q.dsp_params.declick.sensitivity_db, 14.0, 1e-4);
    KOTONE_EXPECT_NEAR(q.dsp_params.declick.max_reduction_db, 10.0, 1e-4);
    KOTONE_EXPECT_TRUE(q.dsp_params.breath.enabled);
    KOTONE_EXPECT_NEAR(q.dsp_params.breath.level_ceiling_dbfs, -43.5, 1e-4);
    KOTONE_EXPECT_NEAR(q.dsp_params.breath.tilt_threshold_db, -2.5, 1e-4);
    KOTONE_EXPECT_NEAR(q.dsp_params.breath.hangover_ms, 120.0, 1e-4);
    KOTONE_EXPECT_NEAR(q.ui_overrides.breath_amount_percent, 60.0, 1e-4);
    KOTONE_EXPECT_NEAR(q.ui_overrides.declick_amount_percent, 80.0, 1e-4);

    // Out-of-range values are clamped
    const std::string wild =
        "{ \"schema_version\": \"1.1.0\", \"profile_name\": \"x\", \"dsp_params\": {"
        " \"declick\": { \"enabled\": true, \"sensitivity_db\": 99, \"max_reduction_db\": -5,"
        " \"crossover_hz\": 50 },"
        " \"breath\": { \"enabled\": true, \"level_ceiling_dbfs\": 10, \"tilt_threshold_db\": -90,"
        " \"max_reduction_db\": 80, \"release_ms\": 0 } },"
        " \"ui_overrides\": { \"breath_amount_percent\": 250 } }";
    const ProfileLoadResult w = parse_profile_json(wild, "fallback", kFs);
    KOTONE_EXPECT_TRUE(w.error == ErrorCode::Ok);
    KOTONE_EXPECT_NEAR(w.profile.dsp_params.declick.sensitivity_db, 30.0, 1e-4);
    KOTONE_EXPECT_NEAR(w.profile.dsp_params.declick.max_reduction_db, 0.0, 1e-4);
    KOTONE_EXPECT_NEAR(w.profile.dsp_params.declick.crossover_hz, 1000.0, 1e-4);
    KOTONE_EXPECT_NEAR(w.profile.dsp_params.breath.level_ceiling_dbfs, -12.0, 1e-4);
    KOTONE_EXPECT_NEAR(w.profile.dsp_params.breath.tilt_threshold_db, -12.0, 1e-4);
    KOTONE_EXPECT_NEAR(w.profile.dsp_params.breath.max_reduction_db, 24.0, 1e-4);
    KOTONE_EXPECT_NEAR(w.profile.dsp_params.breath.release_ms, 0.5, 1e-4);
    KOTONE_EXPECT_NEAR(w.profile.ui_overrides.breath_amount_percent, 100.0, 1e-4);
}

void test_engine_output_finite_with_cleaner_at_all_rates() {
    VoiceProfile p = make_default_neutral_profile();
    p.dsp_params.declick = declick_on();
    p.dsp_params.breath = breath_on(-40.0f, 0.0f);
    for (uint32_t fs : {8000u, 44100u, 48000u, 96000u, 192000u}) {
        auto engine = std::make_unique<DspEngine>(fs);
        KOTONE_EXPECT_TRUE(engine->apply_profile(p));
        Lcg rng(fs);
        std::vector<float> l(fs / 2);
        std::vector<float> r(fs / 2);
        for (size_t i = 0; i < l.size(); ++i) {
            const double v = (i % 4000 == 0) ? 0.9 : 0.05 * rng.next();
            l[i] = static_cast<float>(v);
            r[i] = static_cast<float>(-v);
        }
        float* chans[2] = {l.data(), r.data()};
        engine->process(chans, 2, static_cast<uint32_t>(l.size()));
        bool finite = true;
        for (size_t i = 0; i < l.size(); ++i) {
            finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
        }
        KOTONE_EXPECT_TRUE(finite);
        KOTONE_EXPECT_EQ(engine->non_finite_count(), static_cast<uint64_t>(0));
    }
}

} // namespace

void run_voice_cleaner_tests(int& passed, int& failed) {
    run_case("cleaner_fixed_lookahead_and_transparency", test_fixed_lookahead_and_transparency,
             passed, failed);
    run_case("cleaner_lip_click_removed_in_high_band_only",
             test_lip_click_removed_in_high_band_only, passed, failed);
    run_case("cleaner_no_false_clicks_on_speech_onsets_and_noise",
             test_no_false_clicks_on_speech_onsets_and_noise, passed, failed);
    run_case("cleaner_breath_suppressed_voice_preserved", test_breath_suppressed_voice_preserved,
             passed, failed);
    run_case("cleaner_breath_ceiling_tracks_input_gain_and_amount",
             test_breath_ceiling_tracks_input_gain_and_amount, passed, failed);
    run_case("cleaner_profile_json_compat_and_roundtrip", test_profile_json_compat_and_roundtrip,
             passed, failed);
    run_case("cleaner_engine_output_finite_with_cleaner_at_all_rates",
             test_engine_output_finite_with_cleaner_at_all_rates, passed, failed);
}

} // namespace kotone::test
