# HonestEQ — Product Plan

A system-wide parametric equalizer for macOS, built from scratch.
Goal: match the audible quality and feel of Peace EQ (Windows) on headphones, without the pitch-shift / quality issues encountered when chaining BlackHole + Reaper or similar virtual-device setups.

---

## 1. Why build this

Existing options on macOS that were tried and rejected:

- **BlackHole + Reaper (or other DAW host)** — virtual cable approach. Sound felt "off" / higher pitch than Peace EQ on Windows. Root cause is almost certainly sample-rate mismatch: the host (Reaper) was running at a different rate than the physical output device, so audio was being resampled — and if the resampler is wrong or if the virtual device's nominal rate is being forced, playback ends up sped up or slowed down (which sounds like a pitch shift).
- **eqMac / other FOSS EQs** — quality not satisfactory, want full control over DSP and routing.

So: build our own, optimized for transparent quality on headphones and for matching Peace's filter behavior at any of the three target rates.

---

## 2. Required features

### 2.1 Sample-rate handling (the most important one)
- Must support **44.1 kHz, 48 kHz, and 192 kHz** end-to-end with **no unintended resampling and no pitch shift**.
- The EQ engine must operate at the **same sample rate the physical output device is currently running at**. If the user switches the output device's rate in Audio MIDI Setup (or the app changes it for them), HonestEQ must follow and recompute its filter coefficients for the new rate.
- All filter math (biquad coefficients) is sample-rate dependent. A filter designed for 48 kHz played out at 192 kHz (or vice versa) will sound wrong — bands shift in frequency. We handle this explicitly.

### 2.2 Bands
- **Add band** button.
- **Remove band** button.
- **"Number of bands" text field** — typing a number and confirming sets the band count in one action (truncates from the end if fewer, appends defaults if more).
- Each band: frequency (Hz), gain (dB), Q, and filter type (peaking by default; low-shelf / high-shelf / low-pass / high-pass available).
- Default band layout on first launch should mirror a sensible Peace-style starting set (e.g. 10 bands across log-spaced frequencies).

### 2.3 Gain range per band
- Range: **-12 dB to +12 dB** in each band.
- Must accept **pasted / typed** values directly into the gain field (e.g. paste `+5.5` or `-3.25`) and apply them exactly, with **no quantization or rounding** that audibly affects quality.
- Internally store gains as `double` (64-bit float). DSP path also uses `double` (or at minimum 32-bit float) so 0.01 dB resolution is preserved.

### 2.4 Output device selection
- Drop-down listing all current CoreAudio output devices.
- Selecting a device routes processed audio to it.
- Must update live if devices are plugged in/unplugged (CoreAudio device-change listener).

### 2.5 Pre-amp
- Master gain slider, range **-12 dB to +12 dB**, applied **before** the band filters (so that boosting bands doesn't clip — user pulls pre-amp down to compensate).
- Type/paste numeric value also supported.

### 2.6 Profiles
- **Save profile** with a user-supplied name.
- **Load profile** from saved list.
- File format: plain-text, human-readable, easy to edit/diff.
- Stored per-user under `~/Library/Application Support/HonestEQ/Profiles/<name>.heq` (or `.txt`).
- Saved data: pre-amp value, band count, and for each band: frequency, gain, Q, filter type.

### 2.7 Import Peace-style profiles
- The user has Peace EQ config files (text-based) from their Windows setup. HonestEQ must be able to **paste** the contents of a Peace config or drop the file in and convert it into a HonestEQ profile.
- Peace files use a known line-based format (Preamp / Filter lines with frequency, gain, Q). We parse that and round-trip.

---

## 3. Non-goals (for v1)

- Spatial / surround processing. Stereo only at first.
- Mic / input EQ. Output only.
- Per-app EQ. System-wide single chain only.
- Convolution / IR loading. Parametric biquad EQ only — that's what Peace does.
- Auto-EQ from headphone measurement databases. Manual band editing only. (Can be added later by importing a profile.)

---

## 4. Success criteria

A blind A/B test between HonestEQ on macOS and Peace EQ on Windows, using identical band settings on the same headphones and same source file at 48 kHz, should be indistinguishable. No pitch difference. No audible distortion at moderate gains. CPU usage low enough to be invisible (< 2% of one core at 192 kHz with 20 bands).
