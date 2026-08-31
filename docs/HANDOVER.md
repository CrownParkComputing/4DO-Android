# Handover

State of Retro-3DO as of 2026-08-31, written for someone picking this up cold.

## Standing instructions from the user

These override defaults. Do not re-litigate them.

- **The reference/oracle is the user's own C++ rewrite of Opera**, at
  `~/StudioProjects/4DO-Android/app/cpp/native_core/`, copied to
  `~/retro3do-work/oracle/src/`. Reading it is sanctioned. If stuck, Opera
  itself is also permitted.
- **Provenance is settled and deferred.** `docs/PROVENANCE.md` states plainly
  that the hardware layer is derived from FreeDO/Opera. The user has said they
  will address the licensing later. Do not raise it again unasked.
- **Android must use SAF scoped-folder access only.** Never all-files access.
- **applicationId stays `com.fourdo.android`.** Do not change it.
- **No custom animations or polish** ahead of emulation correctness.
- **Emulation correctness before release plumbing.** No Play uploads, no
  release signing work, unless asked.
- **No guessing.** Every fix should come from the reference source or from a
  measurement, not from a hypothesis. The user has corrected this once already.
- The user does all device testing. Report BIOS/game status plainly.

## Where things stand

225 tests pass. All library floors pass. Two titles are **pixel-identical** to
the reference at frame 20,000: Killing Time and Alone in the Dark 2. Flashback
differs by 4.75%, which is animation phase.

Library at 20,000 frames (`tools/gamecheck/expectations.txt` holds the floors):

| Title | cels | screens | note |
|---|---|---|---|
| Alone in the Dark | 15,842 | 2,127 | past the save screen, into the game |
| Alone in the Dark 2 | 364,750 | 3,642 | pixel-identical to reference |
| Battle Chess | 5,153 | 774 | |
| BattleSport | 171,910 | 853 | BIOS shell — disc rejected, see below |
| Doom | 172,526 | 853 | bad dump, chdman rejects it too |
| Flashback | 104,548 | 2,083 | |
| Killing Time | 60,640 | 895 | pixel-identical to reference |
| PGA Tour 96 | 239,005 | 2,372 | |
| Need for Speed | 13,755 | 2,635 | **road missing in-game — see below** |
| Road Rash | 171,932 | 853 | BIOS shell — disc rejected |
| Wing Commander III d1 | 304 | 4,673 | FMV plays cleanly; it is video, not cels |
| Wing Commander III d2 | — | — | will not open, bad dump |

Device: BIOS boots on the Retroid at 60/60 fps, ~4.8 ms/frame. APK installed as
`com.fourdo.android` v2.1.0 debug.

## THE ACTIVE BUG: Need for Speed's road

**Symptom.** In-game, the cockpit, dials, rear-view mirror and HUD render
perfectly. Roadside sprites (buildings, trees, cars) draw. The road surface, sky
and terrain are missing — flat green, which is the clear colour showing through.

**What is known, all measured:**

- The reference draws this scene correctly. It is a genuine bug on our side.
- In 6,000 frames the reference walks **217,134 cels**; we walk **90,694**. We
  run the cel engine *more* often (6,754 vs 4,806) while walking far fewer cels.
- Cel-by-cel, the two machines are **identical for the first 9 cels and then
  diverge**. At cel #9 both read CCB `0D9FC0`; the reference sees flags
  `4FEE4430`, we see `5FE64430` — the value from the previous run. The game has
  rewritten that CCB and we are reading the stale version.
- The cel engine first runs somewhere between frame 400 and 800; cel #9 is at
  roughly frame 700–800. **The divergence window is small and traceable.**
- This is a **CPU-path divergence, not a rendering one.** Four separate,
  correct rendering fixes (deferred engine start, CCBPRE preamble, the rotated
  quad mapper, the matrix engine) did not move the divergence point at all.

**Next step, already scoped:** PC-trace diff from boot to find the first
instruction where the two CPUs disagree. Both sides have hooks:

- Reference: `PCTRACE=<file>` writes raw 4-byte PCs; `PCTRACESKIP=<n>` skips the
  first n. See `oracle/src/native_arm.c:1424`.
- Ours: **not yet implemented.** `Arm60::step` in `src/core/arm60.cpp` is where
  it goes; follow the `CELLOG` pattern in `madam.cpp` for an env-var-gated log.

Trace ~800 frames (roughly 48M instructions, ~190 MB) and find the first
differing PC. That instruction is the bug.

**Things already ruled out** — do not re-investigate:

