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
| DRAM | `0x00000000` | 1 MB | See below |
| VRAM | `0x00100000` | 1 MB | See below |
| ROM (BIOS, reset vector) | `0x03000000` | 1 MB | Confident |
| NVRAM | `0x03140000` | 32 KB | **TODO(map)** — confirm base and stride |
| MADAM | `0x03300000` | — | **Confirmed** by the boot ROM |
| CLIO | `0x03400000` | — | **Confirmed** by the boot ROM |

The machine is **big-endian as the CPU sees it**. Every word and halfword access
byte-swaps on a little-endian host. A swap that is right in one direction and
wrong in the other produces graphics that are almost correct, which is the most
expensive kind of bug to find later, so both directions are pinned by tests.

Both chip bases were confirmed by disassembling a real boot ROM rather than
taken on trust: at `0x03000068` it builds `0x03400000` for CLIO and at
`0x0300006C` `0x03300000` for MADAM, then immediately reads a CLIO register.

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

## Disc images (`src/core/disc.cpp`)

Not a chip — the layer that turns a file into "give me logical block N". It is
here because the way a CD is stored in a file is the single most common source
of a disc that "does not work".

A sector on the disc is 2352 bytes: sync, header, user data, error correction.
A dump may hold full raw sectors or only the 2048-byte cooked payload, and where
the payload starts depends on the mode:

| Layout | Stride | Payload starts at |
|---|---|---|
| Cooked | 2048 | 0 |
| Raw, mode 1 | 2352 | 16 (sync 12 + header 4) |
| Raw, mode 2 | 2352 | 24 (sync 12 + header 4 + **subheader 8**) |
| Raw, mode 2 (2336) | 2336 | 8 |

**The layout is detected, never assumed from the extension.** `.bin` is usually
raw and `.iso` usually cooked, but both conventions are broken often enough that
trusting them causes the characteristic failure: reading a raw image as cooked
*almost* works, because the first 2048 bytes of sector zero really are data, and
then every sector after it is off by 304 bytes. That looks like a corrupt disc
rather than a misread one. Detection looks for the twelve-byte sync pattern and
then reads the mode byte. A test walks every sector of a raw image rather than
spot-checking, because the failure only appears from the second sector on.

Cue sheets are parsed for track number, type and start position (`mm:ss:ff` at
75 frames per second), with each track's length taken from the next track's
start. A cue naming a WAVE or MP3 file is **refused with an error** rather than
accepted and then playing silence.

## Audio output (`src/core/audio_ring.cpp`)

Not a chip either — the transport between the emulator and the platform's audio
device. It is the one piece with a hard realtime deadline on *both* ends, so it
is lock-free, single-producer single-consumer, and never allocates.

Two policies are deliberate and tested:

- **An underrun yields silence, not a repeated sample.** Repeating is the other
  obvious choice and it buzzes, which sounds like a broken emulator rather than
  a dropped frame.
- **A full ring refuses the excess rather than blocking.** The emulator thread
  must never wait on the audio thread — on Android and iOS that would stall the
  device's whole audio pipeline, not just this app.

`refused()` counts samples the ring had no room for, and the header says plainly
what that does and does not mean: a producer that retries loses nothing even
though the count rises, so it measures back-pressure rather than lost audio. The
emulator never retries, so for the emulator the two coincide. This distinction
exists because a test caught the original counter conflating them.

The console fills the ring with the right number of samples every frame even
though the DSP does not exist yet, so pacing and underrun behaviour are exercised
from the start rather than appearing for the first time when sound is switched
on.

## Control pad (`src/core/pad.cpp`)

Pads daisy-chain on the 3DO — the machine sees one serial stream with every
connected pad in it, which is why it needs no multitap. Up to eight.

Each pad's buttons live in a single atomic word, so the emulation thread always
sees a coherent set rather than a half-applied update. Presses are
read-modify-write rather than stores, because the host may map several sources
(keyboard *and* gamepad) onto pad one and a store would lose whichever arrived
first — which shows up as a diagonal that will not hold. Disconnecting a pad
releases its buttons, or a pad unplugged mid-press leaves the machine holding a
direction forever.

## Threading and pacing (`src/platform/emulator_thread.cpp`, `src/core/frame_mailbox.cpp`)

Emulation runs on its own thread and nowhere else. It publishes finished frames
into a mailbox and pushes audio into a ring; the display takes whatever is
newest whenever it happens to look. Neither side ever waits for the other.

**One pacing policy, in one place.** The core this replaces ran emulation and
GPU presentation on the same thread and then stacked three independent
frame-droppers on top — an audio-starvation check, an adaptive skip ladder, and
a renderer that silently bailed if it could not take a lock. None knew about the
others, so a machine only slightly behind could drop far more frames than any
one of them intended. Dropped frames with correct audio is exactly what people
describe as "runs slow": the mitigation was the symptom.

So the rules are stated once, in the thread:

