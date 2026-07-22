// ProfileSection.swift — bottom row: active-profile picker + action buttons +
// saved-profile list.

import SwiftUI

struct ProfileSection: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("PROFILE")
                .font(.caption)
                .foregroundStyle(.secondary)

            HStack(alignment: .center, spacing: 12) {
                // Active-profile picker: the star + "Active" cue lives INSIDE
                // the label, so the dropdown itself tells the user which is
                // currently active without a separate label to the right.
                Menu {
                    ForEach(state.savedProfiles, id: \.self) { name in
                        Button(name) { /* wired in next iteration */ }
                    }
                    if state.savedProfiles.isEmpty {
                        Text("No saved profiles").disabled(true)
                    }
                } label: {
                    HStack(spacing: 6) {
                        Image(systemName: "star.fill")
                            .foregroundStyle(.yellow)
                        Text("Active — \(state.activeProfileName)")
                            .lineLimit(1)
                    }
                    .frame(maxWidth: 380, alignment: .leading)
                }

                Spacer()

                // Icon-only action buttons with tooltips on hover. Every SF
                // Symbol has slightly different intrinsic dimensions, so we
                // force width AND height on the Image so all four buttons
                // render the exact same size.
                // Order: create, delete, paste, import.
                Button { state.createNewProfile() } label: {
                    Image(systemName: "plus")
                        .frame(width: 16, height: 14)
                }
                .accessibilityLabel("New profile")
                .help("Create a new profile")

                Button(role: .destructive) { /* delete */ } label: {
                    Image(systemName: "trash")
                        .frame(width: 16, height: 14)
                }
                .accessibilityLabel("Delete profile")
                .help("Delete the active profile")

                Button { /* paste */ } label: {
                    Image(systemName: "doc.on.clipboard")
                        .frame(width: 16, height: 14)
                }
                .accessibilityLabel("Paste profile")
                .help("Paste a profile from the clipboard")

                Button { /* import */ } label: {
                    Image(systemName: "square.and.arrow.down")
                        .frame(width: 16, height: 14)
                }
                .accessibilityLabel("Import profile")
                .help("Import a Peace / Equalizer APO config file")
            }

            if !state.savedProfiles.isEmpty {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Saved profiles:")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    ForEach(state.savedProfiles, id: \.self) { name in
                        HStack(spacing: 6) {
                            if name == state.activeProfileName {
                                Image(systemName: "checkmark")
                                    .font(.caption)
                                    .foregroundStyle(.tint)
                            } else {
                                Text("  ")
                            }
                            Text(name)
                                .font(.callout)
                        }
                    }
                }
                .padding(.leading, 4)
            }
        }
    }
}
