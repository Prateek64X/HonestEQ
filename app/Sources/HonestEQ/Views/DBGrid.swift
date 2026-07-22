// DBGrid.swift
//
// Continuous horizontal grid lines drawn BEHIND the band sliders — same
// reference pattern every pro EQ uses (FabFilter Pro-Q, Ableton EQ Eight,
// Logic's Channel EQ). Users read their EQ curve as the "shape" of thumbs
// relative to these lines rather than needing per-slider tick marks.
//
// Lines shown: 0 dB (prominent), ±3, ±6, ±9, ±12 dB (subtle). Draws over
// full band-area width, aligned to the vertical slider track.

import SwiftUI

struct DBGrid: View {
    /// Height of the vertical slider portion this grid overlays.
    let height: CGFloat
    /// Thumb-padding at slider top and bottom. macOS NSSlider (controlSize
    /// .small) is not perfectly symmetric vertically — the thumb at 0 dB
    /// sits a hair below the geometric centre. These values were measured
    /// empirically against the actual NSSlider rendering on macOS 14 and
    /// let the 0 dB grid line hit each slider's 0 thumb position pixel-
    /// accurately.
    let thumbPaddingTop:    CGFloat = 12
    let thumbPaddingBottom: CGFloat = 8

    // dB values with visible grid lines. Order: top of scale → bottom.
    private let stops: [(dB: Int, prominent: Bool)] = [
        (12, false),
        ( 9, false),
        ( 6, false),
        ( 3, false),
        ( 0, true),   // 0 dB — anchor line, more visible
        (-3, false),
        (-6, false),
        (-9, false),
        (-12, false),
    ]

    var body: some View {
        GeometryReader { geo in
            let usable = geo.size.height - thumbPaddingTop - thumbPaddingBottom
            ZStack {
                ForEach(0..<stops.count, id: \.self) { i in
                    let s = stops[i]
                    let t = fractionForDb(s.dB)   // 0 = top (+12), 1 = bottom (-12)
                    line(prominent: s.prominent)
                        .frame(width: geo.size.width)
                        .position(x: geo.size.width / 2,
                                  y: thumbPaddingTop + usable * t)
                }
            }
        }
        .frame(height: height)
        // Not interactive — grid never blocks slider drags.
        .allowsHitTesting(false)
    }

    /// Convert dB (−12…+12) → vertical fraction (0 at top, 1 at bottom).
    private func fractionForDb(_ dB: Int) -> CGFloat {
        return CGFloat(12 - dB) / 24.0
    }

    private func line(prominent: Bool) -> some View {
        Rectangle()
            .fill(prominent
                  ? Color.primary.opacity(0.25)
                  : Color.secondary.opacity(0.10))
            .frame(height: prominent ? 1 : 0.5)
    }
}
