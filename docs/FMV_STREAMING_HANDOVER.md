# FMV streaming fix — 2026-09-05 follow-up

## Current finding

**Twisted's black-screen stall and Phoenix 3's failure to reach gameplay are
fixed by pacing the CD FIFO and allowing expansion DMA to wait for data.**
No game-specific timer adjustment is included in the fix.

The previous handover below is preserved as investigation history. Its claim
that CD timing was decisively excluded was incorrect: `read_data()` refilled
the next sector synchronously as soon as the last byte was consumed. That
bypassed `sector_delay_`, so sweeping the delay constant did not change the
actual streaming path. Identical results from that sweep were not evidence
that sector timing was irrelevant.

`Console::service_expansion_dma()` also assumed all requested bytes were
immediately available. Simply removing the synchronous refill would have
copied zeroes from an empty FIFO and raised a premature completion interrupt.
Both sides of the handshake have to change together.

## Implementation

- Draining a sector leaves the CD FIFO empty until the existing double-speed
  sector timer expires (12.5 MHz / 150 sectors per second).
- DMA copies available bytes, retains its destination and remaining length
  across an empty-FIFO interval, and resumes from `Clio::tick()` when data
  arrives. Partially transferred code invalidates the ARM decode cache.
- The completion interrupt and transfer counter advance only after the whole
  request completes. Already-buffered short requests still complete immediately.
- Clearing/disabling the XBUS DMA channel cancels a waiting request.

This activates the existing inter-sector delay; it does not implement seek
latency, initial-sector latency, or a new drive-speed selection model.

## Evidence and validation

BIOS: `panafz10.bin`. Headless runs use the real game images, not patched guest
code. The existing FIFO and diagnostic changes in the working tree were kept.

| Scenario | Before | After |
|---|---|---|
| Twisted, idle input, 1,500 frames | 0 cels, DMA stuck at 605 | Continues past the stall |
| Twisted, idle input, 6,000 frames | Baseline stalls as above | 12,508 cels, 9,299 DMA completions, 1,714 distinct screens; live-action show intro visible |
| Phoenix 3 Europe, periodic Start/A, 6,000 frames | Stuck at 323 cels / 2,551 DMA completions from frame 2,000 onward | 72,384 cels, 6,263 DMA completions, 909 distinct screens; side-scrolling gameplay visible |
| Daedalus Disc 1, periodic Start/A, 6,000 frames | Not compared in this follow-up | Intro video visible; 568 distinct screens |
| Wing Commander III Disc 1, idle input, 6,000 frames | 1,189 distinct screens | Video visible; 1,125 distinct screens |

The Daedalus floodlight interaction and WC3 Hobbes audio scene still require
scene-specific verification. Intro playback is not proof that those particular
problems are fixed. The truncated local WC3 Disc 2 image remains a separate
issue; this change does not repair game images.

Additional loading regressions were checked with the same inputs: Road Rash
at 3,000 frames remains active (639 -> 580 screens); WC3's intro progresses
slightly less far at a fixed frame count, consistent with paced loading.
BattleSport takes longer to leave its startup sequence but reaches 269,329
cels / 2,529 screens at 6,000 frames. These are smoke checks, not complete
playthroughs or proof of audio synchronization.

257 host tests pass, including three added regression tests covering a
three-sector DMA request (unchanged destination bytes during each sector gap,
exact payload, no early/repeated completion), cancellation through both clear
registers, and immediate completion of a short buffered request.

To reproduce the two confirmed improvements with a built `gamecheck`:

```sh
gamecheck ~/3do-library/bios/panafz10.bin /path/to/library \
  --only Twisted --frames 6000 --shots /tmp/retro3do-fmv
gamecheck ~/3do-library/bios/panafz10.bin /path/to/library \
  --only 'Phoenix 3' --frames 6000 --press --shots /tmp/retro3do-fmv
```

## How the old reference pointed to the fix

Intermediate Opera revisions were built with a direct core harness, adapting
save-state signatures and missing includes. Some intermediate combinations
still cannot boot, so they are not usable FMV regression oracles.

