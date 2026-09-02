# Provenance and implementation-source audit

This file records two different facts which must not be blurred together:

1. where knowledge entered the project historically; and
2. what authority and algorithm the current source uses.

## Historical status

Retro-3DO is **not a clean-room project**. During development, FreeDO-descended
Opera code in `4DO-Android/app/cpp/native_core/` and later the sanctioned local
Opera tree were read and used as a compatibility reference. Some routines were
initially close ports. Replacing those routines later does not undo that prior
exposure, so this repository must not claim that it was independently developed
without knowledge of FreeDO or Opera.

The FreeDO notice also purports to restrict commercial use of knowledge gained
from its source. This document records the fact rather than offering a legal
opinion about the scope or enforceability of that language.

## Current implementation audit

The September 2026 retrospective revisited every routine previously listed as
a direct algorithm port. Each current implementation below was rebuilt around
a public hardware description and a project-owned structure. The reference
history remains disclosed above.

| Area | Current implementation | Primary authority | Retrospective result |
|---|---|---|---|
| ARM60 | Decode cache and ARMv3 interpreter in `arm60.cpp`; value-range ARM6 Booth timing | ARM DDI 0100I; 3DO SDK ARM6 performance guide | The inherited timing formula was wrong for 0/1 and odd bit-count boundaries. It now follows ARM's published `1S+nI` ranges and has boundary tests. |
| MADAM matrix | Generic signed 16.16 row dot-products and explicit project/divide stage | 3DO SDK `operamath.h`; WO 94/10641 | Replaces the former line-for-line matrix formula while retaining the documented double-buffered result contract. |
| CEL rasterisation | Conventional active-edge scan conversion with half-open edges and paired crossings | WO 94/10644; standard polygon scan conversion | Replaces the former edge-crossing port and handles four crossings without reference-shaped control flow. |
| CEL visibility | Bounding geometry plus local tangent cross-products at the four projected corners | WO 94/10644 | Replaces the former corner-winding port. The reconstructed bottom-right tangent also corrects an inherited height/width mix-up. |
| LR-form CEL | Explicit row-pair/word/half traversal | Official Graphics Programming Guide and SDK `hardware.h` | Replaces the former linear-offset adaptation; a four-colour test pins the two rows in every source word. |
| DSPP | Field decoder, operand-group packets, explicit write-back selection, readable branch conditions, table-shaped ALU and barrel shifter | WO 94/16383 | Former packed-bitfield/operand/branch routines were reorganised around the published instruction diagrams. End-to-end DSP programs test signed branching and three-register arithmetic/write-back. |
| SPORT | Explicit protected/source bit fields: `(destination & mask) \| (source & ~mask)` | System architecture material and BIOS SPORT tests | Replaces the algebraically compact reference expression and makes mask polarity auditable. |
| VDLP | Scanline-latched state machine and project pixel/interpolation path | Official Graphics Programming Guide; US 5,502,462; US 5,742,778 | Current walk is scanline timed. A raw persist-zero transition remains a measured compatibility rule; see “fallback facts” below. |
| CLIO | Project interrupt/timer/DMA register model | SDK headers, system patents, ROM disassembly and 3DOessence register documentation | No direct algorithm port remains. Ordinary latches are explicit hardware register state rather than an emulated copy of another program's register file. |
| XBUS/PBUS | FIFO bus and daisy-chain packet models | WO 94/16382, WO 94/10636, SDK CD/controller definitions | Implementations are project code. CD device reply shapes also use the BSD-licensed MAME CR-560B documentation and boot-ROM observations. |

The source-level conclusion is therefore narrower and more accurate than
“clean-room”: **the current algorithms in the old direct-port list have been
independently restructured and are backed by public specifications and project
tests, but the project remains historically reference-exposed.**

## Behavior-only fallback facts still retained

These are not copied routines. They are exact compatibility choices first
resolved with reference help and retained because public documentation is
ambiguous or describes only the surrounding protocol:

- a raw VDL persist count of zero can be replaced before displaying a line;
- the fitted CD drive presents the loaded/ready/spinning `0xE1` state used by
  the stock boot path;
- several uninteresting CLIO reset/latch values and CD reply details were
  parity-checked against Opera after the public-source implementation existed.

Each is isolated and covered by a ROM path, title trace or focused regression.
If physical-hardware measurements become available, those measurements should
replace the fallback authority without changing the surrounding algorithm.

## Public and first-party source set

- 3DO SDK 2.5 `hardware.h`, `operamath.h`, `cdrom.h`, controller definitions,
  and the Graphics Programming Guide.
- WO 94/10636, Player Bus Apparatus and Method.
- WO 94/10641, Audio-Video Computer Architecture.
- WO 94/10643, Improved Method and Apparatus for Processing Image Data.
- WO 94/10644, Spryte Rendering System.
- WO 94/16382, Expansion Bus.
- WO 94/16383, Digital Signal Processor Architecture.
- US 5,502,462 and US 5,742,778 for video display-list timing.
- ARM DDI 0100I and the ARM6 timing material shipped with the 3DO developer
  documentation.
- MAME's BSD-3-Clause CR-560B device documentation for device-specific CD
  command/reply shapes. No MAME source is compiled into this project.

## Original project work outside the chip models

The SDL/Android front end, SAF storage, file browser, touch controls and layout
editor, settings, emulator thread, frame mailbox, audio ring, frame pacing,
disc/CHD integration, regression harness and all **246 current tests** are
project work. Vendored libraries retain their own licences as recorded in
`docs/THIRD_PARTY.md`.

## History of this file

This document replaced the earlier `CLEANROOM.md` claim after development began
using reference source. That old claim was removed because leaving a stale
clean-room statement in the repository would be misleading. The present audit
must remain even when every current routine has since been rewritten: current
implementation provenance and historical exposure are not the same claim.
