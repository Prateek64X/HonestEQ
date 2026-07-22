// AppState.swift — single source of truth for the UI.
// Every SwiftUI view reads from and writes to this object via @EnvironmentObject.
// Writes will (in a follow-up) push updates to the daemon via file IPC.

import Foundation
import SwiftUI

@MainActor
final class AppState: ObservableObject {
    // MARK: Top controls
    @Published var preampDb: Double = 0.0        // -30...+30
    @Published var wetPercent: Double = 100.0    // 0...100
    @Published var isEqActive: Bool = true

    // MARK: Bands
    @Published var bands: [Band] = []

    // MARK: Profiles
    @Published var activeProfileName: String = "No profile"
    @Published var savedProfiles: [String] = []

    // File paths.
    private let supportDir: URL = {
        let base = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/HonestEQ")
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }()
    private var activeProfilePath: URL {
        supportDir.appendingPathComponent("active_profile.txt")
    }
    private var profilesDir: URL {
        let d = supportDir.appendingPathComponent("Profiles")
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }

    init() {
        loadActiveProfileFromDisk()
        refreshSavedProfileList()
    }

    // MARK: Profile I/O

    private func loadActiveProfileFromDisk() {
        guard let text = try? String(contentsOf: activeProfilePath, encoding: .utf8) else {
            // No active profile — provide a sensible default so the UI isn't empty.
            self.activeProfileName = "(no profile loaded)"
            self.bands = Self.defaultBands()
            self.preampDb = 0
            return
        }
        let profile = Profile.parse(text: text, name: "Active")
        self.preampDb = profile.preampDb
        self.bands = profile.bands.isEmpty ? Self.defaultBands() : profile.bands
        // Try to derive name from first `# ...` line.
        if let firstLine = text.split(whereSeparator: \.isNewline).first,
           firstLine.hasPrefix("#") {
            self.activeProfileName = firstLine
                .dropFirst()
                .trimmingCharacters(in: .whitespaces)
        } else {
            self.activeProfileName = "Active profile"
        }
    }

    func refreshSavedProfileList() {
        let entries = (try? FileManager.default.contentsOfDirectory(
            at: profilesDir, includingPropertiesForKeys: nil)) ?? []
        savedProfiles = entries
            .filter { $0.pathExtension == "txt" }
            .map { $0.deletingPathExtension().lastPathComponent }
            .sorted()
    }

    // MARK: Placeholder actions (wired to real logic in a follow-up)

    func createNewProfile() {
        // Follow-up: prompt for name, save empty profile.
        print("createNewProfile()")
    }

    // MARK: Defaults

    /// 31 log-spaced bands at Peace / Pro-Tools frequencies, all at 0 dB, Q = 4.32.
    /// Used when no profile is loaded so the UI has something to display.
    static func defaultBands() -> [Band] {
        let freqs: [Double] = [20, 25, 32, 40, 50, 63, 80, 101, 127, 160,
                               202, 254, 320, 403, 508, 640, 806, 1016, 1280, 1613,
                               2032, 2560, 3225, 4064, 5120, 6451, 8127, 10240,
                               12902, 16255, 20480]
        return freqs.map { f in
            Band(frequencyHz: f, gainDb: 0, q: 4.32)
        }
    }
}
