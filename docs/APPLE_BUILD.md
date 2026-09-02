# iOS and macOS: everything a Mac needs to take this over

Nothing in this document has been run. Every other target in this repository —
Linux, Windows, Android — is built and tested from a Linux machine, and Apple
platforms cannot be. What is here is the whole build set up as far as it can be
set up without a Mac: the CMake targets, the two Info.plists, the entitlements,
the icon catalogue, the export options and the two scripts. What is left is to
run them on a Mac and fix whatever the first real toolchain says.

**Read [First run on a Mac](#first-run-on-a-mac) before anything else.** It is
the list of things most likely to be wrong, and each entry says what the failure
looks like so it is not mistaken for an emulator bug.

There is no iOS or macOS front end. The emulator, the launcher and the platform
layer are the same sources Linux and Android build. SDL supplies the
`UIApplication` on iOS and the `NSApplication` on macOS, and `SDL_Renderer`
resolves to Metal on both. That is the whole port.

## What was already there, and what changed

Already present: `ios/build-ios-macos.sh`, `ios/Info.plist.in`, and an `IOS`
branch in `CMakeLists.txt`. Untested, but the right shape.

Added or corrected for this handover:

- **`src/platform/platform.h`** — and this was a real bug, not tidying. The app
  decided "am I a handheld?" by testing `__APPLE__`, which is also defined on a
  Mac. A macOS build would have come up **fullscreen, unresizable, with a touch
  d-pad drawn over it** and no obvious way out. `RETRO3DO_MOBILE` now asks the
  question properly, via `TargetConditionals.h` — the preprocessor alone cannot
  tell iOS from macOS.
- **iOS storage order** — `SDL_GetPrefPath` returns `Library/Application
  Support`, and `UIFileSharingEnabled` exposes `Documents` **only**. The
  writable directory is therefore the one folder on the device a user cannot put
  a file into. The browser now offers `Documents (Files app)` first. Wrong way
  round, the app looks broken in a way nobody can diagnose: the BIOS is plainly
  visible in the Files app and the emulator cannot see it.
- **macOS bundle target** — `macos/Info.plist.in`, `macos/Retro3DO.entitlements`,
  `macos/build-macos.sh`, plus the `APPLE`/`IOS` split in `CMakeLists.txt`.
- **Icons** — `assets/apple/Assets.xcassets`, generated from
  `assets/branding/retro3do-icon-1024.png`: one 1024×1024 alpha-free image for
  iOS (an icon with an alpha channel is rejected at upload), the full 16→1024
  ladder for macOS.
- **Host tools excluded from iOS builds** — `gamecheck` and the test runner are
  command-line executables, and every product in an iOS project must be signed.
  They failed the build, not merely wasted it.
- **`ios/ExportOptions.plist.in`** — for `xcodebuild -exportArchive`.

## Build it

Both scripts take their settings from the environment and print what they did.

```sh
# macOS: universal .app in macos/build/macos, plus the host tests
macos/build-macos.sh
ARCHS=arm64 macos/build-macos.sh            # Apple silicon only, much faster
TEAM_ID=XXXXXXXXXX macos/build-macos.sh     # signed

# iOS
ios/build-ios-macos.sh                      # device, unsigned
TARGET=simulator ios/build-ios-macos.sh     # simulator, runnable immediately
TEAM_ID=XXXXXXXXXX ios/build-ios-macos.sh   # signed, installs on a device
TEAM_ID=XXXXXXXXXX ARCHIVE=1 ios/build-ios-macos.sh   # .xcarchive and .ipa
```

Both use the **Xcode generator**, and that is load-bearing rather than habitual:
`actool` compiles the asset catalogue into the app icon and only runs under
Xcode. `GENERATOR=Ninja macos/build-macos.sh` builds and runs fine and has a
generic icon — good for iterating, not for anything shipped.

CMake cache variables, if driving the build by hand:

| Variable | Default | What it does |
|---|---|---|
| `RETRO3DO_BUNDLE_ID` | `com.crownparkcomputing.retro3do` | Bundle identifier, both platforms |
| `RETRO3DO_APPLE_TEAM_ID` | *(empty)* | Empty disables signing entirely |
| `RETRO3DO_MARKETING_VERSION` | `1.0` | `CFBundleShortVersionString` |
| `RETRO3DO_BUILD_NUMBER` | `1` | `CFBundleVersion` — must increase on every upload |
| `RETRO3DO_IOS_MIN` | `15.0` | iOS deployment target |
| `RETRO3DO_MACOS_MIN` | `11.0` | macOS deployment target |

## First run on a Mac

In rough order of likelihood.

1. **The generated project is `Retro3DO.xcodeproj` and the scheme is
   `retro3do`** (the CMake target name), while the product is `Retro-3DO.app`
   (its `OUTPUT_NAME`). If `xcodebuild` reports an unknown scheme, that is the
   name to check first — nothing else is wrong.
2. **SDL's own CMake decides the Apple backends.** The vendored SDL builds
   static everywhere except Android. If SDL fails to configure for iOS, fix it
   in SDL's options, not by adding platform code here.
3. **Deployment targets.** 15.0 and 11.0 are chosen to be uncontroversial, not
   measured. If a modern Xcode warns that the target is too old to support, raise
   it in one place — the cache variables above — and nowhere else.
4. **`UILaunchScreen` is an empty dictionary** in `ios/Info.plist.in`, on
   purpose: naming a storyboard pulls in `ibtool`. If App Store validation
   objects, add a real storyboard rather than an image set.
5. **Universal macOS builds compile everything twice.** If a build is
   unaccountably slow the first time, that is why. `ARCHS=arm64` while
   iterating.
6. **Run the host tests on the Mac.** `macos/build-macos.sh` does this for you.
   This is the first time the core is compiled by Apple clang, for two
   architectures — a portability fault surfaces there as a failed test rather
   than as a crash on a user's machine days later. All 253 pass on Linux.

## Getting a BIOS and discs onto a device

This is the part users get stuck on, and it differs per platform.

- **iOS**: `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` are
  both set, so *Retro-3DO* appears under **On My iPhone** in the Files app.
  Anything dropped there lands in `Documents`, which is the first root the
  in-app browser offers. A `.chd` alongside the BIOS is all it takes.
- **Simulator**: no Files sharing. Copy into the container by hand —
  `xcrun simctl get_app_container booted com.crownparkcomputing.retro3do data`
  prints the path.
- **macOS**: the app is unsandboxed, so its browser reaches the user's own
  disks. `Downloads`, `Documents` and `Home` are offered as roots. A disc image
  can also be dropped on the app or passed as `argv`.

## Distribution

**iOS / App Store.** `ARCHIVE=1` produces the `.xcarchive` and exports an
`.ipa`. Uploading needs an App Store Connect API key:

```sh
xcrun altool --upload-app -f <ipa> -t ios --apiKey <key> --apiIssuer <issuer>
```

Two Apple-side facts worth knowing before the first submission: emulators are
allowed on the App Store, but the app must not download executable content, and
it must ship without copyrighted ROM or BIOS material — which is already how
this one works. And per the family-wide rule in the memory index, an index of
what is in the build goes in the **Review Notes on every submission**.

**macOS, direct download** (the default, and simplest):

```sh
codesign --deep --force --options runtime --timestamp \
    --entitlements macos/Retro3DO.entitlements \
    -s "Developer ID Application: <name> (<team>)" Retro-3DO.app
ditto -c -k --keepParent Retro-3DO.app Retro-3DO.zip
xcrun notarytool submit Retro-3DO.zip --keychain-profile <profile> --wait
xcrun stapler staple Retro-3DO.app
```

Without notarisation, a downloaded build is refused with a message about an
unidentified developer, which reads to a user as a broken download.

**macOS App Store** would need work that has not been done. The Mac App Store
requires the App Sandbox, and under the sandbox the in-app file browser sees
every one of the user's folders as empty — it would need an `NSOpenPanel` and
security-scoped bookmarks first, the same shape as the Android SAF work already
in `src/platform/android_storage.cpp`. `macos/Retro3DO.entitlements` says this
too, at the point where someone would otherwise just switch the sandbox on.

## What is genuinely untested

Being specific, because "untested" covering the whole port would hide which
parts are merely unproven and which are unwritten:

- No Apple compiler has ever seen this code. Expect warnings; expect a small
  number of errors in the SDL/ImGui glue.
- **Touch controls on iOS are unexercised.** The layout maths is host-tested
  (`tests/test_pad_layout.cpp`) and the Android path works, but the two touch
  bugs that mattered were only ever found by *rendering the pad and looking at
  it*. Do that on a real iPhone before believing it.
- **Audio on Apple is unexercised.** SDL owns the audio session; the ring buffer
  is host-tested.
- **Gamepads**: SDL handles MFi controllers, and nothing here does anything
  special for them.
- **Retina and the notch.** `SDL_WINDOW_HIGH_PIXEL_DENSITY` is set and the UI
  derives its scale from the window's pixel size rather than trusting
  `SDL_GetWindowDisplayScale` — a lesson from Android, where that call returns
  1.0 on many devices. Whether the safe-area insets are right on a notched
  iPhone is not known.
- The macOS build has no menu bar beyond SDL's default. Not a defect; just not
  done.
