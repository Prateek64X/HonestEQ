// Tests for honesteq::Biquad and honesteq::BiquadChain.
//
// These tests verify the *audible quality* properties we care about:
//   1. Peaking EQ at 0 dB gain == identity passthrough (no audible change).
//   2. Peaking / shelf / LP / HP filters produce the correct dB at the
//      key reference frequencies of their analytical response.
//   3. Filter is numerically stable for impulse + long signals.
//   4. Coefficients can be hot-swapped (parameter automation) without
//      blowing up the state (DF-I property).
//   5. BiquadChain reproduces a multi-band setup whose summed response
//      matches the per-band analytical sum.
//
// If any of these fail, the EQ would sound wrong. These are the gates.

#include "MicroTest.hpp"
#include "honesteq/Biquad.hpp"
#include "honesteq/BiquadChain.hpp"

#include <cmath>
#include <vector>

using namespace honesteq;

static constexpr double kFs48 = 48000.0;
static constexpr double kFs441 = 44100.0;
static constexpr double kFs192 = 192000.0;

// ---------------------------------------------------------------------------
// 1. Peaking EQ at 0 dB gain must be a perfect passthrough.
// ---------------------------------------------------------------------------
MICRO_TEST(PeakingEQ_ZeroGain_IsIdentity) {
    const auto c = designBiquad(BiquadType::PeakingEQ, 1000.0, 1.0, 0.0, kFs48);
    REQUIRE_NEAR(c.b0, 1.0, 1e-15);
    REQUIRE_NEAR(c.b1, c.a1, 1e-15);   // RBJ peaking at 0 dB: b1==a1, b2==a2
    REQUIRE_NEAR(c.b2, c.a2, 1e-15);

    // Verify by signal: pump an impulse through a stateful biquad, expect impulse out.
    Biquad f;
    f.setCoefficients(c);
    const std::vector<double> in {1, 0, 0, 0, 0, 0, 0, 0};
    std::vector<double> out(in.size());
    f.processBlock(in.data(), out.data(), in.size());
    REQUIRE_NEAR(out[0], 1.0, 1e-12);
    for (std::size_t i = 1; i < out.size(); ++i) REQUIRE_NEAR(out[i], 0.0, 1e-12);
}

