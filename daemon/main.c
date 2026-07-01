// HonestEQ — companion daemon (M2, stage 1: passthrough).
//
// Reads audio from HonestEQ's loopback input stream, writes it to a real
// output device (name/UID given on the command line).  No DSP yet; that's
// stage 2.
//
// Signal path:
//
//   any app  →  HonestEQ output stream  →  (driver's ring buffer)
//                                       →  HonestEQ input stream  ← daemon reads
//                                       →  [ring buffer inside daemon]
//                                       →  daemon writes to real output AUHAL
//                                       →  user's actual headphones
//
// Two AudioUnits inside this daemon: one input-only (bound to HonestEQ),
// one output-only (bound to whatever the user asked for on the CLI).  A
// small lock-free SPSC ring buffer sits between them so the callbacks are
// independent.
//
// This runs at 48 kHz Float32 stereo interleaved (matches HonestEQ's ASBD).
// A later stage adds sample-rate-following for the real device.

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// kSampleRate is set at startup from HonestEQ's current nominal rate,
// then applied to both AUHALs. AUHAL will internally resample between our
// rate and the real output device's rate if they differ.
static double gSampleRate = 48000.0;

#define kChannels         2
// 524288 frames = ~10.9 s at 48 kHz, ~2.7 s at 192 kHz. Big enough that a
// 1-second prime always fits, and that clock drift at any supported rate
// won't wrap the ring for many seconds.  Static allocation: 4 MB.
#define kRingFrames       (524288)
#define kRingSamples      (kRingFrames * kChannels)
#define kHonestEQ_UID     CFSTR("HonestEQDevice_UID")

// Bail on non-zero OSStatus with a friendly message.
#define CHECK(call, msg)                                             \
    do {                                                             \
        OSStatus _s = (call);                                        \
        if (_s != noErr) {                                           \
            fprintf(stderr, "ERROR: %s (status = %d '%.4s')\n",      \
                    msg, (int)_s, (const char*)&_s);                 \
            exit(1);                                                 \
        }                                                            \
    } while (0)

// ---------------------------------------------------------------------------
// Ring buffer (SPSC, lock-free).
// Input callback is producer, output render callback is consumer.
// ---------------------------------------------------------------------------

typedef struct {
    float    data[kRingSamples];
    _Atomic uint64_t write_frame;   // next frame to write
    _Atomic uint64_t read_frame;    // next frame to read
    // Diagnostics.
    _Atomic uint64_t underrun_count;
    _Atomic uint64_t total_read;
    _Atomic uint64_t total_written;
    _Atomic uint32_t peak_in_x1e6;  // peak |sample| observed on input side, x1e6
    _Atomic uint32_t peak_out_x1e6; // peak |sample| observed on output side, x1e6
    float    last_l, last_r;        // last sample, held during underruns
} Ring;

static Ring gRing;

// How far behind the writer the reader starts.
// After a rate change we prime the ring with ~1 second of silence and let the
// writer fill it up before real audio comes out. If the writer takes a bit
// longer to stabilize (2-3 s), the reader plays silence — no glitches.
// The value scales with gSampleRate at rebuild time.
#define kInitialPrimeFrames_Base  (48000)   // ~1 second at 48 kHz

static void ring_reset(Ring* r) {
    memset(r->data, 0, sizeof(r->data));
    // Prime by ~1 second at current rate — reader plays silence while writer
    // fills the ring after a startup or rate change.
    uint64_t prime = (uint64_t)gSampleRate;  // 1 second's worth of frames
    if (prime < kInitialPrimeFrames_Base) prime = kInitialPrimeFrames_Base;
    if (prime > kRingFrames / 2) prime = kRingFrames / 2;
    atomic_store(&r->write_frame, prime);
    atomic_store(&r->read_frame,  0);
    atomic_store(&r->underrun_count, 0);
    atomic_store(&r->total_read, 0);
    atomic_store(&r->total_written, 0);
    atomic_store(&r->peak_in_x1e6, 0);
    atomic_store(&r->peak_out_x1e6, 0);
    r->last_l = 0.0f;
    r->last_r = 0.0f;
}

