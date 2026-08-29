#include "clio.h"

#include <algorithm>

#include "arm60.h"

namespace retro3do {
namespace {

// The ARM60 runs at 12.5 MHz. A field is 263 lines at roughly 60 Hz for NTSC,
// which puts a scanline at about 792 CPU cycles. Deriving it rather than
// hardcoding it keeps PAL correct when the region changes the line count.
constexpr u32 kCpuHz = 12500000;
constexpr u32 kFieldsPerSecondNtsc = 60;

u32 cycles_per_scanline(u32 scanlines_per_field) {
    if (scanlines_per_field == 0) {
        return 1;
    }
    return kCpuHz / (kFieldsPerSecondNtsc * scanlines_per_field);
}

// Index of a timer from a register offset inside the timer bank, or -1.
int timer_index(u32 offset, bool& is_reload) {
    if (offset < kClioTimerBase) return -1;
    const u32 relative = offset - kClioTimerBase;
    const u32 span = kClioTimerCount * kClioTimerStride;
    if (relative >= span) return -1;

    is_reload = (relative % kClioTimerStride) >= 4;
    return static_cast<int>(relative / kClioTimerStride);
}

}  // namespace

Clio::Clio(Arm60& cpu) : cpu_(cpu) {
    dsp_window_.assign((kClioDspEnd - kClioDspBase) / 4, 0);
    reset();
}

u32 Clio::dsp_word(u32 offset) const {
    if (offset < kClioDspBase || offset >= kClioDspEnd) return 0;
    return dsp_window_[(offset - kClioDspBase) / 4];
}

void Clio::reset() {
    irq0_pending_ = 0;
    irq0_enabled_ = 0;
    irq1_pending_ = 0;
    irq1_enabled_ = 0;

    for (u32 i = 0; i < kClioTimerCount; ++i) {
        timer_counter_[i] = 0;
        timer_reload_[i] = 0;
    }
    timer_enabled_ = 0;

    vint0_line_ = 0;
    vint1_line_ = 0;
    scanline_ = 0;
    pixel_in_line_ = 0;
    field_complete_ = false;
    field_odd_ = false;
    irq_asserted_ = false;

    mode_ = 0;
    csys_bits_ = 0;

    // A reset is a power-on as far as the machine is concerned. Reporting no
    // cause at all is not a neutral default: the boot ROM tests this against a
    // fixed set of causes and hangs if it recognises none, so zero here means
    // the machine never boots.
    cstat_bits_ = kResetPowerOn;

    std::fill(dsp_window_.begin(), dsp_window_.end(), 0u);
    dsp_writes_ = 0;
    watchdog_ = 0;
    seed_ = 0;
    timer_slack_ = 0;

    irq_asserted_ = false;
    cpu_.set_irq(false);
}

// ---------------------------------------------------------------------------
// Interrupt plumbing
// ---------------------------------------------------------------------------
void Clio::raise(u32 sources) {
    irq0_pending_ |= sources;
    update_cpu_interrupt_line();
}

void Clio::raise_secondary(u32 sources) {
    irq1_pending_ |= sources;
    // Bank 1 does not reach the CPU on its own. It sets a bit in bank 0, so a
    // handler always reads bank 0 first and only then looks at bank 1. Which
    // bit that is has not been confirmed.
    if ((irq1_pending_ & irq1_enabled_) != 0) {
        irq0_pending_ |= kIrqExpansionBus;  // TODO(clio): confirm the chain bit
    }
    update_cpu_interrupt_line();
}

void Clio::update_cpu_interrupt_line() {
    const bool asserted = (irq0_pending_ & irq0_enabled_) != 0;

    // Signal on the RISING EDGE only, rather than holding the line.
    //
    // The boot ROM settles this. Its vertical-blank handler reads a software
    // flag in RAM, returns, and never writes any CLIO register to acknowledge
    // the source - across a whole run it touches only seven CLIO registers,
    // none of them an acknowledgement. With a held line the machine livelocks
    // on its own startup interrupt: enter handler, return, re-enter, forever,
    // and the boot animation never advances.
    if (asserted && !irq_asserted_) {
        cpu_.signal_irq();
    }
    irq_asserted_ = asserted;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
void Clio::tick(u32 cycles) {
    tick_timers(cycles);

    const u32 per_line = cycles_per_scanline(scanlines_per_field_);
    pixel_in_line_ += cycles;

    while (pixel_in_line_ >= per_line) {
        pixel_in_line_ -= per_line;
        ++scanline_;

        if (scanline_ == vint0_line_) {
            raise(kIrqVerticalBlank0);
        }
        if (scanline_ == vint1_line_) {
            raise(kIrqVerticalBlank1);
        }

        if (scanline_ >= scanlines_per_field_) {
            scanline_ = 0;
            field_complete_ = true;
            // Interlaced: the two fields alternate, and the ROM waits on this
            // flag to synchronise with a particular one.
            field_odd_ = !field_odd_;
        }
    }
}

void Clio::tick_timers(u32 cycles) {
    if (timer_enabled_ == 0) {
        return;
    }

    bool fired = false;
    for (u32 i = 0; i < kClioTimerCount; ++i) {
        if ((timer_enabled_ & (1u << i)) == 0) {
            continue;
        }
        // TODO(clio): the real decrement rate is derived from a prescaler this
        // does not model yet, so timers currently run at the CPU clock. That is
        // the right shape but the wrong speed, and anything depending on an
        // absolute timer period will be wrong until the prescaler is known.
        if (timer_counter_[i] > cycles) {
            timer_counter_[i] -= cycles;
            continue;
        }
        timer_counter_[i] = timer_reload_[i];
        fired = true;
    }

    if (fired) {
        raise(kIrqTimer);
    }
}

// ---------------------------------------------------------------------------
// Register interface
// ---------------------------------------------------------------------------
u32 Clio::read(u32 offset) {
    offset &= (kClioWindowSize - 1);
    note_read(offset);

    if (offset >= kClioDspBase && offset < kClioDspEnd) {
        return dsp_window_[(offset - kClioDspBase) / 4];
    }

    bool is_reload = false;
    const int timer = timer_index(offset, is_reload);
    if (timer >= 0) {
        return is_reload ? timer_reload_[timer] : timer_counter_[timer];
    }

    switch (offset) {
        case kClioRevision:    return revision_;
        case kClioCsysBits:    return csys_bits_;
        case kClioVint0:       return vint0_line_;
        case kClioVint1:       return vint1_line_;
        case kClioCstatBits:   return cstat_bits_;
        case kClioWatchdog:    return watchdog_;
        case kClioVCount:
            return (scanline_ & kClioLineMask) |
                   (field_odd_ ? kClioFieldFlag : 0u);
        case kClioHCount:      return pixel_in_line_;
        case kClioSeed:        return seed_;

        case kClioIrq0Pending: return irq0_pending_;
        case kClioIrq0Enable:  return irq0_enabled_;
        case kClioIrq1Pending: return irq1_pending_;
        case kClioIrq1Enable:  return irq1_enabled_;

        case kClioMode:        return mode_;

        // An empty bus that answers.
        //
        // The machine must run its attract sequence with no disc in the drive,
        // so "nothing attached" cannot mean "never replies" - that is a hang,
        // not an empty drive. The bus reports itself ready, and every command
        // reports itself complete, so the ROM's enumeration finishes and finds
        // no devices rather than waiting forever for one.
        case kClioXbusStatus:  return kXbusReady;
        case kClioXbusResult:  return kXbusComplete;
        case kClioBadBits:     return 0;
        case kClioTimerEnable: return timer_enabled_;

        default:
            return 0;
    }
}


bool Clio::register_written(u32 offset) const {
    const u32 index = offset / 4;
    return index < kTrackedRegisters && written_flag_[index];
}

u32 Clio::register_last_write(u32 offset) const {
    const u32 index = offset / 4;
    return index < kTrackedRegisters ? written_value_[index] : 0;
}

u64 Clio::register_reads(u32 offset) const {
    const u32 index = offset / 4;
    return index < kTrackedRegisters ? read_count_[index] : 0;
}

void Clio::note_read(u32 offset) {
    const u32 index = offset / 4;
    if (index < kTrackedRegisters) {
        ++read_count_[index];
    }
}

void Clio::note_write(u32 offset, u32 value) {
    const u32 index = offset / 4;
    if (index < kTrackedRegisters) {
        written_value_[index] = value;
        written_flag_[index] = true;
    }
}

void Clio::write(u32 offset, u32 value) {
    offset &= (kClioWindowSize - 1);
    note_write(offset, value);

    if (offset >= kClioDspBase && offset < kClioDspEnd) {
        dsp_window_[(offset - kClioDspBase) / 4] = value;
        ++dsp_writes_;
        return;
    }

    bool is_reload = false;
    const int timer = timer_index(offset, is_reload);
    if (timer >= 0) {
        if (is_reload) {
            timer_reload_[timer] = value;
        } else {
            timer_counter_[timer] = value;
        }
        return;
    }

    switch (offset) {
        case kClioRevision:  revision_ = value; break;
        case kClioCsysBits:  csys_bits_ = value; break;
        case kClioVint0:     vint0_line_ = value; break;
        case kClioVint1:     vint1_line_ = value; break;
        case kClioWatchdog:  watchdog_ = value; break;
        case kClioSeed:      seed_ = value; break;
        case kClioMode:      mode_ = value; break;
        // The ROM clears this once it has read the reset cause, so the cause is
        // reported once rather than latching forever.
        case kClioCstatBits: cstat_bits_ = value; break;

        // The interrupt registers come in set/clear pairs rather than being
        // read-modify-written. A handler acknowledges by writing the bits it
        // handled to the clear port, which is race-free against a source that
        // fires while the handler is running.
        case kClioIrq0Pending:
            irq0_pending_ |= value;
            update_cpu_interrupt_line();
            break;
        case kClioIrq0Clear:
            irq0_pending_ &= ~value;
            update_cpu_interrupt_line();
            break;
        case kClioIrq0Enable:
            irq0_enabled_ |= value;
            update_cpu_interrupt_line();
            break;
        case kClioIrq0Disable:
            irq0_enabled_ &= ~value;
            update_cpu_interrupt_line();
            break;

        case kClioIrq1Pending:
            irq1_pending_ |= value;
            update_cpu_interrupt_line();
            break;
        case kClioIrq1Clear:
            irq1_pending_ &= ~value;
            update_cpu_interrupt_line();
            break;
        case kClioIrq1Enable:
            irq1_enabled_ |= value;
            update_cpu_interrupt_line();
            break;
        case kClioIrq1Disable:
            irq1_enabled_ &= ~value;
            update_cpu_interrupt_line();
            break;

        case kClioTimerEnable:
            timer_enabled_ |= value;
            break;
        case kClioTimerDisable:
            timer_enabled_ &= ~value;
            break;

        default:
            break;
    }
}

}  // namespace retro3do
