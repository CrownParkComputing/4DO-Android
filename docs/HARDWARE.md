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

## Still to be written

The DSP, SPORT, and the XBUS interface through which the machine actually asks
for sectors — the disc layer above is ready but nothing connects it to the CPU
yet. Each needs its own section here as it lands.
