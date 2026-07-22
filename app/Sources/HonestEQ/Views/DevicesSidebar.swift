// DevicesSidebar.swift — right-side collapsible panel.
// docs/ui-design.md §"Sidebar (right side)"
//
// Simplified per-device UX: a list of output devices, each with a
// dedicated on/off toggle for whether HonestEQ's EQ should apply when
// that device is the active output.
//
// This mirrors Peace EQ's per-device enable/disable model — plug in
// different headphones, HonestEQ automatically applies (or bypasses) EQ
// based on the toggle you set for that device.
//
// Peace calls output devices "Speakers" in its UI; the more common
// macOS phrasing is just "Output". We use "Output" here for consistency
// with Sound Settings.

import SwiftUI

struct DevicesSidebar: View {
    @EnvironmentObject var state: AppState

    // Placeholder device list — real CoreAudio enumeration lands in
    // a follow-up iteration (Model/OutputDevice.swift).
    @State private var devices: [DevicePlaceholder] = [
        .init(name: "External Headphones", isCurrentOutput: true,  eqEnabled: true),
        .init(name: "MacBook Air Speakers", isCurrentOutput: false, eqEnabled: false),
        .init(name: "Sony CH-720N (USB)",   isCurrentOutput: false, eqEnabled: true),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("DEVICES")
                .font(.caption)
                .foregroundStyle(.secondary)

            Text("Enable EQ per output device")
                .font(.subheadline)
                .foregroundStyle(.secondary)

            VStack(spacing: 8) {
                ForEach($devices) { $device in
                    deviceRow($device)
                }
            }

            Spacer()

            Text("EQ is applied when the toggled device is the active system output. macOS Sound Settings selects the active output.")
                .font(.caption)
                .foregroundStyle(.tertiary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding()
        .background(.regularMaterial)
    }

    private func deviceRow(_ device: Binding<DevicePlaceholder>) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Image(systemName: iconFor(device.wrappedValue.name))
                        .foregroundStyle(.secondary)
                    Text(device.wrappedValue.name)
                        .font(.callout)
                }
                if device.wrappedValue.isCurrentOutput {
                    Text("Current output")
                        .font(.caption2)
                        .foregroundStyle(.tint)
                }
            }
            Spacer()
            Toggle("", isOn: device.eqEnabled)
                .toggleStyle(.switch)
                .labelsHidden()
                .controlSize(.mini)
        }
    }

    private func iconFor(_ name: String) -> String {
        let lower = name.lowercased()
        if lower.contains("speaker") { return "speaker.wave.2.fill" }
        if lower.contains("usb")     { return "cable.connector" }
        if lower.contains("airpod")  { return "airpodspro" }
        return "headphones"
    }
}

// Placeholder — real OutputDevice model comes in a follow-up.
private struct DevicePlaceholder: Identifiable {
    let id = UUID()
    let name: String
    let isCurrentOutput: Bool
    var eqEnabled: Bool
}
