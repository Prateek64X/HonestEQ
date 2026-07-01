// HonestEQ — Biquad.hpp
//
// Double-precision RBJ Audio EQ Cookbook biquad in Direct Form I.
// Matches Equalizer APO's filter math byte-for-byte so the same .txt config
// produces numerically identical output to Peace on Windows.
//
// Reference: https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
//
// Design choices and the reasoning behind them are documented in
// docs/technical-plan.md sections 1 and 4.
//
// - double precision throughout (state + coefficients) — round-off noise in
//   direct-form biquads scales as 1/(1 - |pole|^2); at low frequencies and
//   high Q the float32 LSB noise becomes audible. Equalizer APO uses double.
//
// - Direct Form I — safe for time-varying coefficients (no internal overflow,
//   no stale partial sums when coefficients change). TDF-II is better only
//   for static coefficients, which we don't have because the user moves
//   sliders.
//
// - Realtime safe: no allocations, no exceptions, no virtuals, no syscalls.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace honesteq {

enum class BiquadType : std::uint8_t {
    PeakingEQ,    // PK  — RBJ peaking EQ with gain/Q
    LowShelf,     // LS  — RBJ low shelf (Q parameter, default 0.707)
    HighShelf,    // HS  — RBJ high shelf
    LowPass,      // LP  — RBJ low-pass (gain ignored)
    HighPass,     // HP  — RBJ high-pass (gain ignored)
    Notch,        // NO  — RBJ notch (gain ignored)
    BandPass,     // BP  — RBJ band-pass, constant-skirt (gain ignored)
    AllPass,      // AP  — RBJ all-pass (phase shaping only)
};

struct BiquadCoeffs {
    // Normalised (a0 == 1) — store b0, b1, b2, a1, a2.
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    // Returns identity-passthrough coefficients (used as "bypass" / "off" state).
    static constexpr BiquadCoeffs identity() noexcept {
        return BiquadCoeffs{1.0, 0.0, 0.0, 0.0, 0.0};
    }
};

// Compute RBJ cookbook coefficients for a given filter type, center/corner
// frequency Fc (Hz), Q, gain in dB, and sample rate Fs (Hz).
//
// Gain is honoured only by PeakingEQ, LowShelf, HighShelf — ignored by the
// rest (matching Equalizer APO's syntax: LP/HP/NO/BP/AP have no Gain field).
inline BiquadCoeffs designBiquad(BiquadType type,
                                 double fc,
                                 double q,
                                 double gainDb,
                                 double fs) noexcept {
    // Guard against degenerate inputs. Out-of-range freq → identity (bypass).
    if (!(fs > 0.0) || !(fc > 0.0) || !(q > 0.0) || fc >= fs * 0.5) {
        return BiquadCoeffs::identity();
    }

    constexpr double kPi = 3.14159265358979323846;
    const double omega = 2.0 * kPi * fc / fs;
    const double sinW = std::sin(omega);
    const double cosW = std::cos(omega);
    const double alpha = sinW / (2.0 * q);
    const double A = std::pow(10.0, gainDb / 40.0);  // amplitude (sqrt of linear gain)

    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (type) {
        case BiquadType::PeakingEQ: {
            // RBJ Cookbook peakingEQ
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cosW;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cosW;
            a2 = 1.0 - alpha / A;
            break;
        }
        case BiquadType::LowShelf: {
            // RBJ Cookbook lowShelf (Q form)
            const double sqrtA = std::sqrt(A);
            const double twoSqrtA_alpha = 2.0 * sqrtA * alpha;
            b0 =        A * ((A + 1.0) - (A - 1.0) * cosW + twoSqrtA_alpha);
            b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosW);
            b2 =        A * ((A + 1.0) - (A - 1.0) * cosW - twoSqrtA_alpha);
            a0 =             (A + 1.0) + (A - 1.0) * cosW + twoSqrtA_alpha;
            a1 = -2.0      * ((A - 1.0) + (A + 1.0) * cosW);
            a2 =             (A + 1.0) + (A - 1.0) * cosW - twoSqrtA_alpha;
            break;
        }
        case BiquadType::HighShelf: {
            // RBJ Cookbook highShelf (Q form)
            const double sqrtA = std::sqrt(A);
            const double twoSqrtA_alpha = 2.0 * sqrtA * alpha;
            b0 =        A * ((A + 1.0) + (A - 1.0) * cosW + twoSqrtA_alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
            b2 =        A * ((A + 1.0) + (A - 1.0) * cosW - twoSqrtA_alpha);
            a0 =             (A + 1.0) - (A - 1.0) * cosW + twoSqrtA_alpha;
            a1 =  2.0      * ((A - 1.0) - (A + 1.0) * cosW);
            a2 =             (A + 1.0) - (A - 1.0) * cosW - twoSqrtA_alpha;
            break;
        }
        case BiquadType::LowPass: {
            b0 = (1.0 - cosW) * 0.5;
            b1 =  1.0 - cosW;
            b2 = (1.0 - cosW) * 0.5;
            a0 =  1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 =  1.0 - alpha;
            break;
        }
        case BiquadType::HighPass: {
            b0 =  (1.0 + cosW) * 0.5;
            b1 = -(1.0 + cosW);
            b2 =  (1.0 + cosW) * 0.5;
            a0 =   1.0 + alpha;
            a1 =  -2.0 * cosW;
            a2 =   1.0 - alpha;
            break;
        }
        case BiquadType::Notch: {
            b0 =  1.0;
            b1 = -2.0 * cosW;
            b2 =  1.0;
            a0 =  1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 =  1.0 - alpha;
            break;
        }
        case BiquadType::BandPass: {
            // RBJ constant-skirt-gain, peak gain = Q
            b0 =  q * alpha;       // sinW / 2
            b1 =  0.0;
            b2 = -q * alpha;
            a0 =  1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 =  1.0 - alpha;
            break;
        }
        case BiquadType::AllPass: {
            b0 =  1.0 - alpha;
            b1 = -2.0 * cosW;
            b2 =  1.0 + alpha;
            a0 =  1.0 + alpha;
            a1 = -2.0 * cosW;
            a2 =  1.0 - alpha;
            break;
        }
    }

    // Normalise so a0 == 1.
    const double inv_a0 = 1.0 / a0;
    return BiquadCoeffs{
        b0 * inv_a0,
        b1 * inv_a0,
        b2 * inv_a0,
        a1 * inv_a0,
        a2 * inv_a0,
    };
}

