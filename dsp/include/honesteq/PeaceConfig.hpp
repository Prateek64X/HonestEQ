// HonestEQ — PeaceConfig.hpp
//
// Parser for Equalizer APO / Peace EQ config files.
//   - "Preamp: <signed> dB"
//   - "Filter [N]: ON|OFF <TYPE> Fc <num> Hz  Gain <signed> dB  Q <num>"
//   - Lines that don't match are silently ignored (Equalizer APO convention).
//   - Whitespace is tolerant; the filter index is optional.
//
// Reference grammar: https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/

#pragma once

#include "BiquadChain.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace honesteq {

struct Profile {
    double preampDb = 0.0;
    std::vector<BandSpec> bands;
};

namespace detail {

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

// Stream-based token-grabber that skips whitespace.
inline bool peekKeyword(std::istringstream& iss, const char* kw) {
    std::streampos pos = iss.tellg();
    std::string tok;
    if (!(iss >> tok)) { iss.clear(); iss.seekg(pos); return false; }
    if (lower(tok) != lower(kw)) { iss.clear(); iss.seekg(pos); return false; }
    return true;
}

inline bool parseFilterLine(const std::string& line, BandSpec& out) {
    // Format: "Filter [N] : ON|OFF  TYPE  Fc <num> Hz  Gain <num> dB  Q <num>"
    // Either order of Gain / Q / Fc segments after TYPE is tolerated; we
    // require Fc explicitly but Gain/Q can appear in any order or be omitted
    // for filters that don't use them (LP/HP/NO/AP/BP).
    std::istringstream iss(line);
    std::string word;
    if (!(iss >> word) || lower(word) != "filter") return false;

    // Optional filter index, then ':'.
    // The rest of the line up to ':' is ignored (could be a number or empty).
    std::string rest;
    std::getline(iss, rest, ':');
    if (iss.fail()) return false;

    std::istringstream rs(rest);
    iss.clear();

    std::string state;
    if (!(iss >> state)) return false;
    state = lower(state);
    if (state != "on" && state != "off") return false;
    out.enabled = (state == "on");

    std::string typeTok;
    if (!(iss >> typeTok)) return false;
    typeTok = lower(typeTok);

    if      (typeTok == "pk" || typeTok == "peq") out.type = BiquadType::PeakingEQ;
    else if (typeTok == "ls" || typeTok == "lsc") out.type = BiquadType::LowShelf;
    else if (typeTok == "hs" || typeTok == "hsc") out.type = BiquadType::HighShelf;
    else if (typeTok == "lp" || typeTok == "lpq") out.type = BiquadType::LowPass;
    else if (typeTok == "hp" || typeTok == "hpq") out.type = BiquadType::HighPass;
    else if (typeTok == "no")                     out.type = BiquadType::Notch;
    else if (typeTok == "bp")                     out.type = BiquadType::BandPass;
    else if (typeTok == "ap")                     out.type = BiquadType::AllPass;
    else                                           return false;

    // Default Q for shelves where the user might have omitted it.
    out.q = (out.type == BiquadType::LowShelf || out.type == BiquadType::HighShelf) ? 0.70710678118654752 : 1.0;
    out.gainDb = 0.0;
    bool gotFc = false;

    // Now scan for "Fc <num> Hz", "Gain <num> dB", "Q <num>".
    while (iss >> word) {
        std::string wl = lower(word);
        if (wl == "fc") {
            double v; std::string unit;
            if (!(iss >> v)) return false;
            iss >> unit;  // "Hz" — ignored
            out.frequencyHz = v;
            gotFc = true;
        } else if (wl == "gain") {
            double v; std::string unit;
            if (!(iss >> v)) return false;
            iss >> unit;  // "dB"
            out.gainDb = v;
        } else if (wl == "q") {
            double v;
            if (!(iss >> v)) return false;
            out.q = v;
        }
        // Anything else (e.g., extra slope tokens, channel specifiers) ignored.
    }
    return gotFc;
}

inline bool parsePreampLine(const std::string& line, double& outDb) {
    std::istringstream iss(line);
    std::string w;
    if (!(iss >> w)) return false;
    if (lower(w) != "preamp:") {
        // tolerate "Preamp : -6 dB" with space before colon
        if (lower(w) != "preamp") return false;
        std::string colon;
        if (!(iss >> colon) || colon != ":") return false;
    }
    double v;
    if (!(iss >> v)) return false;
    iss >> w;  // "dB"
    outDb = v;
    return true;
}

}  // namespace detail

// Parse from an in-memory string (e.g., pasted from Peace UI).
inline std::string parsePeaceProfile(const std::string& text, Profile& out) {
    out.preampDb = 0.0;
    out.bands.clear();

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::string t = detail::trim(line);
        if (t.empty() || t[0] == '#') continue;

        double preamp;
        if (detail::parsePreampLine(t, preamp)) {
            out.preampDb = preamp;
            continue;
        }

        BandSpec b;
        if (detail::parseFilterLine(t, b)) {
            out.bands.push_back(b);
            continue;
        }
        // Silently ignore unrecognised lines (Channel:, Include:, GraphicEQ:, etc.)
    }
    return {};
}

inline std::string loadPeaceProfile(const std::string& path, Profile& out) {
    std::ifstream f(path);
    if (!f) return "could not open profile file: " + path;
    std::ostringstream ss;
    ss << f.rdbuf();
    return parsePeaceProfile(ss.str(), out);
}

}  // namespace honesteq