static void ring_write(Ring* r, const float* src, UInt32 frames) {
    uint64_t w = atomic_load_explicit(&r->write_frame, memory_order_relaxed);
    float peak = 0.0f;
    for (UInt32 i = 0; i < frames; ++i) {
        uint64_t idx = ((w + i) % kRingFrames) * kChannels;
        float l = src[i * kChannels + 0];
        float rr = src[i * kChannels + 1];
        r->data[idx + 0] = l;
        r->data[idx + 1] = rr;
        float al = l < 0 ? -l : l;
        float ar = rr < 0 ? -rr : rr;
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
    }
    // Store as parts-per-million so we can use an atomic uint32.
    uint32_t p = (uint32_t)(peak * 1000000.0f);
    uint32_t cur = atomic_load_explicit(&r->peak_in_x1e6, memory_order_relaxed);
    if (p > cur) atomic_store_explicit(&r->peak_in_x1e6, p, memory_order_relaxed);
    atomic_store_explicit(&r->write_frame, w + frames, memory_order_release);
    atomic_fetch_add_explicit(&r->total_written, frames, memory_order_relaxed);
}

static void ring_read(Ring* r, float* dst, UInt32 frames) {
    uint64_t rd = atomic_load_explicit(&r->read_frame,  memory_order_relaxed);
    uint64_t w  = atomic_load_explicit(&r->write_frame, memory_order_acquire);
    float last_l = r->last_l, last_r = r->last_r;
    float peak = 0.0f;
    for (UInt32 i = 0; i < frames; ++i) {
        if (rd + i >= w) {
            // Underrun: hold last sample (smoother than jumping to 0).
            dst[i * kChannels + 0] = last_l;
            dst[i * kChannels + 1] = last_r;
            atomic_fetch_add_explicit(&r->underrun_count, 1, memory_order_relaxed);
        } else {
            uint64_t idx = ((rd + i) % kRingFrames) * kChannels;
            last_l = r->data[idx + 0];
            last_r = r->data[idx + 1];
            dst[i * kChannels + 0] = last_l;
            dst[i * kChannels + 1] = last_r;
        }
        float al = last_l < 0 ? -last_l : last_l;
        float ar = last_r < 0 ? -last_r : last_r;
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
    }
    r->last_l = last_l;
    r->last_r = last_r;
    uint32_t p = (uint32_t)(peak * 1000000.0f);
    uint32_t cur = atomic_load_explicit(&r->peak_out_x1e6, memory_order_relaxed);
    if (p > cur) atomic_store_explicit(&r->peak_out_x1e6, p, memory_order_relaxed);
    atomic_store_explicit(&r->read_frame, rd + frames, memory_order_relaxed);
    atomic_fetch_add_explicit(&r->total_read, frames, memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Live rate-following.
// If the real output device's rate changes at runtime (macOS/Apple Music
// auto-negotiation, user changes AMS, etc.), we push the same rate onto
// HonestEQ so the whole pipeline stays single-rate (no SRC anywhere).
// ---------------------------------------------------------------------------

static AudioObjectID gHonestEQ_ID = kAudioObjectUnknown;
static AudioObjectID gOutput_ID   = kAudioObjectUnknown;

// Global AudioUnit handles so the rate-change listener can tear them down
// and recreate them when rates change. Rebuilt from scratch each time.
static AudioUnit gInputUnit  = NULL;
static AudioUnit gOutputUnit = NULL;

// Forward declarations for the rebuild-on-rate-change path.
static AudioUnit setup_auhal(AudioObjectID device_id, int dir);
static void      configure_input_callback(AudioUnit unit);
static void      configure_output_callback(AudioUnit unit);
static void      rebuild_pipeline(void);

static void sync_honesteq_rate_to_output(void) {
    if (gHonestEQ_ID == kAudioObjectUnknown || gOutput_ID == kAudioObjectUnknown) return;
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    Float64 out_rate = 0;
    UInt32 sz = sizeof(out_rate);
    if (AudioObjectGetPropertyData(gOutput_ID, &a, 0, NULL, &sz, &out_rate) != noErr) return;
    if (out_rate <= 0) return;
    Float64 heq_rate = 0;
    sz = sizeof(heq_rate);
    AudioObjectGetPropertyData(gHonestEQ_ID, &a, 0, NULL, &sz, &heq_rate);
    if (heq_rate != out_rate) {
        fprintf(stderr, "[rate] output device changed to %.1f Hz — updating HonestEQ from %.1f Hz.\n",
                out_rate, heq_rate);
        OSStatus s = AudioObjectSetPropertyData(gHonestEQ_ID, &a, 0, NULL,
                                                sizeof(out_rate), &out_rate);
        if (s != noErr) {
            fprintf(stderr, "  [rate] set failed (status=%d).\n", (int)s);
            return;
        }
        // Wait briefly for coreaudiod to propagate the change.
        usleep(150000);
    }
    if (gSampleRate == out_rate) return;
    gSampleRate = out_rate;
    fprintf(stderr, "[rate] rebuilding AUHAL pipeline at %.1f Hz...\n", gSampleRate);
    rebuild_pipeline();
}

static OSStatus rate_change_listener(AudioObjectID id, UInt32 n,
                                     const AudioObjectPropertyAddress addrs[],
                                     void* user) {
    (void)id; (void)n; (void)addrs; (void)user;
    sync_honesteq_rate_to_output();
    return noErr;
}

// ---------------------------------------------------------------------------
// Device lookup
// ---------------------------------------------------------------------------

static AudioObjectID find_device_by_uid(CFStringRef uid) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof(id);
    OSStatus s = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                            sizeof(uid), &uid, &size, &id);
    if (s != noErr) return kAudioObjectUnknown;
    return id;
}

