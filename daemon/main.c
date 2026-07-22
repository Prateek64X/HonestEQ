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
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOMessage.h>

#include "EqBridge.hpp"

// Command-line arg 1 (output device name) — kept in a global so device-
// availability listeners can re-resolve the device object ID if it changes
// (headphones unplug → replug scenarios).
static const char* gOutputDeviceName = NULL;

// The DSP engine. Instantiated at startup; live-swapped on SIGHUP
// (hot-reload of the active profile) and rebuilt on sample-rate changes.
//
// Atomic pointer: the audio thread reads it lock-free once per block, the
// main thread swaps a freshly-loaded bridge in via SIGHUP. Old bridges are
// freed after a brief settle delay so no in-flight audio block can still
// dereference them.
static _Atomic(EqBridge*) gEq = NULL;

// Path to the active profile file — remembered at startup so the SIGHUP
// handler knows what to re-read. Also the CLI overrides, so a hot-swap
// preserves them (a hot-swap without this would silently drop --preamp /
// --intensity flags after every reload).
static char   g_profile_path[1024];
static double g_preamp_override_db      = 0.0;
static bool   g_preamp_override_set     = false;
static double g_intensity_override      = 1.0;
static bool   g_intensity_override_set  = false;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// kSampleRate is set at startup from HonestEQ's current nominal rate,
// then applied to both AUHALs. AUHAL will internally resample between our
// rate and the real output device's rate if they differ.
static double gSampleRate = 48000.0;

#define kChannels         2
// 65536 frames = ~1.37 s at 48 kHz, ~341 ms at 192 kHz. Comfortably above
// the "target + jump threshold + one callback burst" worst case at every
// supported rate — ~15× safety margin at 192 kHz, more at lower rates.
// Static allocation: 512 KB. Same size as the driver-side ring.
#define kRingFrames       (65536)
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

// Ultra-low-latency mode.
// Reader stays this many frames behind writer in steady state.
// Floor: 128 frames (matches the AUHAL callback size we pin below), which is
// ~2.7 ms at 48 kHz. Higher rates scale up to maintain ~2.7 ms of headroom.
#define kAUHALFramesPerCallback (128)
#define kTargetOffsetFramesMin  (kAUHALFramesPerCallback)
#define kTargetOffsetSeconds    (128.0 / 48000.0)   // ~2.67 ms
// If reader falls behind writer by more than this, JUMP forward to
// (write - target) and drop the backlog. 4× target keeps jumps rare.
#define kJumpThresholdMultiple  (4)

static inline uint64_t compute_target_offset(void) {
    uint64_t by_time = (uint64_t)(gSampleRate * kTargetOffsetSeconds);
    return (by_time < kTargetOffsetFramesMin) ? kTargetOffsetFramesMin : by_time;
}