- Pace to the field rate against a monotonic deadline.
- If a frame overruns, let the deadline slip rather than trying to catch up.
  Catching up produces a burst that starves audio and then overruns again.
- If more than four frames behind, reset the deadline instead of accumulating
  debt that can never be paid off — after a stall, a breakpoint or the app being
  backgrounded, sprinting to recover minutes is worse than dropping them.
- Always sleep to the deadline; never spin. A handheld free-running at 100% of a
  core gets hot, and a hot handheld is slower — a loop the old core could enter
  and not leave.

**Presentation is no longer a frame-dropper at all.** A display that misses a
frame simply sees the next one, and emulation never notices.

### The mailbox

A triple buffer: one slot being filled, one holding the newest finished frame,
one being read. Publishing is a single atomic exchange with the "is it fresh"
flag packed into the same word as the slot index, so there is no window in which
a consumer could see an index that does not yet name a complete frame.

Not a queue, because a stale frame is worthless — a display that is behind wants
the *newest* frame, not the oldest unread one, and a queue would add latency
that never recovers. Not a mutex, because the old core's present path took a
lock and dropped the frame if it could not get it, which is a frame-dropper
hidden inside what looks like a rendering detail.

`acquire()` returns nullptr when nothing new has arrived, so the display can
skip a texture upload it does not need. Tests cover the properties that matter:
frames are never seen half-written under mismatched producer/consumer rates,
never go backwards, and the slot being read is never written.

### Two frame rates, both shown

The overlay reports the machine's rate against its target *and* the display's
rate, separately. They are different numbers, and a machine running at half
speed behind a perfectly smooth window is precisely the failure the previous
core made invisible. Audio underruns are shown too, because they are the first
symptom of falling behind and appear before anything is visible on screen.

## Files and storage (`src/platform/storage.cpp`, `src/ui/file_browser.cpp`)

Not hardware, but it decides whether the app is usable at all on the platforms
this project exists for. A desktop user can type a path into a text field; a
phone user cannot, and has no idea what the filesystem looks like anyway. So the
launcher browses.

`Storage::writable_directory()` is where the app may always write, via SDL, which
puts it in the right place per platform: the private data directory on Android,
Application Support on iOS and macOS, AppData on Windows, XDG data on Linux.

`Storage::browse_roots()` offers sensible starting points, app storage first —
on iOS that is the directory the Files app exposes, and the only place a user
can put a disc image without a computer.

Pure path arithmetic lives in `src/core/path.cpp` rather than here, so it can be
tested without a platform layer. `parent()` earns its own tests: navigation
rests on it entirely, and if it is wrong a phone user cannot go up a level and
is stuck with no keyboard to type their way out. The cases that matter are a
trailing separator (`/a/b/` must give `/a`, not `/a/b`, or the first press of
Up appears to do nothing) and a top-level entry (`/foo` must give `/`, not an
empty string, or navigation dead-ends a level early).

### Android: scoped access, no storage permission at all

The app requests **no storage permission of any kind**. The user grants
individual folders through the Storage Access Framework and the app sees exactly
those and nothing else, persisted across restarts. All-files access would need a
Play sensitive-permission declaration and review; this app does not need it and
does not want it. The whole APK declares one permission, `VIBRATE` — the SDL
template's `CAMERA` and `INTERNET` were removed as unused, and `CAMERA` in
particular is a dangerous permission an emulator has no business holding.

The consequence shapes the core, not just the UI: **SAF hands out content URIs,
not paths.** Nothing it returns can be `fopen`ed. So `Disc` and `Console` grew
open-by-descriptor paths, and the file browser has two modes — filesystem paths
elsewhere, document URIs on Android — differing only in how a location is named
and listed.

Three things that are easy to get wrong here, each with a comment where it
matters:

- The grant must be taken with `takePersistableUriPermission` at the moment it
  arrives. Without it the grant dies with the process and the app silently
  forgets the user's library on every restart, which reads as a bug in the app
  rather than a missing call.
- The descriptor must be **detached** from its `ParcelFileDescriptor` before
  being handed to C++. Passing the attached fd lets Java close it underneath
  native code at the next garbage collection — an intermittent read failure that
  would be very hard to attribute.
- A JNI call that leaves a pending Java exception makes the *next* unrelated JNI
  call fail, so exceptions are cleared at every boundary.

There is no "Up" button in document mode. A SAF document URI carries no
derivable parent, and walking up out of a granted tree is precisely what scoped
access exists to prevent.

A cue sheet cannot be opened by descriptor, because it names a sibling file a
descriptor gives no way to reach. That is refused with a message saying to open
the image instead, which beats opening the cue as though it were an image and
then reporting a corrupt disc.

## On-screen controls (`src/core/pad_layout.cpp`, `src/ui/touch_pad.cpp`)

A phone has no buttons. Without these the emulator is unplayable on the platform
it is mainly for, however well it runs.

