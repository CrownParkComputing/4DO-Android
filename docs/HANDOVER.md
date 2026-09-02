# Handover

State of Retro-3DO as of 2026-09-01, written for someone picking this up cold.

## Standing instructions from the user

These override defaults. Do not re-litigate them.

- **The reference/oracle is the user's own C++ rewrite of Opera**, at
  `~/StudioProjects/4DO-Android/app/cpp/native_core/`, copied to
  `~/retro3do-work/oracle/src/`. Reading it is sanctioned. If stuck, Opera
  itself is also permitted.
- **Provenance has now had a full retrospective.** `docs/PROVENANCE.md`
  distinguishes historical FreeDO/Opera exposure from the public authority and
  independently structured algorithm used by each current module. Do not call
  the history clean-room; do not call the current old-port routines unchanged.
- **Android must use SAF scoped-folder access only.** Never all-files access.
- **applicationId stays `com.fourdo.android`.** Do not change it.
- **No custom animations or polish** ahead of emulation correctness.
- **Emulation correctness before release plumbing.** No Play uploads, no
  release signing work, unless asked.
- **No guessing.** Every fix should come from the reference source or from a
  measurement, not from a hypothesis. The user has corrected this once already.
- The user does all device testing. Report BIOS/game status plainly.

## Where things stand

250 tests pass. All library floors pass. Two titles are **pixel-identical** to
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
| Need for Speed | 13,755 | 2,635 | road and terrain now render; targeted gameplay smoke passes |
| Road Rash | 171,932 | 853 | now boots into a live race; targeted gameplay smoke passes |
| Wing Commander III d1 | 304 | 4,673 | FMV plays cleanly; it is video, not cels |
| Wing Commander III d2 | — | — | will not open, bad dump |

Device: BIOS boots on the Retroid at 60/60 fps, ~4.8 ms/frame. APK installed as
`com.fourdo.android` v2.1.0 debug.

The scheduler now gives VDLP one horizontal-blank callback per scanline, based
on the official 3DO Graphics Programming Guide and display-list patent rather
than on Opera's implementation. The VDL root is latched at the field boundary;
pixels and CLUT state are captured as each line is reached. A short headless
smoke run passed for NFS and Road Rash, and on-device NFS gameplay remained
clean at ARM 150%, 59-60/60 fps and about 13.6 ms/frame with no audio-gap
warning.

Android presentation is vsynced. The emulator already runs on its own paced
thread, so this does not control machine speed; it prevents the UI thread from
submitting the same frame hundreds of times per second and visibly tearing
during lateral movement.

The launcher is now a fixed top-navigation layout. The A-Z/0-9 strip and search
stay fixed while only the card child can scroll; the card scrollbar is hidden
and direct finger swipes move the list. Settings contains the optional
RetroMedia integration: native registered-account login (Firebase or local,
according to `/api/auth/config`), an Android-Keystore-encrypted session, an
explicit artwork-type selector, and user-triggered downloads of one matching
piece per library game. Decoded covers are stored as bounded private RGBA cache
files and loaded lazily into SDL textures on the game cards. No password enters
`settings.cfg`, and no download starts automatically.

The VDLP implements 2x output expansion and SDL presents it with vsync, but
neither was the lateral-breakup fix. An exact Opera CCB/source comparison found
that a type-2 transparent packet in a rotated packed cel advanced only the
upper transformed edge. The lower edge stayed behind, so the next visible quad
crossed backwards through the sprite and created horizontal strips. Advancing
both edges fixes the Road Rash rider and the rotating/scrolled scenery in both
titles. The regression is
`a_transparent_run_advances_both_edges_of_a_rotated_packed_cel`.

The full pre-build module comparison and the deliberately remaining boundaries
are in [OPERA_AUDIT.md](OPERA_AUDIT.md).

The post-audit source retrospective replaced the former direct-port routines:
MADAM matrix math, arbitrary-quad rasterisation, winding/visibility, LR-form
addressing, DSP operand/branch/ALU structure, SPORT masking and ARM6 multiply
timing. It also found and fixed an inherited ARM timing boundary error: zero,
one and odd significant-bit ranges were overcharged. The current algorithms and
the few behavior-only fallbacks are catalogued in
[PROVENANCE.md](PROVENANCE.md).

## Resolved: Need for Speed's road

The missing road was a producer-side failure, not a CEL renderer failure. The
game's road CCBs contained valid texture pointers but zero positions and matrix
deltas. It selects a matrix driver from MADAM's revision register; the emulator
reported zero, so NFS selected the wrong result-port layout. MADAM now reports
the read-only Panasonic Green hardware ID `0x01020000`, matching the production
hardware and the sanctioned core.

With that ID, NFS produces non-degenerate road CCBs and renders the road,
terrain, signs and lane markings. A clean release run also verified that the
scene remains correct across pause/resume.

Road Rash had a separate mount failure: READ CAPACITY and READ DISC INFO added
the 150-frame CD lead-in twice. Reporting the physical lead-out with one bias
lets DIPIR accept the verified retail image and the title reaches a live race.

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

The Opera audit then fixed full CLIO/MADAM/XBUS reset state, read-only chip
revisions, ordinary CLIO register latches, DMA/type-0 readback, VDL video-DMA
blanking and VRAM wrapping, skipped-CCB state inheritance, and the rotated
packed-CEL transparent-run edge bug. Each has a focused test; see
[OPERA_AUDIT.md](OPERA_AUDIT.md).

**And the input path: `PadState` was written by the front end and read by
nothing.** The on-screen pad and every controller were connected to a dead end.
Multiple controllers now chain as separate 3DO pads, analogue sticks drive the
directions, triggers act as shoulders.

## Known-unfixed

- BattleSport is rejected after five header sectors. It remains separate from
  the Road Rash lead-out bug fixed above.
- Audio startup now primes the SDL stream with real samples before playback;
  the device overlay reports no startup gaps. Broader listening tests remain.
- Wing Commander III disc 2 will not open at all.
- Device testing outstanding: a game end-to-end, save persistence across a
  force-close, audio, controller mapping, sustained gameplay frame rate.

## Unrelated but time-critical, repeatedly flagged, never actioned

`~/StudioProjects/4DO-Android` is 6 commits ahead of `origin/main`, unpushed.
The signing keystore exists **only** as the GitHub Actions secret
`KEYSTORE_BASE64`; `backup-keystore.yml` has never been run and needs a
`BACKUP_PASSPHRASE` secret. If that repo or that secret is lost, the Play
listing cannot be updated again.