static AudioObjectID find_device_by_name(CFStringRef target_name) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    CHECK(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size),
          "get devices size");
    UInt32 count = size / sizeof(AudioObjectID);
    AudioObjectID* devices = calloc(count, sizeof(AudioObjectID));
    CHECK(AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devices),
          "get devices");

    AudioObjectID match = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; ++i) {
        AudioObjectPropertyAddress name_addr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        CFStringRef name = NULL;
        UInt32 name_size = sizeof(name);
        OSStatus s = AudioObjectGetPropertyData(devices[i], &name_addr,
                                                0, NULL, &name_size, &name);
        if (s == noErr && name != NULL) {
            if (CFStringCompare(name, target_name, kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
                match = devices[i];
                CFRelease(name);
                break;
            }
            CFRelease(name);
        }
    }
    free(devices);
    return match;
}

// Prints the list of devices macOS sees, useful when the user gives a name
// that doesn't match anything.
static void list_devices(void) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size);
    UInt32 count = size / sizeof(AudioObjectID);
    AudioObjectID* devices = calloc(count, sizeof(AudioObjectID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devices);
    fprintf(stderr, "Available audio devices:\n");
    for (UInt32 i = 0; i < count; ++i) {
        AudioObjectPropertyAddress na = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        CFStringRef n = NULL;
        UInt32 ns = sizeof(n);
        if (AudioObjectGetPropertyData(devices[i], &na, 0, NULL, &ns, &n) == noErr && n) {
            char buf[256];
            CFStringGetCString(n, buf, sizeof(buf), kCFStringEncodingUTF8);
            fprintf(stderr, "  %s\n", buf);
            CFRelease(n);
        }
    }
    free(devices);
}

// ---------------------------------------------------------------------------
// AudioUnit setup
// ---------------------------------------------------------------------------

