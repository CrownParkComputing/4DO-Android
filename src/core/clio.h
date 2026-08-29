// CLIO — the 3DO's I/O controller.
//
// CLIO owns the interrupt controller, the timer bank, the video line and pixel
// counters, and the register interface to the DSP and the expansion bus. It is
// the chip that lets the BIOS get anywhere at all: without interrupts and a
// running line counter, boot code spins forever waiting for a vertical blank
// that never arrives.
//
// Confidence, and how it is marked
// --------------------------------
// The interrupt-controller *semantics* below (paired set/clear registers,
// separate enable and disable ports, a second bank chained into the first) are
// the part this implementation is confident about. Individual register offsets
// are collected into one table so a correction is a one-line change; those not
// yet checked against the published hardware documentation are marked
// TODO(clio). Nothing here is derived from another emulator — see
// docs/CLEANROOM.md.
#pragma once

#include <vector>

#include "types.h"

namespace retro3do {

class Arm60;

// Sources on the first interrupt bank. Only the ones the machine currently
// generates are named; the rest of the bits exist but are never raised yet.
// Reset causes reported through CSTATBITS.
enum : u32 {
    kResetPowerOn = 1u << 0,   // cold start
    kResetSoftware = 1u << 1,  // TODO(clio): confirm
    kResetExternal = 1u << 6,  // TODO(clio): confirm
};

enum : u32 {
    kIrqVerticalBlank0 = 1u << 0,
    kIrqVerticalBlank1 = 1u << 1,
    kIrqTimer          = 1u << 2,   // TODO(clio): confirm the timer's bit
    kIrqExpansionBus   = 1u << 3,   // TODO(clio): confirm
};

// Register offsets from the base of the CLIO window. Everything the chip
// decodes goes in this one place.
enum : u32 {
    kClioRevision     = 0x0000,
    kClioCsysBits     = 0x0004,
    kClioVint0        = 0x0008,   // scanline that raises VBL0
    kClioVint1        = 0x000c,   // scanline that raises VBL1
    kClioAudioIn      = 0x0020,   // TODO(clio): confirm
    kClioAudioOut     = 0x0024,   // TODO(clio): confirm
    // CSTATBITS reports WHY the machine started. The BIOS reads it, masks with
    // 0x43 and branches on 1, 2 or 0x40; anything else and it gives up and
    // spins forever. Confirmed by disassembling the boot ROM, which is also
    // what confirmed the MADAM and CLIO base addresses below.
    kClioCstatBits    = 0x0028,
    kClioWatchdog     = 0x0030,

    // The current scanline, as an 11-bit value.
    //
    // Established from the boot ROM, which was previously mapped the other way
    // round here: it loads the literal 0x7FF as a mask, reads this register,
    // and waits for exact values (4, then 0x0A..0x0D), with 390, 478 and 394
    // sitting in the same literal pool. Those are line numbers in a 525-line
    // frame, not pixel positions.
    kClioVCount       = 0x0034,
    kClioHCount       = 0x0038,   // TODO(clio): position within the line
    kClioLineMask     = 0x07ff,
    kClioSeed         = 0x003c,

    // Interrupt bank 0. Reads give pending; writes set or clear.
    // TODO(clio): some published maps also place a random-number register at
    // 0x0040. If that is right, the two are distinguished by something this
    // implementation does not model yet, and reads here will need revisiting.
    kClioIrq0Pending  = 0x0040,
    kClioIrq0Clear    = 0x0044,
    kClioIrq0Enable   = 0x0048,
    kClioIrq0Disable  = 0x004c,

    kClioMode         = 0x0050,
    kClioBadBits      = 0x0054,
    kClioSpare        = 0x0058,

    // Interrupt bank 1, chained into bank 0.
    kClioIrq1Pending  = 0x0060,
    kClioIrq1Clear    = 0x0064,
    kClioIrq1Enable   = 0x0068,
    kClioIrq1Disable  = 0x006c,

