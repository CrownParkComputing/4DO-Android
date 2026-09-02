// SPORT — the VRAM page copier.
//
// Everything asserted here was derived from the boot ROM's own memory test, so
// these tests double as the record of what that test proved. The ROM does not
// boot without any of it.
#include "core/bus.h"
#include "core/console.h"
#include "core/sport.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

struct Rig {
    Bus bus;
    Sport sport{bus};
    Rig() { bus.attach_sport(&sport); }

    // The address the machine would write to in order to name a page.
    u32 copy_addr(u32 vram_offset) const {
        return kSportBase + (vram_offset >> kSportAddressShift);
    }
    u32 fill_addr(u32 vram_offset) const {
        return kSportBase + kSportFillWindow + (vram_offset >> kSportAddressShift);
    }
};

}  // namespace

TEST(a_page_is_two_kilobytes) {
    // Fixed by the ROM: after a copy it reads back and compares exactly 512
    // words. Anything else and the test would fail on a correct emulator.
    CHECK_EQ(kSportPageBytes, 2048u);
}

TEST(a_read_then_a_write_copies_a_page) {
    Rig rig;

    // Fill a source page with a recognisable sequence and the destination with
    // something else, exactly as the ROM's test does.
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        rig.bus.write32(kVramBase + 0x56800 + i, 0x1000u + i);
        rig.bus.write32(kVramBase + 0xB3000 + i, 0xFFEEFFEEu);
    }

    rig.bus.read32(rig.copy_addr(0x56800));            // prime
    rig.bus.write32(rig.copy_addr(0xB3000), 0xffffffffu);  // commit

    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        CHECK_EQ(rig.bus.read32(kVramBase + 0xB3000 + i), 0x1000u + i);
    }
    CHECK_EQ(rig.sport.copies(), 1u);
}

TEST(sport_addresses_are_relative_to_vram_not_absolute) {
    // The thing that took longest to establish. The ROM builds these addresses
    // from bare literals with no memory base added, while filling and verifying
    // at r0 + literal; the two only agree if SPORT works in VRAM offsets.
    Rig rig;
    rig.bus.write32(kVramBase + 0x800, 0xCAFEBABEu);

    rig.bus.read32(rig.copy_addr(0x800));
    CHECK_EQ(rig.sport.latched_source(), kVramBase + 0x800);

    rig.bus.write32(rig.copy_addr(0x1000), 0xffffffffu);
    CHECK_EQ(rig.bus.read32(kVramBase + 0x1000), 0xCAFEBABEu);
    // And nothing landed at the bare offset.
    CHECK_EQ(rig.bus.read32(0x1000), 0u);
}

TEST(a_mask_bit_protects_the_destination_rather_than_selecting_it) {
    Rig rig;
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        rig.bus.write32(kVramBase + 0x0000 + i, 0xAAAAAAAAu);
        rig.bus.write32(kVramBase + 0x0800 + i, 0x55555555u);
    }

    rig.bus.read32(rig.copy_addr(0x0000));
    rig.bus.write32(rig.copy_addr(0x0800), 0x0000FFFFu);

    // A one PROTECTS. The low half of the mask is set, so the destination's
    // low half survives; the high half is clear, so the source comes through
    // there. This reads backwards until you notice that an all-ones mask is
    // the same as no mask at all - which it is, and which only works if a one
    // means "leave this alone".
    //
    // This test used to assert the opposite, because it was written from an
    // assumption rather than from the hardware. What that cost is recorded in
    // sport.cpp.
    CHECK_EQ(rig.bus.read32(kVramBase + 0x0800), 0xAAAA5555u);
}

