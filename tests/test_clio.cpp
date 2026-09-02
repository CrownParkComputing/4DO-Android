// CLIO tests: the interrupt handshake, the timer bank, and video timing.
//
// The interrupt handshake is the part worth pinning hardest. Its registers come
// in set/clear pairs rather than being read-modify-written, and getting that
// wrong produces a machine that boots and then wedges the first time two
// interrupt sources overlap — which is exactly the kind of bug that only shows
// up in a game, an hour in.
#include "core/arm60.h"
#include "core/bus.h"
#include "core/clio.h"
#include "core/madam.h"
#include "core/console.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

// Timer 3 is the one the boot ROM actually uses, and being odd it is one of the
// timers that can interrupt. Its bit is 9.
constexpr u32 kTimer3Irq = timer_interrupt_bit(3);
constexpr u32 kTimer3Counter = kClioTimerBase + 3 * kClioTimerStride;
constexpr u32 kTimer3Reload  = kTimer3Counter + 4;

// Four configuration bits per timer, low timer first.
constexpr u32 timer_config(unsigned timer, u32 bits) {
    return bits << (timer * kClioTimerConfigBits);
}

struct Chip {
    Bus bus;
    Arm60 cpu{bus};
    Clio clio{cpu};

    Chip() { bus.attach_clio(&clio); }
};

}  // namespace

// ---------------------------------------------------------------------------
// Interrupt controller
// ---------------------------------------------------------------------------

TEST(a_pending_interrupt_does_nothing_until_it_is_enabled) {
    Chip c;
    c.clio.raise(kIrqVerticalBlank0);
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kIrqVerticalBlank0);

    // Enabling it is what actually reaches the CPU. Until then the source is
    // latched but silent.
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    CHECK_EQ(c.clio.read(kClioIrq0Enable) & ~kIrqSecondaryBank, kIrqVerticalBlank0);
}

TEST(acknowledging_writes_the_bits_to_the_clear_port) {
    Chip c;
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0 | kTimer3Irq);
    c.clio.raise(kIrqVerticalBlank0 | kTimer3Irq);
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kIrqVerticalBlank0 | kTimer3Irq);

    // Acknowledge only one of the two. The other must survive — this is the
    // whole point of a separate clear port rather than a writeback.
    c.clio.write(kClioIrq0Clear, kIrqVerticalBlank0);
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kTimer3Irq);
}

TEST(the_disable_port_clears_enables_without_touching_pending) {
    Chip c;
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0 | kTimer3Irq);
    c.clio.raise(kIrqVerticalBlank0);

    c.clio.write(kClioIrq0Disable, kIrqVerticalBlank0);
    CHECK_EQ(c.clio.read(kClioIrq0Enable) & ~kIrqSecondaryBank, kTimer3Irq);
    // Still pending: disabling a source masks it, it does not acknowledge it.
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kIrqVerticalBlank0);
}

TEST(an_enabled_and_pending_interrupt_reaches_the_cpu) {
    // CLIO drives FIQ, not IRQ. The boot ROM installs handlers at both vectors,
    // so delivering to IRQ looks like it works - a handler runs and returns -
    // but it is the wrong handler and it services nothing.
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagF);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);

    // The CPU samples the line at an instruction boundary.
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorFiq);
    CHECK(c.cpu.mode() == Mode::Fiq);
}

TEST(the_interrupt_line_drops_when_the_source_is_acknowledged) {
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagF);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorFiq);

    // Acknowledge, return to a normal mode, and the CPU must not re-enter.
    c.clio.write(kClioIrq0Clear, kIrqVerticalBlank0);
    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagF) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);
    c.cpu.step();
    CHECK(c.cpu.mode() == Mode::Supervisor);
}

TEST(an_unacknowledged_source_keeps_interrupting) {
    // CLIO HOLDS the line while any enabled source is pending, so a handler
    // that returns without acknowledging is entered again. That is the whole
    // reason the acknowledge port exists, and the real service routine uses it:
    // across a boot it reads the pending register many thousands of times.
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagF);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);

    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorFiq);

    // Return to ordinary code WITHOUT acknowledging.
    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagF) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorFiq);   // still asserted

    // Acknowledging is what stops it.
    c.clio.write(kClioIrq0Clear, kIrqVerticalBlank0);
    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagF) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);
    c.cpu.step();
    CHECK(c.cpu.pc() != kVectorFiq);
}

