#!/usr/bin/env bash
# Builds Retro-3DO.app for iPhone and iPad, natively on macOS.
#
# This is the same shape as the recipe already proven in Retro-Saturn and
# Retro-C64, and it exists in that form for a reason: cross-building iOS on
# Linux (the mobaiapp/iosbox route) can produce a debug build for sideloading,
# but it cannot produce anything submittable - it does not sign, it cannot run
# ibtool or actool, and it silently drops the deployment target. Anything going
# to TestFlight or the App Store comes from a real Xcode archive on a Mac.
#
# There is no separate iOS front end to build: the emulator, the UI and the
# platform layer are the same sources the Linux and Android builds use. SDL
# provides the UIApplication entry point.
#
# Usage:
#   ios/build-ios-macos.sh                          # device build, unsigned
#   TARGET=simulator ios/build-ios-macos.sh         # runs in the simulator
#   TEAM_ID=XXXXXXXXXX ios/build-ios-macos.sh       # signed, installable
#   TEAM_ID=XXXXXXXXXX ARCHIVE=1 ios/build-ios-macos.sh   # .xcarchive + .ipa
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

# SDL and Dear ImGui are submodules. A clone made without --recursive fails
# later with a missing third_party/SDL/CMakeLists.txt, which reads as a broken
# repository rather than an incomplete checkout.
if [ ! -f "$REPO_ROOT/third_party/SDL/CMakeLists.txt" ] ||
   [ ! -f "$REPO_ROOT/third_party/imgui/imgui.cpp" ]; then
    echo "==> fetching submodules"
    git -C "$REPO_ROOT" submodule update --init --recursive
fi

TARGET="${TARGET:-device}"
IOS_MIN="${IOS_MIN:-15.0}"
TEAM_ID="${TEAM_ID:-}"
ARCHIVE="${ARCHIVE:-0}"

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

if [ "$ARCHIVE" = "1" ] && { [ "$TARGET" = "simulator" ] || [ -z "$TEAM_ID" ]; }; then
    echo "FATAL: ARCHIVE=1 needs a device build and a TEAM_ID." >&2
    echo "       An unsigned or simulator archive cannot be exported or uploaded." >&2
    exit 1
fi

echo "==> configuring for $TARGET (iOS $IOS_MIN, arm64, SDK $(xcrun --sdk "$SYSROOT" --show-sdk-version))"

# CMAKE_OSX_SYSROOT is given as a name rather than a path so CMake resolves it
# through xcrun; hardcoding the path breaks silently on the next Xcode upgrade.
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$SYSROOT" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" \
    -DRETRO3DO_IOS_MIN="$IOS_MIN" \
    -DRETRO3DO_APPLE_TEAM_ID="$TEAM_ID" \
    -DRETRO3DO_BUILD_TESTS=OFF \
    -DRETRO3DO_USE_SYSTEM_SDL=OFF

if [ "$ARCHIVE" = "1" ]; then
    ARCHIVE_PATH="$BUILD_DIR/Retro-3DO.xcarchive"
    EXPORT_DIR="$BUILD_DIR/export"
    echo "==> archiving"
    xcodebuild -project "$BUILD_DIR/Retro3DO.xcodeproj" \
        -scheme retro3do -configuration Release \
        -destination "$DESTINATION" \
        -archivePath "$ARCHIVE_PATH" archive

    echo "==> exporting"
    # The team is substituted rather than committed: an export options file with
    # someone else's team in it fails with an error that names a provisioning
    # profile and not the real cause.
    EXPORT_OPTIONS="$BUILD_DIR/ExportOptions.plist"
    sed "s/TEAM_ID_PLACEHOLDER/$TEAM_ID/" "$HERE/ExportOptions.plist.in" > "$EXPORT_OPTIONS"
    rm -rf "$EXPORT_DIR"
    xcodebuild -exportArchive -archivePath "$ARCHIVE_PATH" \
        -exportOptionsPlist "$EXPORT_OPTIONS" -exportPath "$EXPORT_DIR"
    echo
    echo "Archive: $ARCHIVE_PATH"
    echo "IPA:     $(find "$EXPORT_DIR" -name '*.ipa' -print -quit)"
    echo
    echo "Upload with:"
    echo "  xcrun altool --upload-app -f <ipa> -t ios --apiKey <key> --apiIssuer <issuer>"
    exit 0
fi

echo "==> building"
cmake --build "$BUILD_DIR" --config Release -- -destination "$DESTINATION"

APP="$(find "$BUILD_DIR" -maxdepth 4 -name 'Retro-3DO.app' -print -quit)"
if [ -z "$APP" ]; then
    echo "FATAL: build finished but no .app was produced" >&2
    exit 1
fi

echo
echo "Built: $APP"
echo
if [ "$TARGET" = "simulator" ]; then
    cat <<EOF
Run it:
  xcrun simctl boot "iPhone 15"      # or any installed device name
  open -a Simulator
  xcrun simctl install booted "$APP"
  xcrun simctl launch booted com.crownparkcomputing.retro3DO

The simulator has no Files app sharing, so put a BIOS in the app's Documents
directory by hand:
  xcrun simctl get_app_container booted com.crownparkcomputing.retro3DO data
EOF
elif [ -z "$TEAM_ID" ]; then
    cat <<EOF
This bundle is UNSIGNED and will not install on a device. To get it onto one,
either re-run with TEAM_ID=XXXXXXXXXX, or open the generated project and set a
team by hand:
  open $BUILD_DIR/Retro3DO.xcodeproj
EOF
else
    cat <<EOF
Install on a connected device:
  xcrun devicectl device install app --device <udid> "$APP"

For TestFlight or the App Store, re-run with ARCHIVE=1.
EOF
fi
