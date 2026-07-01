// HonestEQ — BiquadChain.hpp
//
// A bank of N biquads in series, per-channel state, with a pre-amp.
// This is the actual EQ engine — the whole signal chain in one struct.
//
// Layout matches Peace / Equalizer APO: pre-amp first, then each band.
// Stereo is two parallel chains sharing the same coefficient set
// (v1 — per-channel coefficients can be added later for crossfeed etc.)

#pragma once

#include "Biquad.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace honesteq {

struct BandSpec {
    BiquadType type = BiquadType::PeakingEQ;
    double frequencyHz = 1000.0;
    double q = 1.0;
    double gainDb = 0.0;
    bool enabled = true;
};

class BiquadChain {
public:
    // Construct an empty chain. setSampleRate() must be called before processing.
    BiquadChain() = default;

    void setSampleRate(double fs) noexcept {
        if (fs != sampleRate_) {
            sampleRate_ = fs;
            rebuildCoefficients();
            reset();
        }
    }

    double sampleRate() const noexcept { return sampleRate_; }

    // Pre-amp in dB. Applied before the band chain.
    void setPreampDb(double db) noexcept {
        preampDb_ = db;
        preampLinear_ = std::pow(10.0, db / 20.0);
    }
    double preampDb() const noexcept { return preampDb_; }

    // Replace the full band set. Allocates if size changes — do this at
    // setup time, not on the audio thread, unless you've pre-reserved.
    void setBands(const std::vector<BandSpec>& bands) {
        bands_ = bands;
        // Keep per-channel state vectors sized to band count.
        for (auto& ch : channels_) {
            ch.filters.assign(bands_.size(), Biquad{});
        }
        rebuildCoefficients();
    }

    const std::vector<BandSpec>& bands() const noexcept { return bands_; }

    // Mutate a single band by index. Recomputes only that band's coefficients.
    void updateBand(std::size_t i, const BandSpec& b) {
        if (i >= bands_.size()) return;
        bands_[i] = b;
        const auto c = b.enabled
            ? designBiquad(b.type, b.frequencyHz, b.q, b.gainDb, sampleRate_)
            : BiquadCoeffs::identity();
        for (auto& ch : channels_) {
            ch.filters[i].setCoefficients(c);
        }
    }

    // Channels: 1 = mono, 2 = stereo. v1 supports 1 or 2.
    void setChannelCount(std::size_t n) {
        if (n == 0 || n > kMaxChannels) n = 2;
        if (n == channels_.size()) return;
        const std::size_t prev = channels_.size();
        if (n < prev) {
            // shrink
            for (std::size_t i = n; i < prev; ++i) channels_[i].filters.clear();
        }
        // We don't resize the std::array; we just track active count.
        activeChannels_ = n;
        for (std::size_t c = 0; c < activeChannels_; ++c) {
            channels_[c].filters.assign(bands_.size(), Biquad{});
        }
        rebuildCoefficients();
    }
    std::size_t channelCount() const noexcept { return activeChannels_; }

    // Clear filter state on all bands / all channels.
    void reset() noexcept {
        for (auto& ch : channels_) {
            for (auto& f : ch.filters) f.reset();
        }
    }

    // Process one sample of one channel. Mainly for tests; production code
    // should call processBlock for SIMD-friendly tight loops.
    double processSample(std::size_t channel, double x) noexcept {
        if (channel >= activeChannels_) return x;
        double y = x * preampLinear_;
        for (auto& f : channels_[channel].filters) {
            y = f.process(y);
        }
        return y;
    }

    // Process an interleaved block.  `frames` = sample frames per channel.
    // `data` length = frames * activeChannels_. In-place is allowed.
    void processInterleaved(double* data, std::size_t frames) noexcept {
        const std::size_t ch = activeChannels_;
        const double pre = preampLinear_;
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t c = 0; c < ch; ++c) {
                double x = data[i * ch + c] * pre;
                for (auto& f : channels_[c].filters) {
                    x = f.process(x);
                }
                data[i * ch + c] = x;
            }
        }
    }

    // Process planar (separate L/R buffers).  Each buffer is `frames` samples.
    void processPlanar(double* const* channelData, std::size_t frames) noexcept {
        const double pre = preampLinear_;
        for (std::size_t c = 0; c < activeChannels_; ++c) {
            double* buf = channelData[c];
            // Pre-amp pass (could be merged with the first biquad block).
            for (std::size_t i = 0; i < frames; ++i) buf[i] *= pre;
            // Cascade.
            for (auto& f : channels_[c].filters) {
                f.processBlock(buf, buf, frames);
            }
        }
    }

private:
    static constexpr std::size_t kMaxChannels = 2;

    struct Channel {
        std::vector<Biquad> filters;
    };

    void rebuildCoefficients() noexcept {
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            const auto& b = bands_[i];
            const auto c = b.enabled
                ? designBiquad(b.type, b.frequencyHz, b.q, b.gainDb, sampleRate_)
                : BiquadCoeffs::identity();
            for (std::size_t ch = 0; ch < activeChannels_; ++ch) {
                if (i < channels_[ch].filters.size()) {
                    channels_[ch].filters[i].setCoefficients(c);
                }
            }
        }
    }

    double sampleRate_ = 48000.0;
    double preampDb_ = 0.0;
    double preampLinear_ = 1.0;
    std::vector<BandSpec> bands_;
    std::array<Channel, kMaxChannels> channels_{};
    std::size_t activeChannels_ = 2;
};

}  // namespace honesteq
