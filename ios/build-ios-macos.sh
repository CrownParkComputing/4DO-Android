#!/usr/bin/env bash
# Builds Retro-3DO.app for iOS, natively on macOS.
#
# This is the same shape as the recipe already proven in Retro-Saturn and
# Retro-C64, and it exists in that form for a reason: cross-building iOS on
# Linux (the mobaiapp/iosbox route) can produce a debug build for sideloading,
# but it cannot produce anything submittable — it does not sign, it cannot run
# ibtool or actool, and it silently drops the deployment target. Anything going
# to TestFlight or the App Store comes from a real Xcode archive on a Mac.
#
# There is no separate iOS front end to build: the emulator, the UI and the
# platform layer are the same sources the Linux and Android builds use. SDL
# provides the UIApplication entry point.
#
# Usage:
#   ios/build-ios-macos.sh              # device (arm64)
#   TARGET=simulator ios/build-ios-macos.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

TARGET="${TARGET:-device}"
IOS_MIN="${IOS_MIN:-15.0}"

command -v xcrun >/dev/null || {
    echo "FATAL: xcrun not found. This script needs Xcode and only runs on macOS." >&2
    exit 1
}

if [ "$TARGET" = "simulator" ]; then
    SYSROOT="iphonesimulator"
    BUILD_DIR="${BUILD_DIR:-$HERE/build/ios-arm64-simulator}"
    DESTINATION="generic/platform=iOS Simulator"
else
    SYSROOT="iphoneos"
    BUILD_DIR="${BUILD_DIR:-$HERE/build/ios-arm64}"
    DESTINATION="generic/platform=iOS"
fi

echo "==> configuring for $TARGET (iOS $IOS_MIN, arm64, SDK $(xcrun --sdk "$SYSROOT" --show-sdk-version))"

# CMAKE_OSX_SYSROOT is given as a name rather than a path so CMake resolves it
# through xcrun; hardcoding the path breaks silently on the next Xcode upgrade.
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$SYSROOT" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="" \
    -DRETRO3DO_BUILD_TESTS=OFF \
    -DRETRO3DO_USE_SYSTEM_SDL=OFF

echo "==> building"
cmake --build "$BUILD_DIR" --config Release -- -destination "$DESTINATION"

APP="$(find "$BUILD_DIR" -name 'retro3do.app' -maxdepth 4 | head -1)"
if [ -z "$APP" ]; then
    echo "FATAL: build finished but no .app was produced" >&2
    exit 1
fi

echo
echo "Built: $APP"
echo
echo "This bundle is unsigned. To get it onto a device or into TestFlight, open"
echo "the generated project in Xcode, set a development team on the retro3do"
echo "target, and archive:"
echo "  open $BUILD_DIR/Retro3DO.xcodeproj"
