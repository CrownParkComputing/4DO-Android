#include "clio.h"
#include "dsp.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

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
// The drive interrupts when a condition is both asserted AND enabled: status
// available with status interrupts on, or data available with data interrupts
// on. Checked when software touches the bus, which is when the hardware checks
// it - raising it continuously instead floods the handler.
void Clio::raise_xbus_interrupt_if_pending() {
    const u32 poll = xbus_poll_for_device();
    const bool status_pending = (poll & kXbusStatusReady) && (poll & kXbusStatusIrqEnable);
    const bool data_pending   = (poll & kXbusChunkReady)  && (poll & kXbusReadIrqEnable);
    if (status_pending || data_pending) {
        raise(kIrqExpansionBus);
    }
}

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
    timer_delay_ = kTimerDelayReset;
    timer_accumulator_ = 0;

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
    dipir1_ = 0;
    dipir2_ = kDipir2Value;

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
    // Bank 1 does not reach the CPU on its own. ANY pending source in it sets
    // bit 31 of bank 0, so a handler reads bank 0 first and only then looks at
    // bank 1. The enable mask does not gate this: a source that is pending but
    // masked still announces the bank, and it is the handler that decides
    // whether to act.
    if (irq1_pending_ != 0) {
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

// One timer tick. Every enabled timer moves by exactly one count, and the
// carry chain runs from timer 0 upwards: a cascaded timer moves only when the
// one below it wrapped on this same tick, which is what lets adjacent timers
// pair into a 32-bit counter.
//
// The carry starts SET, so a cascaded timer 0 - which has nothing below it -
// still counts. That is what the hardware does and software relies on it.
void Clio::step_timers() {
    u32 raised = 0;
    bool carry = true;

    for (u32 i = 0; i < kClioTimerCount; ++i) {
        const u32 config = timer_config_at(i);
        if ((config & kTimerDecrement) == 0) {
            carry = false;
            continue;
        }

        const bool cascade = (config & kTimerCascade) != 0;
        if (cascade && !carry) {
            carry = false;
            continue;
        }

        // Underflow is the wrap PAST zero, not the arrival at it. A counter
        // loaded with one still has a tick left in it; treating zero as spent
        // makes every delay one count short and, on a cascaded pair, drops a
        // whole carry.
        if (timer_counter_[i]-- != 0) {
            carry = false;
            continue;
        }

        if ((config & kTimerReload) != 0) {
            timer_counter_[i] = timer_reload_[i];
        } else {
            // A spent timer stops. Leaving it enabled costs an interrupt on
            // every subsequent tick and starves everything else.
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

void Clio::tick_timers(u32 cycles) {
    // Timers do not run at the CPU clock. They run off a fixed 21 MHz source
    // divided by the programmable delay in TIMERCTL, so at the stock divider a
    // timer moves roughly once every thirty-eight CPU cycles.
    //
    // Getting this wrong is not a small inaccuracy. The OS calibrates its
    // delays against these counters, so a timer running at the CPU clock makes
    // every timed wait expire about forty times too early - drive spin-up,
    // seek settling, the lot - and the machine gives up on hardware that was
    // about to answer.
    // The ratio is not a whole number of cycles - at the stock divider it is
    // 38.095 - so the accumulator carries a fractional part. Rounding it to 38
    // is a third of a percent fast, which is enough to leave a calibrated wait
    // one iteration short of where the same code lands on real hardware.
    const u64 divider = timer_delay_ != 0 ? timer_delay_ : 1;
    const u64 per_tick = (static_cast<u64>(kTimerCpuHz) * divider * kTimerFixedOne) /
                         kTimerSourceHz;
    const u64 cycles_per_tick = per_tick != 0 ? per_tick : 1;

    timer_accumulator_ += static_cast<u64>(cycles) * kTimerFixedOne;
    while (timer_accumulator_ >= cycles_per_tick) {
        timer_accumulator_ -= cycles_per_tick;
        if (timer_config_ != 0) {
            step_timers();
        }
    }
}

u32 Clio::timer_config_at(u32 timer) const {
    return static_cast<u32>(
        (timer_config_ >> (timer * kClioTimerConfigBits)) & 0xfu);
}

// ---------------------------------------------------------------------------
// Register interface
// ---------------------------------------------------------------------------
namespace {
// A register trace, off unless CLIOLOG names a file. Comparing this sequence
// against a known-good machine's is the only practical way to find where a
// driver stopped believing us.
#if RETRO3DO_TRACING
std::FILE* const g_clio_log = [] {
    const char* path = std::getenv("CLIOLOG");
    return path != nullptr ? std::fopen(path, "w") : nullptr;
}();
// A busy delay loop can fill the whole log with one register, so the range of
// interest can be narrowed: CLIOLOGRANGE=400-600 keeps only the expansion bus.
const u32 g_clio_log_low = [] {
    const char* range = std::getenv("CLIOLOGRANGE");
    return range != nullptr ? static_cast<u32>(std::strtoul(range, nullptr, 16)) : 0u;
}();
const u32 g_clio_log_high = [] {
    const char* range = std::getenv("CLIOLOGRANGE");
    const char* dash = range != nullptr ? std::strchr(range, '-') : nullptr;
    return dash != nullptr ? static_cast<u32>(std::strtoul(dash + 1, nullptr, 16)) : 0x1000u;
}();
const long g_clio_log_limit = [] {
    const char* limit = std::getenv("CLIOLOGMAX");
    return limit != nullptr ? std::strtol(limit, nullptr, 10) : 200000L;
}();
long g_clio_log_count = 0;
#endif
void log_access(char kind, u32 offset, u32 value, u32 pc) {
#if !RETRO3DO_TRACING
    (void)kind; (void)offset; (void)value; (void)pc;
#else
    std::FILE* file = g_clio_log;
    if (file == nullptr || g_clio_log_count >= g_clio_log_limit) {
        return;
    }
    const u32 window = offset & 0xffffu;
    if (window < g_clio_log_low || window >= g_clio_log_high) {
        return;
    }
    std::fprintf(file, "%c %04X %08X %08X\n", kind, offset & 0xffffu, value, pc);
    ++g_clio_log_count;
#endif
}
}  // namespace

u32 Clio::read(u32 offset) {
    const u32 result = read_impl(offset);
    log_access('R', offset, result, cpu_.pc());
    return result;
}

u32 Clio::read_impl(u32 offset) {
    offset &= (kClioWindowSize - 1);
    note_read(offset);

    // Expansion bus windows.
    if (offset >= kClioXbusSelect && offset < kClioXbusData + kClioXbusWindow) {
        switch (offset & ~(kClioXbusWindow - 1)) {
            case kClioXbusSelect:
                return xbus_sel_;
            case kClioXbusPoll: {
                // An address with nothing on it does not read back as zero -
                // it reads 0x30, both state bits set. Reading zero would mean
                // "a device that never has anything ready", which software
                // cannot tell apart from a device that is merely slow.
                u32 poll = kXbusPollUnfitted;
                if (xbus_sel_ == kXbusSelectNone) {
                    poll = xbus_poll_;
                } else if (xbus_sel_ == kXbusCdRomAddress) {
                    poll = xbus_poll_for_device();
                    media_changed_ = false;   // media-access is read-clear
                }
                // Bit 7 of the selection asks for the control nibble alone.
                if ((xbus_sel_modifier_ & 0x80u) != 0) {
                    poll &= 0x0fu;
                }
                return poll;
            }
            case kClioXbusCommand: {            // RD_STAT
                if (xbus_sel_ != kXbusCdRomAddress) {
                    return 0;
                }
                const u8 byte = cdrom_.read_status();
                if (cdrom_.take_interrupt_request()) {
                    raise(kIrqExpansionBus);
                }
                return byte;
            }
            case kClioXbusData:                 // RD_DATA
                return xbus_sel_ == kXbusCdRomAddress ? cdrom_.read_data() : 0;
            default:
                break;
        }
    }

    if (dsp_ != nullptr) {
        if (offset >= kClioDspRead2 && offset < kClioDspRead1) {
            u32 address = ((offset - kClioDspRead2) >> 1) & 0xff;
            address += 0x300;
            return (static_cast<u32>(dsp_->read_data(static_cast<u16>(address))) << 16) |
                   dsp_->read_data(static_cast<u16>(address + 1));
        }
        if (offset >= kClioDspRead1 && offset < kClioDspEnd) {
            u32 address = ((offset - kClioDspRead1) >> 2) & 0xff;
            address += 0x300;
            return dsp_->read_data(static_cast<u16>(address));
        }
        if (offset == kClioDspSemaphore) {
            return dsp_->read_semaphore();
        }
    }
    if (offset == kClioDspNoise) {
        // A hardware noise source, like the one at 0x3C.
        random_state_ += 0x9e3779b9u;
        u32 z = random_state_;
        z = (z ^ (z >> 16)) * 0x85ebca6bu;
        z = (z ^ (z >> 13)) * 0xc2b2ae35u;
        return z ^ (z >> 16);
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
        case kClioTimerDelay:  return timer_delay_;
        case kClioWatchdog:    return watchdog_;
        case kClioVCount:
            return (scanline_ & kClioLineMask) |
                   (field_odd_ ? kClioFieldFlag : 0u);
        case kClioHCount:      return pixel_in_line_;
        // A hardware noise source, not a register that reads back what was
        // written. Returning a constant is not a small inaccuracy to software
        // that uses it to choose between equally valid paths - it makes the
        // machine take the same branch for ever.
        case kClioSeed: {
            u32 z = (random_state_ += 0x9e3779b9u);
            z = (z ^ (z >> 16)) * 0x85ebca6bu;
            z = (z ^ (z >> 13)) * 0xc2b2ae35u;
            return z ^ (z >> 16);
        }

        // Both ports of each pair read back the same value; they differ only in
        // what a WRITE does. Returning zero from the clear ports means software
        // that reads back its own mask through one of them sees nothing set.
        case kClioIrq0Pending:
        case kClioIrq0Clear:    return irq0_pending_;
        // Bit 31 of the first bank's enable mask always reads SET. It is not
        // an enable at all - it is the chip saying the second bank exists and
        // is worth reading. Returning only what software wrote leaves the OS
        // believing there is no second bank, so every source that lives there
        // is silently never serviced.
        case kClioIrq0Enable:
        case kClioIrq0Disable:  return irq0_enabled_ | kIrqSecondaryBank;
        case kClioIrq1Pending:
        case kClioIrq1Clear:    return irq1_pending_;
        case kClioIrq1Enable:
        case kClioIrq1Disable:  return irq1_enabled_;

        case kClioControl:     return control_;
        case kClioMode:        return mode_;

        case kClioXbusCtl:       return xbus_control_;
        case kClioXbusDirection: return xbus_direction_;
        case kClioXbusXferCount: return xbus_xfer_count_;
        case kClioDipir1:        return dipir1_;
        case kClioDipir2:        return dipir2_;
        case kClioBadBits:     return 0;
        case kClioTimerConfigSet0:
        case kClioTimerConfigClear0:
            return static_cast<u32>(timer_config_ & 0xffffffffu);
        case kClioTimerConfigSet8:
        case kClioTimerConfigClear8:
            return static_cast<u32>(timer_config_ >> 32);

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
    log_access('W', offset, value, cpu_.pc());
    write_impl(offset, value);
}

void Clio::write_impl(u32 offset, u32 value) {
    offset &= (kClioWindowSize - 1);
    note_write(offset, value);

    if (offset >= kClioXbusSelect && offset < kClioXbusData + kClioXbusWindow) {
        switch (offset & ~(kClioXbusWindow - 1)) {
            case kClioXbusSelect:
                // SELECTION is two nibbles doing two different jobs. The LOW
                // nibble names the device - only sixteen are addressable, so
                // the high bits are not part of the address at all. The HIGH
                // nibble carries modifiers, of which only bit 7 is used: it
                // asks for the control nibble alone, which is how software
                // probes a slot without disturbing the state bits.
                //
                // Masking the whole byte instead of the low nibble makes every
                // modified selection look like a different device, and the
                // drive - which lives at address zero - stops answering the
                // moment software probes it.
                xbus_sel_ = value & 0x0fu;
                xbus_sel_modifier_ = value & 0xf0u;
                return;

            case kClioXbusPoll:
                // Only the control nibble is writable, and it is written to
                // whichever device is selected. Address 0x0F is not a device
                // but the bus's own register.
                if (xbus_sel_ == kXbusSelectNone) {
                    xbus_poll_ = (xbus_poll_ & 0xf0u) | (value & 0x0fu);
                } else if (xbus_sel_ == kXbusCdRomAddress) {
                    xbus_device_poll_ = (xbus_device_poll_ & 0xf0u) | (value & 0x0fu);
                    raise_xbus_interrupt_if_pending();
                }
                return;

            case kClioXbusCommand: {
                if (xbus_sel_ != kXbusCdRomAddress) {
                    return;
                }
                const bool was_empty = cdrom_.status_empty();
                cdrom_.write_command(static_cast<u8>(value & 0xffu));
                (void)was_empty;
                raise_xbus_interrupt_if_pending();
                return;
            }
            default:
                return;
        }
    }


    if (dsp_ != nullptr && offset >= kClioDspProgram2 && offset < kClioDspEnd) {
        if (offset < kClioDspProgram1) {
            // Two program words per store. The 0x400 bit is a mirror.
            const u32 address = ((offset & ~0x400u) - kClioDspProgram2) >> 1;
            dsp_->write_program(static_cast<u16>(address),
                                static_cast<u16>(value >> 16));
            dsp_->write_program(static_cast<u16>(address + 1),
                                static_cast<u16>(value));
            return;
        }
        if (offset < 0x3000) {
            // One program word per store. The 0x800 bit is a mirror.
            const u32 address = ((offset & ~0x800u) - kClioDspProgram1) >> 2;
            dsp_->write_program(static_cast<u16>(address),
                                static_cast<u16>(value));
            return;
        }
        if (offset < kClioDspData1) {
            const u32 address = ((offset - kClioDspData2) >> 1) & 0xff;
            dsp_->write_data(static_cast<u16>(address),
                             static_cast<u16>(value >> 16));
            dsp_->write_data(static_cast<u16>(address + 1),
                             static_cast<u16>(value));
            return;
        }
        if (offset < kClioDspRead2) {
            const u32 address = ((offset - kClioDspData1) >> 2) & 0xff;
            dsp_->write_data(static_cast<u16>(address), static_cast<u16>(value));
            return;
        }
        return;
    }
    if (dsp_ != nullptr) {
        switch (offset) {
            case kClioDspSemaphore:
                dsp_->write_semaphore(value);
                return;
            case kClioDspReset:
                dsp_->reset();
                return;
            case kClioDspRun:
                // Starting and stopping the DSP is a single register, and a
                // stopped DSP never reaches the instruction that raises the
                // audio interrupt - so the machine above it stops too.
                dsp_->set_running(value > 0);
                return;
            default:
                break;
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
        case kClioTimerDelay: timer_delay_ = value & 0x3ffu; break;

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

        case kClioFifoClear:
            // Stop and clear whichever channels the mask names. The bits are
            // the same ones that enable them.
            xbus_dma_enable_ &= ~value;
            if (channel_handler_ != nullptr) {
                channel_handler_(channel_context_, xbus_dma_enable_, value);
            }
            return;

        case kClioDmaRequestSet:
        case kClioDmaRequestClear:
            if (offset == kClioDmaRequestSet) {
                xbus_dma_enable_ |= value;
            } else {
                xbus_dma_enable_ &= ~value;
            }
            if (channel_handler_ != nullptr) {
                channel_handler_(channel_context_, xbus_dma_enable_, 0);
            }
            // The request bit alone starts the transfer. There is no second
            // enable to check - gating on one in the bus-control register
            // throws away transfers the driver has already committed to.
            if ((xbus_dma_enable_ & kClioDmaXbusBit) != 0) {
                xbus_dma_requested_ = true;
                if (dma_handler_ != nullptr) {
                    dma_handler_(dma_context_);
                }
            }
            return;

        case kClioXbusCtl:
            // A write carrying bit 11 is refused, value and all.
            if ((value & kXbusCtlWriteVeto) == 0) {
                xbus_control_ = value;
            }
            return;
        case kClioXbusDirection:
            xbus_direction_ = value;
            return;
        case kClioXbusType0:
            xbus_type0_ = value;
            return;
        case kClioXbusXferCount:
            xbus_xfer_count_ = value;
            return;

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
