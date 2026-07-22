# HonestEQ — macOS App

SwiftUI GUI on top of the driver + daemon. Design spec: `../docs/ui-design.md`.

## Build

```bash
cd app
./build.sh                 # release, universal binary
./build.sh debug           # debug, faster iteration
```

Produces `build/HonestEQ.app`. Ad-hoc signed (no Developer ID required).

## Run

```bash
open build/HonestEQ.app
```

Or double-click the app in Finder.

Reads the active profile from `~/Library/Application Support/HonestEQ/active_profile.txt` at launch — same location the daemon uses. The app is the "brain": it writes the profile file, daemon consumes it. IPC via file watching (FSEvents) lands in a follow-up.

## Iteration status

**Iteration 1 (current)**: skeleton runs, top-controls row works (Pre-Amp / Dry-Wet sliders with click-to-edit values, On-Off toggle). Band editor, profile management, and device sidebar are placeholders.

**Iteration 2 (next)**: band editor with vertical sliders + Freq / dB / Q text fields per column + response-curve overlay.

**Iteration 3**: profile management (save / load / paste / import).

**Iteration 4**: device sidebar + per-device profile binding.

**Iteration 5**: daemon IPC (write profile changes to the active file, daemon hot-reloads).

**Iteration 6**: menubar hybrid behavior + launch-at-login.

## Requirements

- macOS 14 (Sonoma) or later
- Xcode Command Line Tools (already required for driver + daemon)
