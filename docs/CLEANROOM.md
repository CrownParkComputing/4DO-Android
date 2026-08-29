# Clean-room provenance

This project implements a 3DO Interactive Multiplayer emulator **from scratch**.
It contains no code derived from FreeDO, 4DO, Opera or `opera-libretro`, and it
never has: this repository was started empty for exactly that reason.

## Why this matters

The FreeDO sources — from which Opera and therefore `opera-libretro` descend —
carry a licence header that reads, in part:

> Any commercial uses of FreeDO sources or any knowledge obtained by studying or
> reverse engineering of the sources, or any other material published by FreeDO
> is strictly forbidden without owners approval.
>
> The above notes are taking precedence over GNU LGPL in conflicting situations.

Whatever the enforceability of a clause reaching at "knowledge obtained by
studying", the cheap and certain option is to not rely on the argument. So we
do not read those sources while writing these ones.

## Running a prohibited emulator is allowed; reading it is not

This distinction is the whole rule, and it is easy to lose in the moment:

- **Running** FreeDO, Opera or anything descended from them, and observing what
  it does from the outside - command traces, register values, timing, screen
  output - is black-box measurement. It is permitted, and it is one of the most
  useful tools available.
- **Reading their source** is not permitted, whatever the intention, and the
  knowledge does not stop being derived once acquired.

This was breached during development: Opera's sources were read and facts about
the CD-ROM's poll register, status bits, selection model and DMA trigger were
transcribed into `src/core/`. Those commits were reverted and the same facts
re-derived from MAME (BSD-3-Clause) and from behavioural observation. It is
recorded here rather than quietly fixed, because a clean-room claim is only
worth anything if its failures are visible.

The practical guard: if a fact came out of a debugger or a log, it is fine. If
it came out of an editor, it is not.

## Rules for contributors

1. **Do not read FreeDO, 4DO, Opera or `opera-libretro` source while working on
   `src/core/`.** Not for reference, not "just to check the register order".
2. Work from the permitted sources listed below, and from black-box observation
   of real hardware or of a machine you are entitled to observe.
3. Correctness is established against **behaviour**, never against another
   emulator's source: captured framebuffers, captured audio, and test discs.
4. Record what each module was derived from in `docs/HARDWARE.md`, with enough
   specificity that someone else could follow the same path.

## Permitted sources

- Public ARM architecture documentation for the ARM60 (an ARM6-family core
  implementing the ARMv3 instruction set). The instruction set is ARM's, is
  publicly documented, and has nothing to do with the 3DO.
- The 3DO Company's own published developer documentation (the Portfolio OS
  manuals, hardware and graphics programming guides), which were distributed to
  licensed developers and have since been published.
- Granted patents covering the hardware, which are public by construction.
  In use: The 3DO Company's own PCT applications, archived at
  `github.com/trapexit/3DO-information/hardware/patents`. `WO 94/16382
  "Expansion Bus"` documents the XBUS I/O model, transaction types, Poll
  Register and Status Byte in full; `WO 94/10642` covers the sprite (cel)
  rendering processor and `WO 95/12876` the display-list mechanism.
- **MAME**, specifically its 3DO devices (`src/mame/misc/3do_*.cpp`), which are
  `license:BSD-3-Clause`, copyright Angelo Salese and Wilbert Pol. This is an
  independent implementation and not part of the FreeDO lineage, and BSD-3 
  permits commercial use.

  **If any code here is derived from MAME, this project must ship MAME's
  copyright notice and the BSD-3 licence text.** See `docs/THIRD_PARTY.md`.
  What has been taken so far is register documentation rather than code - the
  expansion bus poll-register bit layout and the fact that SELECTION names a
  device by value - but the obligation attaches to the derivation, not to the
  number of lines, so the notice ships regardless.

- Community hardware documentation that is itself independently derived, where
  its own provenance is clear.
- Black-box measurement: behaviour of real hardware, and of software running on
  it, observed from the outside.

## Prohibited sources

- FreeDO, 4DO, Opera, `opera-libretro`, and anything derived from them.
- Any description of those codebases specific enough to be a transcription of
  them, including a summary written by someone who did read them.