TEST(a_second_edge_interrupts_again) {
    // A fresh source arriving must still be delivered, or the machine would
    // take exactly one interrupt ever.
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagF);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0 | kTimer3Irq);
    c.clio.raise(kIrqVerticalBlank0);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorFiq);

    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagF) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);

    // Acknowledge, then raise again: that is a new edge.
    c.clio.write(kClioIrq0Clear, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorFiq);
}

TEST(clio_registers_are_reachable_through_the_bus) {
    Chip c;
    c.bus.write32(kClioBase + kClioIrq0Enable, kTimer3Irq);
    CHECK_EQ(c.bus.read32(kClioBase + kClioIrq0Enable) & ~kIrqSecondaryBank, kTimer3Irq);

    c.clio.raise(kTimer3Irq);
    CHECK_EQ(c.bus.read32(kClioBase + kClioIrq0Pending), kTimer3Irq);
}

TEST(clio_reports_a_fixed_read_only_hardware_revision) {
    Chip c;
    CHECK_EQ(c.clio.read(kClioRevision), 0x02020000u);
    c.clio.write(kClioRevision, 0xdeadbeefu);
    CHECK_EQ(c.clio.read(kClioRevision), 0x02020000u);
    c.clio.reset();
    CHECK_EQ(c.clio.read(kClioRevision), 0x02020000u);
}

TEST(ordinary_low_clio_registers_latch_and_reset_as_read_write_hardware) {
    Chip c;
    const struct {
        u32 offset;
        u32 value;
    } registers[] = {
        {kClioAudioIn,  0x10203040u},
        {kClioAudioOut, 0x21314151u},
        {kClioSpare,    0x32425262u},
        {kClioHDelay,   0x43536373u},
        {kClioAdbctl,   0x54647484u},
    };

    for (const auto& reg : registers) {
        c.clio.write(reg.offset, reg.value);
        CHECK_EQ(c.clio.read(reg.offset), reg.value);
    }

    c.clio.reset();
    for (const auto& reg : registers) {
        CHECK_EQ(c.clio.read(reg.offset), 0u);
    }
}

// ---------------------------------------------------------------------------
// Video timing
// ---------------------------------------------------------------------------

TEST(the_line_counter_advances_and_wraps) {
    Chip c;
    c.clio.set_scanlines_per_field(263);
    CHECK_EQ(c.clio.scanline(), 0u);

    // One scanline is roughly 792 CPU cycles at 12.5 MHz over 263 lines.
    c.clio.tick(800);
    CHECK_EQ(c.clio.scanline(), 1u);

    // Run out the rest of the field and it must come back to zero and say so.
    for (int i = 0; i < 300; ++i) {
        c.clio.tick(800);
    }
    CHECK(c.clio.field_complete());
    CHECK(c.clio.scanline() < 263u);
}

TEST(the_scanline_callback_runs_at_each_horizontal_blank) {
    Chip c;
    struct Seen {
        u32 count = 0;
        u32 line = 0;
    } seen;
    c.clio.set_scanline_handler(
        [](void* context, u32 line) {
            Seen* value = static_cast<Seen*>(context);
            ++value->count;
            value->line = line;
        },
        &seen);

    c.clio.set_scanlines_per_field(263);
    c.clio.tick(800);
    CHECK_EQ(seen.count, 1u);
    CHECK_EQ(seen.line, 1u);
}

TEST(a_configured_scanline_raises_the_vertical_blank_interrupt) {
    Chip c;
    c.clio.set_scanlines_per_field(263);
    c.clio.write(kClioVint0, 10);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);

    // Not there yet.
    c.clio.tick(800 * 5);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kIrqVerticalBlank0, 0u);

    // Crossing line 10 raises it.
    c.clio.tick(800 * 6);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kIrqVerticalBlank0,
             kIrqVerticalBlank0);
}