static void ring_reset(Ring* r) {
    memset(r->data, 0, sizeof(r->data));
    // No prime — reader and writer both start at 0. Reader emits silence
    // until writer produces data (a few ms at startup / after rate change),
    // then the catch-up logic in ring_read snaps reader to (writer - target).
    atomic_store(&r->write_frame, 0);
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

    // Catch-up: if reader is far behind writer (accumulated backlog from
    // startup, rate change, or transient stall), jump forward to always
    // play "current" audio. Target/threshold scale with sample rate.
    uint64_t target = compute_target_offset();
    uint64_t threshold = target * kJumpThresholdMultiple;
    if (w > rd && (w - rd) > threshold) {
        rd = w - target;
    }

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
    // Advance read_frame to (rd + frames), but never past the writer's
    // position. This is the key invariant: reader can lag writer (that's
    // where our target offset lives) but must never lead it. If we asked
    // for more frames than the ring had, the excess were played as
    // held-silence, and we simply stay at the writer's edge until it
    // produces more — instead of accumulating a permanent lead that
    // creates continuous underruns.
    uint64_t new_rd = rd + frames;
    if (new_rd > w) new_rd = w;
    atomic_store_explicit(&r->read_frame, new_rd, memory_order_relaxed);
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
// Debounced wrapper — coalesces bursty listener fires so we don't
// create multiple input AUHALs in quick succession (each one would
// otherwise trigger a fresh microphone permission dialog).
static void      schedule_debounced_rebuild(const char* reason);

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
    fprintf(stderr, "[rate] output device rate changed to %.1f Hz — scheduling rebuild.\n",
            gSampleRate);
    // Debounced so a plug/unplug cascade (which fires this listener PLUS
    // the device-list listener PLUS others) results in exactly one rebuild.
    schedule_debounced_rebuild("rate change");
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

    // Apply the EQ to the samples before they hit the ring. This keeps the
    // ring holding already-processed audio; the output side is pure playback.
    // Atomic snapshot: SIGHUP may hot-swap the bridge, but we hold this
    // pointer stable for the duration of this callback.
    EqBridge* eq_snapshot = atomic_load_explicit(&gEq, memory_order_acquire);
    eq_bridge_process_interleaved(eq_snapshot, scratch, frames);

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

    // Pin AUHAL's callback buffer size to kAUHALFramesPerCallback for
    // ultra-low-latency operation. If the device rejects it (e.g. can't go
    // that low), AUHAL falls back to its nearest supported size — we log
    // but don't fail. Then read back to confirm what we actually got.
    {
        UInt32 preferred = kAUHALFramesPerCallback;
        OSStatus s = AudioUnitSetProperty(unit, kAudioDevicePropertyBufferFrameSize,
                                          kAudioUnitScope_Global, 0,
                                          &preferred, sizeof(preferred));
        UInt32 actual = 0;
        UInt32 sz = sizeof(actual);
        AudioUnitGetProperty(unit, kAudioDevicePropertyBufferFrameSize,
                             kAudioUnitScope_Global, 0, &actual, &sz);
        if (s != noErr) {
            fprintf(stderr, "  (info) BufferFrameSize=%u refused (status=%d); AUHAL running at %u frames.\n",
                    preferred, (int)s, actual);
        } else {
            fprintf(stderr, "  BufferFrameSize on %s AUHAL = %u frames\n",
                    (dir == 1) ? "input" : "output", actual);
        }
    }

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

    // Push the new rate into the DSP so all biquad coefficients get
    // recomputed for the new Fs. Filter shape is preserved; the analytic
    // response at any Hz is unchanged, but the discrete coefficients change.
    {
        EqBridge* eq = atomic_load_explicit(&gEq, memory_order_acquire);
        if (eq) eq_bridge_set_sample_rate(eq, gSampleRate);
    }

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

// True when we've detected the target output device is currently absent
// (e.g. headphones unplugged). Declared here (near top of stability code)
// because both the debounce logic and the watchdog reference it.
static _Atomic bool gOutputDeviceMissing = false;

// ---------------------------------------------------------------------------
// Debounced rebuild.
//
// macOS fires many CoreAudio property listeners in cascade for a single
// physical event (headphone plug/unplug fires kAudioHardwarePropertyDevices,
// kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyName, and
// several others in ~50 ms). Each listener naively calling rebuild_pipeline()
// creates a fresh input AUHAL — and macOS shows the microphone permission
// dialog once per new input AUHAL if permission isn't persistently granted.
// Result: 6 "allow mic access" prompts per replug.
//
// Debounce coalesces every rebuild request within `kRebuildDebounceMs` into
// a single pipeline rebuild, so one physical event → one AUHAL creation →
// one prompt (at most, once ever if the user grants persistent permission).
// ---------------------------------------------------------------------------
#define kRebuildDebounceMs  300

static _Atomic bool gRebuildScheduled = false;

static void schedule_debounced_rebuild(const char* reason) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&gRebuildScheduled, &expected, true)) {
        // Rebuild already pending; coalesce this request into it.
        return;
    }
    fprintf(stderr, "[rebuild] scheduled in %d ms (reason: %s)\n",
            kRebuildDebounceMs, reason ? reason : "unspecified");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRebuildDebounceMs * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        atomic_store(&gRebuildScheduled, false);
        // Skip if device is legitimately missing — we'll rebuild on reconnect.
        if (atomic_load(&gOutputDeviceMissing)) {
            fprintf(stderr, "[rebuild] skipped — output device still missing.\n");
            return;
        }
        rebuild_pipeline();
    });
}

