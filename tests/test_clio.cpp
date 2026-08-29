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
#include "core/console.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

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
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0 | kIrqTimer);
    c.clio.raise(kIrqVerticalBlank0 | kIrqTimer);
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kIrqVerticalBlank0 | kIrqTimer);

    // Acknowledge only one of the two. The other must survive — this is the
    // whole point of a separate clear port rather than a writeback.
    c.clio.write(kClioIrq0Clear, kIrqVerticalBlank0);
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kIrqTimer);
}

TEST(the_disable_port_clears_enables_without_touching_pending) {
    Chip c;
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0 | kIrqTimer);
    c.clio.raise(kIrqVerticalBlank0);

    c.clio.write(kClioIrq0Disable, kIrqVerticalBlank0);
    CHECK_EQ(c.clio.read(kClioIrq0Enable), kIrqTimer);
    // Still pending: disabling a source masks it, it does not acknowledge it.
    CHECK_EQ(c.clio.read(kClioIrq0Pending), kIrqVerticalBlank0);
}

TEST(an_enabled_and_pending_interrupt_reaches_the_cpu) {
    Chip c;
    // Unmask IRQ at the CPU, which reset leaves masked.
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagI);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);

    // The CPU samples the line at an instruction boundary.
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorIrq);
    CHECK(c.cpu.mode() == Mode::Irq);
}

TEST(the_interrupt_line_drops_when_the_source_is_acknowledged) {
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagI);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorIrq);

    // Acknowledge, return to a normal mode, and the CPU must not re-enter.
    c.clio.write(kClioIrq0Clear, kIrqVerticalBlank0);
    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagI) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);
    c.cpu.step();
    CHECK(c.cpu.mode() == Mode::Supervisor);
}

TEST(a_source_interrupts_once_per_edge_not_continuously) {
    // CLIO signals a rising edge rather than holding the line, so a handler that
    // returns without acknowledging is NOT re-entered.
    //
    // This is the opposite of what an earlier version asserted, and the boot ROM
    // is what settled it: its vertical-blank handler reads a software flag,
    // returns, and never writes any CLIO register. Held level livelocks the
    // machine on its own startup interrupt and the boot animation never runs.
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagI);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);

    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorIrq);

    // Return to normal execution without acknowledging anything.
    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagI) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);
    c.cpu.step();
    // Ordinary execution continues; the edge was consumed.
    CHECK(c.cpu.pc() != kVectorIrq);

    // Still pending and still enabled - the state is visible to software, it
    // simply does not keep interrupting.
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kIrqVerticalBlank0,
             kIrqVerticalBlank0);
}

TEST(a_second_edge_interrupts_again) {
    // A fresh source arriving must still be delivered, or the machine would
    // take exactly one interrupt ever.
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagI);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0 | kIrqTimer);
    c.clio.raise(kIrqVerticalBlank0);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorIrq);

    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagI) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);

    // Acknowledge, then raise again: that is a new edge.
    c.clio.write(kClioIrq0Clear, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorIrq);
}

TEST(clio_registers_are_reachable_through_the_bus) {
    Chip c;
    c.bus.write32(kClioBase + kClioIrq0Enable, kIrqTimer);
    CHECK_EQ(c.bus.read32(kClioBase + kClioIrq0Enable), kIrqTimer);

    c.clio.raise(kIrqTimer);
    CHECK_EQ(c.bus.read32(kClioBase + kClioIrq0Pending), kIrqTimer);
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

TEST(a_timer_reloads_and_raises_an_interrupt_when_it_expires) {
    Chip c;
    c.clio.write(kClioTimerBase, 100);       // timer 0 counter
    c.clio.write(kClioTimerBase + 4, 500);   // timer 0 reload
    c.clio.write(kClioIrq0Enable, kIrqTimer);
    c.clio.write(kClioTimerEnable, 1u << 0);

    c.clio.tick(50);
    CHECK_EQ(c.clio.read(kClioTimerBase), 50u);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kIrqTimer, 0u);

    c.clio.tick(60);  // past zero
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kIrqTimer, kIrqTimer);
    CHECK_EQ(c.clio.read(kClioTimerBase), 500u);
}

TEST(a_disabled_timer_does_not_count) {
    Chip c;
    c.clio.write(kClioTimerBase, 100);
    c.clio.write(kClioTimerEnable, 1u << 0);
    c.clio.write(kClioTimerDisable, 1u << 0);

    c.clio.tick(500);
    CHECK_EQ(c.clio.read(kClioTimerBase), 100u);
}

TEST(timers_are_independent) {
    Chip c;
    c.clio.write(kClioTimerBase, 100);                        // timer 0
    c.clio.write(kClioTimerBase + kClioTimerStride, 900);     // timer 1
    c.clio.write(kClioTimerEnable, (1u << 0) | (1u << 1));

    c.clio.tick(50);
    CHECK_EQ(c.clio.read(kClioTimerBase), 50u);
    CHECK_EQ(c.clio.read(kClioTimerBase + kClioTimerStride), 850u);
}

TEST(a_reset_silences_everything) {
    Chip c;
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank0);
    c.clio.raise(kIrqVerticalBlank0);
    c.clio.write(kClioTimerEnable, 1u << 0);

    c.clio.reset();

    CHECK_EQ(c.clio.read(kClioIrq0Pending), 0u);
    CHECK_EQ(c.clio.read(kClioIrq0Enable), 0u);
    CHECK_EQ(c.clio.read(kClioTimerEnable), 0u);
    CHECK_EQ(c.clio.scanline(), 0u);
}

TEST(a_new_source_still_interrupts_while_an_older_one_stays_pending) {
    // The edge is per source, not per line.
    //
    // The boot ROM never acknowledges anything - it drives its animation by
    // polling the line counter instead - so vertical blank goes pending early
    // and simply stays pending for the rest of the run. A single edge flag for
    // the whole interrupt line stays latched behind it, and every later source
    // is silently swallowed.
    //
    // That is not a subtle failure. It is how the OS came to boot, ask the CD
    // drive fifteen questions, and then idle forever: the expansion bus raised
    // its completion interrupt, the bit sat plainly pending and enabled with
    // interrupts unmasked at the CPU, and the CPU never took it.
    Chip c;
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagI);
    c.clio.write(kClioIrq0Enable, kIrqVerticalBlank1 | kIrqExpansionBus);

    // Vertical blank arrives and is never acknowledged.
    c.clio.raise(kIrqVerticalBlank1);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorIrq);

    // Back to ordinary code, source still pending, still enabled.
    c.cpu.set_cpsr((c.cpu.cpsr() & ~kModeMask & ~kFlagI) |
                   static_cast<u32>(Mode::Supervisor));
    c.cpu.set_reg(15, kRomBase);
    c.cpu.step();
    CHECK(c.cpu.pc() != kVectorIrq);

    // Now a DIFFERENT source arrives. It must be delivered.
    c.clio.raise(kIrqExpansionBus);
    c.cpu.set_cpsr(c.cpu.cpsr() & ~kFlagI);
    c.cpu.step();
    CHECK_EQ(c.cpu.pc(), kVectorIrq);
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
    CHECK_EQ(c.clio.read(kClioXbusResult) & kXbusStatusReady, kXbusStatusReady);
    CHECK_EQ(c.clio.read(kClioIrq0Pending) & kIrqExpansionBus, kIrqExpansionBus);
}
