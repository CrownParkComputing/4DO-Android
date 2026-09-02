# Opera parity audit

Audit date: 2026-09-01.

## Post-audit source retrospective

After parity was established, every routine previously identified as a direct
algorithm port was revisited against ARM/3DO first-party material. The current
matrix, active-edge quad fill, winding/visibility, LR-form traversal, DSP
operand/branch/ALU path, SPORT mask and ARM6 timing implementations are now
project-structured algorithms. This does not erase historical reference
exposure; the detailed status and remaining behavior-only fallbacks are in
[PROVENANCE.md](PROVENANCE.md).

The retrospective also added scanline-latched VDLP operation, CEL
idle/in-process/suspended registers with exact `CURRENTCCB` fetch progression,
and corrected the inherited ARM6 Booth timing formula at zero/one and odd-bit
boundaries. The host suite now passes **246 tests**.

This is the pre-build comparison of Retro-3DO against the sanctioned Opera
reference in `~/retro3do-work/oracle/src`. It covers every hardware module and
records the remaining boundary explicitly. The comparison used Opera's
`native_3do`, `native_arm`, `native_mem`, `native_clio`, `native_dsp`,
`native_madam`, `native_sport`, `native_pbus`, `native_vdlp`, `native_xbus` and
`native_cdrom` implementations, plus paired Road Rash and Need for Speed
gameplay traces.

## Result for this build

No known title-critical Opera parity gap remains on the paths exercised by
Road Rash or The Need for Speed. Both titles use the same ten CD command
opcodes during the audited runs:

`02 09 10 80 82 83 85 8B 8C 8D`

All ten are implemented. Their command boundaries, status replies, data FIFO
and MADAM expansion-DMA path are active rather than bypassed.

The host suite at the initial audit passed **239 tests**. The final deterministic captures exercise
actual lateral motion:

- Road Rash: menu entry, acceleration, sustained right lean, wall collision,
  rider fall and recovery. The rider and background remain coherent.
- The Need for Speed: menu entry, acceleration, shift into gear and a sustained
  left turn from timer `00:01.6` through `00:28.2`. Road, terrain, scenery and
  cockpit remain coherent.

The visual breakup was not an SDL scaling or VDLP interpolation problem. An
exact CCB/source comparison found a packed type-2 transparent packet. The CEL
mapper advanced the upper transformed edge through the skipped pixels but left
the lower edge behind. The following visible quad then crossed back over the
sprite, producing horizontal strips whenever the cel rotated. Both transformed
edges now advance, matching Opera.

## Corrections made during the audit

### CLIO

- The silicon revision is fixed at `0x02020000` and ignores writes.
- Reset now clears the complete timer, interrupt, XBUS, DMA, poll and diagnostic
  state used by the implementation.
- DMA set and clear ports read back the shared enable mask.
- XBUS type-0 state reads back instead of returning zero.
- `AudioIn`, `AudioOut`, `Spare`, `HDelay` and `ADBCTL` now retain ordinary
  register writes, as Opera's backing register file does.

### MADAM and CEL

- MADAM's Green revision remains fixed and read-only.
- Reset clears DMA, XBUS, PBUS, CEL, matrix, FIFO, PLUT, inherited CCB state,
  counters and pending engine state.
- A skipped CCB still loads the CCB/PIXC/PRE/PLUT processor state for the next
  compact CCB; only rasterisation is skipped.
- Transparent packets in rotated packed cels advance both transformed edges.

### VDLP, console and XBUS

- A VDL entry with video DMA disabled produces black.
- VDL framebuffer addresses wrap on the fitted VRAM address lines.
- Console reset restores the selected PAL/NTSC MADAM geometry and clears its
  per-run sample and DMA counters.
- CD reset clears the pending sector delay and the last command bytes.

Every correction above has a focused regression test.

## Hardware-module comparison