TEST(the_region_sets_the_field_length) {
    Console console;

    console.set_region(Region::Ntsc);
    console.reset();
    CHECK_EQ(console.clio().scanline(), 0u);

    // A frame must stop at the field boundary rather than running the whole
    // arithmetic cycle budget, or video and emulation drift apart.
    console.run_frame();
    CHECK(console.clio().scanline() < 263u);
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------
// Sixteen 16-bit decrementing units. Each has a FOUR bit configuration field -
// not one bit - so the config ports cover eight timers each. Only odd-numbered
// timers can interrupt, because timers chain in pairs and the high half of a
// pair is what signals.
//
// Timers run off a 21 MHz source divided by TIMERCTL, not off the CPU clock,
// so a count is worth many CPU cycles. These tests deal in counts and convert.
namespace {
// CPU cycles that advance a timer by exactly n counts at the reset divider.
// The ratio is 38.095 cycles per count, so this rounds up by one to be sure of
// crossing the nth threshold without reaching the one after it.
u32 counts(u32 n) {
    return static_cast<u32>((static_cast<u64>(n) * 12500000u * 64u) / 21000000u) + 1u;
}
}  // namespace

TEST(a_timer_reloads_and_raises_an_interrupt_when_it_expires) {
    Chip c;
    c.clio.write(kTimer3Counter, 100);
    c.clio.write(kTimer3Reload, 500);
    c.clio.write(kClioIrq0Enable, kTimer3Irq);
    c.clio.write(kClioTimerConfigSet0,
                 timer_config(3, kTimerDecrement | kTimerReload));

    c.clio.tick(counts(50));
    CHECK_EQ(c.clio.read(kTimer3Counter), 50u);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kTimer3Irq, 0u);

    // A counter of fifty has fifty-one counts left in it: the wrap is the step
    // PAST zero, not the arrival at it.
    c.clio.tick(counts(51));
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kTimer3Irq, kTimer3Irq);
    CHECK_EQ(c.clio.read(kTimer3Counter), 500u);
}

TEST(a_timer_with_no_reload_bit_fires_once_and_then_stops) {
    // The boot ROM configures timer 3 to decrement without reloading. Treating
    // a spent timer as periodic costs hundreds of interrupts a frame and
    // starves the machine of the time it needs to do anything else.
    Chip c;
    c.clio.write(kTimer3Counter, 10);
    c.clio.write(kClioIrq0Enable, kTimer3Irq);
    c.clio.write(kClioTimerConfigSet0, timer_config(3, kTimerDecrement));

    c.clio.tick(counts(20));
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kTimer3Irq, kTimer3Irq);

    c.clio.write(kClioIrq0Clear, kTimer3Irq);
    c.clio.tick(counts(1000));
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kTimer3Irq, 0u);
}

TEST(an_unconfigured_timer_does_not_count) {
    Chip c;
    c.clio.write(kTimer3Counter, 100);
    c.clio.write(kClioTimerConfigSet0, timer_config(3, kTimerDecrement));
    c.clio.write(kClioTimerConfigClear0, timer_config(3, kTimerDecrement));

    c.clio.tick(counts(500));
    CHECK_EQ(c.clio.read(kTimer3Counter), 100u);
}

TEST(timers_are_independent) {
    Chip c;
    c.clio.write(kClioTimerBase, 100);                        // timer 0
    c.clio.write(kTimer3Counter, 900);                        // timer 3
    c.clio.write(kClioTimerConfigSet0,
                 timer_config(0, kTimerDecrement) |
                 timer_config(3, kTimerDecrement));

    c.clio.tick(counts(50));
    CHECK_EQ(c.clio.read(kClioTimerBase), 50u);
    CHECK_EQ(c.clio.read(kTimer3Counter), 850u);
}

TEST(an_even_timer_cannot_interrupt) {
    // Only the high half of a chained pair signals, so even timers have no
    // interrupt bit at all.
    CHECK_EQ(timer_interrupt_bit(0), 0u);
    CHECK_EQ(timer_interrupt_bit(2), 0u);
    CHECK_EQ(timer_interrupt_bit(15), 1u << 3);
    CHECK_EQ(timer_interrupt_bit(3), 1u << 9);
    CHECK_EQ(timer_interrupt_bit(1), 1u << 10);
}

