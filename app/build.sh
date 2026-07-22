#!/usr/bin/env bash
# Build HonestEQ.app from the Swift Package.
# Usage:  ./build.sh          # release build, universal binary
#         ./build.sh debug    # debug build (faster iteration)

set -euo pipefail

CONFIG="${1:-release}"
PRODUCT="HonestEQ"
APP_BUNDLE="build/${PRODUCT}.app"

echo "==> Building HonestEQ ($CONFIG, native arch)..."
# Native architecture only. Universal (fat arm64+x86_64) requires the full
# Xcode app (XCBuild), which most users don't have — Command Line Tools alone
# handle single-arch builds cleanly.  For personal use, single-arch = fine.
if [ "$CONFIG" = "release" ]; then
    swift build -c release
    BIN_PATH=".build/release/${PRODUCT}"
else
    swift build
    BIN_PATH=".build/debug/${PRODUCT}"
fi

echo "==> Assembling .app bundle at $APP_BUNDLE ..."
rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources"

cp "$BIN_PATH" "$APP_BUNDLE/Contents/MacOS/${PRODUCT}"
cp Info.plist "$APP_BUNDLE/Contents/Info.plist"

echo "==> Ad-hoc signing..."
codesign --force --deep --sign - "$APP_BUNDLE"

echo ""
echo "Built $APP_BUNDLE"
echo "Run with:  open $APP_BUNDLE"
echo "Or double-click it in Finder."