// ---------------------------------------------------------------------------
// Power management — sleep/wake handling (A).
//
// When macOS sleeps, coreaudiod stops running the audio graph and our AUHAL
// callbacks freeze mid-block. On wake, coreaudiod restarts but device object
// IDs may have shifted and AUHALs held references to state that no longer
// exists. Result without a handler: `AudioUnitRender` retry-storm, CPU spike
// (the "laptop lag"), no audio.
//
// Fix: register IOKit power-management notifications. On wake, wait for
// coreaudiod to settle (~500 ms), then rebuild both AUHALs cleanly against
// current device state.
// ---------------------------------------------------------------------------
static io_connect_t          gPMRootPort   = 0;
static IONotificationPortRef gPMNotifyPort = NULL;
static io_object_t           gPMNotifier   = 0;

static void power_callback(void* refcon, io_service_t service,
                           natural_t msg_type, void* msg_arg) {
    (void)refcon; (void)service;
    switch (msg_type) {
        case kIOMessageCanSystemSleep:
            // Grant sleep unconditionally (we're not blocking anything critical).
            IOAllowPowerChange(gPMRootPort, (long)msg_arg);
            break;
        case kIOMessageSystemWillSleep:
            fprintf(stderr, "[power] system will sleep — stopping AUHALs.\n");
            if (gInputUnit)  AudioOutputUnitStop(gInputUnit);
            if (gOutputUnit) AudioOutputUnitStop(gOutputUnit);
            IOAllowPowerChange(gPMRootPort, (long)msg_arg);
            break;
        case kIOMessageSystemHasPoweredOn:
            fprintf(stderr, "[power] system woke — rebuilding audio pipeline (debounced).\n");
            // Small delay so coreaudiod finishes its own recovery before we
            // start rebuilding against it. Then re-resolve the output device
            // by name (its ID may have shifted for USB/BT devices) and
            // schedule a debounced rebuild — coalesces with any device-list
            // events the wake will also generate, so we don't triple-prompt
            // for microphone access.
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                           dispatch_get_main_queue(), ^{
                if (gOutputDeviceName) {
                    CFStringRef nm = CFStringCreateWithCString(NULL, gOutputDeviceName,
                                                               kCFStringEncodingUTF8);
                    AudioObjectID fresh = find_device_by_name(nm);
                    CFRelease(nm);
                    if (fresh != kAudioObjectUnknown) gOutput_ID = fresh;
                }
                schedule_debounced_rebuild("wake");
            });
            break;
        default:
            break;
    }
}

static void setup_power_notifications(void) {
    gPMRootPort = IORegisterForSystemPower(NULL, &gPMNotifyPort,
                                            power_callback, &gPMNotifier);
    if (gPMRootPort == MACH_PORT_NULL) {
        fprintf(stderr, "WARNING: could not register for power notifications.\n");
        return;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(gPMNotifyPort),
                       kCFRunLoopCommonModes);
    fprintf(stderr, "Power notifications registered — daemon self-recovers on wake.\n");
}

// ---------------------------------------------------------------------------
// Device availability listener (B).
//
// Fires whenever the set of audio devices changes: user plugs/unplugs
// headphones, connects/disconnects USB DAC, joins/leaves BT, etc. We check
// whether OUR output device (headphones) is still present.
//
// Semantics for the three failure states the user might see:
//   1. Device disappeared  → gOutput_ID no longer in device list.
//                            Action: log, wait; on device return, rebuild.
//   2. Device volume = 0   → device present, kAudioDevicePropertyVolumeScalar
//                            reads 0. NOT a disconnect — daemon does nothing;
//                            silence is correct behavior. macOS UI handles.
//   3. No audio flowing    → device present, ring's write_frame counter isn't
//                            advancing. Handled by watchdog (D), not here.
// ---------------------------------------------------------------------------
static bool device_still_present(AudioObjectID id) {
    if (id == kAudioObjectUnknown) return false;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr) {
        return false;
    }
    UInt32 count = size / sizeof(AudioObjectID);
    AudioObjectID* devices = calloc(count, sizeof(AudioObjectID));
    if (!devices) return true;   // conservatively say present on OOM
    OSStatus s = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                            0, NULL, &size, devices);
    bool present = false;
    if (s == noErr) {
        for (UInt32 i = 0; i < count; ++i) {
            if (devices[i] == id) { present = true; break; }
        }
    }
    free(devices);
    return present;
}