Positions are fractions of the play area rather than pixels, because a phone, a
foldable and a tablet have wildly different screens and a pixel layout that suits
one is off the edge of another. The layout arithmetic lives in `core` so it can
be tested without a window — a separation that earned itself immediately, since
both of the following were invisible in the code and obvious the moment the pad
was rendered:

- **x is a fraction of width and y a fraction of height, so equal fractional
  offsets are not equal distances.** Laying a d-pad out with matching offsets
  stretches it into a wide diamond on any widescreen phone. Offsets are now
  given in units of the smaller dimension and converted per axis, which is why
  the layout needs the aspect ratio and cannot be a static table.
- **A cluster centre must sit at least (spread + radius) from the edge**, or its
  outermost control hangs off the screen — unpressable, and in layout mode
  undraggable, so there is no way to recover.

Tests assert both across five screen shapes including portrait, plus that no two
controls overlap (a touch resolves to exactly one, so an overlap makes one of
them unreachable).

Touches are tracked **per finger**. A player will hold a direction and press a
button at the same time, and taking only the most recent touch would make that
impossible. A mouse is treated as one more finger with an id no real touch uses,
which makes the controls testable on a desktop.

Layout mode makes dragging move controls instead of pressing them, clamped to
the screen. A stored position outside 0..1 is rejected on load rather than
applied, so a layout file from another build cannot strand a control somewhere
unreachable.

## Settings (`src/core/settings.cpp`)

A flat key/value file. Nothing here warrants a schema, and a text file can be
read and repaired by hand on a device with no debugger attached.

Only the **first** `=` separates key from value: a SAF content URI routinely
contains one, so splitting on the last would truncate every Android path. A
value that is not entirely a number falls back to the caller's default rather
than becoming zero, so a corrupted line does not quietly look like a valid
setting.

**Remembered paths are stored but never trusted.** A SAF grant can be revoked
from system settings and an iOS container is reassigned on every install, so a
path that worked yesterday may be meaningless today. On load the app tries the
remembered BIOS and disc and, if they no longer resolve, forgets them silently —
that is ordinary rather than exceptional and should not be reported as an error.

## Booting a real BIOS

Running a genuine boot ROM found a hang the test suite could never have,
because it depends on the ROM's own expectations rather than on our behaviour:

**The ROM reads CLIO register `0x28` (CSTATBITS), masks it with `0x43`, and
branches on 1, 2 or `0x40`.** It is asking *why the machine started*. CLIO
returned zero, which matches none of those, so the ROM fell through into a
two-instruction infinite loop at `0x030000A8` — a black screen with the CPU
apparently busy. Reporting a power-on reset instead lets it proceed, and it now
runs on across two code regions and writes RAM.

The lesson is that **zero is not a neutral default for a status register.** A
register that reports a cause from a fixed set will hang the ROM if it reports
none of them. Two more of the same shape followed.

**CLIO `0x34` is the line counter, not the pixel counter.** The map had these
the other way round. The ROM loads the literal `0x7FF` as a mask, reads this
register and waits for exact values, with 390, 478 and 394 sitting in the same
literal pool — those are line numbers in a 525-line frame, not pixel positions.

**A slice must be shorter than a scanline.** The CPU and CLIO advance in
alternating chunks, so the CPU can only observe a line number if CLIO happens to
stop on it. With the old 4096-cycle slice — about five lines — the ROM's waits
for an exact line simply never matched. `run_frame` now slices at half a
scanline, which guarantees every line is observable. This is a general hazard
for any polled counter, not just this one.

**MADAM `0x04` reports the memory configuration**, and the ROM's own decode is
what documents it: VRAM megabytes in bits 0-2, and DRAM as the sum of two
two-bit bank fields at bits 3-4 and 5-6. A stock machine is `0x29`. Reporting
zero says the machine has no memory, and the ROM fails its memory test.

With those three in place the ROM gets meaningfully further: it passes the
memory test, **relocates itself into DRAM and runs from there**, and writes
VRAM. It then reaches a panic handler again — an `STMDB` into MADAM followed by
a branch to itself — with `0xFFEEFFEE` in a register, which has the look of a
memory-test pattern. That is the next thing to chase.

### The memory map, corrected by the ROM

VRAM was at `0x00200000` with 2 MB of DRAM below it, which is the commonly
cited layout. The boot ROM disagrees. Its memory test fills and verifies at
`r0 + literal` with **`r0 = 0x00100000`**, while naming SPORT pages from the
bare literal with no base added — and the two only reconcile if VRAM begins at
`0x00100000`. Moving it there, with DRAM as 1 MB and MADAM's memory
configuration reporting `0x21`, is what makes the ROM's own self-test pass.

This is recorded as evidence rather than as settled fact: it is what this ROM
requires, and it conflicts with the usual description of a 2 MB machine. If a
later finding explains `r0` differently, this is the thing to revisit.

