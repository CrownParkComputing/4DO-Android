#include "clio.h"
#include <cstdlib>
#include <cstdio>

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

// The Poll Register, as the patent defines it. Both "valid" bits are ACTIVE
// LOW: high means the corresponding FIFO has nothing to offer.
u32 Clio::xbus_poll_for_device() const {
    // Control nibble as software left it, state nibble from the drive.
    u32 poll = xbus_device_poll_ & 0x0fu;
    if (!cdrom_.status_empty()) poll |= kXbusStatusReady;
    if (cdrom_.has_chunk())     poll |= kXbusChunkReady;
    if (media_changed_)         poll |= kXbusMediaAccess;
    return poll;
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
    timer_config_ = 0;

    vint0_line_ = 0;
    vint1_line_ = 0;
    scanline_ = 0;
    pixel_in_line_ = 0;
    field_complete_ = false;
    field_odd_ = false;
    irq_asserted_ = false;
    signalled_ = 0;
    last_irqs_taken_ = cpu_.irqs_taken();

    mode_ = 0;
    csys_bits_ = 0;

    // A reset is a power-on as far as the machine is concerned. Reporting no
    // cause at all is not a neutral default: the boot ROM tests this against a
    // fixed set of causes and hangs if it recognises none, so zero here means
    // the machine never boots.
    cstat_bits_ = kResetPowerOn;

    std::fill(dsp_window_.begin(), dsp_window_.end(), 0u);
    dsp_writes_ = 0;
    cdrom_.reset();
    watchdog_ = 0;
    seed_ = 0;
    timer_slack_ = 0;

    irq_asserted_ = false;
    signalled_ = 0;
    last_irqs_taken_ = cpu_.irqs_taken();
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
        irq0_pending_ |= kIrqSecondaryBank;
    }
    update_cpu_interrupt_line();
}

