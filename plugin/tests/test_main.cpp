#include "test_framework.hpp"

#include "kotone/dsp_engine.hpp"
#include "kotone/profile_loader.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

std::vector<std::string> get_utf8_args(int argc, char** argv) {
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv != nullptr && wargc > 0) {
        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(wargc));
        for (int i = 0; i < wargc; ++i) {
            args.push_back(std::filesystem::path(wargv[i]).u8string());
        }
        LocalFree(static_cast<HLOCAL>(wargv));
        return args;
    }
#endif
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

int verify_profile_cli(const std::string& profile_path_utf8) {
    constexpr uint32_t kFs = 48000;
    const kotone::ProfileLoadResult res = kotone::load_profile_file(profile_path_utf8, kFs);
    if (!res) {
        std::cerr << "[VERIFY FAIL] " << kotone::error_code_to_string(res.error) << ": "
                  << res.error_message << "\n";
        return 1;
    }

    kotone::DspEngine engine(kFs);
    if (!engine.apply_profile(res.profile)) {
        std::cerr << "[VERIFY FAIL] Failed to apply profile snapshot to DspEngine\n";
        return 1;
    }

    // Process 1 second of stereo audio including speech-level tone and +12 dBFS shout burst
    constexpr uint32_t kTotalFrames = kFs;
    constexpr uint32_t kBlockFrames = 256;
    std::vector<float> left(kBlockFrames, 0.0f);
    std::vector<float> right(kBlockFrames, 0.0f);
    float* ptrs[2] = {left.data(), right.data()};

    const float ceiling_lin = kotone::db_to_linear_f(res.profile.ui_overrides.limiter_ceiling_dbfs);
    float max_abs = 0.0f;

    for (uint32_t offset = 0; offset < kTotalFrames; offset += kBlockFrames) {
        const uint32_t frames = std::min(kBlockFrames, kTotalFrames - offset);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t idx = offset + i;
            const double t = static_cast<double>(idx) / static_cast<double>(kFs);
            // Burst to +12 dBFS between 0.5s and 0.6s
            const double amp = (idx >= kFs / 2 && idx < (kFs * 6) / 10) ? 3.98107 : 0.15;
            const float s = static_cast<float>(amp * std::sin(2.0 * kotone::kPi * 440.0 * t));
            left[i] = s;
            right[i] = s;
        }

        engine.process(ptrs, 2, frames);

        for (uint32_t i = 0; i < frames; ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
                std::cerr << "[VERIFY FAIL] Non-finite audio sample generated during processing\n";
                return 1;
            }
            max_abs = std::max(max_abs, std::abs(left[i]));
            max_abs = std::max(max_abs, std::abs(right[i]));
        }
    }

    if (res.profile.dsp_params.limiter.enabled && max_abs > ceiling_lin + 1e-5f) {
        std::cerr << "[VERIFY FAIL] Output peak " << max_abs << " exceeded limiter ceiling "
                  << ceiling_lin << "\n";
        return 1;
    }

    std::cout << "[VERIFY PASS] profile=\"" << res.profile.profile_name << "\" max_peak=" << max_abs
              << " ceiling=" << ceiling_lin << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string> args = get_utf8_args(argc, argv);

        if (args.size() >= 2 && args[1] == "--verify-profile") {
            if (args.size() < 3) {
                std::cerr << "Usage: kotone_tests --verify-profile <profile.json>\n";
                return 2;
            }
            return verify_profile_cli(args[2]);
        }

        int passed = 0;
        int failed = 0;

        kotone::test::run_dsp_limiter_tests(passed, failed);
        kotone::test::run_dsp_eq_comp_tests(passed, failed);
        kotone::test::run_profile_loader_tests(passed, failed);
        kotone::test::run_voice_cleaner_tests(passed, failed);

        std::cout << "\nSummary: " << passed << " passed, " << failed << " failed.\n";
        return (failed == 0 && kotone::test::ctx().failures == 0) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "[FATAL] Unhandled exception in tests: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "[FATAL] Unknown unhandled exception in tests\n";
        return 1;
    }
}
