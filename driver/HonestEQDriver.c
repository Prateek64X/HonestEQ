// HonestEQ — AudioServerPlugIn (pass 2: property model)
//
// Loaded by coreaudiod from:
//   /Library/Audio/Plug-Ins/HAL/HonestEQ.driver/Contents/MacOS/HonestEQ
//
// Pass 1: factory + vtable + lifecycle  (done)
// Pass 2: property model — name, sample rates, stream format, volume, mute   <-- this file
// Pass 3: IO ring buffer + companion-app IPC + actual passthrough audio
//
// After installing pass 2, "HonestEQ" appears in System Settings → Sound →
// Output. Selecting it will route audio through the driver, but the audio
// is dropped on the floor (no DSP, no destination) until pass 3 wires up
// the ring buffer to the app.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

// AudioServerPlugIn.h doesn't expose the buffer-frame-size property selectors
// (they live in the client-side AudioHardware.h, which we can't include in a
// driver context). Apple's canonical four-char codes are stable — define
// them manually so AUHAL clients can query/set them on our device.
#ifndef kAudioDevicePropertyBufferFrameSize
#define kAudioDevicePropertyBufferFrameSize        ((AudioObjectPropertySelector)'fsiz')
#endif
#ifndef kAudioDevicePropertyBufferFrameSizeRange
#define kAudioDevicePropertyBufferFrameSizeRange   ((AudioObjectPropertySelector)'fsz#')
#endif
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// UUID — must match Info.plist's CFPlugInFactories key.
// ---------------------------------------------------------------------------

#define kHonestEQ_FactoryUUID                                                    \
    CFUUIDGetConstantUUIDWithBytes(NULL,                                         \
        0x9F, 0x3F, 0x1D, 0x89, 0x5C, 0x0A, 0x4F, 0x0E,                          \
        0x9C, 0x7D, 0x1F, 0x1A, 0x0E, 0x3D, 0x5A, 0x7B)

// ---------------------------------------------------------------------------
// Object IDs.  Small integers; we dispatch by switch on these.
// ---------------------------------------------------------------------------

enum {
    kObjectID_PlugIn               = kAudioObjectPlugInObject,  // 1, Apple-defined
    kObjectID_Device               = 2,
    kObjectID_Stream_Output        = 3,
    kObjectID_Volume_Output_Master = 4,
    kObjectID_Mute_Output_Master   = 5,
    kObjectID_Stream_Input         = 6,     // pass 3: loopback stream for the companion app
};

// ---------------------------------------------------------------------------
// Supported sample rates and channel layout.
// ---------------------------------------------------------------------------

static const Float64 kSupportedSampleRates[] = {
    44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
};
static const UInt32  kNumSupportedSampleRates =
    sizeof(kSupportedSampleRates) / sizeof(kSupportedSampleRates[0]);

#define kChannels         2          // stereo
#define kBitsPerSample    32         // 32-bit float internally
#define kBytesPerSample   4
#define kBytesPerFrame    (kChannels * kBytesPerSample)

// Ring buffer between WriteMix (apps → us) and ReadInput (us → companion app).
// Interleaved stereo Float32. Sized for ~1.4 s at 48 kHz so wrap-around is rare
// and the reader can lag the writer by a comfortable margin.
#define kRingFrames       (65536)
static Float32 gRing[kRingFrames * kChannels];

// Ring-buffer length in frames (pass 3 uses this; pass 2 just reports it).
// Chosen so that a single zero-time-stamp period covers ~83 ms at 48 kHz —
// matches BlackHole's safe default.
#define kRingBufferFrameSize  (4096)

// ---------------------------------------------------------------------------
// Driver singleton state.
// ---------------------------------------------------------------------------

typedef struct {
    AudioServerPlugInDriverInterface * vtable_ptr;
    AudioServerPlugInDriverInterface   vtable;
    UInt32                              refcount;
    AudioServerPlugInHostRef            host;

    pthread_mutex_t                     state_mutex;
    Float64                             sample_rate;
    UInt32                              io_running_count;
    UInt32                              buffer_frame_size;   // AUHAL callback size

    UInt64                              io_anchor_host_time;
    Float64                             io_anchor_sample;

    Float32                             output_master_volume;   // 0.0..1.0
    bool                                output_master_mute;
} HonestEQ_Driver;

// Min / max buffer frame sizes we advertise + accept.
#define kMinBufferFrames  (32)
#define kMaxBufferFrames  (8192)

static HonestEQ_Driver gDriver;

// ---------------------------------------------------------------------------
// Helper: build a stereo Float32-LinearPCM AudioStreamBasicDescription
// at a given sample rate. This is the format every app sees and writes.
// ---------------------------------------------------------------------------

static AudioStreamBasicDescription
MakeASBD(Float64 rate)
{
    AudioStreamBasicDescription asbd;
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate       = rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat
                           | kAudioFormatFlagsNativeEndian
                           | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket   = kBytesPerFrame;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = kBytesPerFrame;
    asbd.mChannelsPerFrame = kChannels;
    asbd.mBitsPerChannel   = kBitsPerSample;
    return asbd;
}

// ---------------------------------------------------------------------------
// Address-matching helper. coreaudiod calls property routines with an
// (selector, scope, element) tuple; for most properties on most objects we
// only care about the selector and we accept any scope/element.
// ---------------------------------------------------------------------------

