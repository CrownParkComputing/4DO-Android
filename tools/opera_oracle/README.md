# Opera oracle

Builds the working Opera core as a standalone Linux binary so it can be run on
the same BIOS and disc as this emulator and the two compared directly. It is a
measuring instrument, not a dependency: nothing here is linked into Retro-3DO,
and no Opera source is copied into this repository.

Why it exists: reverse-engineering the boot ROM from the outside took this
project a long way and then stopped paying. Having a second implementation that
*does* boot the disc turns "what does the machine want next" from inference into
a diff.

## Building

Point it at a checkout of the Android project that carries the core:

    CORE=~/StudioProjects/4DO-Android/app/cpp/native_core
    mkdir -p build && cd build
    for f in $CORE/*.c; do
        gcc -c -O2 -std=gnu11 -w -I"$CORE" -I../shim "$f" -o "$(basename $f .c).o"
    done
    g++ -O2 -w -I"$CORE" -I../shim -c ../harness.cpp -o harness.o
    g++ harness.o *.o -o oracle -lm

Three things make it build off Android:

- `-std=gnu11`, because the core does `typedef int bool`, which C23 rejects.
- `shim/android/log.h`, which turns the log macros into fprintf.
- A definitions translation unit. The clock, PRNG, diagnostic port, region and
  fixed-point maths are defined in the app's own `native_core.cpp` rather than
  in the core, so they have to be provided; extracting that file's definition
  block is enough.

The harness also has to supply what the app normally does: disc access
callbacks, and a video buffer, without which the VDLP writes through a null
pointer on the first frame.

## What it showed

Run against the same BIOS and disc, the working core:

- issues REPEATED read commands - `83 82 09 8B 8C 10 10 10 10 8D 8B 8C 10 ...` -
  where this emulator issues exactly one and stops;
- reads LBA 0, 1, 2, 3 and then 122299 onwards, the directory region;
- never triggers the expansion DMA through CLIO 0x304 at all, where this
  emulator does. Same ROM, different data path, which is the divergence worth
  chasing.
