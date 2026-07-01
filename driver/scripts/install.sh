#!/usr/bin/env bash
# Install the HonestEQ.driver bundle into the system HAL plugins folder
# and restart coreaudiod so the new device is picked up.
#
# Requires sudo (because /Library/Audio/Plug-Ins/HAL is a system path).
# No reboot — just a coreaudiod restart, which momentarily silences audio.
# Ad-hoc signed (codesign -s -) so no Developer ID is required for personal use.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRIVER_DIR="$SCRIPT_DIR/.."
BUNDLE="$DRIVER_DIR/build/HonestEQ.driver"
DEST="/Library/Audio/Plug-Ins/HAL/HonestEQ.driver"

if [ ! -d "$BUNDLE" ]; then
    echo "ERROR: $BUNDLE not built yet. Run 'make' first."
    exit 1
fi

echo "Ad-hoc signing $BUNDLE ..."
codesign --force --deep --sign - "$BUNDLE"

echo "Removing any previous install at $DEST ..."
sudo rm -rf "$DEST"

echo "Installing to $DEST ..."
sudo cp -R "$BUNDLE" "$DEST"
sudo chown -R root:wheel "$DEST"
sudo chmod -R 755 "$DEST"

echo "Restarting coreaudiod ..."
sudo killall -9 coreaudiod || true

echo ""
echo "Install complete. Open System Settings -> Sound -> Output."
echo "You should see 'HonestEQ' in the list (audio routing not active yet — pass 2 adds the device & stream property model)."