static AudioStreamBasicDescription make_asbd(void) {
    AudioStreamBasicDescription a = {0};
    a.mSampleRate       = gSampleRate;
    a.mFormatID         = kAudioFormatLinearPCM;
    a.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian
                        | kAudioFormatFlagIsPacked;
    a.mBytesPerPacket   = kChannels * sizeof(float);
    a.mFramesPerPacket  = 1;
    a.mBytesPerFrame    = kChannels * sizeof(float);
    a.mChannelsPerFrame = kChannels;
    a.mBitsPerChannel   = 32;
    return a;
}

// Debug recording of the input side, so we can hear what the daemon receives.
// The first ~15 seconds of audio pulled from HonestEQ are saved to /tmp/honesteq_daemon_in.wav.
// Buffer sized for 16s at up to 192 kHz stereo — worst case ~24 MB static allocation.
#define kDumpSecondsLimit  15
#define kDumpBufferFrames  (192000 * (kDumpSecondsLimit + 1))
static float    gDumpBuffer[kDumpBufferFrames * kChannels];
static uint64_t gDumpFramesWritten = 0;
static uint64_t gDumpFramesLimit = 0;   // set at startup from gSampleRate

static void write_dump_wav(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t sr = (uint32_t)gSampleRate;
    uint32_t data_bytes = (uint32_t)(gDumpFramesWritten * kChannels * sizeof(float));
    uint32_t riff_size = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt_size = 16;
    uint16_t format = 3;              // IEEE float
    uint16_t channels = kChannels;
    uint16_t bps = 32;
    uint32_t byte_rate = sr * kChannels * (bps / 8);
    uint16_t block_align = kChannels * (bps / 8);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sr, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    fwrite(gDumpBuffer, 1, data_bytes, f);
    fclose(f);
    fprintf(stderr, "[dump] wrote %llu frames (%.2f s) to %s\n",
            (unsigned long long)gDumpFramesWritten,
            (double)gDumpFramesWritten / (double)sr, path);
}

// Input callback — the input AudioUnit calls this when new audio is available
// from HonestEQ.  We fetch it via AudioUnitRender and push into the ring.
static OSStatus input_callback(void* user_data,
                               AudioUnitRenderActionFlags* flags,
                               const AudioTimeStamp* ts,
                               UInt32 bus,
                               UInt32 frames,
                               AudioBufferList* io_data) {
    AudioUnit input_unit = *(AudioUnit*)user_data;

    // Allocate a stack buffer for the pulled frames.
    float scratch[4096 * kChannels];
    if (frames > 4096) frames = 4096;

    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = kChannels;
    list.mBuffers[0].mDataByteSize   = frames * kChannels * sizeof(float);
    list.mBuffers[0].mData           = scratch;

    OSStatus s = AudioUnitRender(input_unit, flags, ts, bus, frames, &list);
    if (s != noErr) return s;

    // Diagnostic tap: save the first ~15 seconds to a WAV for offline inspection.
    if (gDumpFramesLimit > 0 && gDumpFramesWritten < gDumpFramesLimit) {
        UInt64 space = gDumpFramesLimit - gDumpFramesWritten;
        UInt64 to_copy = (frames < space) ? frames : space;
        memcpy(&gDumpBuffer[gDumpFramesWritten * kChannels], scratch,
               (size_t)to_copy * kChannels * sizeof(float));
        gDumpFramesWritten += to_copy;
        if (gDumpFramesWritten >= gDumpFramesLimit) {
            write_dump_wav("/tmp/honesteq_daemon_in.wav");
        }
    }

    ring_write(&gRing, scratch, frames);
    return noErr;
}

// Output render callback — the output AudioUnit calls this to get audio to
// play on the real device.  We serve from the ring.
static OSStatus output_render(void* user_data,
                              AudioUnitRenderActionFlags* flags,
                              const AudioTimeStamp* ts,
                              UInt32 bus,
                              UInt32 frames,
                              AudioBufferList* io_data) {
    float* dst = (float*)io_data->mBuffers[0].mData;
    ring_read(&gRing, dst, frames);
    return noErr;
}

