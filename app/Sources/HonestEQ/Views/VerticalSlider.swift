// VerticalSlider.swift — NSSlider bridged into SwiftUI, oriented vertically
// with tick marks on the leading (left) edge. Same widget the Audio MIDI
// Setup app uses for master volume, so it renders 100% native-macOS.

import SwiftUI
import AppKit

struct VerticalSlider: NSViewRepresentable {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let tickCount: Int      // e.g. 5 → ticks at +12, +6, 0, −6, −12

    func makeNSView(context: Context) -> NSSlider {
        let slider = NSSlider(
            target: context.coordinator,
            action: #selector(Coordinator.valueChanged(_:))
        )
        slider.minValue = range.lowerBound
        slider.maxValue = range.upperBound
        slider.doubleValue = value
        slider.isVertical = true
        // No per-slider tick marks — the band editor draws continuous
        // horizontal grid lines across all sliders instead, which is the
        // standard pro-EQ pattern (Pro-Q, EQ Eight, Logic EQ).
        slider.numberOfTickMarks = 0
        _ = tickCount   // parameter kept for API stability
        slider.controlSize = .small
        return slider
    }

    func updateNSView(_ nsView: NSSlider, context: Context) {
        // Only sync when it drifted meaningfully — avoids fighting the user's own drag.
        if abs(nsView.doubleValue - value) > 1e-9 {
            nsView.doubleValue = value
        }
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(value: $value)
    }

    final class Coordinator: NSObject {
        var value: Binding<Double>
        init(value: Binding<Double>) { self.value = value }

        @objc func valueChanged(_ sender: NSSlider) {
            value.wrappedValue = sender.doubleValue
        }
    }
}
