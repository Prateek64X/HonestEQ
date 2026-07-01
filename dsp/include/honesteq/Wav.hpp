// HonestEQ — Wav.hpp
//
// Minimal but format-correct WAV file reader/writer.
// Supports the formats that matter for headphone EQ work:
//   - 16-bit signed PCM
//   - 24-bit signed PCM (packed)
//   - 32-bit signed PCM
//   - 32-bit IEEE float
//   - mono and stereo
//   - any sample rate the file declares (44.1, 48, 88.2, 96, 176.4, 192 kHz, etc.)
//   - both classic WAVE_FORMAT_PCM (code 1) and WAVE_FORMAT_EXTENSIBLE (code 0xFFFE)
//
// Reads into double[] (DSP-native), writes back from double[] preserving the
// original bit depth + channels + sample rate. TPDF dither is applied when
// quantising back to 16-bit; 24-bit and above pass through untouched.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace honesteq {

enum class WavSampleFormat : std::uint8_t {
    PCM16,
    PCM24,
    PCM32,
    Float32,
};

struct WavFile {
    std::uint32_t sampleRate = 48000;
    std::uint16_t channels = 2;
    WavSampleFormat format = WavSampleFormat::PCM16;
    std::vector<double> interleaved;   // length = frames * channels, range ~[-1, +1]

    std::size_t frames() const { return interleaved.size() / channels; }
};

namespace detail {

// Little-endian unaligned reads.
inline std::uint16_t rd16(const std::uint8_t* p) { return (std::uint16_t)(p[0] | (p[1] << 8)); }
inline std::uint32_t rd32(const std::uint8_t* p) {
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8)
         | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}
inline void wr16(std::uint8_t* p, std::uint16_t v) { p[0] = (std::uint8_t)v; p[1] = (std::uint8_t)(v >> 8); }
inline void wr32(std::uint8_t* p, std::uint32_t v) {
    p[0] = (std::uint8_t)v; p[1] = (std::uint8_t)(v >> 8);
    p[2] = (std::uint8_t)(v >> 16); p[3] = (std::uint8_t)(v >> 24);
}

// 24-bit signed read (little-endian, sign-extended into int32).
inline std::int32_t rd24s(const std::uint8_t* p) {
    std::int32_t v = (std::int32_t)((std::uint32_t)p[0]
                                 | ((std::uint32_t)p[1] << 8)
                                 | ((std::uint32_t)p[2] << 16));
    if (v & 0x800000) v |= (std::int32_t)0xFF000000;
    return v;
}
inline void wr24s(std::uint8_t* p, std::int32_t v) {
    p[0] = (std::uint8_t)(v & 0xFF);
    p[1] = (std::uint8_t)((v >> 8) & 0xFF);
    p[2] = (std::uint8_t)((v >> 16) & 0xFF);
}

}  // namespace detail

inline std::string readWav(const std::string& path, WavFile& out) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return "could not open input file: " + path;
    std::fseek(fp, 0, SEEK_END);
    long total = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (total < 44) { std::fclose(fp); return "file too small to be a WAV"; }
    std::vector<std::uint8_t> buf((std::size_t)total);
    if (std::fread(buf.data(), 1, (std::size_t)total, fp) != (std::size_t)total) {
        std::fclose(fp); return "short read on input WAV";
    }
    std::fclose(fp);

    using namespace detail;
    if (std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        return "not a RIFF/WAVE file";
    }

    std::size_t p = 12;
    bool gotFmt = false, gotData = false;
    std::uint16_t formatCode = 0, channels = 0, bitsPerSample = 0;
    std::uint32_t sampleRate = 0;
    const std::uint8_t* dataPtr = nullptr;
    std::uint32_t dataSize = 0;

    while (p + 8 <= buf.size()) {
        char id[5] = {(char)buf[p], (char)buf[p+1], (char)buf[p+2], (char)buf[p+3], 0};
        std::uint32_t sz = rd32(buf.data() + p + 4);
        p += 8;
        if (p + sz > buf.size()) return std::string("chunk '") + id + "' overruns file";

        if (std::strcmp(id, "fmt ") == 0) {
            if (sz < 16) return "fmt chunk too small";
            formatCode    = rd16(buf.data() + p + 0);
            channels      = rd16(buf.data() + p + 2);
            sampleRate    = rd32(buf.data() + p + 4);
            bitsPerSample = rd16(buf.data() + p + 14);
            if (formatCode == 0xFFFE) {
                // WAVE_FORMAT_EXTENSIBLE: read SubFormat GUID at offset 24.
                if (sz < 40) return "extensible fmt chunk too small";
                std::uint16_t subFormat = rd16(buf.data() + p + 24);
                formatCode = subFormat;  // 1 = PCM, 3 = float
            }
            gotFmt = true;
        } else if (std::strcmp(id, "data") == 0) {
            dataPtr = buf.data() + p;
            dataSize = sz;
            gotData = true;
        }
        // chunks are word-aligned
        p += sz + (sz & 1);
    }
    if (!gotFmt) return "missing 'fmt ' chunk";
    if (!gotData) return "missing 'data' chunk";
    if (channels == 0) return "zero channels in header";

    WavSampleFormat fmt;
    if (formatCode == 1 && bitsPerSample == 16) fmt = WavSampleFormat::PCM16;
    else if (formatCode == 1 && bitsPerSample == 24) fmt = WavSampleFormat::PCM24;
    else if (formatCode == 1 && bitsPerSample == 32) fmt = WavSampleFormat::PCM32;
    else if (formatCode == 3 && bitsPerSample == 32) fmt = WavSampleFormat::Float32;
    else {
        return "unsupported WAV format (only 16/24/32-bit PCM and 32-bit float supported)";
    }

    out.sampleRate = sampleRate;
    out.channels = channels;
    out.format = fmt;
    const std::size_t bytesPerSample = bitsPerSample / 8;
    const std::size_t totalSamples = dataSize / bytesPerSample;
    out.interleaved.resize(totalSamples);

    switch (fmt) {
        case WavSampleFormat::PCM16: {
            constexpr double scale = 1.0 / 32768.0;
            for (std::size_t i = 0; i < totalSamples; ++i) {
                std::int16_t s = (std::int16_t)rd16(dataPtr + i * 2);
                out.interleaved[i] = (double)s * scale;
            }
            break;
        }
        case WavSampleFormat::PCM24: {
            constexpr double scale = 1.0 / 8388608.0;
            for (std::size_t i = 0; i < totalSamples; ++i) {
                std::int32_t s = rd24s(dataPtr + i * 3);
                out.interleaved[i] = (double)s * scale;
            }
            break;
        }
        case WavSampleFormat::PCM32: {
            constexpr double scale = 1.0 / 2147483648.0;
            for (std::size_t i = 0; i < totalSamples; ++i) {
                std::int32_t s = (std::int32_t)rd32(dataPtr + i * 4);
                out.interleaved[i] = (double)s * scale;
            }
            break;
        }
        case WavSampleFormat::Float32: {
            for (std::size_t i = 0; i < totalSamples; ++i) {
                std::uint32_t bits = rd32(dataPtr + i * 4);
                float f;
                std::memcpy(&f, &bits, 4);
                out.interleaved[i] = (double)f;
            }
            break;
        }
    }
    return {};  // success
}

