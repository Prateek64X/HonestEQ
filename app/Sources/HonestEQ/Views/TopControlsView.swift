// TopControlsView.swift — Pre-Amp / Dry-Wet / On-Off row at top of the window.
// docs/ui-design.md §"Top controls row"

import SwiftUI

struct TopControlsView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        HStack(alignment: .top, spacing: 24) {
            preampCol
                .frame(maxWidth: .infinity)   // pre-amp uses all remaining space
            wetDryCol
                .frame(width: 140)             // fixed compact column
            onOffCol
                .frame(width: 60)              // fixed narrow column
        }
    }

    // MARK: Pre-Amp — primary control, slider expands to full width

    private var preampCol: some View {
        VStack(spacing: 6) {
            Text("Pre-Amp")
                .font(.subheadline)
                .foregroundStyle(.secondary)

            // Slider fills the whole column width — Peace EQ style.
            // Continuous (no `step:`) so no tick-mark dotted line renders below.
            Slider(value: $state.preampDb, in: -30...30)

            ValueField(value: $state.preampDb, decimals: 1, unit: "dB", range: -30...30)
        }
    }

    // MARK: Dry / Wet — compact secondary control

    private var wetDryCol: some View {
        VStack(spacing: 6) {
            Text("Dry / Wet")
                .font(.subheadline)
                .foregroundStyle(.secondary)

            Slider(value: $state.wetPercent, in: 0...100)

            ValueField(value: $state.wetPercent, decimals: 0, unit: "%", range: 0...100)
        }
    }

    // MARK: On / Off

    private var onOffCol: some View {
        VStack(spacing: 6) {
            Text("EQ")
                .font(.subheadline)
                .foregroundStyle(.secondary)

            Toggle("", isOn: $state.isEqActive)
                .toggleStyle(.switch)
                .labelsHidden()

            Text(state.isEqActive ? "On" : "Off")
                .font(.caption)
                .foregroundStyle(state.isEqActive ? .green : .secondary)
                .frame(minWidth: 40)
        }
    }
}

// MARK: - Editable numeric field
//
// Native macOS bordered TextField that's always editable — value visible
// as an input at all times, drag the paired slider OR type directly.
// Two-way binding via SwiftUI's built-in Double formatter.

struct ValueField: View {
    @Binding var value: Double
    let decimals: Int
    let unit: String
    let range: ClosedRange<Double>

    var body: some View {
        HStack(spacing: 3) {
            TextField(
                "",
                value: $value,
                format: .number.precision(.fractionLength(decimals))
            )
            .textFieldStyle(.roundedBorder)
            .frame(width: 64)
            .multilineTextAlignment(.trailing)
            .onChange(of: value) { _, newValue in
                if newValue < range.lowerBound { value = range.lowerBound }
                if newValue > range.upperBound { value = range.upperBound }
            }

            Text(unit)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }
}