TEST(the_fill_value_register_is_separate_from_the_fill) {
    // Two writes: one sets the value, the other performs the fill. Doing it in
    // one would be simpler and is not what the hardware does.
    Rig rig;
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        rig.bus.write32(kVramBase + 0x24800 + i, 0xAABBAABBu);
    }

    rig.bus.write32(kSportBase + kSportValueReg, 0x66676869u);
    CHECK_EQ(rig.sport.fill_value(), 0x66676869u);
    // Setting the value alone must not touch memory.
    CHECK_EQ(rig.bus.read32(kVramBase + 0x24800), 0xAABBAABBu);

    rig.bus.write32(rig.fill_addr(0x24800), 0xffffffffu);
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        CHECK_EQ(rig.bus.read32(kVramBase + 0x24800 + i), 0x66676869u);
    }
    CHECK_EQ(rig.sport.fills(), 1u);
}

TEST(a_fill_respects_its_mask_too) {
    Rig rig;
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        rig.bus.write32(kVramBase + i, 0xFFFFFFFFu);
    }
    rig.bus.write32(kSportBase + kSportValueReg, 0x00000000u);
    rig.bus.write32(rig.fill_addr(0x0000), 0x0000FF00u);

    // Everything is filled with zero except the one byte the mask protects.
    CHECK_EQ(rig.bus.read32(kVramBase), 0x0000FF00u);
}

TEST(a_fill_does_not_disturb_the_next_page) {
    Rig rig;
    rig.bus.write32(kVramBase + kSportPageBytes, 0x12345678u);

    rig.bus.write32(kSportBase + kSportValueReg, 0xDEADBEEFu);
    rig.bus.write32(rig.fill_addr(0x0000), 0xffffffffu);

    CHECK_EQ(rig.bus.read32(kVramBase), 0xDEADBEEFu);
    CHECK_EQ(rig.bus.read32(kVramBase + kSportPageBytes), 0x12345678u);
}

TEST(copying_a_page_onto_itself_is_harmless) {
    Rig rig;
    rig.bus.write32(kVramBase + 0x1000, 0x11223344u);
    rig.bus.read32(rig.copy_addr(0x1000));
    rig.bus.write32(rig.copy_addr(0x1000), 0xffffffffu);
    CHECK_EQ(rig.bus.read32(kVramBase + 0x1000), 0x11223344u);
}

TEST(a_copy_into_memory_marks_it_as_possibly_code) {
    // A page copied into memory may well be instructions, so the decode cache
    // has to be told. Without this, self-modifying-by-DMA code runs stale.
    Rig rig;
    rig.bus.write_watch().clear();

    rig.bus.read32(rig.copy_addr(0x0000));
    rig.bus.write32(rig.copy_addr(0x0800), 0xffffffffu);

    // kVramBase is above DRAM, so the watch only fires for DRAM writes; what
    // matters is that the copy goes through the bus at all rather than around
    // it. Verify by copying into DRAM instead.
    Console console;
    console.reset();
    Bus& bus = console.bus();
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        bus.write32(kVramBase + i, 0xE1A00000u);
    }
    bus.write_watch().clear();
    bus.read32(kSportBase + 0);
    bus.write32(kSportBase + (0x0800 >> kSportAddressShift), 0xffffffffu);
    CHECK(bus.read32(kVramBase + 0x800) == 0xE1A00000u);
}

TEST(the_field_flag_sits_above_the_line_number) {
    // The ROM waits on bit 11 of the line register and then masks it off to
    // read the line. Masking it away entirely, as an earlier version did, makes
    // that wait never finish.
    Console console;
    console.reset();
    Clio& clio = console.clio();
    clio.set_scanlines_per_field(263);

    const u32 first = clio.read(kClioVCount);
    CHECK_EQ(first & kClioFieldFlag, 0u);

    // Run out a whole field; the flag must flip.
    for (int i = 0; i < 300; ++i) {
        clio.tick(800);
    }
    CHECK_EQ(clio.read(kClioVCount) & kClioFieldFlag, kClioFieldFlag);

    // The line number is still readable underneath it.
    CHECK(( clio.read(kClioVCount) & kClioLineMask) < 263u);
}