- Cel engine port addresses (0x100/0x104/0x108/0x10c) match the reference.
- `pixel_offset` matches the reference's `XY2OFF` exactly.
- The winding cull is not eating the road (73 culled out of 224,081).
- The 1x1 zero-area cels drawing nothing is correct — the reference does the
  same with those CCBs.
- Register 0x28 (engine status) reads 0, which is what software wants.

## The oracle harness — the most useful thing built this session

`~/retro3do-work/oracle/harness_input` runs the reference emulator headless with
input and screenshots. Build:

```
cd ~/retro3do-work/oracle
g++ -O2 -I. -Isrc -c harness_input.cpp -o harness_input.o
g++ -O2 -o harness_input harness_input.o defs.o native_*.o
```

Run:

```
NVRAMIMAGE=~/retro3do-work/formatted_nvram.bin \
PRESSEVERY=180 SHOTEVERY=1000 SHOTPREFIX=/tmp/ref \
CELLOG=/tmp/ref_cels.txt \
./harness_input ~/retro3do-work/panafz10.bin ~/retro3do-work/nfs.bin 24000
```

`NVRAMIMAGE` is essential: without it the reference stops on "NVRAM Full" and
never reaches any game booted straight from a disc. `formatted_nvram.bin` is our
own formatted image.

**Caution when diffing cel logs:** the reference logs `pre0`/`pre1`/`pixc` at the
*top* of its loop, so those are the *previous* cel's registers. Only `address`
and `flags` line up directly.

## Our tools, in ~/retro3do-work

All link against `build/libretro3do_core.a` plus the four libchdr archives. Use
`TMPDIR=$HOME/retro3do-work/tmp` — **/tmp is a 16 GB tmpfs and is full**, and
LTO builds die on it. Other Claude sessions' scratchpads are the bulk of it;
leave them alone.

- `filmstrip <bios> <disc> <total> <every> <prefix> [press_every]` — screenshots
  through one run. `press_every` mashes A+P to walk menus.
- `lastframe <bios> <disc> <frames> <out.ppm>` — one frame.
- `cpi <bios> [disc] [frames]` — cycles per instruction.
- `quad` — prints what the rotated-cel mapper paints, as ASCII.

`CELLOG=<file>` on any of ours logs the cel walk, including write base, cull
decision, step vectors and the raw CCB words for the first three cels of a run.

Regression run (about 17 minutes):

```
./build/gamecheck ~/3do-library/bios/panafz10.bin ~/3do-library/games \
  --frames 20000 --expect tools/gamecheck/expectations.txt
```

Pixel comparison against reference frames (`or_kt.ppm`, `ref_aitd2.ppm`,
`or_fb.ppm` in `~/retro3do-work`) is the strongest check available — if Killing
Time or Alone in the Dark 2 moves off 0.00%, something broke.

## Fixed this session, for context

NVRAM comes up formatted (real hardware is never blank; this alone unblocked
Need for Speed and Alone in the Dark, and the *reference* still has this bug).
NVRAM persists to disk. "Bypass CLUT" is a per-pixel escape hatch selected by
bit 15, not a whole-frame bypass — this was Wing Commander's speckle. SPORT mask
bits protect rather than select. LR-form cels, whose source is stored in the
framebuffer's interleaved layout. The cel engine's four visibility tests. The
rotated-cel scanline fill. CCBPRE — the preamble comes from the source data when
the flag is clear. The cel engine starts *between* instructions, not inside the
store. MADAM's matrix unit, which did not exist at all and is why the 3D titles
were drawing degenerate geometry. ARM6 multiply timing (2-bit Booth, not the
ARM7 byte table) and exception cost.

**And the input path: `PadState` was written by the front end and read by
nothing.** The on-screen pad and every controller were connected to a dead end.
Multiple controllers now chain as separate 3DO pads, analogue sticks drive the
directions, triggers act as shoulders.

## Known-unfixed, besides the road

- Road Rash and BattleSport are rejected after five header sectors. Narrowed to
  the signature block; **the reference rejects them identically**, which points
  at the dumps.
- Audio gaps at startup (3–8 reported by the harness). Never investigated by
  ear; the user has not yet reported on audio quality.
- Wing Commander III disc 2 will not open at all.
- Device testing outstanding: a game end-to-end, save persistence across a
  force-close, audio, controller mapping, sustained gameplay frame rate.

## Unrelated but time-critical, repeatedly flagged, never actioned

`~/StudioProjects/4DO-Android` is 6 commits ahead of `origin/main`, unpushed.
The signing keystore exists **only** as the GitHub Actions secret
`KEYSTORE_BASE64`; `backup-keystore.yml` has never been run and needs a
`BACKUP_PASSPHRASE` secret. If that repo or that secret is lost, the Play
listing cannot be updated again.