void Clio::update_cpu_interrupt_line() {
    // Signal an edge PER SOURCE, rather than once for the line as a whole.
    //
    // Holding the line does not work: the boot ROM enables vertical blank and
    // then never acknowledges it - across a whole run it writes no CLIO clear
    // port at all, because it drives the boot animation by polling the line
    // counter instead. A held line therefore livelocks the machine inside its
    // own vertical-blank handler before the OS finishes starting.
    //
    // But a single edge flag for the whole line is just as wrong, and fails
    // much later and more confusingly. It stays latched while ANY enabled
    // source is pending, so once vertical blank goes pending and stays pending,
    // no other source can ever produce an edge again. The expansion bus would
    // raise its completion interrupt, the bit would sit plainly pending and
    // enabled with interrupts unmasked at the CPU, and the CPU would still
    // never take it - which is exactly how the OS came to boot and then idle
    // forever waiting on a CD command that had in fact already completed.
    //
    // Tracking the signalled set per source gives each one its own edge.
    // CLIO drives the CPU's FIQ, not its IRQ.
    //
    // This is the thing that made the interrupt behaviour so hard to read. The
    // boot ROM installs handlers at BOTH vectors, so delivering to IRQ looks
    // like it works - a handler runs and returns. But it is the wrong handler:
    // it touches no CLIO register, acknowledges nothing, and does nothing
    // useful. Across an entire boot the machine never read the pending register
    // or wrote the clear port, which is impossible for an OS servicing its own
    // interrupts, and was the clue.
    //
    // Delivering to FIQ reaches the real service routine, and with it the
    // acknowledgement that makes a level-held line the correct model.
    const u32 active = irq0_pending_ & irq0_enabled_;
    cpu_.set_fiq(active != 0);
    signalled_ = active;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
void Clio::tick(u32 cycles) {
    cdrom_.tick(cycles);
    if (cdrom_.take_media_changed()) {
        media_changed_ = true;
    }
    if (cdrom_.take_interrupt_request()) {
        raise(kIrqExpansionBus);
    }


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
    if (timer_config_ == 0) {
        return;
    }

    u32 raised = 0;
    bool carry = false;   // did the previous (lower) timer underflow this tick?

    for (u32 i = 0; i < kClioTimerCount; ++i) {
        const u32 config = timer_config_at(i);
        const bool cascade = (config & kTimerCascade) != 0;
        const bool underflowed_below = carry;
        carry = false;

        if ((config & kTimerDecrement) == 0) {
            continue;
        }

        // A cascaded timer is the high half of a wider one: it only moves when
        // the timer below it wraps, which is what lets pairs form 32-bit
        // counters.
        u32 steps = cascade ? (underflowed_below ? 1u : 0u) : cycles;
        if (steps == 0) {
            continue;
        }

        if (timer_counter_[i] > steps) {
            timer_counter_[i] -= steps;
            continue;
        }

        // Underflowed. Reload if configured to, otherwise the timer is spent
        // and stops - firing a spent timer on every subsequent tick costs
        // hundreds of interrupts a frame and starves everything else.
        if ((config & kTimerReload) != 0) {
            timer_counter_[i] = timer_reload_[i];
        } else {
            timer_counter_[i] = 0;
            timer_config_ &= ~(u64{kTimerDecrement} << (i * kClioTimerConfigBits));
        }
        carry = true;
        raised |= timer_interrupt_bit(i);
    }

    if (raised != 0) {
        raise(raised);
    }
}

u32 Clio::timer_config_at(u32 timer) const {
    return static_cast<u32>(
        (timer_config_ >> (timer * kClioTimerConfigBits)) & 0xfu);
}

// ---------------------------------------------------------------------------
// Register interface
// ---------------------------------------------------------------------------
u32 Clio::read(u32 offset) {
    offset &= (kClioWindowSize - 1);
    note_read(offset);

    // Expansion bus windows.
    if (offset >= kClioXbusSelect && offset < kClioXbusData + kClioXbusWindow) {
        switch (offset & ~(kClioXbusWindow - 1)) {
            case kClioXbusSelect:
                return xbus_sel_;
            case kClioXbusPoll: {
                // Device zero is the built-in drive and answers with its own
                // register; anything else reads the bus-level one.
                if (xbus_sel_ != 0) {
                    return xbus_poll_;
                }
                const u32 poll = xbus_poll_for_device();
                media_changed_ = false;   // media-access is read-clear
                return poll;
            }
            case kClioXbusCommand: {            // RD_STAT
                if (xbus_sel_ != 0) {
                    return 0;
                }
                const u8 byte = cdrom_.read_status();
                if (cdrom_.take_interrupt_request()) {
                    raise(kIrqExpansionBus);
                }
                return byte;
            }
            case kClioXbusData:                 // RD_DATA
                return xbus_sel_ == 0 ? cdrom_.read_data() : 0;
            default:
                break;
        }
    }

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

        // Both ports of each pair read back the same value; they differ only in
        // what a WRITE does. Returning zero from the clear ports means software
        // that reads back its own mask through one of them sees nothing set.
        case kClioIrq0Pending:
        case kClioIrq0Clear:    return irq0_pending_;
        case kClioIrq0Enable:
        case kClioIrq0Disable:  return irq0_enabled_;
        case kClioIrq1Pending:
        case kClioIrq1Clear:    return irq1_pending_;
        case kClioIrq1Enable:
        case kClioIrq1Disable:  return irq1_enabled_;

        case kClioControl:     return control_;
        case kClioMode:        return mode_;

        case kClioXbusStatus:  return kXbusReady;
        case kClioBadBits:     return 0;
        case kClioTimerConfigSet0:
        case kClioTimerConfigClear0:
            return static_cast<u32>(timer_config_ & 0xffffffffu);
        case kClioTimerConfigSet8:
        case kClioTimerConfigClear8:
            return static_cast<u32>(timer_config_ >> 32);

        default:
            if (getenv("UNIMPL")) {
                static bool seen[2048/4] = {};
                if (offset/4 < 2048/4 && !seen[offset/4]) {
                    seen[offset/4] = true;
                    fprintf(stderr, "  UNIMPLEMENTED CLIO read +%04X\n", offset);
                }
            }
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

    if (offset >= kClioXbusSelect && offset < kClioXbusData + kClioXbusWindow) {
        switch (offset & ~(kClioXbusWindow - 1)) {
            case kClioXbusSelect:
                // SELECTION names the device by VALUE. 0x8F is a probe rather
                // than a device: answering it wrongly leaves CLIO reporting
                // "too many devices on the bus" and the enumeration fails.
                xbus_sel_ = value & 0xffu;
                if (xbus_sel_ == kXbusSelectProbe) {
                    xbus_poll_ &= 0x0fu;
                } else {
                    xbus_poll_ = (xbus_poll_ & 0x0fu) | 0x90u;
                }
                return;

            case kClioXbusPoll:
                // Only the control nibble is writable.
                if (xbus_sel_ == 0) {
                    xbus_device_poll_ = (value & 0x0fu) | (xbus_device_poll_ & 0xf0u);
                } else {
                    xbus_poll_ = value & 0xffu;
                }
                return;

            case kClioXbusCommand: {
                if (xbus_sel_ != 0) {
                    return;
                }
                const bool was_empty = cdrom_.status_empty();
                cdrom_.write_command(static_cast<u8>(value & 0xffu));
                if (was_empty && !cdrom_.status_empty()) {
                    raise(kIrqExpansionBus);
                }
                return;
            }
            default:
                return;
        }
    }


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

        case kClioControl: {
            // Only bits whose write-enable is set in the next nibble change.
            const u32 enable = (value >> kClioControlWriteEnableShift) &
                               kClioControlMask;
            control_ = (control_ & ~enable) | (value & enable & kClioControlMask);
            return;
        }
        case kClioTimerConfigSet0:
            timer_config_ |= value;
            return;
        case kClioTimerConfigClear0:
            timer_config_ &= ~u64{value};
            return;
        case kClioTimerConfigSet8:
            timer_config_ |= (u64{value} << 32);
            return;
        case kClioTimerConfigClear8:
            timer_config_ &= ~(u64{value} << 32);
            return;
        default:
            break;
    }
}

}  // namespace retro3do