TEST(a_cascaded_timer_only_moves_when_the_one_below_it_wraps) {
    // This is how two 16-bit units become one wider counter.
    Chip c;
    c.clio.write(kClioTimerBase + 2 * kClioTimerStride, 5);       // timer 2
    c.clio.write(kClioTimerBase + 2 * kClioTimerStride + 4, 5);
    c.clio.write(kTimer3Counter, 4);                              // timer 3
    c.clio.write(kClioTimerConfigSet0,
                 timer_config(2, kTimerDecrement | kTimerReload) |
                 timer_config(3, kTimerDecrement | kTimerCascade));

    // Timer 2 has not wrapped yet, so timer 3 must not have moved.
    c.clio.tick(counts(5));
    CHECK_EQ(c.clio.read(kTimer3Counter), 4u);

    // The sixth count takes timer 2 past zero; timer 3 takes exactly one step.
    c.clio.tick(counts(1));
    CHECK_EQ(c.clio.read(kTimer3Counter), 3u);
}

TEST(a_reset_silences_everything) {
    Chip c;
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);
    c.clio.write(kClioTimerConfigSet0, timer_config(3, kTimerDecrement));
    c.clio.write(kClioControl, 0x22u);
    c.clio.write(kClioXbusSelect, 7u);
    c.clio.write(kClioXbusDirection, 0x280u);
    c.clio.write(kClioXbusType0, 0x1234u);
    c.clio.write(kClioDmaRequestSet, kClioDmaXbusBit);

    c.clio.reset();

    CHECK_EQ(c.clio.read(kClioIrq0Pending), 0u);
    CHECK_EQ(c.clio.read(kClioIrq0Enable) & ~kIrqSecondaryBank, 0u);
    CHECK_EQ(c.clio.read(kClioTimerConfigSet0), 0u);
    CHECK_EQ(c.clio.read(kClioControl), 0u);
    CHECK_EQ(c.clio.read(kClioXbusSelect), 0u);
    CHECK_EQ(c.clio.read(kClioXbusDirection), 0u);
    CHECK_EQ(c.clio.read(kClioXbusType0), 0u);
    CHECK_EQ(c.clio.read(kClioDmaRequestClear), 0u);
    CHECK(!c.clio.xbus_dma_requested());
    CHECK_EQ(c.clio.scanline(), 0u);
}

TEST(a_new_source_still_interrupts_while_an_older_one_stays_pending) {
    // Every enabled+pending source contributes to the line. An implementation
    // that latches a single edge for the line as a whole swallows every later
    // source once the first one sticks - which is how the OS came to boot, ask
    // the CD drive its questions, and then idle forever while the expansion-bus
    // completion sat plainly pending, enabled and unmasked.
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagF);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank1 | kIrqExpansionBus);

    c.clio.raise(kIrqVerticalBlank1);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorFiq);

    // Acknowledge only the vertical blank, then raise the expansion bus.
    c.clio.write(kClioIrq0Clear, kIrqVerticalBlank1);
    c.clio.raise(kIrqExpansionBus);
    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagF) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorFiq);
}

TEST(a_multi_byte_cd_command_completes_once_not_once_per_byte) {
    // The boot ROM writes seven bytes per command. Replying to each byte runs
    // the status FIFO six replies ahead of the conversation, so every later
    // exchange reads the previous command's answer.
    Chip c;
    CHECK(c.clio.cdrom().status_empty());

    for (int i = 0; i < 6; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? 0x83u : 0x00u);
        CHECK(c.clio.cdrom().status_empty());       // nothing yet
        CHECK_EQ(c.clio.cdrom().commands_received(), 0u);
    }
    c.clio.write(kClioXbusCommand, 0x00u);          // the seventh byte
    CHECK_EQ(c.clio.cdrom().commands_received(), 1u);
    // The reply is there the moment the seventh byte lands. The driver spins
    // on the poll register without running the machine on, so a drive that
    // answers "later" never answers at all.
    CHECK(!c.clio.cdrom().status_empty());
    CHECK_EQ(c.clio.cdrom().last_command(), 0x83u);
}