| Retro-3DO module | Opera comparison | Current parity | Remaining boundary |
|---|---|---|---|
| `console.cpp` / scheduler | `native_3do.c` | ARM, CLIO and DSP clocks advance together; fields end on CLIO timing; VDLP is invoked at each horizontal blank | Optional ARM boost is intentionally outside Opera: it gives only the CPU more work while all hardware clocks stay native |
| `arm60.cpp` | `native_arm.c` | ARMv3 instruction classes used by current titles, modes, banking, flags, exceptions and measured ARM6 multiply/exception timing | No real prefetch/data-abort path for unmapped accesses; obscure unused ARM edge cases need differential tests; no CPU state serialization |
| `bus.cpp` | `native_mem.c` | Stock 2 MB DRAM, 1 MB VRAM, ROM overlay, big-endian accesses, NVRAM layout, device windows and write-watch | No second/Anvil ROM, diagnostic port, configurable/hires RAM banks or bus abort; unmapped reads return zero |
| `clio.cpp` | `native_clio.c` | Active interrupts, FIQ, timers, line/field count, DSP window, DMA and XBUS registers match; ordinary low latches and reset fixed in this audit | ADB serial behaviour and hardware not exercised by the two titles are not modelled; no state serialization |
| `dsp.cpp` | `native_dsp.c` | DSPP program/data maps, operands, ALU/control engine, semaphore, FIFO I/O, DAC words and audio FIQ are implemented | No state serialization; device audio quality and long-run FIFO timing still need broader listening/compatibility tests |
| `madam.cpp` | `native_madam.c` | CCB loading/inheritance, packed and unpacked/LR sources, PLUT, PIXC processing, visibility, arbitrary quad mapping, matrix, PBUS/XBUS/FIFO DMA match the active paths; CEL status/pause/resume/stop and exact fetch cursor are modelled | CEL retains the shared bus for one synchronous list after the triggering ARM instruction; external cycle-granular forced-yield timing, hires CEL mode and state serialization remain absent |
| `sport.cpp` | `native_sport.c` | Stock-VRAM page latch, copy, fill and protect-mask formula match | Opera's optional four-bank hires replication and state serialization are absent |
| `vdlp.cpp` | `native_vdlp.c` | Linked VDL, persist length, relative links, CLUT/background/bypass, modulo, ENVIDDMA and VRAM wrapping match; entries and pixels are sampled per scanline | Previous-buffer interpolation semantics are not implemented. Opera itself marks interpolation TODO. Retro-3DO's 2x presentation filter is therefore not used as an oracle claim |
| `pbus.cpp` / `pad.cpp` | `native_pbus.c` | Chained standard joypad packet and active-low wire layout match | Flight stick, mouse, light gun, arcade light gun, Orbatak trackball and other daisy-chain devices are absent |
| `xbus.cpp` | `native_xbus.c` / `native_cdrom.c` | Selection/poll/status/data FIFOs, interrupt handshake, active command replies, sector streaming and expansion DMA match the audited titles | Seek `01`, diagnostics `04`, reset `0A`, flush `0B`, pause/resume `0D`, CD audio `0E/0F`, mode sense `84`, header/subcode/identity queries `86`-`8A`, device driver `8E` and `93` are absent or incomplete. Several are also stubs in Opera |
| `disc.cpp` / `chd.cpp` | Opera callbacks/front end | Raw/cooked images, CUE data tracks and CHD data access cover more host formats than the hardware oracle | CD-DA track playback is not connected; compressed audio tracks named by CUE are deliberately rejected |

## Modules without an Opera hardware counterpart

`audio_ring`, `frame_mailbox`, `pad_layout`, `path`, `settings`, the SDL
platform layer and the Android/iOS shells are host infrastructure. They were
reviewed for reset and integration effects, but a source-level Opera parity
claim does not apply to them. Their own unit tests remain the authority.

## Priority after this build

1. Add the missing CD command/query and CD-DA surface as games require it.
2. Add complete save/load serialization across CPU, memory and every chip.
3. Add extra PBUS devices and optional hires/second-ROM hardware only when the
   compatibility target requires them.
4. Add cycle-granular CEL forced-yield arbitration if a title proves it can
   observe that boundary.

Those are compatibility expansion items, not explanations for the Road Rash or
Need for Speed lateral breakup; the deterministic active-motion captures close
that bug at the CEL packet mapper.
