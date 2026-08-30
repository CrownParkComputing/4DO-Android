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

#include <memory>
#include <vector>

#include "types.h"
#include "xbus.h"

namespace retro3do {

class Arm60;

// Sources on the first interrupt bank. Only the ones the machine currently
// generates are named; the rest of the bits exist but are never raised yet.
// Reset causes reported through CSTATBITS. The boot ROM reads this register
// eight instructions in, masks it with 0x43 and dispatches on the result, so it
// is the very first thing the machine is asked and it decides which of two
// entirely different boot paths runs.
//
// A cold power-on is 0x40. Reporting bit 0 instead sends the ROM down the
// soft-reset path, which boots to a picture and looks fine - and then never
// brings up the CD driver, because on a soft reset it is supposed to still be
// there from last time.
enum : u32 {
    kResetSoftware  = 1u << 0,
    kResetWatchdog  = 1u << 1,
    kResetPowerOn   = 1u << 6,   // cold start
};

// Interrupt sources in bank 0, from the community 3DOessence register map -
// independently derived documentation with clear provenance, and the first
// source here that states the assignments outright rather than leaving them to
// be inferred from one ROM's spin loops.
//
// Two earlier guesses were wrong in ways worth recording. The CD-ROM interrupt
// is EXINT at bit 2, which had been guessed first at bit 3 and then at bit 9;
// and bit 2 had been assumed to be "the timer", which is what made the wrong
// answer look self-consistent. The timers actually occupy bits 3..10, one per
// ODD timer, counting downwards from timer 15 at bit 3.
enum : u32 {
    kIrqVerticalBlank0   = 1u << 0,
    kIrqVerticalBlank1   = 1u << 1,

    // EXINT: interrupts from XBUS devices, the CD-ROM among them.
    kIrqExpansionBus     = 1u << 2,

    // Bits 3..10 are the timers. Only an odd-numbered timer can interrupt,
    // because timers chain in pairs and the high half of a pair is what
    // signals. Timer 15 is bit 3 and they descend from there.
    kIrqAudioTimer       = 1u << 11,

    // Bits 12..28 are audio DMA channels.
    kIrqXbusDmaComplete  = 1u << 29,

    // Bit 31 says the extended flags at kClioIrq1Pending are worth reading.
    kIrqSecondaryBank    = 1u << 31,
};

// The interrupt bit belonging to an odd-numbered timer. Even timers cannot
// interrupt at all and return zero.
constexpr u32 timer_interrupt_bit(unsigned timer) {
    return (timer % 2 == 1 && timer < 16) ? (1u << (3 + (15 - timer) / 2)) : 0u;
}

// Register offsets from the base of the CLIO window. Everything the chip
// decodes goes in this one place.
enum : u32 {
    // Reads a fixed hardware identifier - the OS asks what it is running on
    // before it does anything else, and a zero here is not a valid answer.
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

    // The line number occupies the low eleven bits. Bit 11 above it is the
    // FIELD flag - the ROM waits on it directly:
    //
    //   LDR r2, [r0, #0x34]
    //   TST r2, #0x800
    //   BEQ  -4              ; spin until the flag is set
    //   AND r2, r2, #0x7FF
    //   CMP r2, #4           ; then wait for line 4 of that field
    //
    // Masking it off, as an earlier version did, makes that wait never finish.
    kClioLineMask     = 0x07ff,
    kClioFieldFlag    = 0x0800,
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
    // A masked-write control register: to change any of the low four bits you
    // must also set that bit's write-enable in the next four. Clearing bit 0 is
    // a write of 0x10; setting it is 0x11. Writing the value alone does nothing
    // at all, which is a quiet way to lose every setting software makes here.
    //
    //   bit 0  PAL/NTSC selector
    //   bit 1  DSP sound output enable
    //   bit 2  ROM bank selector
    //
    // The boot ROM writes 0x22 - enable bit 1, set bit 1 - to turn on sound
    // output, and reads the register back afterwards.
    kClioControl      = 0x0084,
    kClioControlWriteEnableShift = 4,
    kClioControlMask  = 0x000f,
    kClioAdbctl       = 0x0088,   // TODO(clio): confirm

    // Timer bank: sixteen timers, each a counter followed by its reload value.
    kClioTimerBase    = 0x0100,
    kClioTimerDelay   = 0x0220,   // TIMERCTL: divider off the 21 MHz source
    kClioTimerCount   = 16,
    kClioTimerStride  = 8,

