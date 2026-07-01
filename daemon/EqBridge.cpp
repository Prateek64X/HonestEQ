// EqBridge.cpp — C++ impl of the EqBridge C-ABI.
// Wraps honesteq::BiquadChain and honesteq::loadPeaceProfile.

#include "EqBridge.hpp"

#include "honesteq/BiquadChain.hpp"
#include "honesteq/PeaceConfig.hpp"

#include <cstddef>
#include <cstdint>

// Max samples the bridge will process in a single call. 4096 frames × 2
// channels = 8192 doubles = 64 KB static per bridge. AUHAL callbacks are
// typically 128 frames; this is huge headroom to avoid ever hitting the
// bound in practice.
static constexpr std::size_t kMaxScratchSamples = 8192;

struct EqBridge {
    honesteq::BiquadChain chain;
    double                scratch[kMaxScratchSamples];
    bool                  has_profile = false;
    int                   band_count  = 0;
    double                preamp_db   = 0.0;
};

extern "C" EqBridge* eq_bridge_create(double sample_rate) {
    auto* eq = new (std::nothrow) EqBridge{};
    if (!eq) return nullptr;
    eq->chain.setChannelCount(2);
    eq->chain.setSampleRate(sample_rate > 0 ? sample_rate : 48000.0);
    return eq;
}

extern "C" void eq_bridge_destroy(EqBridge* eq) {
    delete eq;
}

extern "C" int eq_bridge_load_profile(EqBridge* eq, const char* path) {
    if (!eq || !path) return 1;
    honesteq::Profile profile;
    auto err = honesteq::loadPeaceProfile(std::string(path), profile);
    if (!err.empty()) {
        eq->has_profile = false;
        eq->band_count = 0;
        eq->preamp_db = 0.0;
        eq->chain.setBands({});
        eq->chain.setPreampDb(0.0);
        return 1;
    }
    eq->chain.setBands(profile.bands);
    eq->chain.setPreampDb(profile.preampDb);
    eq->has_profile = !profile.bands.empty();
    eq->band_count  = (int)profile.bands.size();
    eq->preamp_db   = profile.preampDb;
    return 0;
}

extern "C" void eq_bridge_set_sample_rate(EqBridge* eq, double sample_rate) {
    if (!eq || sample_rate <= 0) return;
    eq->chain.setSampleRate(sample_rate);
}

extern "C" void eq_bridge_process_interleaved(EqBridge* eq, float* data, unsigned frames) {
    if (!eq || !data || frames == 0) return;
    if (!eq->has_profile) return;   // passthrough
    const std::size_t samples = (std::size_t)frames * 2;
    if (samples > kMaxScratchSamples) return;  // shouldn't happen; bail safely

    // float → double
    for (std::size_t i = 0; i < samples; ++i) eq->scratch[i] = (double)data[i];

    // Process in place (double precision)
    eq->chain.processInterleaved(eq->scratch, (std::size_t)frames);

    // double → float
    for (std::size_t i = 0; i < samples; ++i) data[i] = (float)eq->scratch[i];
}

extern "C" int eq_bridge_has_profile(const EqBridge* eq) {
    return (eq && eq->has_profile) ? 1 : 0;
}

extern "C" int eq_bridge_band_count(const EqBridge* eq) {
    return eq ? eq->band_count : 0;
}

extern "C" double eq_bridge_preamp_db(const EqBridge* eq) {
    return eq ? eq->preamp_db : 0.0;
}

extern "C" void eq_bridge_set_preamp_db(EqBridge* eq, double db) {
    if (!eq) return;
    eq->preamp_db = db;
    eq->chain.setPreampDb(db);
}
