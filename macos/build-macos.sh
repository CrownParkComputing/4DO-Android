#!/usr/bin/env bash
# Builds Retro-3DO.app for macOS, natively on a Mac.
#
# There is no macOS front end to build. The emulator, the UI and the platform
# layer are the same sources the Linux, Android and iOS builds use; SDL supplies
# the NSApplication and the Metal-backed renderer.
#
# The Xcode generator is the default on purpose, not out of habit: actool
# compiles the asset catalogue into the icon, and actool only runs under Xcode.
# A Ninja build works and runs, and has a generic icon. Use it for iterating,
# not for anything you ship (GENERATOR=Ninja).
#
# Usage:
#   macos/build-macos.sh                    # universal, unsigned, ./build
#   ARCHS=arm64 macos/build-macos.sh        # Apple silicon only, faster
#   TEAM_ID=XXXXXXXXXX macos/build-macos.sh # signed with your team
#   GENERATOR=Ninja macos/build-macos.sh    # quick iteration, no icon
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

GENERATOR="${GENERATOR:-Xcode}"
# Universal by default. An Intel Mac cannot run an arm64-only build at all, and
# the failure it gives ("bad CPU type") reads as a corrupt download.
ARCHS="${ARCHS:-arm64;x86_64}"
MACOS_MIN="${MACOS_MIN:-11.0}"
TEAM_ID="${TEAM_ID:-}"
BUILD_DIR="${BUILD_DIR:-$HERE/build/macos}"

[ "$(uname -s)" = "Darwin" ] || {
    echo "FATAL: this script builds the macOS app and only runs on macOS." >&2
    exit 1
}
command -v xcrun >/dev/null || {
    echo "FATAL: xcrun not found. Install Xcode and run xcode-select --install." >&2
    exit 1
}

echo "==> configuring ($GENERATOR, arch $ARCHS, macOS $MACOS_MIN)"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN" \
    -DRETRO3DO_MACOS_MIN="$MACOS_MIN" \
    -DRETRO3DO_APPLE_TEAM_ID="$TEAM_ID" \
    -DRETRO3DO_BUILD_TESTS=ON \
    -DRETRO3DO_USE_SYSTEM_SDL=OFF

echo "==> building"
cmake --build "$BUILD_DIR" --config Release --parallel

APP="$(find "$BUILD_DIR" -maxdepth 4 -name 'Retro-3DO.app' -print -quit)"
[ -n "$APP" ] || { echo "FATAL: build finished but no .app was produced" >&2; exit 1; }

echo "==> running the host test suite"
# These are the same 180-odd tests the Linux build runs. They are not a
# formality here: this is the first time the core has been compiled by Apple
# clang for two architectures, and a portability fault shows up as a failing
# test rather than as a crash on a user's Mac.
( cd "$BUILD_DIR" && ctest --output-on-failure -C Release ) || {
    echo "WARNING: host tests failed - do not ship this build." >&2
}

echo
echo "Built: $APP"
file "$APP/Contents/MacOS/Retro-3DO" || true
echo
if [ -z "$TEAM_ID" ]; then
    cat <<'EOF'
This bundle is UNSIGNED. It runs on this Mac and nowhere else: macOS quarantines
anything downloaded and refuses to open an unsigned app with a message about the
developer being unidentified. To distribute it, see docs/APPLE_BUILD.md - the
short version is a Developer ID signature plus notarisation:

  codesign --deep --force --options runtime --timestamp \
      --entitlements macos/Retro3DO.entitlements \
      -s "Developer ID Application: <name> (<team>)" "<app>"
  ditto -c -k --keepParent "<app>" Retro-3DO.zip
  xcrun notarytool submit Retro-3DO.zip --keychain-profile <profile> --wait
  xcrun stapler staple "<app>"
EOF
fi
