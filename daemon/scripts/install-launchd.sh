#!/bin/bash
# Install HonestEQ daemon as a launchd LaunchAgent.
#
# Also prompts for microphone access up-front (macOS treats our input AUHAL
# as microphone use, even though it's just loopback from the HonestEQ virtual
# device). Getting this once at install time means the daemon runs silently
# afterwards — no permission dialogs during headphone plug/unplug cycles.
#
# After running: daemon auto-starts on login, auto-restarts on crash, and
# auto-recovers from sleep/wake or headphone disconnect via the daemon's
# internal watchdog + exit-and-restart mechanism.
#
# Logs: /tmp/honesteq-daemon.log
set -e

PLIST_NAME="com.honesteq.daemon.plist"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/$PLIST_NAME"
DST="$HOME/Library/LaunchAgents/$PLIST_NAME"

if [ ! -f "$SRC" ]; then
    echo "ERROR: $SRC not found."
    exit 1
fi

# Ensure the daemon binary exists before installing.
DAEMON_BIN="$SCRIPT_DIR/../build/honesteq-daemon"
if [ ! -x "$DAEMON_BIN" ]; then
    echo "WARNING: daemon binary not found at $DAEMON_BIN"
    echo "  Run:  cd \"$SCRIPT_DIR/..\" && make"
    echo "  Then re-run this script."
    exit 2
fi

# Ensure the HAL driver is installed — the daemon won't be able to open its
# input side without it.
if [ ! -d "/Library/Audio/Plug-Ins/HAL/HonestEQ.driver" ]; then
    echo "ERROR: HonestEQ HAL driver not installed at /Library/Audio/Plug-Ins/HAL/"
    echo "  Run the driver installer first:"
    echo "    cd $SCRIPT_DIR/../../driver && ./scripts/install.sh"
    exit 3
fi

echo "==============================================================="
echo "  HonestEQ daemon install — 3 steps"
echo "==============================================================="
echo ""

# ---------------------------------------------------------------------------
# Step 1: unload any previous instance so the mic-warmup below sees a clean
# slate (otherwise the old daemon still owns the AUHAL and macOS won't
# re-prompt).
# ---------------------------------------------------------------------------
echo "[1/3] Stopping any existing HonestEQ daemon..."
if launchctl list 2>/dev/null | grep -q com.honesteq.daemon; then
    launchctl unload "$DST" 2>/dev/null || true
fi
pkill -9 honesteq-daemon 2>/dev/null || true
sleep 0.5
echo "  OK."
echo ""

# ---------------------------------------------------------------------------
# Step 2: warm up microphone permission.
#
# macOS won't grant mic access via CLI — user has to click Allow in a dialog.
# We do two things:
#   (a) Open System Settings → Privacy & Security → Microphone so the user
#       can see where the permission lives and toggle it explicitly.
#   (b) Launch the daemon in the background so macOS surfaces the actual
#       permission prompt (with the "Allow" / "Don't Allow" buttons).
# Then we wait for the user to confirm, and kill the warm-up daemon so
# launchd owns the daemon from step 3 onwards.
# ---------------------------------------------------------------------------
echo "[2/3] Requesting microphone permission..."
echo ""
echo "  A dialog will pop up asking for MICROPHONE access."
echo "  This is because macOS treats our AUHAL input as mic input, even"
echo "  though we only read from the HonestEQ virtual device (loopback)."
echo ""
echo "  Click ALLOW when the dialog appears."
echo ""
echo "  Also opening System Settings → Privacy & Security → Microphone"
echo "  so you can verify the daemon has permission after clicking Allow."
echo ""

# Open the Microphone privacy pane.
open "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone" &

# Launch daemon in background to trigger the prompt.
"$DAEMON_BIN" "External Headphones" > /tmp/honesteq-install-warmup.log 2>&1 &
WARMUP_PID=$!

# Give the user 25 seconds to see the prompt, click Allow, and verify.
echo "  Waiting up to 25 s for you to click Allow..."
for i in $(seq 1 25); do
    if ! kill -0 "$WARMUP_PID" 2>/dev/null; then
        # Daemon exited (either OK'd or failed).
        break
    fi
    sleep 1
done

# Kill warm-up daemon; launchd takes over next.
kill -9 "$WARMUP_PID" 2>/dev/null || true
wait "$WARMUP_PID" 2>/dev/null || true

echo "  Warm-up finished. Microphone permission should now be granted."
echo "  Verify: System Settings → Privacy & Security → Microphone."
echo ""

# ---------------------------------------------------------------------------
# Step 2.5: seed daemon state.plist with a sensible default device name if
# no state file exists yet. Without this, the launchd-loaded daemon would
# have no device to target on first boot.
# ---------------------------------------------------------------------------
STATE_DIR="$HOME/Library/Application Support/HonestEQ"
STATE_FILE="$STATE_DIR/daemon-state.plist"
mkdir -p "$STATE_DIR"
if [ ! -f "$STATE_FILE" ]; then
    cat > "$STATE_FILE" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>outputDeviceName</key>
    <string>External Headphones</string>
</dict>
</plist>
PLIST
    echo "  Seeded daemon-state.plist with default device 'External Headphones'."
    echo "  Change it by editing $STATE_FILE or via the SwiftUI app when ready."
fi

# ---------------------------------------------------------------------------
# Step 3: install and load the LaunchAgent.
# ---------------------------------------------------------------------------
echo "[3/3] Installing launchd agent..."
mkdir -p "$HOME/Library/LaunchAgents"
cp "$SRC" "$DST"
launchctl load "$DST"
sleep 0.5
echo "  OK."
echo ""

echo "==============================================================="
echo "  HonestEQ daemon installed and running."
echo "==============================================================="
echo ""
echo "  Plist:    $DST"
echo "  Logs:     /tmp/honesteq-daemon.log"
echo "  Manage:   launchctl {load,unload,kickstart} $DST"
echo ""
echo "  Tail logs:"
echo "    tail -f /tmp/honesteq-daemon.log"
echo ""
echo "  Uninstall:"
echo "    $SCRIPT_DIR/uninstall-launchd.sh"
echo ""
