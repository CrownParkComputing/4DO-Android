# Retro-3DO

A 3DO Interactive Multiplayer emulator, written from scratch for phones,
tablets and desktops.

One C++ codebase builds for Android, iOS, Linux and Windows. There is no
per-platform front end: SDL provides the window, input, audio and GPU on every
target, and Dear ImGui draws the launcher and the in-game overlay.

> **Status: early, but it draws.** CPU, interrupts, video timing, the CEL engine
> and the display path all run: cels are rotated, scaled and drawn through the
> palette. Disc images load and their tracks are read. What is missing is the
> XBUS link between disc and CPU, and audio — so no real disc boots yet. See
> [Roadmap](#roadmap).

## Why this exists rather than another Opera port

Two reasons, and the second is the one with a deadline attached.

**It is written to be fast on a phone.** The widely-used 3DO cores descend from
a 1990s desktop emulator. Their CPU is a plain interpreter that re-decodes every
instruction on every execution, their graphics chip is scalar C, and they
present frames from inside the emulation thread — so a slow present stalls
emulation, and the machine appears to run slow when it is really being
throttled. This core decodes each instruction once and caches it, and never
presents from the emulation path at all.

**It is ours.** The FreeDO sources that Opera and `opera-libretro` descend from
carry a licence clause forbidding commercial use of the code, or of knowledge
obtained by studying it, without the owners' approval — and stating that this
takes precedence over the LGPL. This project shares no code with them and never
has. See [docs/CLEANROOM.md](docs/CLEANROOM.md), which is a rule for
contributors, not a disclaimer.

## Building

### Linux and Windows

```sh
cmake -S . -B build -DRETRO3DO_USE_SYSTEM_SDL=ON
cmake --build build -j
./build/retro3do
```

Drop `-DRETRO3DO_USE_SYSTEM_SDL=ON` to build the vendored SDL from source
instead, which is what the mobile targets do.

### Tests

```sh
./build/retro3do_tests
```

They run on the host, with no window and no device. A bug found on hardware gets
reproduced here before it gets fixed.

### Android

```sh
cd android
./gradlew :app:assembleDebug -PBUILD_WITH_CMAKE=1
```

Gradle drives the same root `CMakeLists.txt` the desktop build uses; there is no
separate Android copy of the emulator.

### iOS

```sh
ios/build-ios-macos.sh
```

macOS only, and deliberately so — a Linux cross-build can make something that
sideloads but never something submittable. See the comments in that script.

## Layout

```
src/core/       the 3DO itself. No platform headers, ever.
src/platform/   SDL: window, events, pacing, presentation
src/ui/         Dear ImGui: launcher and overlay
tests/          host-side tests
android/        Gradle shell around the shared CMake tree
ios/            Xcode/CMake shell around the shared CMake tree
```

The rule that keeps this working: if something in `src/core/` ever needs a
platform header, it belongs in `src/platform/` instead.

## Roadmap

- [x] ARM60 CPU with a decoded-instruction cache
- [x] Memory map and big-endian bus
- [x] SDL application shell, ImGui launcher
- [x] Android and iOS build paths off one CMake tree
- [x] CLIO — interrupts, timers, video counters
- [x] VDLP — display list to framebuffer
- [x] MADAM — the CEL engine: affine mapping, indexed and direct cels
- [ ] DSP — audio
- [x] Disc images: ISO, BIN, CUE, with sector layout detected not assumed
- [ ] XBUS: connect the disc to the CPU so a real disc boots
- [x] Audio output path: lock-free ring, SDL sink
- [x] Controller: keyboard and gamepad through SDL
- [ ] DSP — the machine is silent until this exists
- [ ] Save states, on-screen pad for touch

## Licence

MIT. The vendored dependencies keep their own: SDL3 (zlib) and Dear ImGui (MIT).
