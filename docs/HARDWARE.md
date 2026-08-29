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
being written, and should become a real abort once MADAM exists.

## CLIO (`src/core/clio.cpp`)

The I/O controller: interrupt controller, timer bank, video line and pixel
counters. It is the chip that lets boot code get anywhere — without interrupts
and a running line counter, the BIOS spins forever waiting for a vertical blank.

The part implemented with confidence is the **interrupt handshake**, because its
shape is unusual enough to be worth stating plainly:

- Pending and enable are separate registers, and each has **separate set and
  clear ports** rather than being read-modify-written. A handler acknowledges by
  writing the bits it handled to the clear port, which cannot lose a source that
  fires while the handler is running.
- Disabling a source **masks it without acknowledging it**; the pending bit
  survives.
- The line into the CPU is **level-sensitive**. A handler that fails to
  acknowledge is re-entered immediately. That is the hardware's behaviour and is
  asserted by a test, so a missing acknowledgement shows up as an obvious loop
  rather than as a mysterious slowdown.
- Bank 1 does not reach the CPU directly; it chains into a bit in bank 0, so a
  handler always reads bank 0 first.

Marked `TODO(clio)`: most individual register offsets, which timer prescaler
sets the decrement rate (timers currently run at the CPU clock — the right shape
at the wrong speed), and which bank-0 bit bank 1 chains into.

A scanline is derived, not hardcoded: 12.5 MHz over 60 fields of 263 lines is
about 792 cycles, and deriving it keeps PAL right when the region changes the
line count.

## VDLP (`src/core/vdlp.cpp`)

The Video Display List Processor. The 3DO has no fixed framebuffer register: the
display is a linked list in VRAM, each entry naming a framebuffer, a line count,
a colour table and the next entry. That is how games change palette or buffer
partway down the screen.

Confident and tested: **pixels are 16-bit RGB555 in VRAM, two per 32-bit
big-endian word**. Five bits per channel are widened to eight by replicating the
top three bits into the low ones. A plain left-shift instead would make white
come out as `0xF8F8F8` — which looks fine in a screenshot and is wrong against
hardware, so both ends of the range are asserted.

Marked `TODO(vdl)`: the bit assignments inside a VDL control word, and the word
order within an entry. Until those are confirmed, `render_linear()` reads VRAM as
a plain framebuffer, which is what the app's test pattern uses to prove the video
path independently of the list format.

Malformed lists are handled rather than trusted — a self-referencing entry, an
entry claiming zero lines, and a walk longer than 1024 entries all terminate.
Games do write garbage here during startup, and an infinite walk would look like
a freeze.

## MADAM (`src/core/madam.cpp`)

The graphics engine. MADAM draws by walking a linked list of Cel Control Blocks;
each CCB names its source pixels, their encoding, and a 2x2 matrix of 16.16
fixed-point deltas mapping the source rectangle onto the destination. Because
that is a general affine step rather than a blit, a cel can be scaled, rotated
and sheared — every textured polygon in a 3DO game is a cel.

Implemented: CCB walking with absolute and relative pointers, the full
fixed-point mapping including the row-to-row bend (`hddx`/`hddy`) that makes a
cel a quad rather than a parallelogram, direct 16-bit cels, indexed 1/2/4/6/8-bit
cels through the PLUT, colour-zero transparency, and clipping.

### Two things worth writing down

**Forward mapping leaves holes.** The engine walks *source* pixels, so writing
one destination pixel per source pixel makes any magnified cel come out as a
grid of dots. Each source pixel therefore fills its whole destination footprint,
with the step counts derived from the step vectors — so a 1:1 cel still costs
exactly one write per pixel, and only magnified cels pay more, proportional to
the destination area they actually cover. Because the footprint follows the step
vectors rather than being a rectangle, it stays correct under rotation. All three
properties have tests; the failure mode looks like a texture effect rather than a
bug, so it would otherwise survive a long time.

**Cels cannot be threaded.** They are drawn in list order and later ones paint
over earlier ones, so handing each CCB to a different thread races on the
destination and produces an intermittently wrong picture rather than a crash.
The safe parallelism is destination row bands *inside* one large cel. This is
recorded because "just thread the cel list" is the natural thing to try and
would be very hard to diagnose afterwards.

Marked `TODO(madam)`: most CCB flag bits, the PRE0/PRE1 field layout (isolated
in two functions so a correction is local), PIXC blending, run-length-coded
source data, and which flag overrides colour-zero transparency.

Note that the size fields are ten bits each, so a cel cannot claim to be larger
than 1024 in either direction. That is what actually bounds the work; the
explicit dimension guard in the renderer is defence in case those field widths
turn out to be wrong.

### Measured throughput

On this development machine (x86-64 desktop, `-O2`, single-threaded). **These are
not phone numbers** — a handheld core is several times slower — but they show
where the headroom is:

| Part | Throughput | |
|---|---|---|
| ARM60 | 128 Mcycle/s | 10.2x realtime against a 12.5 MHz 3DO |
| MADAM | 82 frames/s | 200 rotated, doubled 32x32 cels — 709k pixels a frame, heavier than most real frames |
| VDLP | 6150 frames/s | 320x240; negligible |

The CPU having ten times realtime in a plain interpreter is the decode cache
earning its place, and it means a dynarec is not needed yet. MADAM is the part
with the least margin, which is the expected answer and the reason its inner
loop was built to vectorise from the start.

## Still to be written

The DSP, SPORT and the CD-ROM interface. Each needs its own section here as it
lands, naming the documentation it was built from and how it was verified.