### SPORT

An earlier reading of this concluded the DSP was the blocker. That was wrong,
and the way it was wrong is worth keeping. The panic handler at DRAM `0x178` is
reached from a block at `0x0170` that first calls `0x0A4C` — a routine that
uploads code to the DSP — so it looked as though DSP bring-up had failed. But
`0x016C` is `MOV pc, lr`, a return: the block at `0x0170` is *branched to*, not
fallen into. It is the error handler, and `0x0A4C` is how the machine reports a
failure — almost certainly the self-test failure tone.

Counting settled it: at the moment the error handler is entered there have been
**zero** DSP writes and **five** SPORT accesses. The DSP work all happens
afterwards.

The real failure is a **memory test that drives SPORT**. At DRAM `0x0904` the
ROM:

1. fills a region with an arithmetic sequence (`STR r2,[r1],#4; ADD r2,r2,r3`),
2. waits for the video line to be in the window 10..13, polling CLIO `0x34`,
3. drives SPORT — building addresses as `0x03200000 | (address >> 9)`, the
   classic "the address *is* the command" encoding,
4. reads the region back and compares each word against the expected sequence,
5. and branches away to the error path on the first mismatch.

Nothing was mapped at `0x03200000`, so the SPORT writes went nowhere and the
readback could not match. The region is now mapped and its accesses counted —
silently dropping them is exactly what made this hard to find.

SPORT is now implemented, and the ROM's own test is what documents it. There
are three windows inside the region:

| Offset | Meaning |
|---|---|
| `+0x0000 + page` | Copy. A **read** latches the source page; a **write** copies the latched page here, under the mask written. |
| `+0x2000` | The fill value. A single register, not paged. |
| `+0x4000 + page` | Fill. A **write** fills this page with the fill value, under the mask written. |

A page index is a VRAM offset shifted right by nine, so `page << 9` is the
byte offset — and those offsets are **relative to VRAM, not absolute**, which
was the hardest part to see. A page is **2048 bytes**, fixed by the test reading
back and comparing exactly 512 words.

The mask matters: a destination bit is replaced only where the mask is set,
which is what lets SPORT write one bitplane without disturbing the others.

**CLIO's line register carries a field flag in bit 11**, above the eleven-bit
line number. The ROM waits on it directly — `TST r2, #0x800`, spin, then mask
with `0x7FF` and wait for a particular line — so masking it off makes that wait
never finish.

With SPORT, the map correction and the field flag in place, the ROM **passes its
power-on self test** and **boots to the 3DO logo**.

### Getting to a picture

Two more things were needed, and both were found by recording every register the
ROM programs rather than by reasoning about what it might do:

**MADAM `0x0580` is the display-list address.** The ROM writes exactly one
word-aligned value pointing into VRAM, `0x001B0000`, and this is where. It is an
absolute bus address, not a VRAM offset — as an offset it would be past the end
of a 1 MB VRAM. Nothing had connected MADAM to the VDLP, so the display list
address stayed zero and the screen stayed black no matter how well the ROM ran.

**The framebuffer is interleaved, not linear.** Each 32-bit word holds two
pixels at the same x from two adjacent scanlines: the even line in the high
half, the odd line in the low half. So a pair of lines occupies `width` words,
and consecutive pixels on one line are four bytes apart rather than two.

That one is worth recognising by sight. Reading it linearly produced a logo that
was clearly *the* logo — right colours, right shapes — but squashed to half
width, duplicated across the screen, and with every other line black. It reads
as a stride bug and it is a pixel-order one.

The offset is computed by `framebuffer_offset()` in one place, used by both
MADAM (which writes) and the VDLP (which reads). They were briefly inconsistent,
and the test that caught it was the one that draws a cel and then checks it
appears in the frame — the seam between the two chips.

A useful thing to recognise: the routine at DRAM `0x100` is a **nested delay
loop**, not a hang, and its inner count is chosen by comparing PC against
`0x03000000` — 45 iterations when running from ROM, 2192 from DRAM. It is
calibrating against memory speed. Roughly 7.4 million iterations, so a trace
budget of 40 million instructions stops inside it and looks exactly like a hang.

A harness trap worth recording alongside it: the first trace stepped the CPU
directly and never ticked CLIO, so every poll of a video counter waited forever.
That looks precisely like an emulator bug and was entirely an artefact of the
harness. Drive whole frames.

## CHD recognition (`src/core/chd.cpp`)

CHD is the format most 3DO libraries are actually stored in, so an emulator that
cannot recognise one is confusing to use. Without this a `.chd` falls through to
the raw-image path, finds no sync pattern, is taken for a cooked 2048-byte
image, and reports a plausible sector count computed from *compressed* data —
every read then returns compressed bytes as though they were disc contents.
That presents as a corrupt disc rather than an unsupported file, which is far
worse than a clear refusal.

