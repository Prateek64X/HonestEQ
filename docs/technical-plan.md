# HonestEQ — Technical Plan

Companion to `equalizer-plan.md`. This document picks the architecture, the tech stack, and the engineering plan to get a Peace-quality system EQ on macOS with no pitch shift at 44.1 / 48 / 192 kHz.

---

## 1. Why the BlackHole + Reaper attempt failed

The pitch-shift you heard is sample-rate metadata being ignored somewhere in the chain — not a "bad resampler." If 48 kHz samples are handed to a device running at 192 kHz with no sample-rate conversion (SRC), you get a literal 4× playback speed, i.e. **+2 octaves up**. That is exactly the symptom you described, and it is a known BlackHole failure mode — issue [#465](https://github.com/ExistentialAudio/BlackHole/issues/465) reports it verbatim, and the maintainer confirms: "Sounds like a sample rate issue."

BlackHole itself does **no** SRC — it ships as a ring-buffer passthrough that serves exactly one rate at a time (whatever Audio MIDI Setup has it set to). CoreAudio also does not auto-resample sub-devices in an Aggregate device unless you tick **Drift Correction** explicitly — Apple's own docs say "all devices in the aggregate device need to have the same sample rate." Reaper, BlackHole, and your DAC almost certainly weren't all at the same rate, and drift correction wasn't on. That's the whole bug.

Lesson for HonestEQ: **sample rate is not something you "support" — it's something you actively chase.** The engine has to follow the physical output device's current nominal rate every time it changes, and recompute filter coefficients accordingly.

---

## 2. Why a HAL plugin is the only sensible choice

There is no macOS equivalent of Windows' APO chain. Apple does not expose a hook into `coreaudiod`'s mix graph. The available options for intercepting system output, audited:

| Option | Can capture system audio? | Can emit modified audio? | Verdict |
|---|---|---|---|
| **AudioServerPlugIn** (HAL plugin) — what BlackHole, eqMac, Background Music, SoundSource all use | Yes (user routes output to it) | Yes | **Use this.** |
| **Process Tap** (`CATapDescription`, macOS 14.4+) | Yes (incl. system mix) | **No — read-only.** Tap → aggregate → IOProc one-way. | Useless for EQ playback. |
| **Audio Unit v3 effect extension** | No — host-loaded only | Within host only | Useful as an *in-DAW* plugin, not system-wide. |
| **DriverKit / kext** | n/a | n/a | DriverKit doesn't cover audio mix. Kexts deprecated. |

So the architecture is forced:

```
              ┌────────────────────────────────────────────┐
              │                  macOS                      │
              │                                            │
   apps  ──►  │  default output = "HonestEQ" (virtual)     │
              │             │                              │
              │             ▼  (loopback samples)          │
              │  ┌────────────────────────┐                │
              │  │ HonestEQ.driver        │                │
              │  │ (AudioServerPlugIn,    │                │
              │  │  user-space CFPlugIn)  │                │
              │  └─────────┬──────────────┘                │
              │            │ shared memory ring             │
              │            ▼                                │
              │  ┌────────────────────────┐                │
              │  │ HonestEQ.app           │                │
              │  │  - DSP (biquad chain)  │                │
              │  │  - UI                   │                │
              │  │  - profile I/O          │                │
              │  └─────────┬──────────────┘                │
              │            │ writes via AUHAL/CoreAudio    │
              │            ▼                                │
              │     real output device (DAC, AirPods, …)   │
              └────────────────────────────────────────────┘
```

This is the eqMac / Background Music shape. Confirmed working on macOS 10.10 through current; no kext, no reboot strictly required (a `sudo killall -9 coreaudiod` re-enumerates devices). The user must switch their default output to "HonestEQ" once — the app does that automatically on launch and restores on quit.

---

## 3. Sample-rate strategy — the part that fixes your bug

Three rules:

1. **The virtual device's nominal rate is the master.** When the user (or HonestEQ) sets `kAudioDevicePropertyNominalSampleRate` on the virtual device to 48 kHz, every app that opens it sees 48 kHz. macOS's `coreaudiod` mixer handles per-app conversion on the way in — that's free and high quality.
2. **The real output device's nominal rate is the slave we follow.** On launch, and on every `kAudioDevicePropertyNominalSampleRate` change notification (registered via `AudioObjectAddPropertyListener`), HonestEQ.app:
   - reads the new rate;
   - calls `AudioConverterNew` (Apple's vDSP-accelerated SRC) only if the virtual device rate differs;
   - recomputes every biquad's coefficients for the new Fs (since `omega = 2π·fc/Fs`);
   - flushes filter state to zero to avoid a transient.
3. **Best path = match.** The simplest, lowest-latency, bit-perfect option is to set the virtual device to the same rate as the real output. HonestEQ defaults to that. The supported set (44.1 / 48 / 96 / 176.4 / 192 kHz) is declared in the driver's stream description list. If the user manually forces a mismatch, we still work — but via `AudioConverter`, which produces a clean SRC, not a pitch shift.

This is the explicit fix for the Reaper+BlackHole problem: HonestEQ *always* knows the real device rate and either matches or properly converts. The user never sees a 4× speedup.

---

## 4. DSP design — Peace-grade transparency

The reference algorithm is **RBJ Audio EQ Cookbook biquads** — the same family JUCE, Web Audio, Equalizer APO, and every commercial parametric EQ use. Closed-form coefficients per band:

```
omega = 2 * pi * Fc / Fs
alpha = sin(omega) / (2 * Q)
A     = 10^(gain_dB / 40)
```

For each filter type (peakingEQ, lowShelf, highShelf, lowPass, highPass) the cookbook gives b0..a2 directly. No approximation, no design iteration — closed form.

Engineering choices:

- **64-bit double precision throughout.** Direct-form biquads accumulate round-off noise proportional to `1 / (1 - |pole|²)`. At 192 kHz with a 30 Hz shelf at Q=2, the pole sits at radius ≈ 0.9995 and the noise floor amplification is ~1000×. float32 will introduce audible hash. Equalizer APO (which is what you're comparing against in Peace) uses `double` for state and coefficients. We match.
- **Direct Form I** structure, not Transposed Direct Form II. DF-I is **safe for time-varying coefficients** (Julius Smith: "in DF-I there is no possibility of internal filter overflow… an important, valuable, and unusual property"). TDF-II's state variables are partial sums tied to the previous coefficient set; swapping them mid-stream produces audible clicks. We will get parameter changes from the UI constantly, so DF-I is the right pick.
- **Parameter smoothing.** Each user-facing parameter (band gain, freq, Q, pre-amp) is wrapped in a one-pole smoother with a ~20 ms ramp. We recompute coefficients **per audio block** (typically 256–512 samples) from the smoothed values, not per knob movement. This is JUCE's `SmoothedValue` pattern.
- **Pre-amp before bands.** Single scalar gain applied first, so heavy positive band gains can be balanced by pulling pre-amp negative — same model as Peace.
- **Stereo, linked.** v1 applies identical filtering to L and R. The processing core is per-channel inside, so per-channel/per-band controls are a clean v2.
- **Optionally use `vDSP_biquadm` + `vDSP_biquadm_SetTargetsSingle`.** Accelerate's multichannel biquad takes "target" coefficients and ramps internally, which is zipper-free for free. The catch: `vDSP_biquadm` is single-precision. Our plan is **double-precision DF-I in C++ for the band chain**, and Accelerate only for SIMDable utility loops (pre-amp gain, mixing). The CPU cost of 20 double-precision biquads at 192 kHz is ~1% of one core on Apple Silicon. We are not CPU-bound.

### DSP block diagram

```
input float32 (from driver) ──► float64 convert
                              ──► pre-amp gain
                              ──► biquad #1 (peaking/shelf/etc, DF-I, double)
                              ──► biquad #2
                              ──► …
                              ──► biquad #N
                              ──► soft clip ceiling (-0.5 dBFS, optional)
                              ──► float64 → float32 dither
                              ──► AUHAL output to real device
```

Soft clip is opt-in; default off (Peace also defaults off). Dither (TPDF, 1 LSB) is on by default when converting back to 32-bit float for the device — it costs nothing and prevents low-level truncation correlation. For 24-bit and 32-bit-integer DACs we honor the device's bit depth.

---

## 5. Profile format — Equalizer APO compatible

Use the **Equalizer APO config syntax verbatim** for the file format. This means:

- You can paste your existing Peace `.txt` files into HonestEQ and they Just Work.
- You can copy AutoEq presets straight from `github.com/jaakkopasanen/AutoEq` and they Just Work.
- The file is human-readable, diffable, git-friendly.

Grammar (subset we implement in v1):

```
Preamp: <signed-num> dB
Filter <N>: ON  <TYPE> Fc <num> Hz  Gain <signed-num> dB  Q <num>
Filter <N>: OFF ...        ← parsed, retained, not applied
TYPE ∈ { PK, LS, HS, LSC, HSC, LP, HP, LPQ, HPQ, NO, AP }
```

Rules borrowed from EQ APO:

- Filter index after `Filter` is optional and ignored — `Filter: ON PK Fc 1000 Hz Gain -3 dB Q 1.0` is legal.
- Whitespace flexible.
- Any unrecognized line is silently ignored — `#` comments and blanks just work.
- Unknown filter types are skipped with a non-fatal warning in the app log.

v1 ignores (preserves on load+save, doesn't apply): `Channel:`, `Include:`, `Copy:`, `GraphicEQ:`, `Convolution:`, `Delay:`. These are noted in the parser so v2 can add channel-split and convolution without breaking existing profiles.

Storage location: `~/Library/Application Support/HonestEQ/Profiles/<name>.txt`. A user-visible **Reveal in Finder** button opens that folder. "Save" prompts for a name, "Save As" writes a new file, "Load" lists every `.txt` in the folder. "Paste" opens a sheet with a text area — paste any Peace/EQ APO config, hit OK, it's loaded.

Example saved file:

```
# HonestEQ profile — Sennheiser HD 650 (oratory1990 / AutoEq)
Preamp: -6.1 dB
Filter 1: ON LSC Fc 105 Hz Gain 6.4 dB Q 0.70
Filter 2: ON PK  Fc 8800 Hz Gain 5.1 dB Q 1.42
Filter 3: ON PK  Fc 118 Hz Gain -3.1 dB Q 0.50
Filter 4: ON PK  Fc 37 Hz Gain 0.7 dB Q 3.96
Filter 5: ON PK  Fc 3169 Hz Gain -1.7 dB Q 3.89
Filter 6: ON HSC Fc 10000 Hz Gain -2.1 dB Q 0.70
Filter 7: ON PK  Fc 1227 Hz Gain -1.2 dB Q 2.53
Filter 8: ON PK  Fc 2055 Hz Gain 1.2 dB Q 3.23
Filter 9: ON PK  Fc 587 Hz Gain 0.4 dB Q 1.19
Filter 10: ON PK Fc 5332 Hz Gain -1.1 dB Q 5.75
```

---

## 6. Tech stack

| Layer | Tech | Reason |
|---|---|---|
| Driver (virtual audio device) | **C / Swift**, AudioServerPlugIn API, built as a `.driver` CFPlugIn bundle | Only sanctioned route. eqMac proved Swift works here; we'll likely write the dispatch table in C for clarity and the rest in Swift. |
| DSP core | **C++17**, double-precision, no exceptions, no allocs in audio thread | Realtime safe. Single header per filter type. Trivially unit-testable. Accelerate.framework for SIMD utility loops only. |
| Host app | **Swift + SwiftUI** | Native look, fast to iterate, AppKit interop for the system audio device picker and menu-bar item. |
| Audio I/O in app | **AUHAL** (`kAudioUnitSubType_HALOutput`) for output to the real device; CoreAudio HAL APIs for device enumeration and property listening | Lowest-latency CoreAudio surface. Necessary for the rate-following logic. |
| Shared memory transport (driver → app) | **IOSurface** or `mach_make_memory_entry_64`-backed ring buffer with a semaphore | eqMac and Background Music both do this. Lock-free SPSC ring with `OSAtomic` / `std::atomic`. |
| Sample-rate conversion fallback | **`AudioConverter`** | Apple's high-quality SRC, vDSP accelerated, free. Only invoked when device-rate ≠ virtual-device-rate. |
| Profile parser | C++ or Swift, hand-written line tokenizer | Format is tiny; no need for a parser generator. |
| Build | **Xcode 16 + xcodebuild**; project split into a workspace with three targets: `HonestEQ.driver`, `HonestEQDSP` (static lib), `HonestEQ.app` | Standard mac development. |
| Tests | **XCTest** for the app, **Catch2** for the DSP lib | DSP gets impulse-response tests against reference Python implementation. |
| Installer | `pkgbuild` → `productbuild`, signed Developer ID, notarized via `notarytool`, stapled | Same pipeline BlackHole and eqMac use. |

Why not Electron / cross-platform: latency, audio-thread sensitivity, and the fact that the entire driver layer is Mac-only anyway. There is no portable code worth saving.

---

## 7. Project layout

```
HonestEQ/
├── HonestEQ.xcworkspace
├── driver/                     # AudioServerPlugIn
│   ├── HonestEQDriver.c        # Apple dispatch table (CFPlugIn factory)
│   ├── HonestEQDevice.swift    # virtual device
│   ├── HonestEQStream.swift
│   ├── RingBuffer.cpp/.hpp     # shared with app via IOSurface
│   └── Info.plist
├── dsp/                        # libHonestEQDSP (C++17, no deps but Accelerate)
│   ├── Biquad.hpp              # RBJ closed-form, DF-I, double
│   ├── BiquadChain.hpp
│   ├── PreAmp.hpp
│   ├── Smoother.hpp            # one-pole param smoothing
│   ├── Engine.hpp / .cpp       # owns chain, processes blocks
│   └── tests/                  # Catch2 — impulse responses, sweeps
├── app/                        # SwiftUI host
│   ├── HonestEQApp.swift
│   ├── AudioEngine/
│   │   ├── DeviceManager.swift # CoreAudio device enum + listener
│   │   ├── RateFollower.swift  # sample-rate strategy from §3
│   │   ├── DriverIPC.swift     # shared-memory ring consumer
│   │   └── OutputUnit.swift    # AUHAL writer
│   ├── DSPBridge/
│   │   └── EngineBridge.mm     # thin Obj-C++ shim to libHonestEQDSP
│   ├── Profile/
│   │   ├── ProfileStore.swift  # ~/Library/Application Support/HonestEQ/Profiles
│   │   └── PeaceConfigParser.swift
│   ├── UI/
│   │   ├── BandRow.swift       # one band: freq, gain, Q, type, on/off
│   │   ├── BandsView.swift     # add/remove/set-count
│   │   ├── PreAmpView.swift
│   │   ├── OutputPickerView.swift
│   │   ├── ProfileBarView.swift
│   │   └── PasteImportSheet.swift
│   └── Resources/
└── installer/
    ├── build_pkg.sh
    ├── distribution.xml
    └── postinstall              # killall -9 coreaudiod
```

---

## 8. Build / milestones

**M0 — Hello driver (week 1)**
- Fork from Apple's "Null Audio Server Driver Plug-in" sample.
- Build, install to `/Library/Audio/Plug-Ins/HAL/`, see `HonestEQ` in System Settings → Sound.
- Pass-through audio works (no DSP yet).

**M1 — DSP library standalone (week 1, parallel)**
- Implement RBJ biquads (PK, LS, HS, LP, HP) in C++17.
- Catch2 tests: impulse response of a peaking filter at known Fc/Q/gain matches scipy reference within 1e-10.
- Frequency sweep test: measure magnitude response vs. analytic, < 0.01 dB error.

**M2 — App + driver IPC (week 2)**
- Shared-memory ring between driver and app via IOSurface.
- App reads samples from driver, runs DSP (single hardcoded band), writes to default output device via AUHAL.
- Verify no glitches at 44.1 / 48 / 192 kHz.

**M3 — Sample-rate following (week 2)**
- Device-change and rate-change listeners.
- Reproduce the user's BlackHole+Reaper scenario manually (mismatched rates) — verify HonestEQ produces no pitch shift, while a raw passthrough does. **This is the acceptance gate that proves we fixed the bug.**

**M4 — Full UI (week 3)**
- Add band, remove band, "set N bands" field.
- Per-band freq / gain / Q / type pickers, with paste support and -12…+12 dB clamp.
- Pre-amp slider with paste.
- Output device picker.

**M5 — Profiles + Peace import (week 3)**
- Save / Load / Save As / Reveal in Finder.
- Paste-import sheet — accepts Peace or AutoEq config text.

**M6 — Polish, installer, notarization (week 4)**
- Menu-bar item, launch-at-login, restore output device on quit.
- pkg build, sign, notarize, staple.
- README + LICENSE (BSD or MIT — eqMac's restrictive license is part of why we're not forking).

**M7 — Listening tests**
- Blind A/B against Peace EQ on Windows on the same headphones at 48 kHz with identical band settings. Must be indistinguishable. This is the spec.

---

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Bluetooth / AirPods rate quirks (AirPods report 24 kHz when mic is active) | Rate-follower handles arbitrary rates via `AudioConverter`. Detect and warn the user when output rate < 44.1 kHz. |
| Notarization rejection on the driver | Follow BlackHole's exact pkg structure; hardened runtime on driver and app; `notarytool` not legacy altool. |
| Latency on AirPods (already high) | We add ~5–10 ms (one audio block). Inside the threshold for casual playback; document it. Not suitable for live monitoring — that's a non-goal. |
| User has SIP partially disabled / weird system state | The HAL plugins folder is **not** SIP-protected, so we're fine. Document the manual `killall coreaudiod` fallback. |
| 192 kHz CPU cost | Double-precision DF-I biquad ~10 ns/sample/band on Apple Silicon → 20 bands × 192k × 2ch ≈ 2 ms CPU per second of audio → ~0.2% one core. Trivial. |
| Need to restart audio on driver update | Pkg postinstall runs `killall -9 coreaudiod`. No reboot. |

---

## 10. Locked UX decisions (v1)

These are the user-facing behaviors agreed on at planning time:

- **Hybrid dock + menubar app (Telegram-style).** App has a dock icon and a normal window when active. Clicking the window's red close button does **not** quit — the window hides and the app remains alive as a menubar status item. Clicking the menubar icon reopens the window. Only `Cmd+Q` actually quits.
  - Implementation: `Info.plist` keeps `LSUIElement = false` (so the dock icon is visible). `NSWindowDelegate.windowShouldClose` returns `false` and instead calls `NSApp.hide(nil)`. An `NSStatusItem` in the menubar always exists; clicking it calls `NSApp.unhide(nil)`.
- **Launch at login.** Use `SMAppService.mainApp.register()` (Sonoma+). User can toggle in settings.
- **Per-device profile auto-switching.** Each physical output device (Sony CH-720N, HD 660, MacBook speakers, etc.) can have its own profile bound to it. When `kAudioHardwarePropertyDefaultOutputDevice` changes, HonestEQ looks up the device by its CoreAudio `kAudioDevicePropertyDeviceUID` (stable across reboots and USB ports), loads the bound profile if any, and applies it. If no profile is bound to a device, HonestEQ defaults to **bypass** (zero processing, signal passes through unchanged) — never apply the wrong profile to the wrong cans.
  - Storage: `~/Library/Application Support/HonestEQ/DeviceProfiles.plist` mapping `deviceUID → profileName`.
  - UI: small "Bind current profile to <device name>" button next to the output picker.
- **Profile management.** Save / Save As / Load from a flat list of `.txt` files in `~/Library/Application Support/HonestEQ/Profiles/`. **Paste Peace EQ config** sheet for one-click import of Windows configs.
- **No `$99` cost path for personal use.** Build, ad-hoc sign (`codesign -s -`), install locally. Works forever. The Apple Developer Program enrollment is only relevant if/when distributing publicly with a clean installer.

---

## 11. Open decisions for v2 (not blocking v1)

- Per-channel processing (L/R independent).
- Convolution (Peace doesn't do this either; AutoEq exports IRs).
- Spectrum analyzer overlay on the EQ curve.
- Auto-EQ database integration (pull AutoEq presets in-app).
- Sandboxed mini-app for the App Store using AUv3 instead of HAL (separate product).

---

## 12. References

- AudioServerPlugIn driver interface — https://developer.apple.com/documentation/coreaudio/audioserverplugindriverinterface
- BlackHole source and installer pipeline — https://github.com/ExistentialAudio/BlackHole
- eqMac source — https://github.com/bitgapp/eqMac
- Background Music — https://github.com/kyleneideck/BackgroundMusic
- RBJ Audio EQ Cookbook — https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
- Julius Smith on DF-I vs TDF-II — https://ccrma.stanford.edu/~jos/filters/Direct_Form_I.html
- Apple TN2091 (CoreAudio device properties) — https://developer.apple.com/library/archive/technotes/tn2091/_index.html
- Apple `vDSP_biquadm` — https://developer.apple.com/documentation/accelerate/vdsp_biquadm
- Equalizer APO configuration reference — https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/
- Equalizer APO BiQuad.h (double precision) — https://sourceforge.net/p/equalizerapo/code/HEAD/tree/trunk/filters/BiQuad.h
- BlackHole issue confirming pitch-shift = rate mismatch — https://github.com/ExistentialAudio/BlackHole/issues/465
- AutoEq parametric presets (test corpus) — https://github.com/jaakkopasanen/AutoEq