static OSStatus device_list_changed(AudioObjectID id, UInt32 n,
                                    const AudioObjectPropertyAddress addrs[],
                                    void* user) {
    (void)id; (void)n; (void)addrs; (void)user;
    bool was_missing = atomic_load(&gOutputDeviceMissing);

    // First try to re-resolve by name — object IDs shift when devices are
    // disconnected and reconnected. `gOutput_ID` may point at stale state.
    AudioObjectID fresh = kAudioObjectUnknown;
    if (gOutputDeviceName) {
        CFStringRef nm = CFStringCreateWithCString(NULL, gOutputDeviceName,
                                                   kCFStringEncodingUTF8);
        fresh = find_device_by_name(nm);
        CFRelease(nm);
    }

    if (fresh == kAudioObjectUnknown || !device_still_present(fresh)) {
        // Device is gone.
        if (!was_missing) {
            fprintf(stderr, "[device] output '%s' disconnected — pausing pipeline.\n",
                    gOutputDeviceName ? gOutputDeviceName : "(unknown)");
            if (gOutputUnit) AudioOutputUnitStop(gOutputUnit);
            if (gInputUnit)  AudioOutputUnitStop(gInputUnit);
            atomic_store(&gOutputDeviceMissing, true);
        }
        return noErr;
    }

    // Device is present. If the ID changed or we were previously missing,
    // rebuild the pipeline against the fresh ID. Uses the debounced rebuild
    // so cascaded listener fires (a single plug/unplug can fire the device
    // list listener 3-6 times in quick succession) coalesce into ONE actual
    // rebuild — which means ONE input-AUHAL creation, which means at most
    // ONE microphone permission prompt.
    if (fresh != gOutput_ID || was_missing) {
        fprintf(stderr, "[device] output '%s' available (id=%u%s) — scheduling rebuild.\n",
                gOutputDeviceName ? gOutputDeviceName : "(unknown)",
                fresh,
                was_missing ? ", after reconnect" : "");
        gOutput_ID = fresh;
        atomic_store(&gOutputDeviceMissing, false);
        schedule_debounced_rebuild(was_missing ? "device reconnect" : "device id changed");
    }
    return noErr;
}

static void setup_device_availability_listener(void) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    OSStatus s = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                                device_list_changed, NULL);
    if (s != noErr) {
        fprintf(stderr, "WARNING: could not register device-list listener (status=%d).\n", (int)s);
    } else {
        fprintf(stderr, "Device availability listener registered — auto-pauses on unplug, resumes on replug.\n");
    }
}

// ---------------------------------------------------------------------------
// SIGHUP-triggered profile hot-swap.
//
// Reloads g_profile_path into a FRESH EqBridge, then atomically publishes it
// to the audio thread. The old bridge is destroyed after a short settle
// delay (comfortably longer than any single audio block currently in flight)
// so no reader can still be holding it.
//
// Audio does NOT stop during the swap — the biquad state resets to zero on
// the new bridge, but that discontinuity is inaudible (a few samples of
// filter warmup, ~1 ms). Zero pops, zero silence.
//
// Trigger from anywhere:  pkill -HUP honesteq-daemon
// ---------------------------------------------------------------------------
static void reload_active_profile(void) {
    EqBridge* fresh = eq_bridge_create(gSampleRate);
    if (!fresh) {
        fprintf(stderr, "[reload] failed to allocate new EQ bridge.\n");
        return;
    }
    if (eq_bridge_load_profile(fresh, g_profile_path) != 0 ||
        !eq_bridge_has_profile(fresh)) {
        fprintf(stderr, "[reload] could not parse %s — keeping current profile.\n",
                g_profile_path);
        eq_bridge_destroy(fresh);
        return;
    }
    // Re-apply CLI overrides so hot-swap doesn't silently lose them.
    if (g_preamp_override_set)    eq_bridge_set_preamp_db(fresh, g_preamp_override_db);
    if (g_intensity_override_set) eq_bridge_set_intensity(fresh, g_intensity_override);

    // Publish the new bridge to the audio thread.
    EqBridge* old = atomic_exchange_explicit(&gEq, fresh, memory_order_acq_rel);

    // Wait longer than any single audio block could plausibly be in flight
    // (128 frames = 2.7 ms at 48 kHz; 50 ms is a very safe upper bound
    // across all supported rates plus scheduler jitter).
    usleep(50 * 1000);
    if (old) eq_bridge_destroy(old);

    fprintf(stderr, "[reload] hot-swap complete — bands=%d, preamp=%+.2f dB (%s)\n",
            eq_bridge_band_count(fresh),
            eq_bridge_preamp_db(fresh),
            g_profile_path);
}