inline std::string writeWav(const std::string& path, const WavFile& w) {
    std::uint16_t bps = 16;
    switch (w.format) {
        case WavSampleFormat::PCM16:   bps = 16; break;
        case WavSampleFormat::PCM24:   bps = 24; break;
        case WavSampleFormat::PCM32:   bps = 32; break;
        case WavSampleFormat::Float32: bps = 32; break;
    }
    const std::uint16_t formatCode = (w.format == WavSampleFormat::Float32) ? 3 : 1;
    const std::uint32_t byteRate   = w.sampleRate * w.channels * (bps / 8);
    const std::uint16_t blockAlign = (std::uint16_t)(w.channels * (bps / 8));
    const std::uint32_t dataSize   = (std::uint32_t)(w.interleaved.size() * (bps / 8));
    const std::uint32_t riffSize   = 36 + dataSize;

    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return "could not open output file: " + path;

    using namespace detail;
    std::uint8_t hdr[44];
    std::memcpy(hdr,      "RIFF", 4);
    wr32(hdr + 4, riffSize);
    std::memcpy(hdr + 8,  "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    wr32(hdr + 16, 16);                       // fmt chunk size
    wr16(hdr + 20, formatCode);
    wr16(hdr + 22, w.channels);
    wr32(hdr + 24, w.sampleRate);
    wr32(hdr + 28, byteRate);
    wr16(hdr + 32, blockAlign);
    wr16(hdr + 34, bps);
    std::memcpy(hdr + 36, "data", 4);
    wr32(hdr + 40, dataSize);
    if (std::fwrite(hdr, 1, 44, fp) != 44) { std::fclose(fp); return "short write on WAV header"; }

    // Simple TPDF dither for 16-bit, none for higher bit depths.
    auto clamp = [](double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); };
    std::uint32_t rng = 0xCAFEBABEu;
    auto frand = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return ((double)(int32_t)rng) / 2147483648.0;  // [-1, +1)
    };

    const std::size_t N = w.interleaved.size();
    std::vector<std::uint8_t> outbuf(N * (bps / 8));

    switch (w.format) {
        case WavSampleFormat::PCM16: {
            for (std::size_t i = 0; i < N; ++i) {
                const double dither = (frand() - frand()) * (0.5 / 32768.0);  // TPDF
                double v = w.interleaved[i] * 32768.0 + dither * 32768.0;
                v = clamp(v, -32768.0, 32767.0);
                std::int16_t s = (std::int16_t)(v >= 0 ? v + 0.5 : v - 0.5);
                wr16(outbuf.data() + i * 2, (std::uint16_t)s);
            }
            break;
        }
        case WavSampleFormat::PCM24: {
            for (std::size_t i = 0; i < N; ++i) {
                double v = clamp(w.interleaved[i], -1.0, 1.0) * 8388607.0;
                std::int32_t s = (std::int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
                wr24s(outbuf.data() + i * 3, s);
            }
            break;
        }
        case WavSampleFormat::PCM32: {
            for (std::size_t i = 0; i < N; ++i) {
                double v = clamp(w.interleaved[i], -1.0, 1.0) * 2147483647.0;
                std::int64_t s64 = (std::int64_t)(v >= 0 ? v + 0.5 : v - 0.5);
                if (s64 >  2147483647LL) s64 =  2147483647LL;
                if (s64 < -2147483648LL) s64 = -2147483648LL;
                wr32(outbuf.data() + i * 4, (std::uint32_t)(std::int32_t)s64);
            }
            break;
        }
        case WavSampleFormat::Float32: {
            for (std::size_t i = 0; i < N; ++i) {
                float f = (float)w.interleaved[i];
                std::uint32_t bits;
                std::memcpy(&bits, &f, 4);
                wr32(outbuf.data() + i * 4, bits);
            }
            break;
        }
    }

    if (std::fwrite(outbuf.data(), 1, outbuf.size(), fp) != outbuf.size()) {
        std::fclose(fp);
        return "short write on WAV samples";
    }
    std::fclose(fp);
    return {};
}

}  // namespace honesteq