So a CHD is identified, described (version, uncompressed size, codecs, whether
it is a delta against a parent) and refused with a reason. It is not yet
readable: the data needs a decoder per codec, which is a separate job.

Verified against a real library: `CHD v5, 766 MB uncompressed, cd-lzma +
cd-zlib + cd-flac`.

## What device testing changed

Five things were wrong that no amount of desktop testing would have shown, all
found within a minute of the first install on a handheld:

- **The system status bar sat on top of the app.** Not merely untidy: it covers
  the top row of controls and takes touches meant for them. Fixed with a
  fullscreen theme and `windowLayoutInDisplayCutoutMode=shortEdges`, without
  which a notched device letterboxes the whole app into a black band that looks
  exactly like a broken renderer.
- **The interface was unreadably small.** ImGui's default font is 13 pixels;
  on a 1080p handheld that is illegible at arm's length.
  `SDL_GetWindowDisplayScale` reports 1.0 on many Android devices, so it cannot
  be relied on — the scale is now derived from the window's own pixel size, with
  the reported display scale used only as a floor.
- **Button labels were clipped** ("Test pat"), because widths were in fixed
  pixels while the font scaled.
- **The status line ran off the right edge**, with no wrapping.
- **An em dash rendered as `?`.** The default font has no glyph for it. UI
  strings are ASCII only.

The lesson worth keeping: every one of these is a *presentation* bug, invisible
to a test suite that checks behaviour, and every one makes the app look broken.

Note for anyone testing on a Retroid Pocket Flip 2: it runs a desktop-style
freeform window manager, so the app appears in a floating window rather than
filling the screen, and driving it with blind `adb input tap` is unreliable
because other windows compete for the foreground.

## Interrupts are edge-triggered, not level-held

This reverses what an earlier version of this file asserted, and the boot ROM is
what settled it.

CLIO signals a **rising edge** to the CPU rather than holding the line. A
handler that returns without acknowledging its source is therefore *not*
re-entered.

The evidence is decisive. The ROM's vertical-blank handler pushes registers,
calls a routine that reads a **software flag in RAM** and returns, and never
writes any CLIO register at all — across a whole run it touches exactly seven
CLIO registers, none of them an acknowledgement. With a held line the machine
livelocks on its own startup interrupt: enter handler, return, re-enter,
forever. The logo appears and then nothing ever moves.

`Arm60` keeps both inputs. `set_irq` holds a level, which is the textbook model
and is what a future source may want; `signal_irq` latches one edge, consumed
when the CPU takes it. CLIO uses the latter.

## Byte and halfword access to device registers

`read8`/`read16`/`write8`/`write16` originally handled memory only, so any
byte-wide access to a chip register silently returned zero or vanished. That is
the worst class of gap — invisible, and it produces wrong behaviour far from its
cause. They now route through the containing word, as the hardware does.

## Where it stops now: XBUS

With the above in place the ROM boots, draws its logo, leaves its interrupt
handler and runs its main program. It then polls **CLIO `0x0400`** for a ready
bit (`0x80`) — the expansion bus, through which the CD-ROM is reached. Returning
zero there leaves a fully booted machine sitting on a static logo, which is
exactly what it looked like.

Reporting the bus ready lets it proceed into actual transactions: it writes a
command byte to **CLIO `0x0500`** and polls **CLIO `0x0540`** for bit `0x10`,
meaning the command has completed.

**An empty bus still has to answer.** The machine must run its attract sequence
with no disc in the drive, so "nothing attached" cannot mean "never replies" —
that is a hang, not an empty drive. Reporting every command complete lets the
ROM's device enumeration finish and find nothing, rather than waiting forever
for a device to speak.

With that, the ROM gets substantially further. It now programs the timer bank
(four timers with real reload values), configures XBUS, writes several more
MADAM display registers, **enables interrupts at the CPU** and services them.

It then waits on a **software flag in RAM** — `[0x000FDA70 + 0xFC]` — which a
completion handler would set. Nothing sets it, because there is no real device
behind XBUS to complete anything and raise the interrupt that would.

Three things were ruled out while narrowing this, all worth not repeating:

- **It is not waiting for an interrupt — at all.** Every source in both banks
  was raised in turn, repeatedly, and none of them moved the machine: 64 trials,
  each still spinning in the same ten instructions. That rules out the entire
  "which interrupt signals completion" line of enquiry, which was where I would
  otherwise have kept looking. The flag must be set by code that runs only when
  a device actually answers with meaningful data.

- **It is not the timers.** Forcing every programmed timer to run regardless of
  the enable register changed nothing. (The enable encoding does still look
  wrong — the ROM programs timers 0-3 and then writes a value that this
  implementation reads as enabling timer 12 — but that is not what blocks it.)
- **It is not a missing interrupt enable.** By this point the CPU has interrupts
  unmasked and CLIO has several sources enabled and pending.

