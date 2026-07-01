// EqBridge.hpp — C-callable wrapper around libHonestEQDSP.
//
// main.c is pure C; the DSP library is C++17. This header exposes a small
// C-ABI shim so main.c can call into it without going C++.
//
// Real-time constraints: eq_bridge_process_interleaved is called from an
// audio callback thread and must not allocate, take locks, or block.
// The bridge pre-allocates its scratch buffer in eq_bridge_create.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EqBridge EqBridge;

// Create/destroy. sample_rate is the initial rate; adjust later with
// eq_bridge_set_sample_rate. Returns NULL on allocation failure.
EqBridge* eq_bridge_create(double sample_rate);
void      eq_bridge_destroy(EqBridge* eq);

// Load an Equalizer-APO / Peace-EQ style profile.
// Returns 0 on success, non-zero on failure (file missing, parse error).
// If the file has no filter bands, the chain becomes passthrough.
int       eq_bridge_load_profile(EqBridge* eq, const char* path);

// Update the sample rate. Call this from the rate-change path AFTER
// gSampleRate has been updated. Recomputes all biquad coefficients.
void      eq_bridge_set_sample_rate(EqBridge* eq, double sample_rate);

// Process an interleaved stereo Float32 buffer in-place.
// frames = number of stereo frames (buffer contains 2*frames samples).
// If no profile is loaded, this is a no-op (passthrough).
void      eq_bridge_process_interleaved(EqBridge* eq, float* data, unsigned frames);

// 1 if a non-empty profile is loaded, 0 otherwise.
int       eq_bridge_has_profile(const EqBridge* eq);

// Diagnostic: number of active bands + preamp value.
int       eq_bridge_band_count(const EqBridge* eq);
double    eq_bridge_preamp_db(const EqBridge* eq);

#ifdef __cplusplus
}
#endif