// Stateful single-channel biquad in Direct Form I.
//
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
//
// Per-channel state; for stereo, hold two of these. Coefficients can be
// hot-swapped at any time without risk of internal overflow (DF-I property).
class Biquad {
public:
    Biquad() = default;

    void setCoefficients(const BiquadCoeffs& c) noexcept { coeffs_ = c; }
    const BiquadCoeffs& coefficients() const noexcept { return coeffs_; }

    void reset() noexcept {
        x1_ = x2_ = y1_ = y2_ = 0.0;
    }

    // Single sample tick. Inlined and hot.
    inline double process(double x) noexcept {
        const double y =
            coeffs_.b0 * x +
            coeffs_.b1 * x1_ +
            coeffs_.b2 * x2_ -
            coeffs_.a1 * y1_ -
            coeffs_.a2 * y2_;
        x2_ = x1_;
        x1_ = x;
        y2_ = y1_;
        y1_ = y;
        return y;
    }

    // Block process (in-place is allowed: in == out).
    inline void processBlock(const double* in, double* out, std::size_t n) noexcept {
        // Load state into locals — lets the compiler keep them in registers.
        double x1 = x1_, x2 = x2_, y1 = y1_, y2 = y2_;
        const double b0 = coeffs_.b0, b1 = coeffs_.b1, b2 = coeffs_.b2;
        const double a1 = coeffs_.a1, a2 = coeffs_.a2;
        for (std::size_t i = 0; i < n; ++i) {
            const double x = in[i];
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            out[i] = y;
        }
        x1_ = x1; x2_ = x2; y1_ = y1; y2_ = y2;
    }

private:
    BiquadCoeffs coeffs_ = BiquadCoeffs::identity();
    double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

// Analytical magnitude response of a biquad at a given frequency.
// Useful for unit tests and for drawing the EQ curve in the UI.
// Returns linear magnitude (not dB).
inline double magnitudeAt(const BiquadCoeffs& c, double freqHz, double fs) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    const double w = 2.0 * kPi * freqHz / fs;
    const double cw = std::cos(w);
    const double c2w = std::cos(2.0 * w);
    const double sw = std::sin(w);
    const double s2w = std::sin(2.0 * w);

    // Numerator = b0 + b1*e^-jw + b2*e^-j2w
    const double numRe = c.b0 + c.b1 * cw + c.b2 * c2w;
    const double numIm = -(c.b1 * sw + c.b2 * s2w);
    // Denominator = 1 + a1*e^-jw + a2*e^-j2w  (a0 already normalised to 1)
    const double denRe = 1.0 + c.a1 * cw + c.a2 * c2w;
    const double denIm = -(c.a1 * sw + c.a2 * s2w);

    const double numMag2 = numRe * numRe + numIm * numIm;
    const double denMag2 = denRe * denRe + denIm * denIm;
    return std::sqrt(numMag2 / denMag2);
}

inline double magnitudeAtDb(const BiquadCoeffs& c, double freqHz, double fs) noexcept {
    const double m = magnitudeAt(c, freqHz, fs);
    return 20.0 * std::log10(m);
}

}  // namespace honesteq