// ---------------------------------------------------------------------------
// Daemon state persistence.
//
// Persists the user's chosen output-device name (and later, other daemon-
// scoped preferences) to disk so a bare `honesteq-daemon` with no args on
// next boot resumes to the same device. Writes go to:
//   ~/Library/Application Support/HonestEQ/daemon-state.plist
//
// Priority order for resolving the target device at daemon startup:
//   1. CLI arg   `./honesteq-daemon "Some Device"`   (explicit override)
//   2. state.plist → outputDeviceName                (persisted choice)
//   3. macOS default output device (fallback, user gets a warning log)
//
// Writes debounce the same way the driver's state does, so a future UI
// button that scrubs the device list doesn't hammer the disk.
// ---------------------------------------------------------------------------

static char g_daemon_state_path[1024];
static _Atomic bool g_daemon_state_save_pending = false;

static void daemon_state_path_init(void) {
    const char* home = getenv("HOME");
    snprintf(g_daemon_state_path, sizeof(g_daemon_state_path),
             "%s/Library/Application Support/HonestEQ/daemon-state.plist",
             home ? home : "/tmp");
}

// Ensure the parent directory exists (idempotent).
static void daemon_state_ensure_dir(void) {
    const char* home = getenv("HOME");
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/HonestEQ",
             home ? home : "/tmp");
    mkdir(dir, 0755);
}

// Read the persisted device name into a caller-provided buffer.
// Returns true on success, false if no persisted value.
static bool daemon_state_load_output_device(char* out, size_t out_size) {
    if (g_daemon_state_path[0] == '\0') daemon_state_path_init();

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8*)g_daemon_state_path, strlen(g_daemon_state_path), false);
    if (!url) return false;

    CFReadStreamRef stream = CFReadStreamCreateWithFile(NULL, url);
    CFRelease(url);
    if (!stream || !CFReadStreamOpen(stream)) {
        if (stream) CFRelease(stream);
        return false;
    }

    CFPropertyListRef plist = CFPropertyListCreateWithStream(
        NULL, stream, 0, kCFPropertyListImmutable, NULL, NULL);
    CFReadStreamClose(stream);
    CFRelease(stream);
    if (!plist) return false;
    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        CFRelease(plist);
        return false;
    }

    CFDictionaryRef dict = (CFDictionaryRef)plist;
    CFStringRef name = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("outputDeviceName"));
    bool ok = false;
    if (name && CFGetTypeID(name) == CFStringGetTypeID()) {
        if (CFStringGetCString(name, out, (CFIndex)out_size, kCFStringEncodingUTF8)) {
            ok = true;
        }
    }
    CFRelease(plist);
    return ok;
}

