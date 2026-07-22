// BandEditorView.swift — the EQ band grid.
// docs/ui-design.md §"EQ band editor"

import SwiftUI

struct BandEditorView: View {
    @EnvironmentObject var state: AppState
    @State private var bandCountDraft: String = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            header
            grid
        }
    }

    // MARK: Header — Add / Remove / N-band input

    private var header: some View {
        HStack {
            Text("EQ Bands")
                .font(.headline)

            Spacer()

            Button {
                state.bands.append(Band())
            } label: {
                Image(systemName: "plus")
                    .frame(width: 14, height: 14)
            }
            .accessibilityLabel("Add band")
            .help("Add a new band")

            Button {
                if !state.bands.isEmpty { state.bands.removeLast() }
            } label: {
                Image(systemName: "minus")
                    .frame(width: 14, height: 14)
            }
            .accessibilityLabel("Remove band")
            .help("Remove the last band")

            HStack(spacing: 4) {
                Text("Bands:")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                TextField("", text: $bandCountDraft, prompt: Text("\(state.bands.count)"))
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 50)
                    .onSubmit(applyBandCount)
                    .help("Set exact band count and press Return")
            }
        }
    }

    // MARK: Grid — fixed labels on the left, scrolling band columns on the right

    private var grid: some View {
        HStack(alignment: .top, spacing: 8) {
            labelColumn

            PersistentHorizontalScrollView {
                // Three layers, back to front:
                //  1. dB grid — horizontal reference lines (0 dB prominent)
                //  2. Response curve — cumulative frequency response of the EQ
                //  3. Band sliders — the interactive layer
                ZStack(alignment: .top) {
                    VStack(spacing: 0) {
                        Spacer().frame(height: BandColumn.cellHeight + 4)
                        DBGrid(height: BandColumn.sliderHeight)
                    }

                    VStack(spacing: 0) {
                        Spacer().frame(height: BandColumn.cellHeight + 4)
                        ResponseCurveView(
                            bands: state.bands,
                            preampDb: state.preampDb,
                            intensity: state.wetPercent / 100.0,
                            bypassed: !state.isEqActive
                        )
                        .frame(height: BandColumn.sliderHeight)
                        // No outer padding — ResponseCurveView applies the
                        // same asymmetric thumb-padding (12 top, 8 bottom) as
                        // DBGrid internally, so both draw in the identical
                        // frame and share pixel-perfect 0 dB alignment.
                    }

                    HStack(alignment: .top, spacing: 2) {
                        ForEach($state.bands) { $band in
                            BandColumn(band: $band)
                        }
                    }
                }
                .padding(.bottom, 8)
                .padding(.trailing, 4)
            }
            .frame(maxWidth: .infinity)
            .frame(height: BandColumn.cellHeight * 3
                         + BandColumn.sliderHeight
                         + 8 * 3
                         + 8
                         + 20)
        }
    }

    // MARK: Row labels (Freq / dB scale / dB / Q)

    private var labelColumn: some View {
        VStack(spacing: 4) {
            // Aligns with frequency input row.
            Text("Frequency (Hz)")
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(height: BandColumn.cellHeight, alignment: .center)

            // dB scale beside the sliders — matches the tick marks the
            // NSSliders render on their leading edge.
            dbScale
                .frame(height: BandColumn.sliderHeight)

            // Aligns with dB row.
            Text("dB")
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(height: BandColumn.cellHeight, alignment: .center)

            // Aligns with Q row.
            Text("Q")
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(height: BandColumn.cellHeight, alignment: .center)
        }
        .frame(width: 72)
    }

    private var dbScale: some View {
        // Asymmetric top/bottom padding — matches NSSlider's actual thumb
        // extremes on macOS 14 (slightly more padding on top). See DBGrid.
        VStack(alignment: .trailing, spacing: 0) {
            scaleLabel("+12")
            Spacer()
            scaleLabel("+6")
            Spacer()
            scaleLabel("0", emphasized: true)
            Spacer()
            scaleLabel("−6")
            Spacer()
            scaleLabel("−12")
        }
        .padding(.top, 12)
        .padding(.bottom, 8)
        .padding(.trailing, 4)
    }

    private func scaleLabel(_ text: String, emphasized: Bool = false) -> some View {
        Text(text)
            .font(.caption2)
            .foregroundStyle(emphasized ? Color.primary : .secondary)
            .monospacedDigit()
    }

    // MARK: Band-count text field handler

    private func applyBandCount() {
        guard let n = Int(bandCountDraft.trimmingCharacters(in: .whitespaces)) else { return }
        let clamped = max(1, min(255, n))
        if clamped > state.bands.count {
            let extra = clamped - state.bands.count
            for _ in 0..<extra { state.bands.append(Band()) }
        } else if clamped < state.bands.count {
            state.bands = Array(state.bands.prefix(clamped))
        }
        bandCountDraft = ""
    }
}