At `fc55a85`, restoring only the timer-4 write adjustment removed in `dfa5034`
changes Twisted from black/stalled to continuing video and reads. The same
diagnostic adjustment makes Retro-3DO draw video. This is evidence that the
old Opera reference relied on a timing workaround, not that its timers were
identical to ours. Correcting the FIFO/DMA pacing fixes Retro-3DO without that
workaround. The earlier claims of a proven missing-message cause and one
proven common root for all four reported scenes were too strong.

Experimental harnesses, source snapshots, traces and screenshots from this
follow-up are in `/tmp/fmv-investigation/` (temporary). The shipping changes
and regression tests are in this repository; no runtime environment variables
or experimental timer overrides are required.

---

# Historical handover — superseded conclusions above

# Handover: FMV / DataStreamer titles hang (Twisted, Phoenix 3, Daedalus, WC3)

Status as of 2026-09-05. Author: investigation via headless traces + a git-bisect
of upstream Opera. This document is the durable summary; the blow-by-blow lab
notes are in `~/retro3do-work/hang_ORACLE.md` (sessions 1–8).

---

## 1. Symptoms reported

| Title | Reported symptom |
|---|---|
| Twisted: The Game Show (USA) | Does not work at all (black screen) |
| Phoenix 3 (Europe) | Never reaches gameplay |
| The Daedalus Encounter (USA) | Freezes/glitches when the probe floodlight is switched on |
| Wing Commander III (Hobbes scene) | Sound issues during the cutscene |

**All four are FMV / streamed-media scenes.** They share one root failure (below),
so they are one bug, not four.

## 2. One-line diagnosis

A **timing/ordering-sensitive deadlock in the 3DO OS DataStreamer**: the video
subscriber never receives its chunk messages, so it never consumes frames,
buffers never recycle, and the whole stream (audio+video+disc) deadlocks in a
three-way buffer-credit stall. This is a **real upstream Opera regression** that
our core faithfully reproduces — it is NOT unique to Retro-3DO.

## 3. The decisive evidence (git bisect of Opera itself)

Cloned `github.com/libretro/opera-libretro`, unshallowed (565 commits), and
bisected with a headless "does Twisted's intro keep playing past frame 1000"
oracle.

- **Last good:** `7fb1a3c` — plays Twisted (intro FMV, 530 distinct colors,
  sustained audio past frame 1500).
- **First bad:** an atomic 11-commit refactor batch dated **2026-05-31..06-01**
  (only its tip `fc55a85` compiles; the rest are transitional, so it could not be
  sub-bisected with the frontend used). Headline commits:
  - `9744216 cdrom: add ODE protocol and timed media reads` (+4833 lines)
  - `2df833f libopera: improve CLIO timer execution`
  - `522b0d4 libopera: centralize reset and frame scheduling`
  - `dfa5034 libopera: model CLIO and ARM reset side effects`
  - `0a90041 libopera: clean device reset and PBUS behavior`
  - `7ab1bc2 xbus: add polling and v2 savedata`
- **Current upstream `opera_libretro.so` wedges Twisted identically to us**
  (verified by driving the real `.so` on the real CHD). Our core matches current
  (broken) Opera; the pre-batch Opera is the working reference.

So: the bug was introduced by Opera's cdrom/timer/scheduling refactor. Whatever
that batch changed about device timing/ordering, our core reproduces it.

## 4. The precise failure mechanism (measured, well-bounded)

Traced headlessly on Twisted (BIOS `panafz10.bin`, the intro auto-plays; no input
needed):

1. Boot, BIOS POST, disc mount, stream setup: **byte-identical** between our core
   and the working old-Opera (same CreateItem/FindItem/OpenItem SWI counts).
2. The stream starts. Audio subscriber and video subscriber are both created.
   The DataStreamer delivers chunks to two subscriber channels — call them
   `0x94` (video) and `0x97` (audio).
3. **Audio works.** Its channel keeps flowing; audio is audibly correct for the
   full (short) intro audio track, then stops cleanly at end-of-track.
4. **Video never starts.** The video subscriber's message port (`0x2CF`) receives
   its chunks' delivery calls but **never gets a message**:
   - GetMsg success site `0xB5144`: **685 on old-Opera, 0 on ours**.
   - GetMsg failure site `0xB5140`: **21 on ours** (empty-port retries).
