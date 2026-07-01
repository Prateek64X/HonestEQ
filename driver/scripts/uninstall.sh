#!/usr/bin/env bash
# Remove HonestEQ.driver from /Library/Audio/Plug-Ins/HAL/ and restart coreaudiod.
set -euo pipefail

DEST="/Library/Audio/Plug-Ins/HAL/HonestEQ.driver"

if [ -d "$DEST" ]; then
    echo "Removing $DEST ..."
    sudo rm -rf "$DEST"
fi

echo "Restarting coreaudiod ..."
sudo killall -9 coreaudiod || true

echo "Done."
