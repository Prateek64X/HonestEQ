# HonestEQ

A system-wide parametric equalizer for macOS. Built to match the audible quality of Peace EQ (Windows) and Poweramp (Android) on the Mac.

## Why

If you've EQ'd your headphones with Peace/Equalizer APO on Windows or Poweramp on Android, you know how good a bit-perfect parametric EQ chain sounds. macOS doesn't have a first-party system EQ, and the existing third-party tools (eqMac, BlackHole+Reaper, SoundSource) either resample audio in ways that shift pitch, or use single-precision math that muddies dense multi-band cuts.

HonestEQ is my attempt at doing it right on the Mac, from scratch, with no Apple Developer fee for personal use.

## What it is

Four components:

- **`driver/`** — CoreAudio HAL plug-in (`HonestEQ.driver`, C). Registers a virtual output device named "HonestEQ" in System Settings.
- **`daemon/`** — Router (`honesteq-daemon`, C + C++ bridge). Reads HonestEQ's loopback, applies EQ via `libHonestEQDSP`, writes to your real output device.
- **`dsp/`** — Double-precision RBJ biquad library (C++17). Matches Equalizer APO's math bit-for-bit. 9 automated correctness tests.
- **`dsp/tools/honesteq-render`** — Offline CLI. Applies an EQ profile to a WAV file. Great for validating your profile shape.

## Status

Early alpha. **Works and I use it daily.** Not yet packaged as an installer, no GUI — configure via text profile files (Equalizer APO / Peace EQ format).

Real-world performance on the test rig:
- **~5 ms end-to-end latency at 48 kHz** (~2.7 ms at 192 kHz) — same class as Peace EQ
- **Seamless sample-rate switching** — 44.1 / 48 / 88.2 / 96 / 192 kHz transitions cleanly, no pitch shift
- **Bit-perfect pipeline** — HonestEQ auto-mirrors the output device's rate; zero resampling
- **31-band double-precision DSP** — same output as `honesteq-render`, live, system-wide

## Tested on

- **MacBook Air M5** (2026)
- **macOS Tahoe 26.5.1 (25F80)**
- **Sony WH-CH720N** headphones, 3.5 mm wired
- Verified end-to-end at **44.1 / 48 / 88.2 / 96 kHz**
- Supports **16 / 24 / 32-bit PCM + 32-bit float**, **mono / stereo**

Should work on any Apple Silicon Mac running macOS 11 (Big Sur) or later. Intel Macs are also built (fat binary) but untested.

---

## Quick start

Fresh clone to hearing music in 5 commands:

```bash
git clone https://github.com/Prateek64X/HonestEQ.git
cd HonestEQ

# 1. Build & install the driver (asks for sudo once; safe — no reboot needed)
(cd driver && make && ./scripts/install.sh)

# 2. Copy a profile into place (or write your own — see "EQ Profiles" below)
mkdir -p ~/Library/Application\ Support/HonestEQ
cp profiles/sony_ch720n_neutral_sennheiser.txt \
   ~/Library/Application\ Support/HonestEQ/active_profile.txt

# 3. Build the daemon
(cd daemon && make)

# 4. Run the daemon — keep this terminal open (Ctrl-C to stop)
./daemon/build/honesteq-daemon "External Headphones"
```

Then in **System Settings → Sound → Output**, select **"HonestEQ"**. Any app's audio now flows through your EQ.

To hear raw audio again: pick your real device (External Headphones, MacBook Speakers) in the same menu.

---

## Requirements

- macOS 11.0+ (Apple Silicon or Intel)
- **Xcode Command Line Tools** — install with:
  ```bash
  xcode-select --install
  ```

Nothing else. No paid Apple Developer account, no App Store, no Homebrew dependencies.

---

## Build

Three independent things you can build:

### Driver (the HAL plug-in)

```bash
cd driver
make                # produces build/HonestEQ.driver
./scripts/install.sh  # installs to /Library/Audio/Plug-Ins/HAL/ (sudo required, one-time)
```

`install.sh` ad-hoc signs (`codesign -s -`) so no Developer ID is required. It also runs `sudo killall -9 coreaudiod` at the end to reload the driver — you'll get ~1 second of silence, then HonestEQ appears in Sound.

### Daemon (the router)

```bash
cd daemon
make                # produces build/honesteq-daemon
```

Rebuild it any time the DSP or daemon code changes. No sudo, no install step — you just run the resulting binary.

### DSP tests + offline render tool

```bash
cd dsp
make test           # runs 9 correctness tests (should all pass)
make tool           # builds honesteq-render CLI
```

`honesteq-render` usage:
```bash
./build/honesteq-render <profile.txt> <input.wav> <output.wav> [--start SEC] [--duration SEC]
```

Great for validating your profile against a reference — process the same WAV on Mac and on Windows (Peace EQ) and A/B them; they should sound identical.

---

## Run

While the daemon is running, HonestEQ acts as your system output.

```bash
./daemon/build/honesteq-daemon "External Headphones"
```

The argument is the **name** of the real output device where you actually want to hear audio (e.g. `"External Headphones"`, `"MacBook Air Speakers"`, `"Sony CH-720N"`, etc). If you give a wrong name, the daemon prints all available devices and exits — copy one and re-run.

Startup log tells you what's happening:

```
HonestEQ device found (id=346)
Output device 'External Headphones' found (id=339)
Initial rates: HonestEQ = 48000.0 Hz, External Headphones = 48000.0 Hz
Daemon internal rate: 48000.0 Hz. No SRC anywhere in the pipeline.
EQ profile loaded: /Users/you/Library/Application Support/HonestEQ/active_profile.txt
  Bands: 31, Preamp: +0.00 dB
  BufferFrameSize on input AUHAL = 128 frames
  BufferFrameSize on output AUHAL = 128 frames
Live rate-following: enabled
HonestEQ daemon running — DSP active (31 bands, preamp +0.00 dB).
```