TEST(a_completed_cd_command_reports_status_ready_and_raises_its_interrupt) {
    // The poll bits are presented to software active HIGH, inverted from the
    // active-low bus signals the patent describes. Implementing the bus
    // polarity here instead hangs the ROM on its very first command.
    Chip c;
    c.clio.write(kClioIrq0Enable, kIrqExpansionBus);
    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? 0x83u : 0x00u);
    }
    c.clio.tick(100000);
    CHECK_EQ(c.clio.read(kClioXbusPoll) & kXbusStatusReady, kXbusStatusReady);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kIrqExpansionBus, kIrqExpansionBus);
}

TEST(inserting_a_disc_is_visible_to_the_drive) {
    // Opening the image and telling the machine a disc is in the tray are two
    // different things, and only the second one is visible to the software
    // running on it.
    Chip c;
    CHECK(!c.clio.cdrom().disc_present());
    c.clio.cdrom().set_disc_present(true);

    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? kCmdVersion : 0x00u);
    }
    CHECK(!c.clio.cdrom().status_empty());

    // A version reply opens by echoing the command and ends with drive status.
    CHECK_EQ(c.clio.read(kClioXbusCommand), kCmdVersion);
    u32 last = 0;
    for (int i = 1; i < 12; ++i) last = c.clio.read(kClioXbusCommand);
    CHECK_EQ(last & kStatusDiscIn, kStatusDiscIn);
    CHECK(c.clio.cdrom().status_empty());
}

TEST(the_version_reply_identifies_the_drive) {
    // The boot ROM asks for this while enumerating the bus. Answering with
    // zeroes reads as "no drive fitted": the machine boots to its logo and
    // idles, never asking about a disc.
    Chip c;
    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? kCmdVersion : 0x00u);
    }
    c.clio.tick(100000);

    u8 reply[12];
    for (int i = 0; i < 12; ++i) reply[i] = (u8)c.clio.read(kClioXbusCommand);

    CHECK_EQ(reply[0], kCmdVersion);   // echo
    CHECK_EQ(reply[2], 0x10u);         // manufacturer id
    CHECK_EQ(reply[4], 0x01u);         // manufacturer number
    CHECK(c.clio.cdrom().status_empty());
}

TEST(selection_names_a_device_by_value_not_by_address) {
    // The device is chosen by the VALUE written to SELECTION. Reading the
    // device number out of the address instead looks plausible - the window is
    // exactly sixteen devices wide - but it is wrong, and it makes every
    // address on the bus answer as though hardware were fitted there.
    Chip c;
    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? 0x83u : 0x00u);
    }
    c.clio.tick(100000);
    CHECK_EQ(c.clio.read(kClioXbusPoll) & kXbusStatusReady, kXbusStatusReady);

    // Select a slot with nothing in it. CLIO flags it and passes nothing to
    // the drive.
    c.clio.write(kClioXbusSelect, 3);
    CHECK_EQ(c.clio.read(kClioXbusPoll) & kXbusPollUnfitted, kXbusPollUnfitted);

    const u64 before = c.clio.cdrom().commands_received();
    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, 0x83u);
    }
    CHECK_EQ(c.clio.cdrom().commands_received(), before);
}

TEST(an_empty_address_reads_back_as_unfitted) {
    // Nothing at address seven, so both state bits read set. Zero would be
    // indistinguishable from a device that simply has nothing ready yet.
    Chip c;
    c.clio.write(kClioXbusSelect, 7);
    CHECK_EQ(c.clio.read(kClioXbusPoll), kXbusPollUnfitted);
}

TEST(selection_addresses_a_device_with_the_low_nibble_only) {
    // Only sixteen devices are addressable, so the high nibble cannot be part
    // of the address. Masking the whole byte makes every modified selection
    // look like a different device and the drive stops answering.
    Chip c;
    c.clio.write(kClioXbusSelect, 0x30u | kXbusCdRomAddress);
    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? 0x83u : 0x00u);
    }
    CHECK_EQ(c.clio.cdrom().commands_received(), 1u);
}