// Convenience: write a value into out_data, set used to its size.
#define EMIT(value_expr, c_type)                                                 \
    do {                                                                         \
        const c_type _v = (value_expr);                                          \
        if (out_data) {                                                          \
            if (in_data_size < sizeof(c_type)) return kAudioHardwareBadPropertySizeError; \
            *(c_type*)out_data = _v;                                             \
        }                                                                        \
        if (out_used) *out_used = sizeof(c_type);                                \
        return kAudioHardwareNoError;                                            \
    } while (0)

#define EMIT_SIZE(size_expr)                                                     \
    do {                                                                         \
        if (out_size) *out_size = (UInt32)(size_expr);                           \
        return kAudioHardwareNoError;                                            \
    } while (0)

// ---------------------------------------------------------------------------
// PlugIn object property handlers (object id = kObjectID_PlugIn).
// ---------------------------------------------------------------------------

static Boolean plugin_HasProperty(const AudioObjectPropertyAddress* a) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
    }
    return false;
}

static OSStatus plugin_GetPropertyDataSize(const AudioObjectPropertyAddress* a, UInt32* out_size) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioPlugInPropertyTranslateUIDToDevice:
            EMIT_SIZE(sizeof(AudioObjectID));
        case kAudioObjectPropertyManufacturer:
        case kAudioPlugInPropertyResourceBundle:
            EMIT_SIZE(sizeof(CFStringRef));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            EMIT_SIZE(sizeof(AudioObjectID));  // one owned object: the device
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus plugin_GetPropertyData(const AudioObjectPropertyAddress* a,
                                       UInt32 qualifier_size, const void* qualifier_data,
                                       UInt32 in_data_size, UInt32* out_used, void* out_data) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:    EMIT(kAudioObjectClassID, AudioClassID);
        case kAudioObjectPropertyClass:        EMIT(kAudioPlugInClassID,  AudioClassID);
        case kAudioObjectPropertyOwner:        EMIT(kAudioObjectUnknown,  AudioObjectID);
        case kAudioObjectPropertyManufacturer: EMIT(CFSTR("HonestEQ"),    CFStringRef);
        case kAudioPlugInPropertyResourceBundle: EMIT(CFSTR(""),           CFStringRef);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            const AudioObjectID dev = kObjectID_Device;
            if (out_data) {
                if (in_data_size < sizeof(AudioObjectID))
                    return kAudioHardwareBadPropertySizeError;
                *(AudioObjectID*)out_data = dev;
            }
            if (out_used) *out_used = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            // Qualifier is a CFStringRef containing a device UID; we return the
            // matching device ID (or kAudioObjectUnknown).
            if (qualifier_size != sizeof(CFStringRef) || qualifier_data == NULL)
                return kAudioHardwareBadPropertySizeError;
            CFStringRef requested_uid = *(const CFStringRef*)qualifier_data;
            AudioObjectID match = kAudioObjectUnknown;
            if (requested_uid && CFEqual(requested_uid, CFSTR("HonestEQDevice_UID"))) {
                match = kObjectID_Device;
            }
            EMIT(match, AudioObjectID);
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------
// Device object property handlers (object id = kObjectID_Device).
// ---------------------------------------------------------------------------

static Boolean device_HasProperty(const AudioObjectPropertyAddress* a) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyBufferFrameSizeRange:
            return true;
    }
    return false;
}

