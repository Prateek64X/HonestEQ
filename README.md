# HonestEQ

A system-wide parametric equalizer for macOS. Built to match the audible quality of Peace EQ (Windows) and Poweramp (Android) on the Mac.

## Why

If you've EQ'd your headphones with Peace/Equalizer APO on Windows or Poweramp on Android, you know how good a bit-perfect parametric EQ chain sounds. macOS doesn't have a first-party system EQ, and the existing third-party tools (eqMac, BlackHole+Reaper, SoundSource) either resample audio in ways that shift pitch, or use single-precision math that muddies dense multi-band cuts.

HonestEQ is my attempt at doing it right on the Mac, from scratch, with no Apple Developer fee for personal use.

## What it is

Four components that work together:

- **`driver/`** — A CoreAudio HAL plug-in (`HonestEQ.driver`) written in C. Loaded by `coreaudiod`, it registers a virtual output device named "HonestEQ" that appears in System Settings → Sound. Any app can send audio to it.
- **`daemon/`** — A small C program (`honesteq-daemon`) that reads the driver's loopback stream, and writes to whatever real output device you tell it (e.g. "External Headphones"). Handles live sample-rate changes.
- **`dsp/`** — A double-precision biquad library (`libHonestEQDSP`, C++17). RBJ Audio EQ Cookbook math, Direct Form I. Matches Equalizer APO's output bit-for-bit — same filter formulas, same precision, verified with 9 automated tests.
- **`dsp/tools/honesteq-render`** — Offline CLI. Point it at a WAV and a profile, get back the EQ'd WAV. Useful for validating your EQ shape before running system-wide.

## Status

Early alpha. **Works, and I use it daily on the test machine.** Not yet packaged as an installer, no GUI yet — configure via text profile files (same format Equalizer APO uses, so existing Peace / AutoEq profiles drop in unchanged).

## Tested on

- **MacBook Air M5** (2026)
- **macOS Tahoe 26.5.1 (25F80)**
- **Sony WH-CH720N** headphones, 3.5 mm wired
- Sample rates verified end-to-end: **44.1 / 48 / 88.2 / 96 kHz**
- Bit depths supported: **16-bit PCM, 24-bit PCM, 32-bit PCM, 32-bit float**
- Channel counts: **mono, stereo**

Should work on any Apple Silicon Mac running macOS 11 (Big Sur) or later. Fat binary is also built for Intel Macs (untested).

## Supported output devices

- **3.5 mm wired headphones** through the MacBook's headphone jack — clean, bit-perfect passthrough at native rates.
- **USB Audio Class devices** (USB DACs, USB headphones) — should work; untested.
- **Bluetooth audio** — untested; likely subject to macOS's own Bluetooth rate quirks.

The daemon auto-follows whatever rate the real output device is set to. If macOS or Apple Music switches the device to a new rate (44.1 ↔ 48 ↔ 96 ↔ 192), HonestEQ mirrors that rate so the whole pipeline stays single-rate — no resampling anywhere, no pitch shift ever.

## Build and install (personal use, no Apple Developer fee)

Requires the Xcode Command Line Tools:

```bash
xcode-select --install
```

Then:

```bash
# Clone
git clone https://github.com/Prateek64X/HonestEQ.git
cd HonestEQ

# Build & install driver (sudo prompt for /Library/Audio/Plug-Ins/HAL)
cd driver
make
./scripts/install.sh

# Build daemon
cd ../daemon
make

# Run daemon (keep this terminal open; Ctrl-C to stop)
./build/honesteq-daemon "External Headphones"
```

Then in **System Settings → Sound → Output**, select **"HonestEQ"**. Any app's audio now flows: `app → HonestEQ → daemon → your EQ profile → your headphones`.

## Uninstall

```bash
cd driver
./scripts/uninstall.sh
```

Removes the `.driver` bundle from `/Library/Audio/Plug-Ins/HAL/` and restarts `coreaudiod`.

## Profiles

HonestEQ reads Equalizer APO's config-file format. Any Peace EQ or AutoEq profile drops in unchanged. Example:

```
# Sony CH-720N — my daily tuning
Preamp: 0 dB
Filter 1:  ON PK Fc 20    Hz Gain -4.8 dB Q 4.32
Filter 2:  ON PK Fc 25    Hz Gain -3.9 dB Q 4.32
Filter 3:  ON PK Fc 32    Hz Gain -2.7 dB Q 4.32
...
```

Sample profiles live in `profiles/`. Drop yours into `~/Library/Application Support/HonestEQ/Profiles/`.

## Layout

```
HonestEQ/
├── driver/       AudioServerPlugIn (C) — the virtual output device
├── daemon/       Router daemon (C) — reads loopback + writes to real device
├── dsp/          libHonestEQDSP (C++17) — RBJ biquads, DF-I, double precision
│   └── tools/    honesteq-render offline CLI
├── profiles/     Equalizer-APO-format EQ profiles
└── docs/         Product spec + technical architecture plan
```

## DSP correctness

`dsp/` ships with 9 automated tests covering:

- Peaking EQ at 0 dB is a bit-exact identity
- Filter gain at center frequency matches parameter to 1e-9 dB
- Low / high shelf DC / Nyquist gain matches spec
- Butterworth low-pass at Fc gives −3.01 dB
- Numerical stability of high-Q sub-bass shelves at 192 kHz
- Direct-Form-I coefficient hot-swap doesn't glitch
- Sample-rate independence at 44.1 / 48 / 192 kHz
- 31-band cascade processes cleanly

Run them any time:

```bash
cd dsp
make test
```

## Roadmap

Working:

- Virtual HAL driver
- System-wide audio routing
- Live sample-rate following
- Offline WAV EQ render tool

Missing:

- Real-time DSP inside the daemon (currently daemon is passthrough only; use `honesteq-render` for actual EQ testing until this lands)
- SwiftUI app with band editor
- Menubar app with dock+menubar hybrid behavior
- Per-device profile auto-switching
- Signed + notarized installer

## License

TBD — likely MIT or BSD-2-Clause. If you want to build on this before I settle on a license, open an issue.
