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
- Community hardware documentation that is itself independently derived, where
  its own provenance is clear.
- Black-box measurement: behaviour of real hardware, and of software running on
  it, observed from the outside.

## Prohibited sources

- FreeDO, 4DO, Opera, `opera-libretro`, and anything derived from them.
- Any description of those codebases specific enough to be a transcription of
  them, including a summary written by someone who did read them.
