# HonestEQ — Companion Daemon

Runs alongside the driver. Opens HonestEQ as an input device (reading its loopback),
processes audio through the DSP chain, and writes to the user's real output device
(e.g. External Headphones).

## Build

```bash
cd daemon
make
```

Produces `build/honesteq-daemon`.

## Run

```bash
# Argument: name (or UID) of the real output device to send audio to.
./build/honesteq-daemon "External Headphones"
```

While this is running, and HonestEQ is selected as system output, any app's audio
gets EQ'd and comes out of your headphones.

## Stages

- **Stage 1 (this build)**: passthrough only — audio flows HonestEQ → daemon → real output, no DSP.
- **Stage 2**: apply BiquadChain from `dsp/` — same math verified in `honesteq-render`.
- **Stage 3**: profile hot-reload from `~/Library/Application Support/HonestEQ/active_profile.txt`.
- **Stage 4**: launchd plist so it starts at login.
