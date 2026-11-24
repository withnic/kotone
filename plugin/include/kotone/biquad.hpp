#pragma once

#include "kotone/dsp_types.hpp"

#include <array>
#include <cmath>

namespace kotone {

struct BiquadCoeffs {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    static BiquadCoeffs identity() noexcept;
    static BiquadCoeffs highpass(double sample_rate, double cutoff_hz, double q) noexcept;
    static BiquadCoeffs bandpass_constant_peak(double sample_rate, double freq_hz,
                                               double q) noexcept;
    static BiquadCoeffs peaking(double sample_rate, double freq_hz, double gain_db,
                                double q) noexcept;
    static BiquadCoeffs low_shelf(double sample_rate, double freq_hz, double gain_db,
                                  double q) noexcept;
    static BiquadCoeffs high_shelf(double sample_rate, double freq_hz, double gain_db,
                                   double q) noexcept;
    static BiquadCoeffs from_eq_band(double sample_rate, const EqBandParams& band) noexcept;
};

// Computes 1-stage (12 dB/oct) or 2-stage (24 dB/oct) Butterworth HPF coefficients.
std::array<BiquadCoeffs, 2> compute_butterworth_hpf(double sample_rate,
                                                    const HpfParams& params) noexcept;

// Direct Form II Transposed biquad state in double precision with Flush-to-Zero denormal
// protection.
class BiquadState {
public:
    void reset() noexcept {
        z1_ = 0.0;
        z2_ = 0.0;
    }

    double process(double x, const BiquadCoeffs& c) noexcept {
        if (!std::isfinite(x)) {
            reset();
            return 0.0;
        }
        const double y = c.b0 * x + z1_;
        const double next_z1 = c.b1 * x - c.a1 * y + z2_;
        const double next_z2 = c.b2 * x - c.a2 * y;

        if (!std::isfinite(y) || !std::isfinite(next_z1) || !std::isfinite(next_z2)) {
            reset();
            return 0.0;
        }

        z1_ = flush_denormal(next_z1);
        z2_ = flush_denormal(next_z2);
        return y;
    }

private:
    double z1_ = 0.0;
    double z2_ = 0.0;
};

} // namespace kotone
