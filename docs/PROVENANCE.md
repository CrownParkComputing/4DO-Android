# Provenance

What in this emulator is original work, and what was derived from an existing
one. Written to be disclosed, so it errs towards claiming less rather than more.

## The short version

The **application** is original. The **hardware behaviour** is substantially
derived from an existing 3DO emulator, and in a number of specific routines the
algorithm is a direct port rather than an independent reimplementation.

Anyone assessing this project should treat it as a derivative work of FreeDO /
Opera, not as clean-room.

## What the reference is, and why that matters

Development used a reference emulator at `4DO-Android/app/cpp/native_core/`.
That code is FreeDO-descended: 7 of its 59 source files carry the FreeDO notice
verbatim, and 239 distinct `opera_*` symbols run through it. It is Opera's
architecture under a different directory name.

The FreeDO notice reads, in part:

> Any commercial uses of FreeDO sources or any knowledge obtained by studying or
> reverse engineering of the sources, or any other material published by FreeDO
> is strictly forbidden without owners approval.
>
> The above notes are taking precedence over GNU LGPL in conflicting situations.

That clause reaches at "knowledge obtained by studying", not only at copied
text. So the distinction between "we ported this routine" and "we learned this
fact and rewrote it" does not help under the licence as written, whatever a
court would eventually make of such a clause.

## Specifically derived from the reference

Each of these was found by reading the reference's source and reproducing its
behaviour. Where the structure of the routine follows the original closely
enough that "port" is the honest word, it says so.

**Direct algorithm ports:**

| What | Where | Notes |
|---|---|---|
| Matrix engine | `madam.cpp` | The 4x4 / 3x3 / project-by-z arithmetic, including the double-buffered outputs |
| Rotated-cel scanline fill | `madam.cpp` `plot_quad` | Edge crossings, winding gating, the four-crossing case |
| Cel visibility tests | `madam.cpp` `cel_is_invisible` | Winding permission, bounding box, axis-aligned winding, corner wind |
| LR-form cel addressing | `madam.cpp` | Including the height-doubling and the interleave |
| DSPP | `dsp.cpp` | Instruction set, register maps, operand decode |
| SPORT masking and window decode | `sport.cpp` | The `((d^s)&m)^s` form |
| ARM6 multiply timing | `arm60.cpp` | The significant-bit cycle formula |

**Behavioural facts taken from the reference** (expressed in our own code, but
learned by reading theirs): CCB flag bit positions and load semantics; the
packed-cel packet format; the 12.20 vs 16.16 split in the step vectors; the
CLUT-bypass bit-15 rule; the VDL walk, line modulo table and field timing; CLIO
register values, interrupt banks and timer rate; XBUS selection model and drive
replies; PBUS report layout; the cel engine starting between instructions;
where the preamble lives when CCBPRE is clear.

The formatted NVRAM image in `bus.cpp` was captured by **running** the reference
and reading the bytes its boot ROM wrote. That is black-box measurement, not
source derivation.

## Original work

- The whole application: SDL/Android front end, SAF storage, launcher, file
  browser, on-screen controls and layout editor, settings.
- Threading and presentation: `EmulatorThread`, `FrameMailbox`, `AudioRing`,
  and the single frame-pacing policy that replaced the previous core's three
  independent frame-droppers.
- The ARM60 interpreter's implementation — decode cache, dispatch, exception
  handling — though its *cycle model* now matches the reference's.
- Disc handling and the CHD path (over vendored libchdr, BSD/LGPL).
- All 225 tests, the `gamecheck` regression harness and its expectation floors.

## Rough proportions

The emulation core is about 7,400 lines of C++. Around 5,200 of those are the
chip-behaviour layer — MADAM, VDLP, CLIO, DSPP, SPORT, XBUS and the ARM cycle
model — and that layer should be regarded as reference-derived throughout, with
the routines in the table above being ports. The remaining ~2,200 lines, plus
the entirety of `src/platform`, `src/ui`, `tests/` and `tools/`, are original.

By line count of the shipped application the original share is much larger,
because the front end dwarfs the core. That framing would flatter the position
and it is not the one that matters: what matters is that the part which makes it
an *emulator* is derived.

## History of this file

This document replaces `CLEANROOM.md`, which claimed the project contained no
code derived from FreeDO, 4DO or Opera and never had. That claim was true when
written and is not true now. It stopped being true when development moved to
working directly from the reference sources, which was a deliberate decision
taken with the licence position understood — not an accident, and not something
discovered later.

The old file is removed rather than amended, because a stale clean-room claim in
a repository is worse than no claim at all.