The routine the loop calls each iteration turned out to be a red herring worth
naming: it only increments a counter at `[0x0002F2F0 + 0x50]`. It looks like a
timeout or a poll, and is neither — it is a spin counter.

So XBUS proper is the next piece, and what it needs is now clear: not just a
ready bit and a completion bit, but a **device that answers commands with
meaningful data**. The 3DO's CD-ROM drive is built in, so enumeration should
find a device present with no disc in it — "no device at all" is not a state the
machine is ever really in, and may itself be why the driver never initialises.

### What the flag actually is, and why the reply must be DMA

Scanning DRAM for the instruction that would set the awaited flag found six
candidates, and all of them sit inside **structure initialisers** — runs of
stores filling consecutive fields at `0xFC`, `0x100`, `0x104`, `0x108`. So the
flag is not a completion signal at all: it is a field in a **driver record**,
populated when the CD driver is installed. The wait loop means "wait until the
driver exists", and the driver is only built once a device enumerates.

Tracking reads as well as writes then narrowed it further. Across a whole run
the ROM reads **CLIO `0x0540` exactly once**, writes `0x0500` once, and never
reads any other port in that range. One command, one status read, and it stops.
So the device's *reply* does not come back through a port at all.

What it does do is program **MADAM `0x0218`, `0x021C`, `0x0238`, `0x023C`**,
which have the shape of DMA channel setup. That is where a reply would land: the
device DMAs its identification into memory and the ROM reads it from there.

Five different status replies were tried — complete, error, ready-with-error,
several combinations — and none moved the machine. That is consistent with the
reply being data in memory rather than bits in a status port.

### MADAM's DMA channels