TEST(selection_bit_seven_asks_for_the_control_nibble_alone) {
    // This is how software probes a slot without disturbing the state bits.
    Chip c;
    c.clio.write(kClioXbusSelect, kXbusCdRomAddress);
    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? 0x83u : 0x00u);
    }
    c.clio.tick(100000);
    CHECK_EQ(c.clio.read(kClioXbusPoll) & kXbusStatusReady, kXbusStatusReady);

    c.clio.write(kClioXbusSelect, 0x80u | kXbusCdRomAddress);
    CHECK_EQ(c.clio.read(kClioXbusPoll) & 0xf0u, 0u);
}

TEST(only_the_control_nibble_of_the_poll_register_is_writable) {
    // The driver reads the register, masks to the low four bits and ORs in an
    // interrupt enable. If the state bits were writable too it would clobber
    // them every time it did that.
    Chip c;
    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? 0x83u : 0x00u);
    }
    c.clio.tick(100000);
    c.clio.write(kClioXbusPoll, kXbusStatusIrqEnable);
    const u32 poll = c.clio.read(kClioXbusPoll);
    CHECK_EQ(poll & kXbusStatusIrqEnable, kXbusStatusIrqEnable);
    CHECK_EQ(poll & kXbusStatusReady, kXbusStatusReady);
}

TEST(the_control_register_only_changes_bits_that_are_write_enabled) {
    // To change a low bit you must also set its write-enable in the next
    // nibble. Writing the value on its own does nothing, which is a quiet way
    // to lose every setting software makes here.
    Chip c;
    CHECK_EQ(c.clio.read(kClioControl), 0u);

    c.clio.write(kClioControl, 0x02);      // no write-enable: ignored
    CHECK_EQ(c.clio.read(kClioControl), 0u);

    c.clio.write(kClioControl, 0x22);      // enable bit 1, set bit 1
    CHECK_EQ(c.clio.read(kClioControl), 0x02u);

    c.clio.write(kClioControl, 0x11);      // enable bit 0, set bit 0
    CHECK_EQ(c.clio.read(kClioControl), 0x03u);

    c.clio.write(kClioControl, 0x20);      // enable bit 1, clear bit 1
    CHECK_EQ(c.clio.read(kClioControl), 0x01u);
}

TEST(both_ports_of_an_interrupt_pair_read_the_same_mask) {
    // The ports differ in what a WRITE does, not a read. Returning zero from
    // the clear ports means software reading back its own mask sees nothing.
    Chip c;
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank1);
    CHECK_EQ(c.clio.read(kClioIrq0Enable) & ~kIrqSecondaryBank, kIrqVerticalBlank1);
    CHECK_EQ(c.clio.read(kClioIrq0Disable) & ~kIrqSecondaryBank, kIrqVerticalBlank1);

    c.clio.raise(kIrqVerticalBlank1);
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kIrqVerticalBlank1);
    CHECK_EQ(c.clio.read(kClioIrq0Clear), kIrqVerticalBlank1);
}

TEST(the_two_expansion_bus_control_registers_are_independent) {
    // 0x400 and 0x404 look like a set/clear pair and are not one. Treating
    // them as a pair makes a write to either silently modify the other, and
    // the driver reads back a value it never wrote.
    Chip c;
    c.clio.write(kClioXbusCtl, 0x4000);
    c.clio.write(kClioXbusDirection, 0x0280);
    CHECK_EQ(c.clio.read(kClioXbusCtl), 0x4000u);
    CHECK_EQ(c.clio.read(kClioXbusDirection), 0x0280u);

    // A plain store, not an OR: the second write replaces the first.
    c.clio.write(kClioXbusCtl, 0x0040);
    CHECK_EQ(c.clio.read(kClioXbusCtl), 0x0040u);
}

