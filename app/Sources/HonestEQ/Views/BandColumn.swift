// BandColumn.swift — one band's stacked controls:
//   [ Frequency input ]
//   [ vertical slider ] with dB tick marks
//   [ dB input       ]
//   [ Q input        ]
//
// docs/ui-design.md §"EQ band editor"

import SwiftUI

struct BandColumn: View {
    @Binding var band: Band

    /// Column width — tightened so more bands fit at typical window sizes.
    /// Text-field cells inside are narrower still (text is centered).
    static let columnWidth: CGFloat = 44
    static let sliderHeight: CGFloat = 220
    static let cellHeight: CGFloat = 22
    static let textFieldWidth: CGFloat = 40   // slightly narrower than the column

    var body: some View {
        VStack(spacing: 4) {
            // Frequency (Hz)
            TextField("", value: $band.frequencyHz,
                      format: .number.precision(.fractionLength(0)))
                .textFieldStyle(.roundedBorder)
                .frame(width: Self.textFieldWidth, height: Self.cellHeight)
                .multilineTextAlignment(.center)
                .font(.caption)

            // Vertical gain slider (−12 to +12 dB, 5 tick marks: ±12, ±6, 0)
            VerticalSlider(value: $band.gainDb, range: -12...12, tickCount: 5)
                .frame(width: Self.columnWidth, height: Self.sliderHeight)

            // Gain in dB
            TextField("", value: $band.gainDb,
                      format: .number.precision(.fractionLength(1)))
                .textFieldStyle(.roundedBorder)
                .frame(width: Self.textFieldWidth, height: Self.cellHeight)
                .multilineTextAlignment(.center)
                .font(.caption)

            // Q factor
            TextField("", value: $band.q,
                      format: .number.precision(.fractionLength(2)))
                .textFieldStyle(.roundedBorder)
                .frame(width: Self.textFieldWidth, height: Self.cellHeight)
                .multilineTextAlignment(.center)
                .font(.caption)
        }
    }
}
