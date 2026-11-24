#include "kotone/biquad.hpp"

#include <algorithm>
#include <cmath>

namespace kotone {
namespace {

BiquadCoeffs normalize_coeffs(double b0, double b1, double b2, double a0, double a1,
                              double a2) noexcept {
    if (!std::isfinite(a0) || std::abs(a0) < 1e-12) {
        return BiquadCoeffs::identity();
    }
    const double inv_a0 = 1.0 / a0;
    BiquadCoeffs c;
    c.b0 = b0 * inv_a0;
    c.b1 = b1 * inv_a0;
    c.b2 = b2 * inv_a0;
    c.a1 = a1 * inv_a0;
    c.a2 = a2 * inv_a0;

    if (!std::isfinite(c.b0) || !std::isfinite(c.b1) || !std::isfinite(c.b2) ||
        !std::isfinite(c.a1) || !std::isfinite(c.a2)) {
        return BiquadCoeffs::identity();
    }
    return c;
}

double sanitize_fs(double sample_rate) noexcept {
    if (!std::isfinite(sample_rate) || sample_rate < static_cast<double>(kMinSampleRate) ||
        sample_rate > static_cast<double>(kMaxSampleRate)) {
        return static_cast<double>(kDefaultSampleRate);
    }
    return sample_rate;
}

} // namespace

BiquadCoeffs BiquadCoeffs::identity() noexcept {
    return BiquadCoeffs{1.0, 0.0, 0.0, 0.0, 0.0};
}

BiquadCoeffs BiquadCoeffs::highpass(double sample_rate, double cutoff_hz, double q) noexcept {
    const double fs = sanitize_fs(sample_rate);
    const double fc = std::clamp(std::isfinite(cutoff_hz) ? cutoff_hz : 80.0, 20.0, fs * 0.45);
    const double q_safe = std::clamp(std::isfinite(q) ? q : 0.7071067811865476, 0.1, 18.0);

    const double w0 = 2.0 * kPi * fc / fs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * q_safe);

    const double b0 = (1.0 + cw) * 0.5;
    const double b1 = -(1.0 + cw);
    const double b2 = (1.0 + cw) * 0.5;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cw;
    const double a2 = 1.0 - alpha;