// Bind an AUHAL unit to a specific device in a specific direction.
// dir = 0 for output, 1 for input.
static AudioUnit setup_auhal(AudioObjectID device_id, int dir) {
    AudioComponentDescription desc = {
        .componentType         = kAudioUnitType_Output,
        .componentSubType      = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioUnit unit;
    CHECK(AudioComponentInstanceNew(comp, &unit), "AudioComponentInstanceNew");

    UInt32 enable = 1, disable = 0;
    if (dir == 1) {
        // Enable input bus (bus 1), disable output bus.
        CHECK(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Input, 1, &enable, sizeof(enable)),
              "enable input bus");
        CHECK(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Output, 0, &disable, sizeof(disable)),
              "disable output bus (for input unit)");
    }

    CHECK(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0, &device_id, sizeof(device_id)),
          "bind device");

    AudioStreamBasicDescription asbd = make_asbd();
    if (dir == 1) {
        // Format that we want AudioUnitRender to hand us.
        CHECK(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Output, 1, &asbd, sizeof(asbd)),
              "set input unit output-scope format");
    } else {
        CHECK(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input, 0, &asbd, sizeof(asbd)),
              "set output unit input-scope format");
    }
    return unit;
}

// Wire up callbacks after setup_auhal returns a unit.
static void configure_input_callback(AudioUnit unit) {
    AURenderCallbackStruct cb = { .inputProc = input_callback,
                                  .inputProcRefCon = &gInputUnit };
    CHECK(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0, &cb, sizeof(cb)),
          "set input callback");
}

static void configure_output_callback(AudioUnit unit) {
    AURenderCallbackStruct cb = { .inputProc = output_render,
                                  .inputProcRefCon = NULL };
    CHECK(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof(cb)),
          "set render callback");
}

