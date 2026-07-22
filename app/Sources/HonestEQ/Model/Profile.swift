// Profile.swift — read/write Peace / Equalizer APO configuration files.
// Same format as libHonestEQDSP's PeaceConfig.hpp parses, so profiles
// round-trip cleanly between the offline render CLI, the daemon, and this app.

import Foundation

struct Profile: Identifiable {
    let id: UUID
    var name: String
    var preampDb: Double
    var bands: [Band]

    init(id: UUID = UUID(), name: String, preampDb: Double = 0, bands: [Band] = []) {
        self.id = id
        self.name = name
        self.preampDb = preampDb
        self.bands = bands
    }
}

// MARK: - Parse

extension Profile {
    /// Parse a Peace / Equalizer APO config text into a Profile.
    static func parse(text: String, name: String) -> Profile {
        var preamp = 0.0
        var bands: [Band] = []

        for line in text.split(whereSeparator: \.isNewline) {
            let raw = line.trimmingCharacters(in: .whitespaces)
            if raw.isEmpty || raw.hasPrefix("#") { continue }

            if let p = parsePreampLine(raw) {
                preamp = p
                continue
            }
            if let b = parseFilterLine(raw) {
                bands.append(b)
            }
        }
        return Profile(name: name, preampDb: preamp, bands: bands)
    }

    private static func parsePreampLine(_ line: String) -> Double? {
        // "Preamp: -1.5 dB"
        let lower = line.lowercased()
        guard lower.hasPrefix("preamp") else { return nil }
        let scanner = Scanner(string: line)
        _ = scanner.scanUpToString(":")
        _ = scanner.scanString(":")
        guard let v = scanner.scanDouble() else { return nil }
        return v
    }

    private static func parseFilterLine(_ line: String) -> Band? {
        // "Filter 1: ON PK Fc 1000 Hz Gain -3 dB Q 1.0"
        let lower = line.lowercased()
        guard lower.hasPrefix("filter") else { return nil }

        // Skip past ':'
        guard let colon = line.firstIndex(of: ":") else { return nil }
        let after = String(line[line.index(after: colon)...])
            .trimmingCharacters(in: .whitespaces)

        // Split by whitespace, walk tokens.
        var tokens = after.split(whereSeparator: \.isWhitespace).map(String.init)
        guard tokens.count >= 3 else { return nil }

        // ON / OFF
        let onoff = tokens.removeFirst().uppercased()
        let enabled = (onoff == "ON")

        // Filter type
        let typeStr = tokens.removeFirst().uppercased()
        let type = FilterType(rawValue: typeStr) ?? .peaking

        var fc: Double = 1000
        var gain: Double = 0
        var q: Double = (type == .lowShelf || type == .highShelf) ? 0.707 : 4.32

        // Walk remaining tokens looking for Fc / Gain / Q markers.
        var i = 0
        while i < tokens.count {
            let key = tokens[i].uppercased()
            if key == "FC" || key == "F" {
                if i + 1 < tokens.count, let v = Double(tokens[i + 1]) {
                    fc = v
                    i += 2
                    // skip units token if next is "Hz"
                    if i < tokens.count, tokens[i].uppercased() == "HZ" { i += 1 }
                    continue
                }
            }
            if key == "GAIN" {
                if i + 1 < tokens.count, let v = Double(tokens[i + 1]) {
                    gain = v
                    i += 2
                    if i < tokens.count, tokens[i].uppercased() == "DB" { i += 1 }
                    continue
                }
            }
            if key == "Q" {
                if i + 1 < tokens.count, let v = Double(tokens[i + 1]) {
                    q = v
                    i += 2
                    continue
                }
            }
            i += 1
        }

        return Band(type: type, frequencyHz: fc, gainDb: gain, q: q, enabled: enabled)
    }
}

// MARK: - Serialize

extension Profile {
    /// Write to Equalizer APO / Peace text format. Round-trips with parse().
    func serialize() -> String {
        var out = ""
        out.append("# \(name)\n\n")
        out.append(String(format: "Preamp: %g dB\n\n", preampDb))
        for (i, b) in bands.enumerated() {
            let onoff = b.enabled ? "ON" : "OFF"
            out.append(String(format:
                "Filter %d: %@ %@ Fc %g Hz Gain %g dB Q %g\n",
                i + 1, onoff, b.type.rawValue, b.frequencyHz, b.gainDb, b.q))
        }
        return out
    }
}