5. Video can't consume → its 4 buffers never recycle → DataAcq runs out of free
   buffers and stops issuing CD reads (frozen at 605 sectors) → the streamer
   daemon parks in `WaitSignal(8)` forever (last active ~frame 745, ~74.8% of a
   900-frame run). Three-way credit deadlock. **Zero cels are ever drawn.**

The chunk data itself is correct — verified byte-perfect in DRAM. The failure is
purely that the delivered video chunks never become messages on port `0x2CF`,
despite our core running the *same OS code with the same inputs* as the machine
that succeeds.

## 5. What has been DECISIVELY EXCLUDED (do not re-investigate)

Each excluded by direct measurement; several reversed an earlier working
hypothesis, so they are worth stating plainly:

- **CD sector timing** — `kSectorDelay` swept 8,000 → 2,000,000 cycles gives
  byte-identical results (same DMA count, same instruction count). The
  "EXINT burst" (695 vs old's 291 in one window) is a *symptom* of the deadlock
  (our core does the work then stalls; old keeps streaming), not a cause.
- **DSPP interpreter** — bit-exact against upstream Opera's `opera_dsp.c` for
  120,000 passes across Twisted's entire audio stream (differential harness,
  `hang_dspreplay.cpp`). Our DSPP is correct.
- **CLIO timers** — our 21 MHz-divider model matches pre-regression Opera; flag
  encodings identical. Timer-3 heartbeat fires healthily (~100/frame) — our FIQ
  profile matches the *working* Opera, not the broken one.
- **Interrupt delivery** — all XBUS DMA-complete interrupts delivered and cleared;
  IRQ enable masks match a healthy run; FIQ vs IRQ routing correct.
- **The audio subscriber path** — end-to-end correct (all folio calls return
  success; boundaries serviced on time; legitimate end-of-track stop).
- **The `FIXMODE` game-hack table** — 0 in the working machine too; irrelevant.
- **Instruction-level PC diff** of ours vs old — defeated by asynchronous
  FIQ-arrival jitter in OS wait-spin loops; not a usable technique here.

## 6. Suspected root cause (unconfirmed)

With CD timing, DSPP, timers, IRQ delivery and audio all excluded, and the failure
being "a message never arrives at a port despite identical code + inputs", the
cause is one of:

1. **A differing memory/hardware-read value** consumed inside the DataStreamer's
   video chunk→port delivery (an uninitialised-memory, NVRAM, or hardware-register
   read that returns a subtly different value on our core than on real hardware /
   old-Opera), OR
2. **A fine-grained interrupt-ordering race** — the relative ordering of a device
   FIQ vs a critical section in the folio, which the Opera batch perturbed and our
   core matches.

(2) is favoured because the bisect points at a *timing/scheduling* refactor, not a
data change, and because the failure is order-of-operations rather than wrong data
(the chunk bytes are perfect).

## 7. Two concrete ways to finish it

**Option A — sub-bisect the Opera batch (names the exact breaking change).**
The 11-commit batch could not be sub-bisected because the intermediate commits do
not build against the frontend used. Fix the frontend to build each transitional
commit (they change `opera_cdrom_set_callbacks`/timer signatures — provide shims),
then bisect within the batch. The single breaking commit's diff, read against our
corresponding subsystem, tells you exactly what timing/ordering to change.

**Option B — differential dump of port 0x2CF's queue (names the missing insert).**
Both our core and old-Opera (`bin/oldoracle2`, source-instrumented) are fully
instrumentable. Dump the message-port `0x2CF` internal structure (its message
linked-list head/tail) at the moment of the first video GetMsg on both machines.
Old has a message linked; ours does not. Then trace *backwards* from the insert
(the SWI/folio code that links a message into a port) to the branch that our core
skips — that branch's controlling read is the bug.

Option B is more direct and needs no build surgery; Option A is more definitive.

## 8. Code changes currently in the working tree (uncommitted)

Two **genuine behavioural fixes** (found via the upstream comparison; both improve
a real title — BattleSport gained ~15% cels / ~46% more distinct screens — and
neither fixes nor regresses the FMV bug):

- `src/core/madam.cpp` `fifo_output()`: the DSP **output-channel reload gate**
  tested input-channel enable bits (`1u << channel`) instead of output-channel
  bits (`1u << (channel + 16)`). Output channels could never loop. Fixed.