    // Timer configuration. NOT one bit per timer: each timer has a FOUR bit
    // configuration field, so the ports below cover eight timers each. The boot
    // ROM writes 0x00001000 to the first of them, which under a one-bit-per-
    // timer reading looks like "enable timer 12" - a timer it never programs,
    // whose counter is therefore zero, which then underflows on every tick. The
    // correct reading is timer 3, whose period the ROM does set.
    //
    // Writing ones to a Set port sets those configuration bits; writing ones to
    // the matching Clear port clears them.
    kClioTimerConfigSet0    = 0x0200,   // timers 0..7
    kClioTimerConfigClear0  = 0x0204,
    kClioTimerConfigSet8    = 0x0208,   // timers 8..15
    kClioTimerConfigClear8  = 0x020c,
    kClioTimerConfigBits    = 4,

    // The four configuration bits of a timer.
    kTimerDecrement = 0x1,   // counts at all
    kTimerReload    = 0x2,   // reloads on underflow rather than stopping
    kTimerCascade   = 0x4,   // only counts when the timer below it wraps

    // The expansion bus, through which the CD-ROM is reached. Its status
    // register carries a ready bit; the boot ROM polls it in a tight loop once
    // it has drawn its logo:
    //
    //   LDR r1, [r0]        ; r0 = 0x03400400
    //   TST r1, #0x80
    //   BEQ -4              ; spin until ready
    //
    // Returning zero here is what leaves a booted machine sitting on a static
    // logo: it has finished starting up and is waiting for the disc hardware to
    // answer.
    // The ROM's device enumeration, decoded from the loop it spins in:
    //
    //   ADD  r0, r0, #0x03400000
    //   ...
    //   STR  r1, [r0, #0x100]     ; command byte -> 0x03400500
    //   LDR  r0, [r0, #0x140]     ; status       <- 0x03400540
    //   AND  r0, r0, #0xFF
    //   TST  r0, #0x10            ; wait for the operation to complete
    //   BEQ  -5
    //
    // The expansion bus, as the boot ROM drives it. Established by logging the
    // traffic in order and reading it against WO 94/16382:
    //
    //   +0x0500  eighteen writes of zero at start-up, then single bytes.
    //            That opening burst is the patent's ID-assignment procedure -
    //            the system strobes seventeen times so each device can count
    //            its own address - so this is the bus strobe / select port.
    //   +0x0580  the Command FIFO. The ROM writes 0x83 followed by six more
    //            bytes: one multi-byte command, exactly as WR_COM describes.
    //   +0x0540  the result read.
    //
    // The patent defines the bus signals StatValid- and ChunkValid- as ACTIVE
    // LOW, but the ROM waits for bit 4 to be SET after issuing a command, so
    // CLIO presents them to software already inverted. Implementing the bus
    // polarity here instead would hang on every command.
    // The expansion bus. Four transaction windows, each 0x40 bytes wide, and
    // that width is the point: sixteen devices at four bytes each, so the low
    // address bits carry the DEVICE NUMBER. That is the SELECTION mechanism -
    // the bus patent describes selection at length but never says how the host
    // expresses it, and it turns out to be the address itself.
    //
    //   0x0400..0x04FF  XBUS DMA control
    //   0x0500..0x053F  SELECTION on write, reserved on read
    //   0x0540..0x057F  WR_POL on write, RD_POL on read
    //   0x0580..0x05BF  WR_COM on write, RD_STAT on read
    //   0x05C0..0x05FF  WR_DATA on write, RD_DATA on read
    kClioXbusStatus   = 0x0400,
    kClioXbusSelect   = 0x0500,
    kClioXbusPoll     = 0x0540,
    kClioXbusCommand  = 0x0580,
    kClioXbusData     = 0x05c0,
    kClioXbusWindow   = 0x0040,   // one window: sixteen devices, four bytes each

    // Poll register bits. The patent defines the underlying bus signals as
    // ACTIVE LOW; CLIO presents them to software inverted, so a set bit means
    // the FIFO has something. The boot ROM agrees - it issues a command and
    // waits for bit 4 to be SET.
    //
    // The low nibble is writable control, the high nibble read-only state. The
    // driver's own writes confirm the control bits: it reads the register,
    // masks to the low four bits, and ORs in 1, 2 or 4 at three different call
    // sites - the three interrupt enables below, in order.
    // From MAME's 3DO CLIO (BSD-3-Clause), which documents the register:
    //   ---- ---x  status interrupt enable
    //   ---- --x-  read interrupt enable
    //   ---- -x--  write interrupt enable
    //   ---- x---  reset
    //   ---x ----  status valid
    //   --x- ----  read valid
    //   -x-- ----  write valid
    //   x--- ----  media access, cleared by reading
    kXbusStatusIrqEnable = 0x0001,
    kXbusReadIrqEnable   = 0x0002,
    kXbusWriteIrqEnable  = 0x0004,
    kXbusReset           = 0x0008,
    kXbusStatusReady     = 0x0010,
    kXbusChunkReady      = 0x0020,
    kXbusWriteValid      = 0x0040,
    kXbusMediaAccess     = 0x0040,

