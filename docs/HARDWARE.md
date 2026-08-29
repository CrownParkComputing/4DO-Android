# Hardware notes and provenance

Where each part of the emulated machine came from. The rule from
[CLEANROOM.md](CLEANROOM.md) is that nothing here is derived from another
emulator's source, and that anything not yet checked against a citable source is
marked as such rather than quietly asserted.

## ARM60 (`src/core/arm60.cpp`)

The 3DO's CPU is an ARM60: an ARM6-family core running the **ARMv3** instruction
set at 12.5 MHz. Everything implemented is ARM's own architecture, publicly
documented, and independent of the 3DO.

Implemented: data processing with the full barrel shifter, MUL/MLA, single and
block data transfer, branch and branch-with-link, SWI, SWP, MRS/MSR, the
undefined-instruction trap, all seven processor modes with correct register
banking, and IRQ/FIQ sampled at instruction boundaries.

Details worth recording because they are easy to get subtly wrong, and each has
a test:

- **PC reads eight ahead** of the executing instruction, and twelve ahead when a
  register-specified shift adds a pipeline slot.
- **Immediate shift amount zero is not "no shift"** except for LSL. `LSR #0`
  encodes `LSR #32`, `ASR #0` encodes `ASR #32`, and `ROR #0` encodes `RRX`.
- **A register-specified shift of zero** leaves both value and carry untouched,
  for every shift type.
- **Carry after a subtraction is set when there is no borrow** — ARM's
  convention, the opposite of some other architectures.
- **Overflow is signed, carry is unsigned**, and they are independent.
- **`S` with PC as the destination** means "return from exception": it restores
  CPSR from the current mode's SPSR rather than setting flags.
- **LDM/STM always transfer upwards** from the lowest address, whichever
  direction the addressing mode implies.

Not implemented, deliberately: Thumb, long multiply, and coprocessor
instructions. None exist on an ARM60 in a 3DO; the coprocessor space takes the
undefined trap.

### The decode cache

Each instruction is decoded once into a `Decoded` record and cached in 4 KB
pages, so executing is a load and a dispatch rather than a re-decode. Writes to
DRAM are recorded by the bus (`WriteWatch`) and the console invalidates the
affected range every few thousand cycles, because the 3DO's OS does relocate
code.

One design note, learned the hard way and now covered by a test: the CPU cannot
detect "a branch was taken" by comparing PC against the value the pipeline
installed. A branch with an offset of zero lands exactly there, and would be
mistaken for a fall-through. Every handler that writes PC says so explicitly.

## Memory map (`src/core/bus.h`)

| Region | Base | Size | Confidence |
|---|---|---|---|
| DRAM | `0x00000000` | 2 MB | Confident |
| VRAM | `0x00200000` | 1 MB | Confident |
| ROM (BIOS, reset vector) | `0x03000000` | 1 MB | Confident |
| NVRAM | `0x03140000` | 32 KB | **TODO(map)** — confirm base and stride |
| MADAM | `0x03300000` | — | **TODO(map)** — confirm |
| CLIO | `0x03400000` | — | **TODO(map)** — confirm |

The machine is **big-endian as the CPU sees it**. Every word and halfword access
byte-swaps on a little-endian host. A swap that is right in one direction and
wrong in the other produces graphics that are almost correct, which is the most
expensive kind of bug to find later, so both directions are pinned by tests.

Reads of unmapped space currently return zero rather than raising a data abort.
That is a scaffolding decision to keep early boot alive while the chips are
being written, and should become a real abort once CLIO exists.

## Still to be written

MADAM (the CEL engine), CLIO, VDLP, the DSP, SPORT and the CD-ROM interface.
Each needs its own section here as it lands, naming the documentation it was
built from and how it was verified.
