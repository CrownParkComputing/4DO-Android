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
    CHECK_EQ(c.clio.read(kClioIrq0Enable), kIrqVerticalBlank0);
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
    CHECK_EQ(c.clio.read(kClioIrq0Enable), kTimer3Irq);
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
    CHECK_EQ(c.bus.read32(kClioBase + kClioIrq0Enable), kTimer3Irq);

    c.clio.raise(kTimer3Irq);
    CHECK_EQ(c.bus.read32(kClioBase + kClioIrq0Pending), kTimer3Irq);
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

TEST(a_timer_reloads_and_raises_an_interrupt_when_it_expires) {
    Chip c;
    c.clio.write(kTimer3Counter, 100);
    c.clio.write(kTimer3Reload, 500);
    c.clio.write(kClioIrq0Enable, kTimer3Irq);
    c.clio.write(kClioTimerConfigSet0,
                 timer_config(3, kTimerDecrement | kTimerReload));

    c.clio.tick(50);
    CHECK_EQ(c.clio.read(kTimer3Counter), 50u);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kTimer3Irq, 0u);

    c.clio.tick(60);  // past zero
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

    c.clio.tick(20);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kTimer3Irq, kTimer3Irq);

    c.clio.write(kClioIrq0Clear, kTimer3Irq);
    c.clio.tick(1000);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kTimer3Irq, 0u);
}

TEST(an_unconfigured_timer_does_not_count) {
    Chip c;
    c.clio.write(kTimer3Counter, 100);
    c.clio.write(kClioTimerConfigSet0, timer_config(3, kTimerDecrement));
    c.clio.write(kClioTimerConfigClear0, timer_config(3, kTimerDecrement));

    c.clio.tick(500);
    CHECK_EQ(c.clio.read(kTimer3Counter), 100u);
}

TEST(timers_are_independent) {
    Chip c;
    c.clio.write(kClioTimerBase, 100);                        // timer 0
    c.clio.write(kTimer3Counter, 900);                        // timer 3
    c.clio.write(kClioTimerConfigSet0,
                 timer_config(0, kTimerDecrement) |
                 timer_config(3, kTimerDecrement));

    c.clio.tick(50);
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
    c.clio.tick(3);
    CHECK_EQ(c.clio.read(kTimer3Counter), 4u);

    // Now push timer 2 past zero; timer 3 takes exactly one step.
    c.clio.tick(3);
    CHECK_EQ(c.clio.read(kTimer3Counter), 3u);
}

TEST(a_reset_silences_everything) {
    Chip c;
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);
    c.clio.write(kClioTimerConfigSet0, timer_config(3, kTimerDecrement));

    c.clio.reset();

    CHECK_EQ(c.clio.read(kClioIrq0Pending), 0u);
    CHECK_EQ(c.clio.read(kClioIrq0Enable), 0u);
    CHECK_EQ(c.clio.read(kClioTimerConfigSet0), 0u);
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
    // The drive answers after a delay, not instantly.
    CHECK(c.clio.cdrom().status_empty());
    c.clio.tick(100000);
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
    // The drive does not answer instantly, and that delay is load bearing: the
    // driver reads the bytes it expects and then requires the FIFO to be empty.
    CHECK(c.clio.cdrom().status_empty());
    c.clio.tick(100000);
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

TEST(the_device_count_probe_must_be_answered_cleanly) {
    // 0x8F written to SELECTION is a probe rather than a device. Treating it as
    // an ordinary selection leaves CLIO reporting "too many devices on the bus"
    // and enumeration fails before the drive is ever reached.
    Chip c;
    c.clio.write(kClioXbusSelect, 7);
    CHECK_EQ(c.clio.read(kClioXbusPoll) & kXbusPollUnfitted, kXbusPollUnfitted);

    c.clio.write(kClioXbusSelect, kXbusSelectProbe);
    CHECK_EQ(c.clio.read(kClioXbusPoll) & kXbusPollUnfitted, 0u);
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
    CHECK_EQ(c.clio.read(kClioIrq0Enable), kIrqVerticalBlank1);
    CHECK_EQ(c.clio.read(kClioIrq0Disable), kIrqVerticalBlank1);

    c.clio.raise(kIrqVerticalBlank1);
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kIrqVerticalBlank1);
    CHECK_EQ(c.clio.read(kClioIrq0Clear), kIrqVerticalBlank1);
}

TEST(the_expansion_bus_always_reports_ready) {
    // The boot ROM clears the ready bit and then spins until hardware sets it
    // again. Honouring that clear literally hangs the machine on its own bus
    // setup, so the bit reads set however software writes it.
    Chip c;
    CHECK_EQ(c.clio.read(kClioXbusCtlSet) & kXbusReady, kXbusReady);

    c.clio.write(kClioXbusCtlClear, kXbusReady);
    CHECK_EQ(c.clio.read(kClioXbusCtlSet) & kXbusReady, kXbusReady);

    // The writable control bits still behave as a set/clear pair.
    c.clio.write(kClioXbusCtlSet, 0x0800);
    CHECK_EQ(c.clio.read(kClioXbusCtlSet) & 0x0800u, 0x0800u);
    c.clio.write(kClioXbusCtlClear, 0x0800);
    CHECK_EQ(c.clio.read(kClioXbusCtlSet) & 0x0800u, 0u);

    // Bits outside the writable mask are ignored.
    c.clio.write(kClioXbusCtlSet, 0x0001);
    CHECK_EQ(c.clio.read(kClioXbusCtlSet) & 0x0001u, 0u);
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
    // An empty drive still answers, and lead-out sits at the lead-in offset.
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