    // Written to SELECTION as a device-count probe. Answering it wrongly makes
    // CLIO report "too many devices on the bus".
    // Address 0x0F is the bus itself rather than a device on it, and zero is
    // where the built-in drive sits.
    kXbusCdRomAddress    = 0x0000,
    kXbusSelectNone      = 0x000f,
    // Selecting anything other than the device-count probe leaves the bus-level
    // poll register flagged. From MAME's CLIO, which sets exactly this.
    kXbusPollUnfitted    = 0x0030,

    kXbusReady        = 0x0080,

    // Expansion-bus control. These are two INDEPENDENT registers, not a
    // set/clear pair - treating them as a pair makes every write to one of them
    // silently modify the other, and the driver reads back a value it never
    // wrote. 0x400 has one quirk: a write with bit 11 set is refused outright.
    kClioXbusCtl      = 0x0400,
    kClioXbusDirection = 0x0404,
    kXbusCtlWriteVeto = 0x0800,
    kClioXbusType0    = 0x0408,
    kClioXbusXferCount= 0x040c,

    // DIPIR - "disc inserted player interrupt request". How the machine learns
    // which device had media change under it.
    //
    //   bit 15  active
    //   bit 14  happened before reset
    //   bits 0..7  device number
    // DMA request enable and disable, per MAME's CLIO. The expansion-bus
    // transfer runs when request bit 20 is set AND bit 11 of the bus control
    // register is set; the address and length live in MADAM and mean nothing on
    // their own.
    kClioDmaRequestSet   = 0x0304,
    kClioDmaRequestClear = 0x0308,
    kClioDmaXbusBit      = 0x00100000,
    kXbusCtlDmaEnable    = 0x00000800,   // bit 11

    kClioDipir1       = 0x0410,
    kClioDipir2       = 0x0414,
    kDipirActive      = 0x8000,
    kDipirBeforeReset = 0x4000,

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
    u32  read_impl(u32 offset);
    void write(u32 offset, u32 value);
    void write_impl(u32 offset, u32 value);

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

    // Bring-up diagnostics: the last value written to each low register, and
    // whether it was ever written at all. Knowing WHICH registers a boot ROM
    // programs, and with what, is most of the work of finding out what it
    // expects - and guessing at that from behaviour alone is very slow.
    static constexpr u32 kTrackedRegisters = 2048;
    bool register_written(u32 offset) const;
    u32  register_last_write(u32 offset) const;

    // Reads are tracked too. Which ports software *interrogates*, and how often,
    // says as much about a protocol as which ones it writes - and a port that is
    // read but never written is usually the reply half of a transaction.
    u64  register_reads(u32 offset) const;

private:
    void note_write(u32 offset, u32 value);
    void note_read(u32 offset);
    void update_cpu_interrupt_line();
    void tick_timers(u32 cycles);
    void step_timers();
    u32  timer_config_at(u32 timer) const;

    Arm60& cpu_;

    // Bank 0 is the one wired to the CPU. Bank 1 raises a bit in bank 0 rather
    // than reaching the CPU directly, so a handler always reads bank 0 first.
    // Which enabled sources have already had an edge signalled. See
    // update_cpu_interrupt_line().
    u32 signalled_ = 0;

    // How many IRQs the CPU had taken last time we looked. CLIO clears the
    // sources it delivered once the CPU accepts them - see tick().
    u64 last_irqs_taken_ = 0;

    u32 irq0_pending_ = 0;
    u32 irq0_enabled_ = 0;
    u32 irq1_pending_ = 0;
    u32 irq1_enabled_ = 0;

    // The timer source, and the divider software programs through TIMERCTL.
    static constexpr u32 kTimerSourceHz = 21000000;
    static constexpr u32 kTimerCpuHz    = 12500000;
    static constexpr u32 kTimerDelayReset = 64;
    u32 timer_delay_ = kTimerDelayReset;
    static constexpr u64 kTimerFixedOne = 1u << 16;   // accumulator fraction
    u64 timer_accumulator_ = 0;
    u32 timer_counter_[kClioTimerCount] = {};
    u32 timer_reload_[kClioTimerCount]  = {};