// Serialize the current device name to disk. Atomic write via .tmp + rename.
static void daemon_state_write_synchronous(void) {
    if (g_daemon_state_path[0] == '\0') daemon_state_path_init();
    if (gOutputDeviceName == NULL || gOutputDeviceName[0] == '\0') return;

    daemon_state_ensure_dir();

    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!dict) return;
    CFStringRef name = CFStringCreateWithCString(
        NULL, gOutputDeviceName, kCFStringEncodingUTF8);
    if (name) {
        CFDictionarySetValue(dict, CFSTR("outputDeviceName"), name);
        CFRelease(name);
    }
    CFDataRef data = CFPropertyListCreateData(
        NULL, dict, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    CFRelease(dict);
    if (!data) return;

    char tmp_path[1200];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_daemon_state_path);
    FILE* f = fopen(tmp_path, "wb");
    if (f) {
        fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), f);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        rename(tmp_path, g_daemon_state_path);
    }
    CFRelease(data);
}

// Debounced save: coalesces bursty updates. Same pattern as rebuild debounce.
static void daemon_state_mark_dirty(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_daemon_state_save_pending, &expected, true)) {
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        atomic_store(&g_daemon_state_save_pending, false);
        daemon_state_write_synchronous();
    });
}

// Flush on clean shutdown so a last-second change isn't lost.
static void daemon_state_flush_at_exit(void) {
    if (atomic_load(&g_daemon_state_save_pending)) {
        daemon_state_write_synchronous();
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    daemon_state_path_init();
    atexit(daemon_state_flush_at_exit);

    // Optional CLI overrides for preamp and intensity.
    double preamp_override_db  = 0.0;
    bool   preamp_override_set = false;
    double intensity_override  = 1.0;
    bool   intensity_set       = false;

    // Parse args starting at index 1 — first positional non-flag arg (if any)
    // is treated as the output device name. Flags (--preamp / --intensity)
    // can appear anywhere. Everything is optional now: if no device is given,
    // we fall back to the persisted state, then to the macOS default output.
    const char* cli_device = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--preamp") == 0 && i + 1 < argc) {
            preamp_override_db = atof(argv[++i]);
            preamp_override_set = true;
        } else if (strcmp(argv[i], "--intensity") == 0 && i + 1 < argc) {
            intensity_override = atof(argv[++i]);
            intensity_set = true;
        } else if (argv[i][0] != '-') {
            // First positional arg wins as device name; ignore later ones.
            if (cli_device == NULL) cli_device = argv[i];
        }
    }

    // Resolve target device: CLI arg → persisted state → macOS default.
    // Note: static buffer so gOutputDeviceName can point at it for the
    // process lifetime (device-availability + power listeners read it).
    static char resolved_device_name[256];
    if (cli_device) {
        snprintf(resolved_device_name, sizeof(resolved_device_name), "%s", cli_device);
    } else if (daemon_state_load_output_device(resolved_device_name,
                                               sizeof(resolved_device_name))) {
        fprintf(stderr, "Using persisted output device: '%s'\n", resolved_device_name);
    } else {
        // Neither CLI nor state — show usage and exit so the user picks.
        fprintf(stderr,
            "usage: %s [<output-device-name>] [--preamp DB] [--intensity FRACTION]\n"
            "  <output-device-name>  target output device (optional if a persisted\n"
            "                        state file exists at\n"
            "                        ~/Library/Application Support/HonestEQ/daemon-state.plist)\n"
            "  --preamp DB           override profile's preamp (dB, e.g. -6)\n"
            "  --intensity FRACTION  scale every band gain, default 1.0 = 100%%\n"
            "                        0.5 = half strength, 0.0 = passthrough, 1.5 = 50%% stronger\n\n"
            "  e.g. %s \"External Headphones\"\n"
            "       %s \"External Headphones\" --preamp -6\n"
            "       %s \"External Headphones\" --intensity 0.7\n\n",
            argv[0], argv[0], argv[0], argv[0]);
        list_devices();
        return 1;
    }
    // Remember the target device name so device-availability + power
    // callbacks can re-resolve it after disconnect/wake.
    gOutputDeviceName = resolved_device_name;

    // Persist the resolved choice (debounced) so a bare `honesteq-daemon`
    // on next start reuses it. If it hasn't changed, this is essentially a
    // no-op (the debounce fires 500ms after the last change and the disk
    // write is small).
    daemon_state_mark_dirty();

    // Find HonestEQ (the source of audio we consume).
    AudioObjectID honesteq = find_device_by_uid(kHonestEQ_UID);
    if (honesteq == kAudioObjectUnknown) {
        fprintf(stderr, "ERROR: HonestEQ virtual device not found. "
                        "Is the driver installed at /Library/Audio/Plug-Ins/HAL/?\n");
        return 2;
    }
    fprintf(stderr, "HonestEQ device found (id=%u)\n", honesteq);

    // Find the real output device by name from the resolved name.
    // Startup retry loop — cover two real-world cases:
    //   (a) launchd loads us at login before coreaudiod has fully enumerated
    //       the device topology → transient "not found" that resolves in
    //       ~1-3 s. Without retry we'd bail here and enter a launchd
    //       restart loop.
    //   (b) User's headphones happen to be unplugged when the daemon starts
    //       (came back from sleep, computer moved) → device appears within
    //       tens of seconds when re-plugged. Waiting gracefully is better
    //       UX than exit + relaunch.
    //
    // Poll every 500 ms for up to 30 s. If still not found, exit(3) —
    // launchd's own 3 s throttle then handles the retry across restart.
    CFStringRef target = CFStringCreateWithCString(NULL, gOutputDeviceName, kCFStringEncodingUTF8);
    AudioObjectID output_dev = find_device_by_name(target);
    if (output_dev == kAudioObjectUnknown) {
        fprintf(stderr, "Output device '%s' not present yet — waiting up to 30 s...\n",
                gOutputDeviceName);
        for (int attempt = 0; attempt < 60; ++attempt) {   // 60 × 500 ms = 30 s
            usleep(500 * 1000);
            output_dev = find_device_by_name(target);
            if (output_dev != kAudioObjectUnknown) {
                fprintf(stderr, "  ...appeared after %.1f s.\n",
                        (double)(attempt + 1) * 0.5);
                break;
            }
        }
    }
    if (output_dev == kAudioObjectUnknown) {
        fprintf(stderr, "ERROR: no device named '%s' after 30 s wait.\n\n",
                gOutputDeviceName);
        list_devices();
        CFRelease(target);
        return 3;
    }
    CFRelease(target);
    fprintf(stderr, "Output device '%s' found (id=%u)\n", gOutputDeviceName, output_dev);

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
            honesteq_rate, gOutputDeviceName, out_rate);

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

    // --- DSP setup -------------------------------------------------------
    // Instantiate the EQ engine and load a profile.
    // Default location: ~/Library/Application Support/HonestEQ/active_profile.txt
    // If missing, daemon runs as passthrough (still applies rate-following
    // and clean routing, just no filtering).
    EqBridge* initial_eq = eq_bridge_create(gSampleRate);
    if (initial_eq == NULL) {
        fprintf(stderr, "ERROR: failed to create EQ engine.\n");
        return 4;
    }
    const char* home = getenv("HOME");
    snprintf(g_profile_path, sizeof(g_profile_path),
             "%s/Library/Application Support/HonestEQ/active_profile.txt",
             home ? home : "/tmp");
    if (eq_bridge_load_profile(initial_eq, g_profile_path) == 0 &&
        eq_bridge_has_profile(initial_eq)) {
        fprintf(stderr, "EQ profile loaded: %s\n", g_profile_path);
        fprintf(stderr, "  Bands: %d, Preamp: %+.2f dB\n",
                eq_bridge_band_count(initial_eq), eq_bridge_preamp_db(initial_eq));
    } else {
        fprintf(stderr, "No EQ profile at %s — running passthrough.\n", g_profile_path);
        fprintf(stderr, "  Drop a Peace/Equalizer APO config there and restart the daemon.\n");
    }
    if (preamp_override_set) {
        eq_bridge_set_preamp_db(initial_eq, preamp_override_db);
        fprintf(stderr, "  Preamp overridden via --preamp: %+.2f dB\n", preamp_override_db);
        // Remember so hot-reload preserves it.
        g_preamp_override_db  = preamp_override_db;
        g_preamp_override_set = true;
    }
    if (intensity_set) {
        eq_bridge_set_intensity(initial_eq, intensity_override);
        fprintf(stderr, "  Intensity overridden via --intensity: %.2f (%.0f%%)\n",
                intensity_override, intensity_override * 100.0);
        g_intensity_override     = intensity_override;
        g_intensity_override_set = true;
    }
    // Publish to the audio thread. Must happen BEFORE AudioOutputUnitStart
    // so the input callback never sees a NULL bridge.
    atomic_store_explicit(&gEq, initial_eq, memory_order_release);
    // ---------------------------------------------------------------------
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

    // Stability handlers — sleep/wake (A), device availability (B).
    // The watchdog (D) is set up alongside the stats timer further down.
    setup_power_notifications();
    setup_device_availability_listener();

    if (eq_bridge_has_profile(initial_eq)) {
        fprintf(stderr, "HonestEQ daemon running — DSP active (%d bands, preamp %+0.2f dB).\n",
                eq_bridge_band_count(initial_eq), eq_bridge_preamp_db(initial_eq));
    } else {
        fprintf(stderr, "HonestEQ daemon running — passthrough (no profile loaded).\n");
    }
    fprintf(stderr, "Select 'HonestEQ' as system output; audio will come out of '%s'.\n"
                    "Press Ctrl-C to stop.\n\n", gOutputDeviceName);

    // Diagnostic thread: print ring stats every 2 seconds.
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                              2 * NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    static uint64_t prev_underruns = 0, prev_read = 0, prev_written = 0;
    // Watchdog (D): if no audio has flowed through the input side for
    // kWatchdogStallTimeoutTicks consecutive ticks (2 s each), the pipeline
    // is stuck (typically post-wake, mid-driver reload, or a CoreAudio hiccup
    // the other handlers didn't catch). We exit(2) and let launchd relaunch
    // us — cleanest possible recovery. If the output device is legitimately
    // missing (checked via gOutputDeviceMissing), we skip the watchdog since
    // paused-on-purpose isn't a bug.
    static const int kWatchdogStallTimeoutTicks = 5;   // 5 × 2 s = 10 s
    static int  watchdog_stall_ticks = 0;
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

        // --- Watchdog ---
        bool device_gone = atomic_load(&gOutputDeviceMissing);
        if (device_gone) {
            watchdog_stall_ticks = 0;   // paused-on-purpose, not stuck
        } else if (d_w == 0) {
            watchdog_stall_ticks++;
            fprintf(stderr, "[watchdog] no input frames this tick (%d/%d)\n",
                    watchdog_stall_ticks, kWatchdogStallTimeoutTicks);
            if (watchdog_stall_ticks >= kWatchdogStallTimeoutTicks) {
                fprintf(stderr, "[watchdog] pipeline stuck %d s — exiting for launchd to restart.\n",
                        kWatchdogStallTimeoutTicks * 2);
                fflush(stderr);
                _exit(2);
            }
        } else {
            watchdog_stall_ticks = 0;
        }

        prev_underruns = u; prev_read = r; prev_written = w;
    });
    dispatch_resume(timer);

    // ---- SIGHUP → hot-swap active profile --------------------------------
    // libdispatch takes over signal delivery once SIGHUP is set to SIG_IGN;
    // the block below runs on a background dispatch queue (NOT the audio
    // thread), so file IO in reload_active_profile is safe.
    //
    // Trigger from anywhere with:  pkill -HUP honesteq-daemon
    // A/B two profiles instantly:
    //   cp profiles/A.txt ~/Library/Application\ Support/HonestEQ/active_profile.txt
    //   pkill -HUP honesteq-daemon
    //   # ...listen...
    //   cp profiles/B.txt ~/Library/Application\ Support/HonestEQ/active_profile.txt
    //   pkill -HUP honesteq-daemon
    signal(SIGHUP, SIG_IGN);
    dispatch_source_t sig_src = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, SIGHUP, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_event_handler(sig_src, ^{ reload_active_profile(); });
    dispatch_resume(sig_src);
    fprintf(stderr, "SIGHUP handler installed — `pkill -HUP honesteq-daemon` hot-reloads the active profile.\n");

    // Run forever — CoreAudio handles the audio threads for us.
    CFRunLoopRun();
    return 0;
}