The layout is now known, read off the driver that programs them at DRAM
`0x1A7FC`:

    LDR r3, [r0], #4
    STR r3, [r1, #0x18]     ; r1 = MADAM + 0x200  -> channel 3 address
    LDR r0, [r0]
    STR r0, [r1, #0x1C]     ;                     -> channel 3 length
    ...
    STR r2, [r1, #0x38]     ;                     -> channel 7 address
    STR r0, [r1, #0x3C]!    ;                     -> channel 7 length

So the channels are an array based at **MADAM `0x0200` with an eight-byte
stride**: an address then a length. `0x218` and `0x238` are channels 3 and 7,
which is what the expansion bus uses. The registers are implemented and
readable; **no transfer is performed**, because what a device would place in
that buffer depends on the command set.

### Three experiments that close off the shortcuts

Worth recording, because each looked like it might work and none did:

**Feeding the DMA buffer a marker changes nothing** — the ROM never reads it.
The buffer is only consulted after the driver exists, so there is no way to
learn the reply format by watching what the ROM inspects. That was the most
promising route and it is closed.

**Forcing the awaited flag runs a great deal of code and then parks.** Writing
a non-zero value into the driver record makes the machine execute 3403 distinct
instructions instead of ten, reach User mode, and then settle into a four
instruction loop that shifts a stack value left forever. So the wait really is
the gate, and the operating system genuinely needs the driver rather than merely
a flag saying one exists. A shortcut here produces a machine that runs further
and then hangs somewhere less obvious, which is worse than not booting.

**The device table is empty.** The enumeration reads `table[n]` at DRAM
`0x26B94` and every slot is zero, so it dereferences null and sends whatever
byte happens to be at address `0x14` as its command. That is the *consequence*
of no device having been enumerated, not the cause — the table is populated by
whatever answers the bus first.

### The expansion bus, from the 3DO Company's own patent

`WO 94/16382 "Expansion Bus"` documents the protocol properly, which removes the
guesswork the previous sections were stuck behind. It is a granted patent —
public by construction, and on this project's permitted-sources list.

**The bus is a FIFO model.** Each device holds its own FIFOs: a Command FIFO the
system writes into, a Data Return FIFO the device sends bulk data back through,
a Status Return FIFO for status, and (on writable devices) a Data Write FIFO.
So a reply does not come back as bits in a status port, which is exactly what
this core's behaviour had already implied.

**Every command returns at least one Status Byte** when it completes, and
interrupts signal that the Status or Data Return FIFO has something in it. The
Status Return interrupt means "the command has finished".

Status Byte: bit 4 is **ERROR**; all other bits are device-dependent. A status
byte with no error also asserts that all expected data reached the Data Return
FIFO.

Poll Register:

| Bit | Name | Meaning |
|---|---|---|
| 0 | Reserved | reads 0 |
| 1 | Interrupt Disable- | disables interrupts when low |
| 2 | Media Access | high if the media may have been physically accessed; writing 1 clears it |
| 3 | Reset- | resets the device; must not disturb its address |
| 4 | StatValid- | **high when the Status Return FIFO has nothing left** |
| 5 | ChunkValid- | **high when the Data Return FIFO holds no complete chunk** |
| 6, 7 | Reserved | read 0 |

Note both valid bits are **active low**, which is the opposite of the obvious
reading and would have been a very easy thing to get backwards.

Transactions are selected by three control lines: `SELECTION` (a device stays
selected until a SELECTION naming a different address), `WR_COM`, `RD_STAT`,
`WR_DATA`, `RD_DATA`, `RD_POL`, `WR_POL`. Devices are numbered 0-15 and take
their address at power-up by counting strobes, so the CD-ROM's address is a
consequence of where it sits on the chain rather than something fixed in silicon.

## The expansion bus, as the ROM actually drives it

The port map was settled by logging every expansion-bus access **in order**,
with the PC that made it, and reading that trace against the patent. Counting
accesses alone had produced a wrong answer twice; the ordering is what made it
legible.

| Port | Role | How it was established |
|---|---|---|
| `CLIO+0x0400` | bus status | read fifteen times, before anything else |
| `CLIO+0x0500` | strobe / select | **eighteen writes of zero** at start-up |
| `CLIO+0x0540` | result read | read after every strobe |
| `CLIO+0x0580` | Command FIFO (`WR_COM`) | `0x83` then six more bytes |

The opening burst of eighteen zero writes to `0x0500` is what identified it. The
patent describes an ID-assignment procedure in which the system strobes
seventeen times after reset so each device can determine its own address by
counting. Nothing else in the protocol looks like that, and no other port
receives such a burst.

That in turn moved the Command FIFO to `0x0580`. An earlier reading had the
command port at `0x0500`, which is where the strobes go — so the ID-assignment
burst was being delivered to the drive as eighteen commands.

### Commands are seven bytes

The ROM writes `0x83` followed by exactly six further bytes before it begins
polling for a reply, and every later command is the same width. Replying per
byte — which is what the first implementation did — makes one command look like
seven, so the reply FIFO runs six answers ahead of the conversation and every
subsequent exchange reads the previous command's answer.

### The poll bits are presented inverted

The patent defines `StatValid-` and `ChunkValid-` as **active low**. CLIO does
not present them that way: the ROM issues a command and then waits for bit 4 to
be **set**. Implementing the bus polarity at the register interface hangs the
machine on its very first command. CLIO inverts them, and software sees "the
FIFO has something" as a one.

### Completion is an interrupt, and it is bit 9

Every command returns at least one Status Byte, and the Status Return interrupt
is how the system learns the command finished. Which CLIO bit carries it was
settled by asking the ROM rather than guessing: once the OS boots it enables
`0x60000206` and idles. Bits 1 and 2 — vertical blank and the timer — do fire.
Bits 9, 29 and 30 are enabled and never fire, so the source the idle OS is
blocked on is one of those three. Bit 3, the original guess, is not even enabled.

## Interrupts go to FIQ, not IRQ

CLIO drives the ARM's **fast** interrupt. This one register of wiring hid behind
every other interrupt problem in this file, because delivering to IRQ *looks*
like it works: the boot ROM installs a handler at both vectors, so a handler
runs and returns quite happily.

It is simply the wrong handler. The tell was that across an entire boot the
machine never read the pending register and never wrote the clear port - which
is impossible for an OS servicing its own interrupts, and is what eventually
gave it away. Delivered to FIQ, the real service routine reads the pending
register over twelve thousand times in 120 frames.

Everything below was an attempt to explain the symptoms of this bug without
having found it, and is kept because each wrong turn is a plausible one.

## Interrupts: the edge is per source

This one was wrong twice, in opposite directions, and both failures are worth
recording because each looks like the other's fix.

**Holding the line livelocks the machine.** The boot ROM enables vertical blank
and then never acknowledges it — across a whole run it writes no CLIO clear port
at all, because it drives the boot animation by polling the line counter
(`CLIO+0x0034`, read over 155,000 times in 120 frames) instead. A level-held
line therefore re-enters the vertical-blank handler forever, before the OS
finishes starting.

**But a single edge flag for the whole line is just as wrong**, and fails much
later and far more confusingly. It stays latched for as long as *any* enabled
source is pending, so once vertical blank goes pending and stays pending, no
other source can ever produce an edge again. Symptom: the OS boots, asks the CD
drive fifteen questions, and idles forever — while the expansion-bus completion
bit sits plainly pending and enabled, with interrupts unmasked at the CPU, and
the CPU never takes it.

Both of those readings were attempts to work around delivering to the wrong
vector. With FIQ and the real service routine, the ordinary hardware model is
also the correct one: CLIO **holds** the line while any enabled source is
pending, and software acknowledges through the clear port. Both historical
failure modes are still pinned by tests.

## Expansion bus: selection is by VALUE

The device is named by the value written to SELECTION, not by the address
written to. Reading the device number out of the address is a very plausible
mistake - the window is 0x40 bytes, which is exactly sixteen devices at four
bytes each - and it was made here. It makes every address on the bus answer as
though hardware were fitted there.

`0x8F` written to SELECTION is a probe rather than a device: it asks whether
the bus is over-populated. Answering it as an ordinary selection leaves CLIO
reporting "too many devices" and enumeration fails before the drive is reached.

The poll register splits into a writable control nibble and a read-only state
nibble:

| Bit | Meaning |
|---|---|
| 0 | status interrupt enable |
| 1 | read interrupt enable |
| 2 | write interrupt enable |
| 3 | reset |
| 4 | status valid |
| 5 | read valid |
| 6 | write valid |
| 7 | media access (read-clear) |

The driver confirms the control bits directly: at three separate call sites it
reads the register, masks to the low four bits, and ORs in 1, 2 or 4.

## The CD conversation, read out of the driver

The driver was disassembled from the running machine's DRAM - black-box
analysis of software we are entitled to observe - which settled what no
document to hand covers.

The reply reader:

```
loop:  poll(device); tst r0,#16      ; status valid?
       beq exit                      ; not ready -> stop early
       read RD_STAT -> buffer
       loop while bytes remain
exit:  poll(device); tst r0,#16
       strne <error>, [r3,#92]       ; STILL ready -> record an ERROR
```

Two things follow, and both had been got wrong. The FIFO must hold **exactly**
the number of bytes expected and be empty afterwards - anything left over is an
error, so a completion queued the instant the acknowledgement is drained fails
every command and produces a fifteen-deep retry loop. And the drive does not
answer instantly: the completion has to arrive late enough that the reply loop
has already finished.

The acknowledgement opens by **echoing the command byte**. That is not a guess:
sweeping all 256 values of that byte against the real ROM changes the machine's
behaviour for exactly one of them, and it is the opcode just sent.

After the reply the driver waits on a separate asynchronous event:

```
wait:  poll(device); tst r0,#16
       beq wait
       read RD_STAT
       tst r0,#8                     ; test bit 3
```

## The expansion-bus control register

`CLIO+0x0400` is a set/clear pair like the interrupt registers, writable only
through the mask `0xca80`. Bit 7 is a hardware completion flag rather than a
software one: the boot ROM CLEARS it and then spins until it comes back set.

Honouring that clear literally hangs the machine on its own bus setup - a
million reads of this one register and no further progress. Our bus finishes a
transaction the moment it starts, so the bit always reads set.

`CLIO+0x0410` and `0x0414` are DIPIR - "disc inserted player interrupt request"
- carrying an active flag, a happened-before-reset flag, and a device number.
They are implemented, but worth recording: **this BIOS never reads them**, so
whatever tells it about a disc, it is not these.

## Where this reaches

The BIOS boots, initialises the OS, brings up the expansion bus, assigns device
addresses, installs the CD driver, issues commands to the drive and receives
their replies — and renders **the 3DO startup logo**, correctly: red diamond,
blue block, dithered gold sphere, contact shadow and wordmark, on the light
panel over black.

## What the boot actually does, traced

The CPU carries an optional execution map: one bit per word of DRAM, set the
first time an address is executed. Diffing it per frame answers "what did the
machine do, and when did it stop doing it", which sampling the PC at frame
boundaries cannot - a boot sequence is thousands of functions deep and the
interesting function runs for a few hundred microseconds, so it is never the
address you happen to catch.

The picture it gives:

| Frame | What runs |
|---|---|
| 1-15 | early start-up; the logo is drawn |
| 36-47 | video timing calibration, CD driver init |
| 86-90 | the OS and operator start; an I/O request is built and a syscall blocks |
| 146 | that syscall returns and forty instructions run |
| after | nothing new, ever |

The syscall entered at frame 90 and returning at frame 146 is a **blocking
wait** - the call sites resolve to SWI trampoline stubs of the form
`mov r12,#n; b common`, the Portfolio OS syscall table. Around 0.93 seconds
pass between the two, and what returns is a signal mask which the code then
tests bit by bit.

So the machine is not hung: it is a live event loop that has run out of events.

### Two things this rules out

**It is not blocked on the CD.** Removing the CD-ROM device from the bus
entirely produces the same terminal path, shifted three frames. Inserting a disc
produces a byte-identical execution map to not inserting one - the BIOS never
takes a different path, because it never looks.

**It is not an interrupt problem.** The machine takes about 126 FIQs a frame,
and the real service routine reads the pending register thousands of times.

### The honest boundary

The logo is drawn but does not yet animate. The machine reaches the OS idle loop
— a counter increment, touching no hardware at all — having completed its CD
conversation. What is missing is the **command set**: what `0x83` and `0x8F`
mean, and the shape of the reply the drive DMAs back. The transport is now
complete and characterised; the vocabulary spoken over it is not.

Fabricating replies until the animation runs would be actively harmful: it would
produce a driver that works for the wrong reason, and every later conclusion
would rest on it. That needs documentation, not more guessing.

## Still to be written

The DSP, and the CD command set above the transport. Each needs its own section
here as it lands.