    // Four configuration bits per timer, packed low timer first.
    u64 timer_config_ = 0;

    // The expansion bus selects a device by the VALUE written to SELECTION, not
    // by the address written to. `xbus_poll_` is the bus-level register used
    // when a device other than the built-in drive is selected; the drive keeps
    // its own control nibble in `xbus_device_poll_`.
    u32 control_ = 0;
    u32 xbus_sel_ = 0;
    u32 xbus_dma_enable_ = 0;
    u32 xbus_poll_ = 0;
    // The control nibble comes up with every bit SET, not clear. This is not a
    // detail: the driver reads the poll register before it writes one, and a
    // device whose control nibble reads back zero is taken for an empty slot -
    // so a zero here makes the machine scan all sixteen addresses and find
    // nothing, with the drive sitting right there answering commands.
    static constexpr u32 kXbusPollControlReset = 0x0fu;
    u32 xbus_device_poll_ = kXbusPollControlReset;
    u32 xbus_sel_modifier_ = 0;
    bool media_changed_ = false;
    bool xbus_dma_requested_ = false;
    void (*dma_handler_)(void*) = nullptr;
    void* dma_context_ = nullptr;
    u32 xbus_control_ = kXbusReady;
    u32 xbus_direction_ = 0;
    u32 xbus_type0_ = 0;
    u32 xbus_xfer_count_ = 0;
    // The boot ROM reads both DIPIR registers and tests them together: if the
    // pair is all-zero it decides the machine has no disc-change hardware and
    // takes a path that never brings the CD driver up at all. The second one
    // reads a fixed 0x4000, and that constant is the difference between a
    // machine that mounts a disc and one that boots to a logo and stops.
    static constexpr u32 kDipir2Value = 0x4000;
    u32 dipir1_ = 0;
    u32 dipir2_ = kDipir2Value;

    u32 vint0_line_ = 0;
    u32 vint1_line_ = 0;

    u32 scanline_ = 0;
    u32 pixel_in_line_ = 0;
    u32 scanlines_per_field_ = 263;

    bool field_complete_ = false;
    bool field_odd_ = false;
    bool irq_asserted_ = false;  // retained for reset bookkeeping

    static constexpr u32 kClioRevisionValue = 0x02020000;
    u32 revision_ = kClioRevisionValue;
    u32 random_state_ = 0xdeadbeefu;
    u32 mode_ = 0;
    u32 csys_bits_ = 0;
    u32 cstat_bits_ = 0;
    u32 watchdog_ = 0;
    u32 seed_ = 0;
    u32 timer_slack_ = 0;
    u32 written_value_[kTrackedRegisters] = {};
    bool written_flag_[kTrackedRegisters] = {};
    u64 read_count_[kTrackedRegisters] = {};

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

    // The expansion bus. The CD-ROM drive is built into the machine, so it is
    // always attached; whether a disc is in it is a separate question.
    CdRomDevice& cdrom() { return cdrom_; }

    // Which register 0x0540 actually is has not been settled from the ROM's
    // behaviour alone, and the two candidates behave differently. Selectable so
    // both can be tried against a real boot ROM rather than argued about.
    enum class XbusResultRegister { Poll, Status };
    void set_xbus_result_register(XbusResultRegister which) { xbus_result_ = which; }

private:
    u32 xbus_poll_for_device() const;
    void raise_xbus_interrupt_if_pending();

public:
    // Set when software pulls the expansion-bus DMA trigger; cleared once the
    // transfer has been served.
    bool xbus_dma_requested() const { return xbus_dma_requested_; }
    void clear_xbus_dma_request() { xbus_dma_requested_ = false; }

    // The expansion transfer runs INSIDE the store that triggers it, before the
    // CPU executes another instruction - not at the next convenient boundary.
    // The host writes the trigger and then looks at the result straight away.
    void set_xbus_dma_handler(void (*handler)(void*), void* context) {
        dma_handler_ = handler;
        dma_context_ = context;
    }
    void set_xbus_ready(bool ready) {
        if (ready) xbus_control_ |= kXbusReady; else xbus_control_ &= ~kXbusReady;
    }

private:


    CdRomDevice cdrom_;
    XbusResultRegister xbus_result_ = XbusResultRegister::Poll;
};

}  // namespace retro3do
