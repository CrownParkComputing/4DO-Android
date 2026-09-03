# Retro-3DO built-in demonstration ROM

This directory contains the complete source for the small boot ROM used by
Retro-3DO's **Demo** button.  It is original Crown Park Computing code and does
not contain Portfolio, a commercial 3DO BIOS, SDK libraries, game data, or code
copied from another firmware.

The program runs directly on the emulated ARM60 and exercises the normal
hardware paths:

- ARM instructions and DRAM read/write self-test;
- MADAM's VDL root register and the VDLP's interleaved RGB555 framebuffer;
- CLIO's video counter and DSPP program window;
- MADAM player-bus DMA for controller input; and
- the DSPP DAC path for audio output.

Use the D-pad to move the white marker. A, B, and C change its colour. The ROM
image is zero-padded by the application's normal BIOS loader to the one-megabyte
ROM window; only the authored program bytes are embedded in the application.

## Rebuilding

Run `./demo/build-demo-rom.sh`. The script needs Clang, LLD, LLVM objcopy, and
`xxd`. It targets big-endian ARMv4 but the source deliberately uses only ARMv3
instructions available on the 3DO's ARM60. The generated header is committed so
Android and Apple builds do not need a cross-assembler.

Copyright (c) 2026 Crown Park Computing. Released under the repository's MIT
license.