// Tear down and rebuild both AUHALs at the current gSampleRate.
// Called by the rate-change listener when the output device rate changes.
static void rebuild_pipeline(void) {
    if (gInputUnit) {
        AudioOutputUnitStop(gInputUnit);
        AudioUnitUninitialize(gInputUnit);
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = NULL;
    }
    if (gOutputUnit) {
        AudioOutputUnitStop(gOutputUnit);
        AudioUnitUninitialize(gOutputUnit);
        AudioComponentInstanceDispose(gOutputUnit);
        gOutputUnit = NULL;
    }
    // Reset the ring so no stale samples from the old rate get consumed.
    ring_reset(&gRing);
    gDumpFramesWritten = 0;
    gDumpFramesLimit   = (uint64_t)(gSampleRate * kDumpSecondsLimit);

    // Rebuild fresh at the new rate. make_asbd() reads gSampleRate.
    gInputUnit  = setup_auhal(gHonestEQ_ID, 1);
    configure_input_callback(gInputUnit);
    gOutputUnit = setup_auhal(gOutput_ID, 0);
    configure_output_callback(gOutputUnit);

    if (AudioUnitInitialize(gInputUnit)  != noErr ||
        AudioUnitInitialize(gOutputUnit) != noErr ||
        AudioOutputUnitStart(gInputUnit)  != noErr ||
        AudioOutputUnitStart(gOutputUnit) != noErr) {
        fprintf(stderr, "[rate] AUHAL rebuild failed.\n");
        return;
    }
    fprintf(stderr, "[rate] pipeline rebuilt at %.1f Hz.\n", gSampleRate);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr,
            "usage: %s <output-device-name>\n"
            "  e.g. %s \"External Headphones\"\n\n",
            argv[0], argv[0]);
        list_devices();
        return 1;
    }

    // Find HonestEQ (the source of audio we consume).
    AudioObjectID honesteq = find_device_by_uid(kHonestEQ_UID);
    if (honesteq == kAudioObjectUnknown) {
        fprintf(stderr, "ERROR: HonestEQ virtual device not found. "
                        "Is the driver installed at /Library/Audio/Plug-Ins/HAL/?\n");
        return 2;
    }
    fprintf(stderr, "HonestEQ device found (id=%u)\n", honesteq);

    // Find the real output device by name from the CLI arg.
    CFStringRef target = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    AudioObjectID output_dev = find_device_by_name(target);
    if (output_dev == kAudioObjectUnknown) {
        fprintf(stderr, "ERROR: no device named '%s'.\n\n", argv[1]);
        list_devices();
        CFRelease(target);
        return 3;
    }
    CFRelease(target);
    fprintf(stderr, "Output device '%s' found (id=%u)\n", argv[1], output_dev);

    // Query both devices' current rates.
    Float64 honesteq_rate = 0, out_rate = 0;
    {
        AudioObjectPropertyAddress a = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        UInt32 sz = sizeof(Float64);
        AudioObjectGetPropertyData(honesteq,   &a, 0, NULL, &sz, &honesteq_rate);
        sz = sizeof(Float64);
        AudioObjectGetPropertyData(output_dev, &a, 0, NULL, &sz, &out_rate);
    }

    // Remember the IDs so the rate-change listener can find them.
    gHonestEQ_ID = honesteq;
    gOutput_ID   = output_dev;

    fprintf(stderr, "Initial rates: HonestEQ = %.1f Hz, %s = %.1f Hz\n",
            honesteq_rate, argv[1], out_rate);

    // Force HonestEQ to match the output device's current rate.
    // This makes the entire pipeline single-rate → no SRC anywhere → bit-perfect.
    // Works for any rate the output device is at (44.1 / 48 / 96 / 192 kHz).
    if (out_rate > 0 && out_rate != honesteq_rate) {
        AudioObjectPropertyAddress a = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        fprintf(stderr, "Forcing HonestEQ to %.1f Hz (matches output device)...\n", out_rate);
        OSStatus s = AudioObjectSetPropertyData(honesteq, &a, 0, NULL,
                                                sizeof(Float64), &out_rate);
        if (s != noErr) {
            fprintf(stderr, "  WARNING: could not set HonestEQ rate (status=%d). Continuing anyway.\n", (int)s);
        } else {
            // Give coreaudiod a moment to propagate the change, then re-query.
            usleep(200000);
            UInt32 sz = sizeof(Float64);
            AudioObjectGetPropertyData(honesteq, &a, 0, NULL, &sz, &honesteq_rate);
            fprintf(stderr, "  HonestEQ now at %.1f Hz.\n", honesteq_rate);
        }
    }

    // Daemon runs at whatever rate HonestEQ ended up at (which equals output rate).
    if (honesteq_rate > 0) gSampleRate = honesteq_rate;
    else if (out_rate > 0) gSampleRate = out_rate;

    fprintf(stderr, "Daemon internal rate: %.1f Hz. No SRC anywhere in the pipeline.\n",
            gSampleRate);

    // Set the 15-second dump limit based on the actual sample rate.
    gDumpFramesLimit = (uint64_t)(gSampleRate * kDumpSecondsLimit);
    if ((Float64)gSampleRate != out_rate) {
        fprintf(stderr, "  Note: rates differ — AUHAL will resample internally.\n");
    }

    ring_reset(&gRing);

    // Set up input AUHAL (pulls from HonestEQ).
    gInputUnit = setup_auhal(honesteq, 1);
    configure_input_callback(gInputUnit);

    // Set up output AUHAL (writes to real device).
    gOutputUnit = setup_auhal(output_dev, 0);
    configure_output_callback(gOutputUnit);

    CHECK(AudioUnitInitialize(gInputUnit),  "init input unit");
    CHECK(AudioUnitInitialize(gOutputUnit), "init output unit");

    // Diagnostic: print the actual formats AUHAL settled on (may differ
    // from what we requested if the device only supports specific formats).
    #define DUMP_ASBD(unit, scope, bus, name)                                        \
        do {                                                                         \
            AudioStreamBasicDescription _a;                                          \
            UInt32 _sz = sizeof(_a);                                                 \
            OSStatus _s = AudioUnitGetProperty((unit),                               \
                kAudioUnitProperty_StreamFormat, (scope), (bus), &_a, &_sz);         \
            if (_s == noErr) {                                                       \
                fprintf(stderr, "  %-42s: %.1f Hz, %u ch, %u bits, flags=0x%x\n",    \
                    name, _a.mSampleRate, _a.mChannelsPerFrame,                      \
                    _a.mBitsPerChannel, (unsigned)_a.mFormatFlags);                  \
            }                                                                        \
        } while (0)
    fprintf(stderr, "Actual AUHAL formats after init:\n");
    DUMP_ASBD(gInputUnit,  kAudioUnitScope_Input,  1, "input unit  bus 1 input  (from device)");
    DUMP_ASBD(gInputUnit,  kAudioUnitScope_Output, 1, "input unit  bus 1 output (to us)");
    DUMP_ASBD(gOutputUnit, kAudioUnitScope_Input,  0, "output unit bus 0 input  (from us)");
    DUMP_ASBD(gOutputUnit, kAudioUnitScope_Output, 0, "output unit bus 0 output (to device)");

    CHECK(AudioOutputUnitStart(gInputUnit),  "start input unit");
    CHECK(AudioOutputUnitStart(gOutputUnit), "start output unit");

    // Register a live listener on the OUTPUT device's rate. If macOS / Apple
    // Music / etc. changes the External Headphones rate (44.1 ↔ 48 ↔ 192 kHz),
    // we mirror that onto HonestEQ so the whole pipeline stays single-rate.
    {
        AudioObjectPropertyAddress a = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        OSStatus s = AudioObjectAddPropertyListener(output_dev, &a,
                                                    rate_change_listener, NULL);
        if (s != noErr) {
            fprintf(stderr, "WARNING: could not register rate-change listener (status=%d)\n", (int)s);
        } else {
            fprintf(stderr, "Live rate-following: enabled (HonestEQ will mirror output device rate).\n");
        }
    }

    fprintf(stderr, "HonestEQ daemon running (stage 1: passthrough, no DSP yet).\n"
                    "Select 'HonestEQ' as system output; audio will come out of '%s'.\n"
                    "Press Ctrl-C to stop.\n\n", argv[1]);

    // Diagnostic thread: print ring stats every 2 seconds.
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                              2 * NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    static uint64_t prev_underruns = 0, prev_read = 0, prev_written = 0;
    dispatch_source_set_event_handler(timer, ^{
        uint64_t u = atomic_load(&gRing.underrun_count);
        uint64_t r = atomic_load(&gRing.total_read);
        uint64_t w = atomic_load(&gRing.total_written);
        uint64_t rd_frame = atomic_load(&gRing.read_frame);
        uint64_t wr_frame = atomic_load(&gRing.write_frame);
        int64_t  fill = (int64_t)wr_frame - (int64_t)rd_frame;
        uint32_t pi = atomic_exchange(&gRing.peak_in_x1e6,  0);
        uint32_t po = atomic_exchange(&gRing.peak_out_x1e6, 0);
        // Counters get reset to 0 when the pipeline rebuilds; guard the
        // subtraction against underflow so the log stays readable.
        uint64_t d_u = (u >= prev_underruns) ? (u - prev_underruns) : 0;
        uint64_t d_r = (r >= prev_read)      ? (r - prev_read)      : r;
        uint64_t d_w = (w >= prev_written)   ? (w - prev_written)   : w;
        fprintf(stderr, "[stats] fill=%+lld u=+%llu r=+%llu w=+%llu peak_in=%.4f peak_out=%.4f\n",
                (long long)fill,
                (unsigned long long)d_u,
                (unsigned long long)d_r,
                (unsigned long long)d_w,
                (double)pi / 1000000.0,
                (double)po / 1000000.0);
        prev_underruns = u; prev_read = r; prev_written = w;
    });
    dispatch_resume(timer);

    // Run forever — CoreAudio handles the audio threads for us.
    CFRunLoopRun();
    return 0;
}
