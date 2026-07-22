#!/bin/bash
# Remove the HonestEQ daemon LaunchAgent.
# Also kills any running instance.
set -e

PLIST="$HOME/Library/LaunchAgents/com.honesteq.daemon.plist"

if [ -f "$PLIST" ]; then
    launchctl unload "$PLIST" 2>/dev/null || true
    rm -f "$PLIST"
    echo "Removed $PLIST"
else
    echo "(No launchd plist at $PLIST — nothing to remove.)"
fi

pkill -9 honesteq-daemon 2>/dev/null || true
echo "HonestEQ daemon unloaded."
