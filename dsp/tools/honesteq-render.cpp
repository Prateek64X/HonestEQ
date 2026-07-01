// honesteq-render — CLI tool that applies a Peace EQ / Equalizer APO config
// to a WAV file. Output is a new WAV in the same format (rate, bit depth,
// channels) as the input.
//
// Usage:
//   honesteq-render <profile.txt> <input.wav> <output.wav>
//
// Internally:
//   - reads the wav into double[] at native sample rate
//   - parses the Peace profile
//   - configures a BiquadChain
//   - processes in place
//   - writes the wav back in the original format

#include "honesteq/Biquad.hpp"
#include "honesteq/BiquadChain.hpp"
#include "honesteq/PeaceConfig.hpp"
#include "honesteq/Wav.hpp"

#include <cstdio>
#include <string>

using namespace honesteq;

static const char* formatName(WavSampleFormat f) {
    switch (f) {
        case WavSampleFormat::PCM16:   return "16-bit PCM";
        case WavSampleFormat::PCM24:   return "24-bit PCM";
        case WavSampleFormat::PCM32:   return "32-bit PCM";
        case WavSampleFormat::Float32: return "32-bit float";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "honesteq-render — apply a Peace/Equalizer APO profile to a WAV file.\n\n"
            "Usage:\n"
            "  %s <profile.txt> <input.wav> <output.wav> [--start <sec>] [--duration <sec>]\n\n"
            "Examples:\n"
            "  %s profile.txt in.wav out.wav                       # full file\n"
            "  %s profile.txt in.wav out.wav --start 140           # from 2:20 to end\n"
            "  %s profile.txt in.wav out.wav --start 140 --duration 20\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    const std::string profilePath = argv[1];
    const std::string inPath      = argv[2];
    const std::string outPath     = argv[3];
    double startSec = 0.0;
    double durationSec = -1.0;  // -1 means "to end"
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--start" || a == "-s") && i + 1 < argc) {
            startSec = std::atof(argv[++i]);
        } else if ((a == "--duration" || a == "-d") && i + 1 < argc) {
            durationSec = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 1;
        }
    }

    Profile profile;
    if (auto err = loadPeaceProfile(profilePath, profile); !err.empty()) {
        std::fprintf(stderr, "profile error: %s\n", err.c_str());
        return 2;
    }
    std::printf("profile: preamp = %+0.2f dB, %zu bands\n",
                profile.preampDb, profile.bands.size());

    WavFile wav;
    if (auto err = readWav(inPath, wav); !err.empty()) {
        std::fprintf(stderr, "input WAV error: %s\n", err.c_str());
        return 3;
    }
    std::printf("input:   %s, %u Hz, %u ch, %zu frames (%.2f sec)\n",
                formatName(wav.format), wav.sampleRate, wav.channels,
                wav.frames(), (double)wav.frames() / wav.sampleRate);

    if (wav.channels < 1 || wav.channels > 2) {
        std::fprintf(stderr, "unsupported channel count: %u (only mono and stereo for now)\n",
                     wav.channels);
        return 4;
    }

    // Apply --start / --duration trimming if requested.
    if (startSec > 0.0 || durationSec > 0.0) {
        const std::size_t startFrame = (std::size_t)(startSec * wav.sampleRate);
        std::size_t endFrame = wav.frames();
        if (durationSec > 0.0) {
            const std::size_t durFrames = (std::size_t)(durationSec * wav.sampleRate);
            if (startFrame + durFrames < endFrame) endFrame = startFrame + durFrames;
        }
        if (startFrame >= wav.frames()) {
            std::fprintf(stderr, "--start (%.2f s) is past end of file (%.2f s)\n",
                         startSec, (double)wav.frames() / wav.sampleRate);
            return 6;
        }
        const std::size_t firstSample = startFrame * wav.channels;
        const std::size_t lastSample  = endFrame  * wav.channels;
        wav.interleaved.erase(wav.interleaved.begin() + (std::ptrdiff_t)lastSample,
                              wav.interleaved.end());
        wav.interleaved.erase(wav.interleaved.begin(),
                              wav.interleaved.begin() + (std::ptrdiff_t)firstSample);
        std::printf("trimmed: %.2f s → %.2f s (%zu frames)\n",
                    startSec,
                    startSec + (double)wav.frames() / wav.sampleRate,
                    wav.frames());
    }

    BiquadChain chain;
    chain.setSampleRate((double)wav.sampleRate);
    chain.setChannelCount(wav.channels);
    chain.setBands(profile.bands);
    chain.setPreampDb(profile.preampDb);

    chain.processInterleaved(wav.interleaved.data(), wav.frames());

    if (auto err = writeWav(outPath, wav); !err.empty()) {
        std::fprintf(stderr, "output WAV error: %s\n", err.c_str());
        return 5;
    }
    std::printf("output:  %s (same format as input)\n", outPath.c_str());
    std::printf("done.\n");
    return 0;
}