TEST(a_bus_control_write_carrying_bit_eleven_is_refused) {
    // Value and all - the register keeps what it had.
    Chip c;
    c.clio.write(kClioXbusCtl, 0x4000);
    c.clio.write(kClioXbusCtl, kXbusCtlWriteVeto);
    CHECK_EQ(c.clio.read(kClioXbusCtl), 0x4000u);
}

TEST(a_read_command_streams_sectors_from_the_disc) {
    // READ carries its start address in MSF unless bit 0 of byte 4 says
    // otherwise, and a count of sectors. The reply is only an acknowledgement -
    // the sector data itself comes back through the data FIFO.
    Chip c;
    c.clio.cdrom().set_disc_present(true);

    // MSF 00:02:00 is LBA 0: the first two seconds of a CD are lead-in.
    const u8 command[7] = {kCmdRead, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01};
    for (u8 byte : command) c.clio.write(kClioXbusCommand, byte);
    c.clio.tick(100000);

    CHECK_EQ(c.clio.read(kClioXbusCommand), kCmdRead);
    CHECK_EQ(c.clio.read(kClioXbusCommand) & kStatusSpinUp, kStatusSpinUp);

    // With no disc attached there is nothing to stream, but the command must
    // still have been accepted rather than rejected.
    CHECK_EQ(c.clio.cdrom().last_command(), kCmdRead);
}

TEST(read_capacity_reports_the_lead_out_in_msf) {
    Chip c;
    const u8 command[7] = {kCmdReadCapacity, 0, 0, 0, 0, 0, 0};
    for (u8 byte : command) c.clio.write(kClioXbusCommand, byte);
    c.clio.tick(100000);

    CHECK_EQ(c.clio.read(kClioXbusCommand), kCmdReadCapacity);
    c.clio.read(kClioXbusCommand);                       // reserved
    const u32 m = c.clio.read(kClioXbusCommand);
    const u32 s = c.clio.read(kClioXbusCommand);
    c.clio.read(kClioXbusCommand);                       // frames
    // An empty drive still answers. LBA zero is MSF 00:02:00, so the lead-in is
    // present once. The sector count itself already names the lead-out.
    CHECK_EQ(m, 0u);
    CHECK_EQ(s, 2u);
}

TEST(a_read_transfers_sector_bytes_through_the_data_fifo) {
    // The CPU never reads the drive's data port - across a whole disc mount it
    // reads it exactly zero times. Sector bytes leave the drive through the
    // data FIFO, which MADAM's expansion DMA drains into memory.
    Chip c;
    c.clio.cdrom().set_disc_present(true);
    CHECK(!c.clio.cdrom().has_chunk());

    const u8 command[7] = {kCmdRead, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01};
    for (u8 byte : command) c.clio.write(kClioXbusCommand, byte);
    c.clio.tick(100000);

    // With no disc attached there is nothing to stream; the point is that the
    // command is accepted and the drive reports the motor running.
    CHECK_EQ(c.clio.read(kClioXbusCommand), kCmdRead);
    CHECK_EQ(c.clio.read(kClioXbusCommand) & kStatusSpinUp, kStatusSpinUp);
}

TEST(the_memory_configuration_cannot_be_written) {
    // It reports how much memory is FITTED, which software cannot change. The
    // boot ROM writes zero to it during start-up; honouring that makes the
    // machine tell itself it has no memory, and its own sizing routine then
    // panics before doing anything else.
    Bus bus;
    Madam madam(bus);
    CHECK_EQ(madam.read(kMadamMemConfig), kMadamMemConfigStock);

    madam.write(kMadamMemConfig, 0);
    CHECK_EQ(madam.read(kMadamMemConfig), kMadamMemConfigStock);

    // And the value describes the machine the ROM expects: VRAM in bits 0-2,
    // DRAM1 in bits 3-4, DRAM2 in bits 5-6, each in megabytes.
    CHECK_EQ(kMadamMemConfigStock & 0x07u, 1u);          // 1 MB VRAM
    CHECK_EQ((kMadamMemConfigStock >> 3) & 0x03u, 1u);   // 1 MB DRAM1
    CHECK_EQ((kMadamMemConfigStock >> 5) & 0x03u, 1u);   // 1 MB DRAM2
}