If it says "running — passthrough (no profile loaded)", add a profile (next section) and restart the daemon.

Live diagnostic stats print every 2 seconds:

```
[stats] fill=+0 u=+0 r=+96000 w=+96000 peak_in=0.28 peak_out=0.28
```

Healthy = `fill` stable, `u` (underruns) near 0, `peak_in == peak_out`.

---

## EQ Profiles

HonestEQ reads plain-text **Equalizer APO** / **Peace EQ** configuration files. Any existing Peace or AutoEq profile drops in unchanged.

### Adding a profile

The daemon reads a single active profile from:

```
~/Library/Application Support/HonestEQ/active_profile.txt
```

Copy any profile into that location and restart the daemon:

```bash
mkdir -p ~/Library/Application\ Support/HonestEQ
cp your_profile.txt ~/Library/Application\ Support/HonestEQ/active_profile.txt

# Restart daemon to load
# (Ctrl-C in the daemon terminal, then re-run)
./daemon/build/honesteq-daemon "External Headphones"
```

### Profile format

Same as Equalizer APO's config file:

```
Preamp: -1.5 dB
Filter 1:  ON PK Fc 20    Hz Gain -4.8 dB Q 4.32
Filter 2:  ON PK Fc 25    Hz Gain -3.9 dB Q 4.32
Filter 3:  ON PK Fc 32    Hz Gain -2.7 dB Q 4.32
Filter 4:  ON PK Fc 40    Hz Gain -2.7 dB Q 4.32
...
```

Supported filter types: `PK` (peaking), `LS` / `LSC` (low shelf), `HS` / `HSC` (high shelf), `LP` / `LPQ` (low-pass), `HP` / `HPQ` (high-pass), `NO` (notch), `BP` (band-pass), `AP` (all-pass).

Number of bands is unlimited — Peace's typical 31-band 1/3-octave format works fine.

### Sample profile included

`profiles/sony_ch720n_neutral_sennheiser.txt` — my daily-driver tuning. Diffused-field target + Sennheiser HD 600 (2020) corrections applied to the Sony CH-720N. 31 bands, Q = 4.32.

---

## Uninstall

Removes the `.driver` bundle from `/Library/Audio/Plug-Ins/HAL/` and restarts coreaudiod:

```bash
cd driver
./scripts/uninstall.sh
```

Stop the daemon with Ctrl-C in its terminal. Delete `~/Library/Application Support/HonestEQ/` to remove your saved profile.

---

## Layout

```
HonestEQ/
├── driver/       AudioServerPlugIn (C) — the virtual output device
├── daemon/       Router (C + C++ bridge) — reads loopback, EQs, writes to real device
├── dsp/          libHonestEQDSP (C++17) — RBJ biquads, DF-I, double precision
│   ├── include/
│   ├── tests/    MicroTest + Catch2-compatible correctness tests
│   └── tools/    honesteq-render offline CLI
├── profiles/     Sample Equalizer-APO-format EQ profiles
└── docs/         Product spec + technical architecture
```

---

## DSP correctness

`dsp/` ships with 9 automated tests covering:

- Peaking EQ at 0 dB gain is a bit-exact identity
- Filter gain at center frequency matches parameter to 1e-9 dB
- Low / high shelf DC / Nyquist gain matches spec
- Butterworth low-pass at Fc gives −3.01 dB
- Numerical stability of high-Q sub-bass shelves at 192 kHz
- Direct-Form-I coefficient hot-swap doesn't glitch
- Sample-rate independence at 44.1 / 48 / 192 kHz
- 31-band cascade processes cleanly

Run:
```bash
cd dsp
make test
```

---

## Troubleshooting

**No audio when I select HonestEQ:**
- Is the daemon running? It has to be running to route audio to your real output device.
- Did you give the daemon the correct output-device name? The startup log lists all devices if the name doesn't match.

**HonestEQ missing from Sound settings:**
- Reinstall the driver: `cd driver && ./scripts/install.sh`
- Or restart coreaudiod manually: `sudo killall -9 coreaudiod`

**Audio has clicks or dropouts:**
- Check daemon stats — if `underruns` grows continuously, timing is off. Usually caused by a device buffer size mismatch.
- Try switching output device rate in Audio MIDI Setup — 48 kHz is the safest default.

**Volume slider doesn't move or is greyed out:**
- Rebuild + reinstall the driver: `cd driver && make clean && make && ./scripts/install.sh` — the master-volume control lives in the driver, and any missing / stale build can leave it disabled.
- If the slider position sticks after driver reinstall, try `sudo killall -9 coreaudiod` to force a full re-registration.

**"Not accepted" warnings on install:**
- `codesign` may complain about existing signatures — usually harmless, the driver still installs. If it doesn't, check permissions on `/Library/Audio/Plug-Ins/HAL/`.

---

## Roadmap

Working:

- Virtual HAL driver
- System-wide audio routing with live rate-following
- Real-time DSP (RBJ biquads, double precision)
- Offline WAV EQ render tool
- Peace / Equalizer APO profile compatibility
- Sub-10 ms latency

Missing:

- SwiftUI app with band editor
- Menubar app (dock + menubar hybrid)
- Per-device profile auto-switching
- launchd plist for auto-start at login
- Signed + notarized installer

---

## License

TBD — likely MIT or BSD-2-Clause. If you want to build on this before I settle on a license, open an issue.