- `src/core/madam.cpp/.h` input FIFO: models the **8-word hardware FIFO** between
  the DMA engine and the DSPP (per WO 94/10641 — DMA position leads consumption),
  with FIFO-depth status read-back. `fill_input_fifo()` + `Fifo::staged[]`.

Plus **diagnostic instrumentation**, all behind `RETRO3DO_TRACING` or `getenv`
(zero effect on release builds — release build clean, **254 tests pass**):

- `src/core/arm60.cpp`: `SWITRACE` (one line per SWI: site, number, r0–r2),
  `RETWATCH` (log r0/r1/r2 at given return addresses), `DWATCH` (log 32-bit stores
  to a watched address with PC).
- `src/core/clio.*`: `g_clio_raise_bits[]` per-source interrupt-raise counters,
  `irq0_pending()/irq0_enabled()` accessors.
- `src/core/dsp.*`: `DSPSTEP` per-instruction trace, `peek_program/peek_raw_data/
  poke_raw_data/peek_pc` accessors.
- `src/core/madam.h`: `MadamStats` FIFO counters.

Decide whether to keep the diagnostics in-tree or strip them to just the two
fixes; they are useful for finishing this bug and are release-safe.

## 9. Tooling built (in `~/retro3do-work/`)

Reference machines and harnesses — **binaries in `~/retro3do-work/bin/`,
sources alongside as `hang_*.cpp` / in `~/retro3do-work/oldsrc`,`newsrc`**:

- `bin/oldoracle2` — **the working reference**: Opera `7fb1a3c` linked with a
  headless harness, plays Twisted. Env: `SWITRACE`, `DRAMDUMP`, `PCTRACE_OLD`,
  `PCTRACE_SKIP`, `FRAMEDUMP`, `READLOG`. Source: `~/retro3do-work/oldsrc/`.
- `bin/lrloader` — minimal libretro frontend; drives any `opera_libretro.so`.
  `OPT_opera_bios=panafz10.bin` required; `LRLOG=1`, `FRAMEDUMP=x.ppm`.
- `bin/uporacle` / `bin/neworacle2` — current-upstream Opera, direct-linked.
- `bin/tl` (`hang_timeline.cpp`) — our core: per-window cels/instructions/DMA +
  hot-PC + FIQ-raise histogram. The workhorse. Env: `NOPRESS`, `PCTRACE`,
  `SWITRACE`, `MADAMLOG`, `CLIOLOG`, `CLIOLOGRANGE`, `FIFOREADLOG`.
- `bin/dspreplay_up` (`hang_dspreplay.cpp`) — DSPP differential vs upstream.
- `bin/chd2iso`, `bin/dump`, `bin/execmap`, `bin/clockwatch`, `bin/bit16watch`,
  `bin/exintsrc`, `bin/dspsnap`, `bin/nmem` — targeted probes (see `hang_ORACLE.md`).
- `hang_clio_diff.py`, `hang_cmdseq.py` — trace comparators.
- `iso/twisted.iso`, `iso/roadrash.iso` — flat images (the harnesses read raw
  2048-byte sectors, not CHD).

### Reproduce the failure in ~1 minute

```sh
# ours (wedges: cels stay 0, dma freezes at 605)
~/retro3do-work/bin/tl ~/3do-library/bios/panafz10.bin \
  "/media/jon/NAS-Roms/RetroMedia/3do/Twisted - The Game Show (USA).chd" 1500 250

# working reference (plays: 530 colors, reads continue past LBA 286690)
cd /tmp && READLOG=1 FRAMEDUMP=/tmp/old.ppm \
  ~/retro3do-work/bin/oldoracle2 ~/3do-library/bios/panafz10.bin \
  ~/retro3do-work/iso/twisted.iso 1500
```

## 10. Notes / loose ends

- `~/3do-library/games/Wing Commander III ... (Disc 2).chd` is a **truncated copy**
  (71 MB vs 542 MB on the NAS) — re-pull it; not an emulator bug.
- Killing Time draws 0 cels headlessly on our core in the baseline too — a
  separate, pre-existing issue, unrelated to this one.
- The oracle build objects live in `/tmp/oldbuild2`,`/tmp/newbuild2`,`/tmp/upbuild`
  which are volatile; the *sources* are preserved in `~/retro3do-work/oldsrc` and
  `newsrc`. Rebuild recipe is in `hang_ORACLE.md`.