    return normalize_coeffs(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs BiquadCoeffs::bandpass_constant_peak(double sample_rate, double freq_hz,
                                                  double q) noexcept {
    const double fs = sanitize_fs(sample_rate);
    const double fc = std::clamp(std::isfinite(freq_hz) ? freq_hz : 6500.0, 20.0, fs * 0.48);
    const double q_safe = std::clamp(std::isfinite(q) ? q : 2.5, 0.1, 18.0);

    const double w0 = 2.0 * kPi * fc / fs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * q_safe);

    const double b0 = alpha;
    const double b1 = 0.0;
    const double b2 = -alpha;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cw;
    const double a2 = 1.0 - alpha;

    return normalize_coeffs(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs BiquadCoeffs::peaking(double sample_rate, double freq_hz, double gain_db,
                                   double q) noexcept {
    if (!std::isfinite(gain_db) || std::abs(gain_db) < 1e-6) {
        return identity();
    }
    const double fs = sanitize_fs(sample_rate);
    const double fc = std::clamp(std::isfinite(freq_hz) ? freq_hz : 1000.0, 20.0, fs * 0.48);
    const double g = std::clamp(gain_db, -18.0, 18.0);
    const double q_safe = std::clamp(std::isfinite(q) ? q : 1.0, 0.1, 18.0);

    const double a_amp = std::pow(10.0, g / 40.0);
    const double w0 = 2.0 * kPi * fc / fs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * q_safe);

    const double b0 = 1.0 + alpha * a_amp;
    const double b1 = -2.0 * cw;
    const double b2 = 1.0 - alpha * a_amp;
    const double a0 = 1.0 + alpha / a_amp;
    const double a1 = -2.0 * cw;
    const double a2 = 1.0 - alpha / a_amp;

    return normalize_coeffs(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs BiquadCoeffs::low_shelf(double sample_rate, double freq_hz, double gain_db,
                                     double q) noexcept {
    if (!std::isfinite(gain_db) || std::abs(gain_db) < 1e-6) {
        return identity();
    }
    const double fs = sanitize_fs(sample_rate);
    const double fc = std::clamp(std::isfinite(freq_hz) ? freq_hz : 120.0, 20.0, fs * 0.48);
    const double g = std::clamp(gain_db, -18.0, 18.0);
    const double q_safe = std::clamp(std::isfinite(q) ? q : 0.7071067811865476, 0.1, 18.0);

    const double a_amp = std::pow(10.0, g / 40.0);
    const double w0 = 2.0 * kPi * fc / fs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * q_safe);
    const double two_sqrt_a_alpha = 2.0 * std::sqrt(a_amp) * alpha;

    const double b0 = a_amp * ((a_amp + 1.0) - (a_amp - 1.0) * cw + two_sqrt_a_alpha);
    const double b1 = 2.0 * a_amp * ((a_amp - 1.0) - (a_amp + 1.0) * cw);
    const double b2 = a_amp * ((a_amp + 1.0) - (a_amp - 1.0) * cw - two_sqrt_a_alpha);
    const double a0 = (a_amp + 1.0) + (a_amp - 1.0) * cw + two_sqrt_a_alpha;
    const double a1 = -2.0 * ((a_amp - 1.0) + (a_amp + 1.0) * cw);
    const double a2 = (a_amp + 1.0) + (a_amp - 1.0) * cw - two_sqrt_a_alpha;

    return normalize_coeffs(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs BiquadCoeffs::high_shelf(double sample_rate, double freq_hz, double gain_db,
                                      double q) noexcept {
    if (!std::isfinite(gain_db) || std::abs(gain_db) < 1e-6) {
        return identity();
    }
    const double fs = sanitize_fs(sample_rate);
    const double fc = std::clamp(std::isfinite(freq_hz) ? freq_hz : 10000.0, 20.0, fs * 0.48);
    const double g = std::clamp(gain_db, -18.0, 18.0);
    const double q_safe = std::clamp(std::isfinite(q) ? q : 0.7071067811865476, 0.1, 18.0);

    const double a_amp = std::pow(10.0, g / 40.0);
    const double w0 = 2.0 * kPi * fc / fs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * q_safe);
    const double two_sqrt_a_alpha = 2.0 * std::sqrt(a_amp) * alpha;

    const double b0 = a_amp * ((a_amp + 1.0) + (a_amp - 1.0) * cw + two_sqrt_a_alpha);
    const double b1 = -2.0 * a_amp * ((a_amp - 1.0) + (a_amp + 1.0) * cw);
    const double b2 = a_amp * ((a_amp + 1.0) + (a_amp - 1.0) * cw - two_sqrt_a_alpha);
    const double a0 = (a_amp + 1.0) - (a_amp - 1.0) * cw + two_sqrt_a_alpha;
    const double a1 = 2.0 * ((a_amp - 1.0) - (a_amp + 1.0) * cw);
    const double a2 = (a_amp + 1.0) - (a_amp - 1.0) * cw - two_sqrt_a_alpha;

    return normalize_coeffs(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs BiquadCoeffs::from_eq_band(double sample_rate, const EqBandParams& band) noexcept {
    if (!band.enabled) {
        return identity();
    }
    switch (band.type) {
    case EqBandType::Peaking:
        return peaking(sample_rate, band.freq_hz, band.gain_db, band.q);
    case EqBandType::LowShelf:
        return low_shelf(sample_rate, band.freq_hz, band.gain_db, band.q);
    case EqBandType::HighShelf:
        return high_shelf(sample_rate, band.freq_hz, band.gain_db, band.q);
    }
    return identity();
}

std::array<BiquadCoeffs, 2> compute_butterworth_hpf(double sample_rate,
                                                    const HpfParams& params) noexcept {
    std::array<BiquadCoeffs, 2> stages = {BiquadCoeffs::identity(), BiquadCoeffs::identity()};
    if (!params.enabled) {
        return stages;
    }
    if (params.slope_db_per_oct == 12) {
        constexpr double kButterworth2Q = 0.7071067811865476; // 1 / sqrt(2)
        stages[0] = BiquadCoeffs::highpass(sample_rate, params.cutoff_hz, kButterworth2Q);
        stages[1] = BiquadCoeffs::identity();
    } else {
        // 4th-order Butterworth (24 dB/oct) cascade Q factors
        constexpr double kButterworth4Q1 = 0.5411961001461970; // 1 / (2 * cos(pi/8))
        constexpr double kButterworth4Q2 = 1.3065629648763766; // 1 / (2 * cos(3*pi/8))
        stages[0] = BiquadCoeffs::highpass(sample_rate, params.cutoff_hz, kButterworth4Q1);
        stages[1] = BiquadCoeffs::highpass(sample_rate, params.cutoff_hz, kButterworth4Q2);
    }
    return stages;
}

} // namespace kotone