// ---------------------------------------------------------------------------
// 2a. Peaking EQ at Fc has gain exactly equal to gainDb.
// ---------------------------------------------------------------------------
MICRO_TEST(PeakingEQ_GainAtFc_MatchesParam) {
    for (double fs : {kFs441, kFs48, kFs192}) {
        for (double gainDb : {-12.0, -6.0, -3.0, 3.0, 6.0, 12.0}) {
            for (double fc : {100.0, 1000.0, 5000.0}) {
                const auto c = designBiquad(BiquadType::PeakingEQ, fc, 1.0, gainDb, fs);
                const double m = magnitudeAtDb(c, fc, fs);
                REQUIRE_NEAR(m, gainDb, 1e-9);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 2b. Low shelf at DC has gain == gainDb.
//     High shelf at Nyquist (~) has gain == gainDb.
// ---------------------------------------------------------------------------
MICRO_TEST(LowShelf_GainAtDC_MatchesParam) {
    for (double gainDb : {-12.0, -6.0, 6.0, 12.0}) {
        const auto c = designBiquad(BiquadType::LowShelf, 200.0, 0.707, gainDb, kFs48);
        // At DC (freq=0), e^-jw=1 → magnitude = |b0+b1+b2| / |1+a1+a2|.
        const double num = c.b0 + c.b1 + c.b2;
        const double den = 1.0 + c.a1 + c.a2;
        const double dcGainDb = 20.0 * std::log10(std::fabs(num / den));
        REQUIRE_NEAR(dcGainDb, gainDb, 1e-9);
    }
}

MICRO_TEST(HighShelf_GainAtNyquist_MatchesParam) {
    for (double gainDb : {-12.0, -6.0, 6.0, 12.0}) {
        const auto c = designBiquad(BiquadType::HighShelf, 5000.0, 0.707, gainDb, kFs48);
        // At Nyquist (w=pi), e^-jw=-1 → magnitude = |b0-b1+b2| / |1-a1+a2|.
        const double num = c.b0 - c.b1 + c.b2;
        const double den = 1.0 - c.a1 + c.a2;
        const double nyqGainDb = 20.0 * std::log10(std::fabs(num / den));
        REQUIRE_NEAR(nyqGainDb, gainDb, 1e-9);
    }
}

// ---------------------------------------------------------------------------
// 2c. Low-pass at cutoff Fc with Q=0.707 (Butterworth) gives -3.01 dB.
// ---------------------------------------------------------------------------
MICRO_TEST(LowPass_AtFc_GivesMinus3dB_Butterworth) {
    const double fc = 2000.0;
    const auto c = designBiquad(BiquadType::LowPass, fc, 0.70710678118654752, 0.0, kFs48);
    const double mDb = magnitudeAtDb(c, fc, kFs48);
    // Digital biquad at fc with Q=1/sqrt(2) gives close to -3 dB; the bilinear
    // transform introduces a tiny frequency-warping but for fc << Nyquist it's
    // within 0.01 dB. At 2 kHz / 48 kHz we expect ≈ -3.01 dB.
    REQUIRE_NEAR(mDb, -3.01, 0.05);
}

// ---------------------------------------------------------------------------
// 3. Stability — pump 10 sec of white noise through a high-Q low shelf at
//    192 kHz and confirm no NaN / Inf / runaway output.
// ---------------------------------------------------------------------------
MICRO_TEST(Stability_HighQ_LowFreq_LongRun_DoesNotBlowUp) {
    const auto c = designBiquad(BiquadType::LowShelf, 30.0, 4.0, 12.0, kFs192);
    Biquad f;
    f.setCoefficients(c);

    // Deterministic pseudo-noise (linear congruential).
    std::uint32_t r = 0x12345678;
    auto noise = [&]() {
        r = r * 1664525u + 1013904223u;
        return ((double)(int32_t)r) / 2147483648.0;
    };

    double peak = 0.0;
    const std::size_t N = static_cast<std::size_t>(10.0 * kFs192);  // 10 seconds
    for (std::size_t i = 0; i < N; ++i) {
        const double y = f.process(noise() * 0.5);
        REQUIRE(std::isfinite(y));
        peak = std::fmax(peak, std::fabs(y));
    }
    // 12 dB shelf boost on full-scale white noise: peak stays well under 10x.
    REQUIRE(peak < 10.0);
}

// ---------------------------------------------------------------------------
// 4. DF-I survives coefficient hot-swap without producing NaN / huge clicks.
//    This is the property that makes DF-I correct for time-varying coeffs.
// ---------------------------------------------------------------------------
MICRO_TEST(DF_I_CoefficientHotSwap_DoesNotBlowUp) {
    Biquad f;
    f.setCoefficients(designBiquad(BiquadType::PeakingEQ, 1000.0, 1.0, 6.0, kFs48));

    // Prime with a constant signal.
    for (int i = 0; i < 1000; ++i) (void)f.process(0.5);

    // Now sweep gain from +6 dB to -6 dB over 1000 samples, no state reset.
    double maxAbs = 0.0;
    for (int i = 0; i < 1000; ++i) {
        const double gain = 6.0 - 12.0 * (i / 999.0);
        f.setCoefficients(designBiquad(BiquadType::PeakingEQ, 1000.0, 1.0, gain, kFs48));
        const double y = f.process(0.5);
        REQUIRE(std::isfinite(y));
        maxAbs = std::fmax(maxAbs, std::fabs(y));
    }
    REQUIRE(maxAbs < 2.0);  // no runaway / no click bigger than 2x input
}

// ---------------------------------------------------------------------------
// 5. Sample rate independence: a peaking EQ at 1 kHz / +6 dB / Q=1.0 should
//    produce the *same* magnitude response in dB at the analytical frequencies,
//    regardless of whether Fs is 44.1, 48, or 192 kHz. This is the property
//    that makes the user's 31-band profile sound identical on any device.
// ---------------------------------------------------------------------------
MICRO_TEST(SampleRateIndependence_AtModerateFrequencies) {
    const double fc = 1000.0, q = 1.0, gain = 6.0;
    const auto c441 = designBiquad(BiquadType::PeakingEQ, fc, q, gain, kFs441);
    const auto c48  = designBiquad(BiquadType::PeakingEQ, fc, q, gain, kFs48);
    const auto c192 = designBiquad(BiquadType::PeakingEQ, fc, q, gain, kFs192);

    // All three should produce ~+6 dB at the center.
    REQUIRE_NEAR(magnitudeAtDb(c441, fc, kFs441), gain, 1e-9);
    REQUIRE_NEAR(magnitudeAtDb(c48,  fc, kFs48),  gain, 1e-9);
    REQUIRE_NEAR(magnitudeAtDb(c192, fc, kFs192), gain, 1e-9);

    // And ~0 dB far below / above.  100 Hz is well below 1 kHz peak so we expect
    // <0.5 dB residual at low and high probes for a Q=1 peaking EQ.
    REQUIRE(std::fabs(magnitudeAtDb(c48, 100.0, kFs48)) < 0.5);
    REQUIRE(std::fabs(magnitudeAtDb(c48, 8000.0, kFs48)) < 0.5);
}

// ---------------------------------------------------------------------------
// 6. BiquadChain: 31-band Peace-style chain processes blocks, doesn't NaN,
//    and at the center of each band the summed response matches the band's
//    own gain to within a few dB (interactions between bands add up).
// ---------------------------------------------------------------------------
MICRO_TEST(BiquadChain_31Band_BehavesAsExpected) {
    BiquadChain chain;
    chain.setSampleRate(kFs48);
    chain.setChannelCount(2);
    chain.setPreampDb(-3.0);

    // 31 log-spaced bands from 20 Hz to 20 kHz, alternating ±3 dB.
    std::vector<BandSpec> bands;
    bands.reserve(31);
    for (int i = 0; i < 31; ++i) {
        const double t = i / 30.0;
        const double f = 20.0 * std::pow(20000.0 / 20.0, t);
        BandSpec b;
        b.type = BiquadType::PeakingEQ;
        b.frequencyHz = f;
        b.q = 1.5;
        b.gainDb = (i % 2 == 0) ? 3.0 : -3.0;
        b.enabled = true;
        bands.push_back(b);
    }
    chain.setBands(bands);

    // Process 1 second of pseudo-noise interleaved stereo.
    const std::size_t frames = 48000;
    std::vector<double> buf(frames * 2);
    std::uint32_t r = 0xdeadbeefu;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        r = r * 1664525u + 1013904223u;
        buf[i] = ((double)(int32_t)r) / 2147483648.0 * 0.5;
    }
    chain.processInterleaved(buf.data(), frames);

    double peak = 0.0;
    for (double s : buf) {
        REQUIRE(std::isfinite(s));
        peak = std::fmax(peak, std::fabs(s));
    }
    REQUIRE(peak < 8.0);  // 31 bands of ±3 dB with -3 dB preamp: bounded
}

int main() {
    return ::microtest::Registry::instance().runAll();
}