static OSStatus device_GetPropertyDataSize(const AudioObjectPropertyAddress* a, UInt32* out_size) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
            EMIT_SIZE(sizeof(UInt32));
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
            EMIT_SIZE(sizeof(CFStringRef));
        case kAudioObjectPropertyOwnedObjects: {
            // device owns: 2 streams (in+out) + 1 volume + 1 mute = 4
            EMIT_SIZE(4 * sizeof(AudioObjectID));
        }
        case kAudioDevicePropertyStreams: {
            // Global scope returns both directions; per-direction scope returns one.
            if (a->mScope == kAudioObjectPropertyScopeInput ||
                a->mScope == kAudioObjectPropertyScopeOutput) {
                EMIT_SIZE(sizeof(AudioObjectID));
            }
            EMIT_SIZE(2 * sizeof(AudioObjectID));
        }
        case kAudioObjectPropertyControlList:
            EMIT_SIZE(2 * sizeof(AudioObjectID));   // volume + mute
        case kAudioDevicePropertyRelatedDevices:
            EMIT_SIZE(sizeof(AudioObjectID));       // just us
        case kAudioDevicePropertyNominalSampleRate:
            EMIT_SIZE(sizeof(Float64));
        case kAudioDevicePropertyAvailableNominalSampleRates:
            EMIT_SIZE(kNumSupportedSampleRates * sizeof(AudioValueRange));
        case kAudioDevicePropertyPreferredChannelsForStereo:
            EMIT_SIZE(2 * sizeof(UInt32));
        case kAudioDevicePropertyPreferredChannelLayout:
            EMIT_SIZE(sizeof(AudioChannelLayout));
        case kAudioDevicePropertyBufferFrameSize:
            EMIT_SIZE(sizeof(UInt32));
        case kAudioDevicePropertyBufferFrameSizeRange:
            EMIT_SIZE(sizeof(AudioValueRange));
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus device_GetPropertyData(const AudioObjectPropertyAddress* a,
                                       UInt32 in_data_size, UInt32* out_used, void* out_data) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:    EMIT(kAudioObjectClassID, AudioClassID);
        case kAudioObjectPropertyClass:        EMIT(kAudioDeviceClassID,  AudioClassID);
        case kAudioObjectPropertyOwner:        EMIT(kObjectID_PlugIn,     AudioObjectID);
        case kAudioObjectPropertyName:         EMIT(CFSTR("HonestEQ"),    CFStringRef);
        case kAudioObjectPropertyManufacturer: EMIT(CFSTR("HonestEQ"),    CFStringRef);
        case kAudioDevicePropertyDeviceUID:    EMIT(CFSTR("HonestEQDevice_UID"), CFStringRef);
        case kAudioDevicePropertyModelUID:     EMIT(CFSTR("HonestEQModel_UID"),  CFStringRef);
        case kAudioDevicePropertyTransportType: EMIT(kAudioDeviceTransportTypeVirtual, UInt32);
        case kAudioDevicePropertyClockDomain:   EMIT(0u, UInt32);
        case kAudioDevicePropertyDeviceIsAlive: EMIT(1u, UInt32);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:         EMIT(1u, UInt32);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:   EMIT(1u, UInt32);
        case kAudioDevicePropertyLatency:        EMIT(0u, UInt32);
        case kAudioDevicePropertySafetyOffset:   EMIT(0u, UInt32);
        case kAudioDevicePropertyIsHidden:       EMIT(0u, UInt32);
        case kAudioDevicePropertyZeroTimeStampPeriod: EMIT((UInt32)kRingBufferFrameSize, UInt32);
        case kAudioDevicePropertyDeviceIsRunning: {
            pthread_mutex_lock(&gDriver.state_mutex);
            UInt32 v = (gDriver.io_running_count > 0) ? 1u : 0u;
            pthread_mutex_unlock(&gDriver.state_mutex);
            EMIT(v, UInt32);
        }
        case kAudioObjectPropertyOwnedObjects: {
            const AudioObjectID owned[4] = {
                kObjectID_Stream_Output,
                kObjectID_Stream_Input,
                kObjectID_Volume_Output_Master,
                kObjectID_Mute_Output_Master,
            };
            const UInt32 count_fits = in_data_size / sizeof(AudioObjectID);
            const UInt32 emit = (count_fits < 4 ? count_fits : 4);
            if (out_data) memcpy(out_data, owned, emit * sizeof(AudioObjectID));
            if (out_used) *out_used = emit * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyStreams: {
            if (a->mScope == kAudioObjectPropertyScopeOutput) {
                EMIT((AudioObjectID)kObjectID_Stream_Output, AudioObjectID);
            }
            if (a->mScope == kAudioObjectPropertyScopeInput) {
                EMIT((AudioObjectID)kObjectID_Stream_Input, AudioObjectID);
            }
            // Global scope: both streams.
            const AudioObjectID streams[2] = {
                kObjectID_Stream_Output, kObjectID_Stream_Input,
            };
            const UInt32 count_fits = in_data_size / sizeof(AudioObjectID);
            const UInt32 emit = (count_fits < 2 ? count_fits : 2);
            if (out_data) memcpy(out_data, streams, emit * sizeof(AudioObjectID));
            if (out_used) *out_used = emit * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioObjectPropertyControlList: {
            const AudioObjectID controls[2] = {
                kObjectID_Volume_Output_Master,
                kObjectID_Mute_Output_Master,
            };
            const UInt32 count_fits = in_data_size / sizeof(AudioObjectID);
            const UInt32 emit = (count_fits < 2 ? count_fits : 2);
            if (out_data) memcpy(out_data, controls, emit * sizeof(AudioObjectID));
            if (out_used) *out_used = emit * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyRelatedDevices:
            EMIT((AudioObjectID)kObjectID_Device, AudioObjectID);
        case kAudioDevicePropertyNominalSampleRate: {
            pthread_mutex_lock(&gDriver.state_mutex);
            Float64 v = gDriver.sample_rate;
            pthread_mutex_unlock(&gDriver.state_mutex);
            EMIT(v, Float64);
        }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            const UInt32 count_fits = in_data_size / sizeof(AudioValueRange);
            const UInt32 emit = (count_fits < kNumSupportedSampleRates
                                 ? count_fits : kNumSupportedSampleRates);
            if (out_data) {
                AudioValueRange* dst = (AudioValueRange*)out_data;
                for (UInt32 i = 0; i < emit; ++i) {
                    dst[i].mMinimum = kSupportedSampleRates[i];
                    dst[i].mMaximum = kSupportedSampleRates[i];
                }
            }
            if (out_used) *out_used = emit * sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (in_data_size < 2 * sizeof(UInt32))
                return kAudioHardwareBadPropertySizeError;
            ((UInt32*)out_data)[0] = 1;   // L = channel 1
            ((UInt32*)out_data)[1] = 2;   // R = channel 2
            if (out_used) *out_used = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            AudioChannelLayout layout;
            memset(&layout, 0, sizeof(layout));
            layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            EMIT(layout, AudioChannelLayout);
        }
        case kAudioDevicePropertyBufferFrameSize: {
            pthread_mutex_lock(&gDriver.state_mutex);
            UInt32 v = gDriver.buffer_frame_size;
            pthread_mutex_unlock(&gDriver.state_mutex);
            if (v == 0) v = 512;   // default if not yet set
            EMIT(v, UInt32);
        }
        case kAudioDevicePropertyBufferFrameSizeRange: {
            AudioValueRange r = {
                .mMinimum = (Float64)kMinBufferFrames,
                .mMaximum = (Float64)kMaxBufferFrames,
            };
            EMIT(r, AudioValueRange);
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus device_IsPropertySettable(const AudioObjectPropertyAddress* a, Boolean* settable) {
    *settable = (a->mSelector == kAudioDevicePropertyNominalSampleRate ||
                 a->mSelector == kAudioDevicePropertyBufferFrameSize);
    return kAudioHardwareNoError;
}

static OSStatus device_SetPropertyData(const AudioObjectPropertyAddress* a,
                                       UInt32 in_data_size, const void* in_data) {
    if (a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (in_data_size < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 requested = *(const Float64*)in_data;
        // Accept only supported rates.
        Boolean ok = false;
        for (UInt32 i = 0; i < kNumSupportedSampleRates; ++i) {
            if (kSupportedSampleRates[i] == requested) { ok = true; break; }
        }
        if (!ok) return kAudioHardwareIllegalOperationError;

        // Do NOT apply the rate change here — the correct flow per Apple's
        // AudioServerPlugIn contract is:
        //   SetPropertyData → RequestDeviceConfigurationChange → (coreaudiod
        //   stops IO, calls PerformDeviceConfigurationChange, restarts IO)
        // Applying the change in SetPropertyData causes property queries to
        // see the new rate before the timing anchors and streams have been
        // re-primed. That's the bug we saw at 96 kHz: writer stuck at old
        // cadence while reader ran at new cadence.
        if (gDriver.host) {
            // Encode the new rate into change_action; Perform reads it back.
            gDriver.host->RequestDeviceConfigurationChange(
                gDriver.host, kObjectID_Device, (UInt64)requested, NULL);
        }
        return kAudioHardwareNoError;
    }
    if (a->mSelector == kAudioDevicePropertyBufferFrameSize) {
        if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        UInt32 requested = *(const UInt32*)in_data;
        if (requested < kMinBufferFrames) requested = kMinBufferFrames;
        if (requested > kMaxBufferFrames) requested = kMaxBufferFrames;
        pthread_mutex_lock(&gDriver.state_mutex);
        gDriver.buffer_frame_size = requested;
        pthread_mutex_unlock(&gDriver.state_mutex);
        return kAudioHardwareNoError;
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------
// Stream object property handlers.
// ---------------------------------------------------------------------------

static Boolean stream_HasProperty(const AudioObjectPropertyAddress* a) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
    }
    return false;
}

static OSStatus stream_GetPropertyDataSize(const AudioObjectPropertyAddress* a, UInt32* out_size) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
            EMIT_SIZE(sizeof(UInt32));
        case kAudioObjectPropertyName:
            EMIT_SIZE(sizeof(CFStringRef));
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            EMIT_SIZE(sizeof(AudioStreamBasicDescription));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            EMIT_SIZE(kNumSupportedSampleRates * sizeof(AudioStreamRangedDescription));
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus stream_GetPropertyData(AudioObjectID id,
                                       const AudioObjectPropertyAddress* a,
                                       UInt32 in_data_size, UInt32* out_used, void* out_data) {
    const Boolean is_input = (id == kObjectID_Stream_Input);
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: EMIT(kAudioObjectClassID, AudioClassID);
        case kAudioObjectPropertyClass:     EMIT(kAudioStreamClassID,  AudioClassID);
        case kAudioObjectPropertyOwner:     EMIT(kObjectID_Device,     AudioObjectID);
        case kAudioObjectPropertyName:
            EMIT(is_input ? CFSTR("HonestEQ Input Stream") : CFSTR("HonestEQ Output Stream"),
                 CFStringRef);
        case kAudioStreamPropertyIsActive:  EMIT(1u, UInt32);
        case kAudioStreamPropertyDirection: EMIT(is_input ? 1u : 0u, UInt32);
        case kAudioStreamPropertyTerminalType:
            EMIT(is_input ? (UInt32)kAudioStreamTerminalTypeMicrophone
                          : (UInt32)kAudioStreamTerminalTypeSpeaker, UInt32);
        case kAudioStreamPropertyStartingChannel: EMIT(1u, UInt32);
        case kAudioStreamPropertyLatency:         EMIT(0u, UInt32);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            pthread_mutex_lock(&gDriver.state_mutex);
            Float64 rate = gDriver.sample_rate;
            pthread_mutex_unlock(&gDriver.state_mutex);
            AudioStreamBasicDescription asbd = MakeASBD(rate);
            EMIT(asbd, AudioStreamBasicDescription);
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            const UInt32 count_fits = in_data_size / sizeof(AudioStreamRangedDescription);
            const UInt32 emit = (count_fits < kNumSupportedSampleRates
                                 ? count_fits : kNumSupportedSampleRates);
            if (out_data) {
                AudioStreamRangedDescription* dst = (AudioStreamRangedDescription*)out_data;
                for (UInt32 i = 0; i < emit; ++i) {
                    dst[i].mFormat       = MakeASBD(kSupportedSampleRates[i]);
                    dst[i].mSampleRateRange.mMinimum = kSupportedSampleRates[i];
                    dst[i].mSampleRateRange.mMaximum = kSupportedSampleRates[i];
                }
            }
            if (out_used) *out_used = emit * sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------
// Volume control + Mute control property handlers.
// ---------------------------------------------------------------------------

static Boolean control_HasProperty(AudioObjectID id, const AudioObjectPropertyAddress* a) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
            return true;
    }
    if (id == kObjectID_Volume_Output_Master) {
        switch (a->mSelector) {
            case kAudioLevelControlPropertyScalarValue:
            case kAudioLevelControlPropertyDecibelValue:
            case kAudioLevelControlPropertyDecibelRange:
            case kAudioLevelControlPropertyConvertScalarToDecibels:
            case kAudioLevelControlPropertyConvertDecibelsToScalar:
                return true;
        }
    }
    if (id == kObjectID_Mute_Output_Master) {
        if (a->mSelector == kAudioBooleanControlPropertyValue) return true;
    }
    return false;
}

static OSStatus control_GetPropertyDataSize(AudioObjectID id,
                                            const AudioObjectPropertyAddress* a,
                                            UInt32* out_size) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioBooleanControlPropertyValue:
            EMIT_SIZE(sizeof(UInt32));
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            EMIT_SIZE(sizeof(Float32));
        case kAudioLevelControlPropertyDecibelRange:
            EMIT_SIZE(sizeof(AudioValueRange));
    }
    (void)id;
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus control_GetPropertyData(AudioObjectID id,
                                        const AudioObjectPropertyAddress* a,
                                        UInt32 in_data_size, UInt32* out_used, void* out_data) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (id == kObjectID_Volume_Output_Master) EMIT(kAudioLevelControlClassID, AudioClassID);
            if (id == kObjectID_Mute_Output_Master)   EMIT(kAudioBooleanControlClassID, AudioClassID);
            break;
        case kAudioObjectPropertyClass:
            if (id == kObjectID_Volume_Output_Master) EMIT(kAudioVolumeControlClassID, AudioClassID);
            if (id == kObjectID_Mute_Output_Master)   EMIT(kAudioMuteControlClassID,   AudioClassID);
            break;
        case kAudioObjectPropertyOwner:      EMIT(kObjectID_Device, AudioObjectID);
        case kAudioControlPropertyScope:     EMIT((UInt32)kAudioObjectPropertyScopeOutput, UInt32);
        case kAudioControlPropertyElement:   EMIT((UInt32)kAudioObjectPropertyElementMain, UInt32);
    }
    if (id == kObjectID_Volume_Output_Master) {
        switch (a->mSelector) {
            case kAudioLevelControlPropertyScalarValue: {
                pthread_mutex_lock(&gDriver.state_mutex);
                Float32 v = gDriver.output_master_volume;
                pthread_mutex_unlock(&gDriver.state_mutex);
                EMIT(v, Float32);
            }
            case kAudioLevelControlPropertyDecibelValue: {
                pthread_mutex_lock(&gDriver.state_mutex);
                Float32 scalar = gDriver.output_master_volume;
                pthread_mutex_unlock(&gDriver.state_mutex);
                Float32 db = (scalar <= 0.00001f) ? -96.0f
                              : 20.0f * log10f(scalar);
                EMIT(db, Float32);
            }
            case kAudioLevelControlPropertyDecibelRange: {
                AudioValueRange r = { -96.0, 0.0 };
                EMIT(r, AudioValueRange);
            }
            case kAudioLevelControlPropertyConvertScalarToDecibels: {
                if (in_data_size < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                Float32 scalar = *(const Float32*)out_data;
                Float32 db = (scalar <= 0.00001f) ? -96.0f : 20.0f * log10f(scalar);
                EMIT(db, Float32);
            }
            case kAudioLevelControlPropertyConvertDecibelsToScalar: {
                if (in_data_size < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                Float32 db = *(const Float32*)out_data;
                Float32 scalar = powf(10.0f, db / 20.0f);
                EMIT(scalar, Float32);
            }
        }
    }
    if (id == kObjectID_Mute_Output_Master) {
        if (a->mSelector == kAudioBooleanControlPropertyValue) {
            pthread_mutex_lock(&gDriver.state_mutex);
            UInt32 v = gDriver.output_master_mute ? 1u : 0u;
            pthread_mutex_unlock(&gDriver.state_mutex);
            EMIT(v, UInt32);
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus control_IsPropertySettable(AudioObjectID id,
                                           const AudioObjectPropertyAddress* a,
                                           Boolean* settable) {
    if (id == kObjectID_Volume_Output_Master) {
        if (a->mSelector == kAudioLevelControlPropertyScalarValue ||
            a->mSelector == kAudioLevelControlPropertyDecibelValue) {
            *settable = true;
            return kAudioHardwareNoError;
        }
    }
    if (id == kObjectID_Mute_Output_Master) {
        if (a->mSelector == kAudioBooleanControlPropertyValue) {
            *settable = true;
            return kAudioHardwareNoError;
        }
    }
    *settable = false;
    return kAudioHardwareNoError;
}

static OSStatus control_SetPropertyData(AudioObjectID id,
                                        const AudioObjectPropertyAddress* a,
                                        UInt32 in_data_size, const void* in_data) {
    if (id == kObjectID_Volume_Output_Master) {
        if (a->mSelector == kAudioLevelControlPropertyScalarValue) {
            if (in_data_size < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            Float32 v = *(const Float32*)in_data;
            if (v < 0) v = 0; if (v > 1) v = 1;
            pthread_mutex_lock(&gDriver.state_mutex);
            gDriver.output_master_volume = v;
            pthread_mutex_unlock(&gDriver.state_mutex);
            return kAudioHardwareNoError;
        }
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) {
            if (in_data_size < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            Float32 db = *(const Float32*)in_data;
            Float32 scalar = powf(10.0f, db / 20.0f);
            if (scalar < 0) scalar = 0; if (scalar > 1) scalar = 1;
            pthread_mutex_lock(&gDriver.state_mutex);
            gDriver.output_master_volume = scalar;
            pthread_mutex_unlock(&gDriver.state_mutex);
            return kAudioHardwareNoError;
        }
    }
    if (id == kObjectID_Mute_Output_Master) {
        if (a->mSelector == kAudioBooleanControlPropertyValue) {
            if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            UInt32 v = *(const UInt32*)in_data;
            pthread_mutex_lock(&gDriver.state_mutex);
            gDriver.output_master_mute = (v != 0);
            pthread_mutex_unlock(&gDriver.state_mutex);
            return kAudioHardwareNoError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------
// CFPlugIn IUnknown methods.
// ---------------------------------------------------------------------------

static HRESULT HonestEQ_QueryInterface(void* self, REFIID iid, LPVOID* out) {
    if (out == NULL) return E_POINTER;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) ||
        CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        gDriver.refcount += 1;
        *out = &gDriver;
        result = S_OK;
    } else {
        *out = NULL;
    }
    CFRelease(requested);
    return result;
}

static ULONG HonestEQ_AddRef(void* self) {
    (void)self;
    gDriver.refcount += 1;
    return gDriver.refcount;
}

static ULONG HonestEQ_Release(void* self) {
    (void)self;
    if (gDriver.refcount > 0) gDriver.refcount -= 1;
    return gDriver.refcount;
}

// ---------------------------------------------------------------------------
// Lifecycle methods.
// ---------------------------------------------------------------------------

static OSStatus HonestEQ_Initialize(AudioServerPlugInDriverRef self,
                                    AudioServerPlugInHostRef host) {
    (void)self;
    gDriver.host = host;
    gDriver.sample_rate = 48000.0;
    gDriver.buffer_frame_size = 512;   // AUHAL will override via SetProperty
    gDriver.output_master_volume = 1.0f;
    gDriver.output_master_mute = false;
    gDriver.io_running_count = 0;
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_CreateDevice(AudioServerPlugInDriverRef self,
                                     CFDictionaryRef desc,
                                     const AudioServerPlugInClientInfo* info,
                                     AudioObjectID* out_dev) {
    (void)self; (void)desc; (void)info; (void)out_dev;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus HonestEQ_DestroyDevice(AudioServerPlugInDriverRef self, AudioObjectID dev) {
    (void)self; (void)dev;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus HonestEQ_AddDeviceClient(AudioServerPlugInDriverRef self,
                                        AudioObjectID dev,
                                        const AudioServerPlugInClientInfo* info) {
    (void)self; (void)dev; (void)info;
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_RemoveDeviceClient(AudioServerPlugInDriverRef self,
                                            AudioObjectID dev,
                                            const AudioServerPlugInClientInfo* info) {
    (void)self; (void)dev; (void)info;
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef self,
                                                          AudioObjectID dev,
                                                          UInt64 action, void* info) {
    (void)self; (void)dev; (void)info;
    // action carries the rate we requested in SetPropertyData.
    Float64 new_rate = (Float64)action;
    if (new_rate <= 0) return kAudioHardwareNoError;
    Boolean ok = false;
    for (UInt32 i = 0; i < kNumSupportedSampleRates; ++i) {
        if (kSupportedSampleRates[i] == new_rate) { ok = true; break; }
    }
    if (!ok) return kAudioHardwareIllegalOperationError;

    // IO is stopped for the duration of this call. Reset timing anchors and
    // zero the ring so the new rate starts fresh with no stale samples.
    pthread_mutex_lock(&gDriver.state_mutex);
    gDriver.sample_rate         = new_rate;
    gDriver.io_anchor_host_time = mach_absolute_time();
    gDriver.io_anchor_sample    = 0;
    memset(gRing, 0, sizeof(gRing));
    pthread_mutex_unlock(&gDriver.state_mutex);
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef self,
                                                        AudioObjectID dev,
                                                        UInt64 action, void* info) {
    (void)self; (void)dev; (void)action; (void)info;
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------------------
// Property dispatch — routes to per-object handlers.
// ---------------------------------------------------------------------------

static Boolean HonestEQ_HasProperty(AudioServerPlugInDriverRef self,
                                   AudioObjectID id, pid_t pid,
                                   const AudioObjectPropertyAddress* a) {
    (void)self; (void)pid;
    if (a == NULL) return false;
    switch (id) {
        case kObjectID_PlugIn:               return plugin_HasProperty(a);
        case kObjectID_Device:               return device_HasProperty(a);
        case kObjectID_Stream_Output:
        case kObjectID_Stream_Input:         return stream_HasProperty(a);
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Output_Master:   return control_HasProperty(id, a);
    }
    return false;
}

static OSStatus HonestEQ_IsPropertySettable(AudioServerPlugInDriverRef self,
                                           AudioObjectID id, pid_t pid,
                                           const AudioObjectPropertyAddress* a,
                                           Boolean* settable) {
    (void)self; (void)pid;
    if (settable == NULL || a == NULL) return kAudioHardwareIllegalOperationError;
    *settable = false;
    switch (id) {
        case kObjectID_Device:               return device_IsPropertySettable(a, settable);
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Output_Master:   return control_IsPropertySettable(id, a, settable);
    }
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_GetPropertyDataSize(AudioServerPlugInDriverRef self,
                                            AudioObjectID id, pid_t pid,
                                            const AudioObjectPropertyAddress* a,
                                            UInt32 qsize, const void* qdata, UInt32* out_size) {
    (void)self; (void)pid; (void)qsize; (void)qdata;
    if (a == NULL) return kAudioHardwareIllegalOperationError;
    switch (id) {
        case kObjectID_PlugIn:        return plugin_GetPropertyDataSize(a, out_size);
        case kObjectID_Device:        return device_GetPropertyDataSize(a, out_size);
        case kObjectID_Stream_Output:
        case kObjectID_Stream_Input:  return stream_GetPropertyDataSize(a, out_size);
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Output_Master: return control_GetPropertyDataSize(id, a, out_size);
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus HonestEQ_GetPropertyData(AudioServerPlugInDriverRef self,
                                        AudioObjectID id, pid_t pid,
                                        const AudioObjectPropertyAddress* a,
                                        UInt32 qsize, const void* qdata,
                                        UInt32 in_size, UInt32* used, void* data) {
    (void)self; (void)pid;
    if (a == NULL) return kAudioHardwareIllegalOperationError;
    switch (id) {
        case kObjectID_PlugIn:
            return plugin_GetPropertyData(a, qsize, qdata, in_size, used, data);
        case kObjectID_Device:        return device_GetPropertyData(a, in_size, used, data);
        case kObjectID_Stream_Output:
        case kObjectID_Stream_Input:  return stream_GetPropertyData(id, a, in_size, used, data);
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Output_Master:
            return control_GetPropertyData(id, a, in_size, used, data);
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus HonestEQ_SetPropertyData(AudioServerPlugInDriverRef self,
                                        AudioObjectID id, pid_t pid,
                                        const AudioObjectPropertyAddress* a,
                                        UInt32 qsize, const void* qdata,
                                        UInt32 in_size, const void* data) {
    (void)self; (void)pid; (void)qsize; (void)qdata;
    if (a == NULL) return kAudioHardwareIllegalOperationError;
    switch (id) {
        case kObjectID_Device:        return device_SetPropertyData(a, in_size, data);
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Output_Master:
            return control_SetPropertyData(id, a, in_size, data);
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------
// IO methods — still stubs, no audio path yet. Pass 3 adds the ring buffer.
// ---------------------------------------------------------------------------

static OSStatus HonestEQ_StartIO(AudioServerPlugInDriverRef self,
                                AudioObjectID dev, UInt32 cid) {
    (void)self; (void)dev; (void)cid;
    pthread_mutex_lock(&gDriver.state_mutex);
    if (gDriver.io_running_count == 0) {
        gDriver.io_anchor_host_time = mach_absolute_time();
        gDriver.io_anchor_sample    = 0;
        // Zero the ring so we don't play garbage on start.
        memset(gRing, 0, sizeof(gRing));
    }
    gDriver.io_running_count += 1;
    pthread_mutex_unlock(&gDriver.state_mutex);
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_StopIO(AudioServerPlugInDriverRef self,
                               AudioObjectID dev, UInt32 cid) {
    (void)self; (void)dev; (void)cid;
    pthread_mutex_lock(&gDriver.state_mutex);
    if (gDriver.io_running_count > 0) gDriver.io_running_count -= 1;
    pthread_mutex_unlock(&gDriver.state_mutex);
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_GetZeroTimeStamp(AudioServerPlugInDriverRef self,
                                          AudioObjectID dev, UInt32 cid,
                                          Float64* out_sample, UInt64* out_host, UInt64* out_seed) {
    (void)self; (void)dev; (void)cid;
    // Convert host-time elapsed since IO start into a sample count at the
    // current nominal rate; report that as the next zero-time-stamp.
    pthread_mutex_lock(&gDriver.state_mutex);
    Float64 rate   = gDriver.sample_rate;
    UInt64  anchor = gDriver.io_anchor_host_time;
    Float64 sample = gDriver.io_anchor_sample;
    pthread_mutex_unlock(&gDriver.state_mutex);

    static mach_timebase_info_data_t tb = { 0, 0 };
    if (tb.denom == 0) mach_timebase_info(&tb);

    UInt64 now = mach_absolute_time();
    UInt64 diff = now - anchor;
    Float64 ns = (Float64)diff * (Float64)tb.numer / (Float64)tb.denom;
    Float64 frames_elapsed = ns * 1e-9 * rate;
    Float64 period = (Float64)kRingBufferFrameSize;
    Float64 periods = floor(frames_elapsed / period);
    Float64 cur_sample = sample + periods * period;
    UInt64  cur_host   = anchor + (UInt64)((periods * period / rate) * 1e9 * (Float64)tb.denom / (Float64)tb.numer);

    if (out_sample) *out_sample = cur_sample;
    if (out_host)   *out_host   = cur_host;
    if (out_seed)   *out_seed   = 1;
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_WillDoIOOperation(AudioServerPlugInDriverRef self,
                                           AudioObjectID dev, UInt32 cid, UInt32 op,
                                           Boolean* will, Boolean* will_inplace) {
    (void)self; (void)dev; (void)cid;
    Boolean handled = false;
    switch (op) {
        // WriteMix: apps → us. We copy their mixed audio into the ring.
        case kAudioServerPlugInIOOperationWriteMix:
        // ReadInput: us → reader. Reader gets what we captured earlier.
        case kAudioServerPlugInIOOperationReadInput:
            handled = true; break;
    }
    if (will)         *will = handled;
    if (will_inplace) *will_inplace = true;   // we can process in place
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_BeginIOOperation(AudioServerPlugInDriverRef self,
                                          AudioObjectID dev, UInt32 cid, UInt32 op,
                                          UInt32 frames,
                                          const AudioServerPlugInIOCycleInfo* info) {
    (void)self; (void)dev; (void)cid; (void)op; (void)frames; (void)info;
    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_DoIOOperation(AudioServerPlugInDriverRef self,
                                       AudioObjectID dev, AudioObjectID stream,
                                       UInt32 cid, UInt32 op,
                                       UInt32 frames,
                                       const AudioServerPlugInIOCycleInfo* info,
                                       void* main, void* secondary) {
    (void)self; (void)dev; (void)stream; (void)cid; (void)secondary;
    if (main == NULL || info == NULL) return kAudioHardwareNoError;

    // Apply master mute/volume to the audio as it flows through.
    pthread_mutex_lock(&gDriver.state_mutex);
    const bool  muted = gDriver.output_master_mute;
    const float vol   = gDriver.output_master_volume;
    pthread_mutex_unlock(&gDriver.state_mutex);

    Float32* buf = (Float32*)main;

    if (op == kAudioServerPlugInIOOperationWriteMix) {
        // Apps have written `frames` * 2 samples of Float32 into `buf`.
        // Copy them into the ring at the current output sample time.
        const UInt64 base = (UInt64)info->mOutputTime.mSampleTime;
        const float g = muted ? 0.0f : vol;
        for (UInt32 i = 0; i < frames; ++i) {
            const UInt64 idx = ((base + i) % kRingFrames) * kChannels;
            gRing[idx + 0] = buf[i * kChannels + 0] * g;
            gRing[idx + 1] = buf[i * kChannels + 1] * g;
        }
        return kAudioHardwareNoError;
    }

    if (op == kAudioServerPlugInIOOperationReadInput) {
        // A reader (companion app) is pulling from the loopback.
        // Copy `frames` samples from the ring at the input sample time.
        const UInt64 base = (UInt64)info->mInputTime.mSampleTime;
        for (UInt32 i = 0; i < frames; ++i) {
            const UInt64 idx = ((base + i) % kRingFrames) * kChannels;
            buf[i * kChannels + 0] = gRing[idx + 0];
            buf[i * kChannels + 1] = gRing[idx + 1];
        }
        return kAudioHardwareNoError;
    }

    return kAudioHardwareNoError;
}

static OSStatus HonestEQ_EndIOOperation(AudioServerPlugInDriverRef self,
                                        AudioObjectID dev, UInt32 cid, UInt32 op,
                                        UInt32 frames,
                                        const AudioServerPlugInIOCycleInfo* info) {
    (void)self; (void)dev; (void)cid; (void)op; (void)frames; (void)info;
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------------------
// Static vtable + factory.
// ---------------------------------------------------------------------------

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL,
    HonestEQ_QueryInterface,
    HonestEQ_AddRef,
    HonestEQ_Release,
    HonestEQ_Initialize,
    HonestEQ_CreateDevice,
    HonestEQ_DestroyDevice,
    HonestEQ_AddDeviceClient,
    HonestEQ_RemoveDeviceClient,
    HonestEQ_PerformDeviceConfigurationChange,
    HonestEQ_AbortDeviceConfigurationChange,
    HonestEQ_HasProperty,
    HonestEQ_IsPropertySettable,
    HonestEQ_GetPropertyDataSize,
    HonestEQ_GetPropertyData,
    HonestEQ_SetPropertyData,
    HonestEQ_StartIO,
    HonestEQ_StopIO,
    HonestEQ_GetZeroTimeStamp,
    HonestEQ_WillDoIOOperation,
    HonestEQ_BeginIOOperation,
    HonestEQ_DoIOOperation,
    HonestEQ_EndIOOperation,
};

__attribute__((visibility("default")))
void* HonestEQ_Create(CFAllocatorRef allocator, CFUUIDRef requested_type) {
    (void)allocator;
    if (!CFEqual(requested_type, kAudioServerPlugInTypeUUID)) return NULL;
    pthread_mutex_init(&gDriver.state_mutex, NULL);
    gDriver.vtable = gDriverInterface;
    gDriver.vtable_ptr = &gDriver.vtable;
    gDriver.refcount = 1;
    return &gDriver;
}
