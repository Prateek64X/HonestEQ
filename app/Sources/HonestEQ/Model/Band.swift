// Band.swift — a single EQ band.
// Matches honesteq::BandSpec on the C++ side (Peace / Equalizer APO Filter line).

import Foundation

enum FilterType: String, CaseIterable, Identifiable {
    case peaking = "PK"
    case lowShelf = "LS"
    case highShelf = "HS"
    case lowPass = "LP"
    case highPass = "HP"
    case notch = "NO"
    case bandPass = "BP"
    case allPass = "AP"

    var id: String { rawValue }
    var displayName: String {
        switch self {
        case .peaking:   return "Peaking"
        case .lowShelf:  return "Low Shelf"
        case .highShelf: return "High Shelf"
        case .lowPass:   return "Low-Pass"
        case .highPass:  return "High-Pass"
        case .notch:     return "Notch"
        case .bandPass:  return "Band-Pass"
        case .allPass:   return "All-Pass"
        }
    }
}

struct Band: Identifiable, Hashable {
    let id: UUID
    var type: FilterType
    var frequencyHz: Double     // e.g. 1000
    var gainDb: Double           // e.g. -3.5
    var q: Double                // e.g. 4.32 (ISO 1/3-octave standard)
    var enabled: Bool

    init(id: UUID = UUID(),
         type: FilterType = .peaking,
         frequencyHz: Double = 1000,
         gainDb: Double = 0,
         q: Double = 4.32,
         enabled: Bool = true) {
        self.id = id
        self.type = type
        self.frequencyHz = frequencyHz
        self.gainDb = gainDb
        self.q = q
        self.enabled = enabled
    }
}
