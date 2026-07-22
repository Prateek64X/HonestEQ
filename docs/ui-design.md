# HonestEQ UI Design

The GUI on top of the daemon. Native macOS SwiftUI app, one main window, close-to-menubar hybrid (Telegram-style — see `technical-plan.md` §10). This document specifies layout, controls, and interaction behavior *before* any Swift is written.

## Design principles

- **Native to macOS.** SwiftUI + AppKit interop. Every control is a stock Apple control (`Slider`, `Toggle`, `Menu`, `Button`, `TextField`). No custom-drawn knobs, no re-skinned widgets. Users should feel this could ship in System Settings.
- **Apple's HIG throughout.** System font (SF Pro), standard corner radii, `.regularMaterial` backgrounds, proper padding (`.padding()` defaults are fine 90% of the time), automatic dark mode, respects Reduce Motion, Dynamic Type-aware.
- **Simple like Peace EQ, ergonomic like the Music app.** Everything visible in one window. No hidden menus, no modal wizards. Add band → click, edit band → click, save profile → click.
- **Live preview.** Every change hits the daemon within one audio block (~5 ms). Sliding a gain knob → hear the shift immediately. Same for on/off, wet/dry, add/remove band.
- **Keyboard-first optional.** Every action has a keyboard shortcut (⌘⇧A add band, ⌘S save, ⌘⌥I import, etc.).

## Technology stack