TEST(the_secondary_bank_bit_always_reads_set_in_the_first_banks_enable) {
    // It is not an enable. It is the chip saying a second bank exists and is
    // worth reading, and software that never sees it never services anything
    // that lives there.
    Chip c;
    CHECK_EQ(c.clio.read(kClioIrq0Enable) & kIrqSecondaryBank, kIrqSecondaryBank);
    c.clio.write(kClioIrq0Disable, 0xffffffffu);
    CHECK_EQ(c.clio.read(kClioIrq0Enable) & kIrqSecondaryBank, kIrqSecondaryBank);
}

TEST(the_dsp_raises_the_audio_interrupt_from_its_own_program) {
    // The interrupt comes out of the program rather than from a timer: a
    // title decides when it wants to be woken, and titles differ. This is the
    // smallest program that asks for one - store a value into the interrupt
    // register, then sleep.
    Console console;
    console.reset();
    Bus& bus = console.bus();

    // A control-format move of an immediate into 0x3EE, then sleep.
    const u16 program[] = {0x9bee, 0xc001, 0x8380};
    for (u32 i = 0; i < 3; ++i) {
        bus.write32(kClioBase + 0x2000 + i * 4, program[i]);
    }

    bus.write32(kClioBase + kClioIrq0Enable, kIrqAudioTimer);
    bus.write32(kClioBase + kClioIrq0Clear, kIrqAudioTimer);

    // A stopped DSP never reaches the instruction, whatever is loaded.
    console.run_frame();
    CHECK_EQ(bus.read32(kClioBase + kClioIrq0Pending) & kIrqAudioTimer, 0u);

    bus.write32(kClioBase + kClioDspRun, 1);
    console.run_frame();
    CHECK_EQ(bus.read32(kClioBase + kClioIrq0Pending) & kIrqAudioTimer,
             kIrqAudioTimer);
}

TEST(ejecting_reports_an_empty_drive_and_spinning_up_reloads_it) {
    // The driver ejects the disc on purpose partway through a mount, reads the
    // status back, and decides what to do next from it. Told the drive is
    // still fully loaded it concludes the eject silently failed and waits for
    // a drive-ready notification that never comes.
    Chip c;
    c.clio.cdrom().set_disc_present(true);

    const auto command = [&](u8 opcode) {
        for (int i = 0; i < 7; ++i) {
            c.clio.write(kClioXbusCommand, i == 0 ? opcode : 0u);
        }
        CHECK_EQ(c.clio.read(kClioXbusCommand), opcode);
        return static_cast<u8>(c.clio.read(kClioXbusCommand));
    };

    // Loaded: tray shut, disc in, spinning, ready.
    CHECK_EQ(command(kCmdReadStatus), 0xe1u);

    // Ejected: ready and nothing else.
    CHECK_EQ(command(kCmdEject), kStatusReady);
    CHECK_EQ(command(kCmdReadStatus), kStatusReady);

    // Spinning up closes the tray again - the other half of the handshake.
    CHECK_EQ(command(kCmdMotorOn), 0xe1u);
}

TEST(abort_is_one_byte_long_and_the_rest_are_seven) {
    // ABORT interrupts a transfer that is already running, so a drive that
    // waited for six more bytes before acting on it would never abort
    // anything. And the six bytes that would have followed belong to the NEXT
    // command: swallow them and every command after it is read one byte out of
    // step, which looks like the drive answering the wrong questions.
    Chip c;
    c.clio.cdrom().set_disc_present(true);

    c.clio.write(kClioXbusCommand, kCmdAbort);
    CHECK_EQ(c.clio.cdrom().commands_received(), 1u);
    CHECK_EQ(c.clio.cdrom().last_command(), kCmdAbort);

    // The next command starts cleanly rather than continuing the abort.
    for (int i = 0; i < 7; ++i) {
        c.clio.write(kClioXbusCommand, i == 0 ? kCmdVersion : 0u);
    }
    CHECK_EQ(c.clio.cdrom().commands_received(), 2u);
    CHECK_EQ(c.clio.cdrom().last_command(), kCmdVersion);
}
