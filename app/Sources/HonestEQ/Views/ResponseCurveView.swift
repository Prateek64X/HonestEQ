// ResponseCurveView.swift
//
// Draws the cumulative frequency-response curve of the current EQ using the
// RBJ biquad magnitude formula — the same math libHonestEQDSP applies to
// audio in real time. Result: the curve on screen matches what you HEAR, not
// just where the thumbs are placed (accounts for filter overlap between
// adjacent bands, preamp offset, and dry/wet intensity scaling).
//
// Drawn behind the band sliders with a translucent fill under the curve.
// Non-interactive (`.allowsHitTesting(false)`) so slider drags always work.

import SwiftUI

struct ResponseCurveView: View {
    let bands: [Band]
    let preampDb: Double
    let intensity: Double    // 0.0 – 1.0+  (from wetPercent / 100)
    let bypassed: Bool

    /// How many frequency points to sample across the curve. 300 is plenty
    /// smooth at typical window widths; each point is ~30 ns of math.
    private static let numPoints = 300

    /// Sample rate used for evaluating the biquad magnitude. The magnitude
    /// response is nearly rate-invariant at moderate ratios, so any common
    /// rate produces a nearly-identical curve; 48 kHz matches the daemon
    /// default and keeps the math self-consistent.
    private static let sampleRate: Double = 48000

    /// Vertical range of the plotted curve, in dB.
    private static let displayRangeDb: Double = 12

    /// Must match DBGrid's thumb-padding values exactly so the curve's 0 dB
    /// baseline, ±12 dB extremes, and the DBGrid's horizontal reference
    /// lines land on the same pixels. NSSlider (controlSize .small) is not
    /// perfectly symmetric vertically; these numbers were measured against
    /// its actual thumb positions on macOS 14.
    private let thumbPaddingTop:    CGFloat = 12
    private let thumbPaddingBottom: CGFloat = 8

    var body: some View {
        Canvas { context, size in
            drawCurve(in: &context, size: size)
        }
        .allowsHitTesting(false)
    }

    private func drawCurve(in context: inout GraphicsContext, size: CGSize) {
        let enabledBands = bands.filter { $0.enabled }
        guard enabledBands.count >= 2 else { return }

        // Log-lerp frequency across screen X.
        let firstFreq = bands.first!.frequencyHz
        let lastFreq  = bands.last!.frequencyHz
        guard firstFreq > 0, lastFreq > firstFreq else { return }
        let ratio = lastFreq / firstFreq

        // Vertical usable area = frame minus asymmetric thumb padding.
        // Zero-dB baseline y lands on DBGrid's 0 dB line pixel-for-pixel.
        let usable = size.height - thumbPaddingTop - thumbPaddingBottom
        let zeroY  = thumbPaddingTop + usable / 2

        // Build line path (curve) and fill path (curve → 0 dB line closed).
        var linePath = Path()
        var fillPath = Path()
        fillPath.move(to: CGPoint(x: 0, y: zeroY))

        // Precompute preamp contribution (scaled by intensity, same as the
        // daemon does before running the biquad chain).
        let effectivePreampDb = preampDb * intensity

        for i in 0..<Self.numPoints {
            let t = Double(i) / Double(Self.numPoints - 1)
            let freq = firstFreq * pow(ratio, t)

            // Cumulative: preamp offset + sum of all band contributions.
            var dB = effectivePreampDb
            for band in enabledBands {
                dB += peakingMagnitudeDb(
                    fc: band.frequencyHz,
                    gainDb: band.gainDb * intensity,
                    q: max(0.1, band.q),
                    freq: freq
                )
            }

            let clamped = min(Self.displayRangeDb, max(-Self.displayRangeDb, dB))
            let x = CGFloat(t) * size.width
            // Map dB → y within the thumb-padded usable region:
            //   +12 dB → thumbPaddingTop        (top of slider thumb travel)
            //    0 dB → thumbPaddingTop + usable/2  (matches DBGrid's 0 line)
            //   -12 dB → thumbPaddingTop + usable  (bottom of slider thumb travel)
            let fracFromTop = (Self.displayRangeDb - clamped) / (Self.displayRangeDb * 2)
            let y = thumbPaddingTop + CGFloat(fracFromTop) * usable

            if i == 0 {
                linePath.move(to: CGPoint(x: x, y: y))
            } else {
                linePath.addLine(to: CGPoint(x: x, y: y))
            }
            fillPath.addLine(to: CGPoint(x: x, y: y))
        }
        fillPath.addLine(to: CGPoint(x: size.width, y: zeroY))
        fillPath.closeSubpath()

        // Colour based on bypass state.
        let strokeStyle: GraphicsContext.Shading
        let fillStyle:   GraphicsContext.Shading
        if bypassed {
            strokeStyle = .color(Color.secondary.opacity(0.35))
            fillStyle   = .color(Color.secondary.opacity(0.05))
        } else {
            strokeStyle = .color(Color.accentColor.opacity(0.9))
            fillStyle   = .color(Color.accentColor.opacity(0.15))
        }
        context.fill(fillPath, with: fillStyle)
        context.stroke(linePath, with: strokeStyle, lineWidth: 1.6)
    }

    /// RBJ peaking EQ magnitude at a given frequency, in dB.
    /// Matches Biquad.hpp's designBiquad() → magnitudeAt() exactly.
    private func peakingMagnitudeDb(fc: Double,
                                    gainDb: Double,
                                    q: Double,
                                    freq: Double) -> Double {
        let fs = Self.sampleRate
        // Guard against degenerate cases (fc ≥ Nyquist, non-positive Q, …).
        if fc <= 0 || fc >= fs * 0.5 || q <= 0 { return 0 }
        // Trivial 0 dB → identity, saves the log.
        if abs(gainDb) < 1e-6 { return 0 }

        let omega = 2.0 * .pi * fc / fs
        let sinW = sin(omega)
        let cosW = cos(omega)
        let alpha = sinW / (2.0 * q)
        let A = pow(10.0, gainDb / 40.0)

        let b0 = 1.0 + alpha * A
        let b1 = -2.0 * cosW
        let b2 = 1.0 - alpha * A
        let a0 = 1.0 + alpha / A
        let a1 = -2.0 * cosW
        let a2 = 1.0 - alpha / A

        // Evaluate |H(e^jw)| at f = freq.
        let w = 2.0 * .pi * freq / fs
        let cw = cos(w)
        let c2w = cos(2.0 * w)
        let sw = sin(w)
        let s2w = sin(2.0 * w)

        let numRe = b0 + b1 * cw + b2 * c2w
        let numIm = -(b1 * sw + b2 * s2w)
        let denRe = a0 + a1 * cw + a2 * c2w
        let denIm = -(a1 * sw + a2 * s2w)

        let numMag2 = numRe * numRe + numIm * numIm
        let denMag2 = denRe * denRe + denIm * denIm
        if denMag2 <= 0 { return 0 }
        let mag = sqrt(numMag2 / denMag2)
        if mag <= 0 { return -96 }
        return 20.0 * log10(mag)
    }
}