| Layer | Choice |
|---|---|
| Framework | **SwiftUI** (macOS 14 Sonoma+ minimum for `NavigationSplitView` polish, `Gauge`) |
| Backend interop | **Obj-C++ bridge** to the existing `libHonestEQDSP` for preview curve rendering + reusing biquad math |
| App ↔ daemon IPC | v1 — **file-based** (write `active_profile.txt`, daemon watches with FSEvents and hot-reloads). v2 — XPC service. |
| Persistence | **UserDefaults** for window state, per-device profile bindings; **`.txt` files** under `~/Library/Application Support/HonestEQ/Profiles/` for actual profiles (Equalizer APO format, so they diff/git nicely) |
| Icons | SF Symbols throughout |
| Bundle ID | `com.honestEQ.app` (matches driver's `com.honestEQ.driver`) |

## Main window — expanded (sidebar visible)

```
╭────────────────────────────────────────────────────────────────────────────────╮
│  ● ● ●    HonestEQ                                             ⇥ Hide Sidebar  │  ← titlebar / toolbar
├────────────────────────────────────────────────────────────┬───────────────────┤
│                                                            │                   │
│   Pre-Amp      Dry / Wet       EQ                          │   DEVICES         │
│   ┌───────┐    ┌───────┐    ┌──────────┐                   │                   │
│   │ ──●── │    │ ────● │    │  ◯════   │  Active           │  Output through:  │
│   │  0.0  │    │ 100%  │    └──────────┘                   │  ⦿ External       │
│   │  dB   │    │       │                                   │    Headphones     │
│   └───────┘    └───────┘                                   │  ○ MacBook Air    │
│                                                            │    Speakers       │
│  ─────────────────────────────────────────────────────────  │  ○ Sony CH-720N   │
│                                                            │    (USB)          │
│   EQ Bands                       + Add   − Remove          │                   │
│                                                            │  ────────────     │
│                        Bands  [ 31 ] ↩                     │                   │
│                                                            │  Per-device       │
│  ┌──────────────────────────────────────────────────────┐  │  profile:         │
│  │                                                      │  │                   │
│  │  +6 ┃                                                │  │  External         │
│  │     ┃    ▲                                           │  │  Headphones       │
│  │  +3 ┃    │                                           │  │  ┌─────────────┐  │
│  │     ┃  ▼ │  ▼   ▲                                    │  │  │CH-720N      │  │
│  │   0 ┃──▬─▬──▬─▬─▬─▬─▬─▬─▬─▬─▬─▬─▬─▬─▬─▬─▬─▬─▬─▬──── │  │  │Neutral   ⌄  │  │
│  │     ┃  │ │  │   │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │      │  │  └─────────────┘  │
│  │  −3 ┃  ▼ ▼  ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼      │  │                   │
│  │     ┃                                                │  │  MacBook Air      │
│  │  −6 ┃                                                │  │  Speakers         │
│  │                                                      │  │  ┌─────────────┐  │
│  │  20  50  100  200  500  1k  2k  5k  10k  20k   Hz    │  │  │ (bypass) ⌄  │  │
│  └──────────────────────────────────────────────────────┘  │  └─────────────┘  │
│                                                            │                   │
│  ──────────────────────────────────────────────────────    │                   │
│                                                            │                   │
│   PROFILE                                                  │                   │
│                                                            │                   │
│   ┌────────────────────────────────────┐  ⭐ Active         │                   │
│   │  Sony CH-720N Neutral Sennheiser ⌄ │                   │                   │
│   └────────────────────────────────────┘                   │                   │
│                                                            │                   │
│   + New   ⤴ Import   ⎘ Paste   🗑 Delete                    │                   │
│                                                            │                   │
│   Saved profiles:                                          │                   │
│     • Sony CH-720N Neutral Sennheiser  ← active            │                   │
│     • HD 650 Reference                                     │                   │
│     • IEM Bass Boost                                       │                   │
│                                                            │                   │
╰────────────────────────────────────────────────────────────┴───────────────────╯
```

## Main window — collapsed (sidebar hidden)

```
╭────────────────────────────────────────────────────────────────────────────────╮
│  ● ● ●    HonestEQ                                             ⇤ Show Sidebar  │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│   Pre-Amp      Dry / Wet       EQ                                              │
│   ┌───────┐    ┌───────┐    ┌──────────┐                                       │
│   │ ──●── │    │ ────● │    │  ◯════   │  Active                               │
│   │  0.0  │    │ 100%  │    └──────────┘                                       │
│   │  dB   │    │       │                                                       │
│   └───────┘    └───────┘                                                       │
│                                                                                │
│  ─────────────────────────────────────────────────────────────────────────     │
│                                                                                │
│   EQ Bands                                          + Add    − Remove          │
│                                                                                │
│                                        Bands  [ 31 ] ↩                         │
│                                                                                │
│  ┌──────────────────────────────────────────────────────────────────────────┐  │
│  │  ...  (band editor — same layout as expanded, uses full window width)  │  │
│  └──────────────────────────────────────────────────────────────────────────┘  │
│                                                                                │
│   PROFILE   [Sony CH-720N Neutral Sennheiser ⌄]  ⭐ Active                     │
│   + New   ⤴ Import   ⎘ Paste   🗑 Delete                                        │
│                                                                                │
╰────────────────────────────────────────────────────────────────────────────────╯
```

The sidebar collapses with a `NavigationSplitViewVisibility` binding. Toolbar button toggles it; the animation is SwiftUI-default (smooth spring), which respects Reduce Motion.

## Component specs

### Toolbar (top of window)

- **Title**: "HonestEQ"
- **Trailing button**: SF Symbol `sidebar.right` — toggles the right sidebar
- **Trailing overflow menu** (three dots): Preferences, Reveal profiles folder, Restart daemon, About

### Top controls row — Pre-Amp, Dry/Wet, On/Off

Rendered as a horizontal row of three grouped `GroupBox`es (or `.padding()`-separated `HStack`s inside a rounded `.thinMaterial` container).

Apple doesn't provide a native rotary knob (`Slider` is the closest). Every "knob-like" control in HonestEQ is a compact horizontal `Slider` with a **click-to-edit numeric label** below it — the same pattern used in Logic Pro, Ableton, FabFilter, and Apple's own AU plug-ins. Users can drag OR type.

**Text-input principle** (applies to every value in the app):
- Every displayed numeric value is a `TextField` you can click to type in
- Drag path for coarse changes, type path for precise values
- Return to apply, Escape to cancel
- Out-of-range values clamp to the control's declared range with a brief visual acknowledgement

**Pre-Amp**
- **Control**: horizontal `Slider(value: $preampDb, in: -30...30, step: 0.1)` — matches Peace EQ's range (±30 dB standard). Enough negative headroom for heavy multi-band cuts, enough positive gain for niche corrective use.
- **Value label** below slider: `TextField` bound to the same value, formatted as `"±X.X dB"`. **Click the label → cursor enters, user types a new value (e.g. `-15.5`, `+3`, `0`), presses Return → applied.** Escape cancels the edit. This is the primary path for precise values — the slider is for coarse dragging.
- **Reset**: double-click on slider or on the value label resets to 0.0 dB.
- **Keyboard on slider**: ← / → for ±0.1 dB, ⇧← / ⇧→ for ±1.0 dB
- **Keyboard on text field**: Return to apply, Escape to cancel, ↑↓ arrow keys for ±0.1 dB while editing
- **Out-of-range typed values**: clamped to ±30 (with a subtle "clamped" indicator: label briefly flashes yellow, or a `.help()` tooltip explains the limit).

**Dry/Wet** ("EQ Intensity")
- **Control**: horizontal `Slider(value: $wetPercent, in: 0...100, step: 1)`
- **Value label**: click-to-edit `TextField` formatted as `"X %"`
- **Semantic name in UI**: "Intensity" (matches CLI flag `--intensity`)
- **Reset**: double-click → 100%
- **Keyboard**: same as pre-amp — arrows for ±1%, ⇧+arrows for ±10%

**On / Off**
- **Control**: SwiftUI `Toggle` with `.toggleStyle(.switch)` — the standard macOS switch
- **Label**: right of the switch, either "Active" (green when on) or "Bypass" (grey when off)
- **When off**: all EQ knobs / sliders remain interactable and remembered, but the daemon runs passthrough. Turning back on restores instantly.

### EQ band editor

Middle third of the window — the main workspace. Layout mirrors Peace EQ's PEQ view: vertical sliders on top, per-band editable text fields below each slider, all click-to-type.

**Per-band column layout (top to bottom):**

```
     │              ← vertical slider (drag to change gain)
     │
   ──▬── 0 dB       ← zero-gain axis
     │
     │
   ┌─────┐
   │50   │ Hz       ← frequency (click to edit, or drag over label to nudge)
   └─────┘
   ┌─────┐
   │-3.5 │ dB       ← gain value (bound to slider, also editable)
   └─────┘
   ┌─────┐
   │4.32 │ Q        ← Q factor (click to edit)
   └─────┘
   ⊗                 ← per-band on/off toggle (SF Symbol, tiny)
```

- **Vertical slider** (SwiftUI `Slider` with `axis: .vertical`, macOS 14+). Range −12 to +12 dB, step 0.1. Drag to change gain, double-click to reset to 0 dB.
- **Frequency `TextField`** — accepts values like `50`, `1000`, `1.5k`, `20k`. Applied on Return. Numeric-only; log-scale reformats display.
- **Gain `TextField`** — bidirectional binding to the slider's value. Type `-6.5`, press Return → slider snaps to −6.5 dB. Or drag slider → text updates. Same value, two entry paths.
- **Q `TextField`** — defaults to 4.32 on new bands. Range 0.1–20 (higher than 20 becomes numerically unstable at extreme frequencies; clamped with a warning). Applied on Return.
- **Per-band on/off**: tiny circular SF Symbol icon (`circle.fill` green / `circle` grey) below the Q field. Click toggles the band without deleting it — matches Peace's per-band on/off.
- **Right-click any element** in the column → context menu with **Filter type** (PK / LS / HS / LP / HP / NO / BP / AP), **Duplicate band**, **Delete band**.

- **Response curve overlay** (behind the sliders): a live-computed cumulative magnitude curve rendered as a `Path` on a `Canvas`. Updates within one audio block when any parameter changes. Shows what the *combined* EQ actually does (not just individual band peaks). Colored with `Color.accentColor.opacity(0.3)`.
- **Zero-gain axis line**: horizontal `.foregroundStyle(.secondary)` line across the middle.
- **Horizontal scroll**: if band count × per-column width exceeds window width, the whole band strip becomes horizontally scrollable (`ScrollView(.horizontal)`) with the response curve stretching the full width behind it.

Above the band area, a row with:

- **`+ Add band`**: appends a new band at the log-midpoint of the last two, gain 0 dB, Q 4.32, type PK.
- **`− Remove band`**: removes the currently-selected band (highlighted with a subtle blue outline).
- **`Bands: [N] ↩`**: `TextField` with a placeholder "31". User types a number, presses Return → chain truncates or extends to that count. Log-spaced auto-fills any new bands.

### Sidebar (right side)

Standard `NavigationSplitView(sidebar:)` on the **trailing** edge — respects Apple's "primary content in leading, ancillary in trailing" convention.

**Content**:

- **"DEVICES" section header** (small caps, secondary color, per HIG)
- **"Output through:"** — a `Picker` with `.pickerStyle(.radioGroup)` listing every current CoreAudio output device (from `AudioObjectGetPropertyData`). Selecting one:
  - Changes the daemon's `--output-device` argument (via IPC or restart)
  - Sets the system default output to that device (so playback actually goes there)
- **Divider**
- **"Per-device profile"** section — one row per known output device (whether currently plugged in or not, remembered from past sessions):
  - Device name (with SF Symbol icon: `headphones`, `speaker.wave.2`, `airpodspro`, etc.)
  - `Menu` beneath it listing every saved profile + "(none — bypass)". Selecting binds that profile to that device.
  - On device change (unplug/replug), daemon auto-loads the bound profile. Same behavior as Peace's device-aware EQ.

Sidebar collapsible via toolbar button OR standard split-view drag handle.

### Profile management (bottom)

- **Active profile picker**: `Menu` showing current profile name with a dropdown chevron. Selecting swaps the active profile in the daemon within one audio block.
- **Active badge**: SF Symbol `star.fill` in `.tint(.yellow)` next to the picker, with label "Active".
- **Action buttons row** (SF Symbols + labels):
  - `+ New` → creates blank profile, prompts for name
  - `⤴ Import` → NSOpenPanel filtered to `.txt` — parses any Peace/AutoEq config, saves as new profile
  - `⎘ Paste` → sheet with a `TextEditor` — user pastes raw config text, hit Save
  - `🗑 Delete` → confirmation alert, removes the file
- **Profile list** below: `List` of saved profiles. Active one has `Image(systemName: "checkmark")` before its name. Click to switch active. Right-click for Rename / Duplicate / Reveal in Finder.

## Interactions

| Action | Path |
|---|---|
| Change a band gain | Drag its slider |
| Change band frequency | Right-click slider → "Frequency" → text field |
| Change band Q | Right-click slider → "Q" → text field |
| Add a band at log-midpoint | Click `+ Add band` |
| Set N bands in one shot | Type into "Bands: [N]" and press Return |
| Toggle EQ on/off | Click the "Active" toggle |
| Adjust pre-amp | Drag the Pre-Amp slider |
| Reset pre-amp / wet-dry | Double-click the slider |
| Save current settings as a new profile | `+ New`, name it |
| Import Peace config from your PC | `⤴ Import` |
| Paste an AutoEq config from clipboard | `⎘ Paste`, ⌘V, save |
| Switch profile | Click profile picker or list row |
| Bind a profile to a specific headphone | Sidebar → per-device dropdown |
| Show / hide sidebar | Toolbar button or drag divider |
| Reveal profile folder | Toolbar overflow menu |

Keyboard shortcuts: ⌘S save profile, ⌘⇧A add band, ⌘⌫ remove band, ⌘0 reset pre-amp, ⌘1 toggle EQ on/off, ⌘\\ toggle sidebar, ⌘⌥I import, ⌘⌥P paste.

## State management — how the UI syncs with the daemon

Two directions, both file-based in v1:

**UI → daemon**

- Any user change (add band, adjust gain, change profile) is immediately written to `~/Library/Application Support/HonestEQ/active_profile.txt` and any relevant sidecar files (per-device bindings in a `.plist`).
- Daemon watches `active_profile.txt` via `FSEventStreamCreate` and reloads within ~100 ms of the write completing.
- **Latency budget** from slider drag → audible change: file write + FSEvents debounce + daemon reload + one audio block ≈ **150–250 ms**. Feels instant to the user; only measurable by A/B recording.

**Daemon → UI**

- App polls the daemon's stats endpoint (Unix socket, added later) or reads status from a file the daemon writes every second.
- v1 doesn't need this — the UI is the sole authority on what should be playing. Daemon is a slave.

## Directory layout for the app

```
app/
├── HonestEQ.xcodeproj
├── HonestEQ/
│   ├── HonestEQApp.swift              # entry point, .commands { }, launch-at-login registration
│   ├── AppState.swift                 # ObservableObject: bands, preamp, wetDry, on/off, profiles
│   ├── Views/
│   │   ├── MainWindow.swift           # NavigationSplitView container
│   │   ├── TopControlsView.swift      # Pre-Amp / Dry-Wet / On-Off row
│   │   ├── BandEditorView.swift       # vertical sliders + response curve overlay
│   │   ├── BandRow.swift              # one band's slider + labels + context menu
│   │   ├── ResponseCurveView.swift    # Canvas rendering the summed frequency response
│   │   ├── DevicesSidebar.swift       # trailing NavigationSplitView content
│   │   └── ProfileSection.swift       # bottom: picker + buttons + list
│   ├── Bridge/
│   │   ├── DSPBridge.h                # Obj-C header exposing libHonestEQDSP to Swift
│   │   ├── DSPBridge.mm               # thin bridge: BiquadChain response curve for UI preview
│   │   └── DaemonIPC.swift            # writes profile files, monitors daemon status
│   ├── Model/
│   │   ├── Band.swift                 # Codable, ObservableObject
│   │   ├── Profile.swift              # Codable — Equalizer APO format read/write
│   │   └── OutputDevice.swift         # CoreAudio device wrapper
│   └── Resources/
│       ├── Assets.xcassets            # app icon
│       └── Info.plist                 # LSUIElement = NO (dock+menubar hybrid handled at runtime)
```

## Menubar hybrid behavior (per technical-plan §10)

- App normally has a dock icon and a normal window.
- Clicking the **window close button (red)** does *not* quit — the window hides and the app stays running as a menubar item.
- Clicking the menubar item reopens the main window (same NSWindow instance, unhidden).
- **⌘Q** actually quits.
- Menubar icon is a small SF Symbol `waveform.circle.fill` (green when EQ is active, grey when bypassed).

## Non-goals for v1

Explicitly out of scope for the first UI cut, defer to later:

- Convolution / IR loading
- Spectrum analyzer overlay
- Per-band on/off toggles (already available via right-click; no dedicated UI element to keep the band grid clean)
- Cross-fade between profiles (currently instant swap; acceptable for typical use)
- Preset store / online catalog of profiles
- Multi-window (multiple simultaneous windows editing different profiles)

## Notes on accessibility

- Every slider has `.accessibilityLabel` and `.accessibilityValue` announcing the parameter, current value, and units ("Pre-amplifier, negative three point five decibels").
- VoiceOver friendly: no gesture-based-only controls; every action has a keyboard equivalent.
- Sliders respond to standard VoiceOver focus + arrow-key adjustment.
- Reduced Motion respected — no springy overshoot in sidebar/panel transitions when the setting is on.
