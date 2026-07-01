# HonestEQ — AudioServerPlugIn (HAL plugin)

User-space virtual audio device for macOS. Built as a CFPlugIn bundle (`HonestEQ.driver`), installed into `/Library/Audio/Plug-Ins/HAL/`, and loaded by `coreaudiod` at next restart.

When loaded, it registers a virtual output device named "HonestEQ" that the user selects as their default system output. Any app's audio routed to it flows through the driver, which (in future iterations) applies the EQ chain and forwards processed audio to the user's real output device.

## Build & install (personal use, ad-hoc signed — $0)

```bash
cd driver
make            # builds HonestEQ.driver bundle
./scripts/install.sh    # installs to /Library/Audio/Plug-Ins/HAL/ and restarts coreaudiod (needs sudo, once)
```

After install, open System Settings → Sound → Output. You should see "HonestEQ" as an output choice.

## Uninstall

```bash
./scripts/uninstall.sh
```

## Architecture

Single C file (`HonestEQDriver.c`) implementing `AudioServerPlugInDriverInterface`. Bundle metadata in `Info.plist`. No Swift in the driver layer — C keeps the dispatch table simple and the build dependency-free.

The driver communicates with the HonestEQ.app via an `IOSurface`-backed ring buffer (to be added in pass 3). The app reads the loopback stream, runs the BiquadChain DSP we already built and tested in `dsp/`, and writes processed audio to the real output device via AUHAL.