    kClioHDelay       = 0x0080,   // TODO(clio): confirm
    kClioAdbio        = 0x0084,   // TODO(clio): confirm
    kClioAdbctl       = 0x0088,   // TODO(clio): confirm

    // Timer bank: sixteen timers, each a counter followed by its reload value.
    kClioTimerBase    = 0x0100,
    kClioTimerCount   = 16,
    kClioTimerStride  = 8,

    // Which timers are running. Paired set/clear ports, like the interrupt
    // registers — writing a one to the set port starts that timer, writing a
    // one to the clear port stops it.
    kClioTimerEnable  = 0x0200,   // TODO(clio): confirm
    kClioTimerDisable = 0x0204,   // TODO(clio): confirm

    // The DSP lives inside CLIO's window rather than having its own chip
    // select. The boot ROM uploads a program here, starts it, waits, and then
    // gives up - so the DSP is on the critical path to booting, not merely to
    // sound.
    //
    // Established by disassembling the ROM: at DRAM 0x0A64 and 0x0A80 it runs
    // two copy loops into 0x03403000 and 0x03403400, and its literal pool holds
    // 0x034017E8, 0x034017FC and 0x03401800 as control registers, alongside
    // values (0x9900C000, 0x9901C000, 0x83808000) that look like DSP
    // instructions.
    kClioDspBase      = 0x1000,
    kClioDspEnd       = 0x4000,

    kClioWindowSize   = 0x10000,
};

class Clio {
public:
    explicit Clio(Arm60& cpu);

    void reset();

    // Called with the number of CPU cycles that have just elapsed. Advances the
    // video counters and the timer bank, and raises interrupts as they fall due.
    void tick(u32 cycles);

    u32  read(u32 offset);
    void write(u32 offset, u32 value);

    // --- interrupt sources -------------------------------------------------
    void raise(u32 sources);          // bank 0
    void raise_secondary(u32 sources);  // bank 1

    // --- video timing ------------------------------------------------------
    void set_scanlines_per_field(u32 lines) { scanlines_per_field_ = lines; }
    u32  scanline() const { return scanline_; }

    // True while the machine is in vertical blank, which is what the display
    // side asks in order to know a field has finished.
    bool field_complete() const { return field_complete_; }
    void clear_field_complete() { field_complete_ = false; }

private:
    void update_cpu_interrupt_line();
    void tick_timers(u32 cycles);

    Arm60& cpu_;

    // Bank 0 is the one wired to the CPU. Bank 1 raises a bit in bank 0 rather
    // than reaching the CPU directly, so a handler always reads bank 0 first.
    u32 irq0_pending_ = 0;
    u32 irq0_enabled_ = 0;
    u32 irq1_pending_ = 0;
    u32 irq1_enabled_ = 0;

    u32 timer_counter_[kClioTimerCount] = {};
    u32 timer_reload_[kClioTimerCount]  = {};
    u32 timer_enabled_ = 0;

    u32 vint0_line_ = 0;
    u32 vint1_line_ = 0;

    u32 scanline_ = 0;
    u32 pixel_in_line_ = 0;
    u32 scanlines_per_field_ = 263;

    bool field_complete_ = false;

    u32 revision_ = 0;
    u32 mode_ = 0;
    u32 csys_bits_ = 0;
    u32 cstat_bits_ = 0;
    u32 watchdog_ = 0;
    u32 seed_ = 0;
    u32 timer_slack_ = 0;

    // The DSP is not emulated. Its window is backed by plain storage so that an
    // uploaded program is retained and can be read back and inspected, rather
    // than being silently dropped - which would make the upload itself
    // impossible to study.
    std::vector<u32> dsp_window_;
    u64 dsp_writes_ = 0;

public:
    // What the machine has written into the DSP window, for diagnostics.
    u64 dsp_writes() const { return dsp_writes_; }
    u32 dsp_word(u32 offset) const;
};

}  // namespace retro3do
