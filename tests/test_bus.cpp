// Memory map and byte-order tests.
//
// The 3DO is big-endian as the CPU sees it, and this host is not. Every word
// access therefore swaps, and a swap that is right in one direction and wrong
// in the other produces graphics that are almost right — the most expensive
// kind of bug to chase later. So it is pinned down here.
#include "core/bus.h"
#include "core/console.h"
#include "test_harness.h"

using namespace retro3do;

TEST(words_are_stored_big_endian) {
    Bus bus;
    bus.write32(0x100u, 0x11223344u);

    CHECK_EQ(bus.read8(0x100u), 0x11u);
    CHECK_EQ(bus.read8(0x101u), 0x22u);
    CHECK_EQ(bus.read8(0x102u), 0x33u);
    CHECK_EQ(bus.read8(0x103u), 0x44u);
    CHECK_EQ(bus.read32(0x100u), 0x11223344u);
}

TEST(halfwords_are_stored_big_endian) {
    Bus bus;
    bus.write16(0x200u, 0xABCDu);
    CHECK_EQ(bus.read8(0x200u), 0xABu);
    CHECK_EQ(bus.read8(0x201u), 0xCDu);
    CHECK_EQ(bus.read16(0x200u), 0xABCDu);
}

TEST(word_accesses_ignore_the_low_address_bits) {
    Bus bus;
    bus.write32(0x300u, 0xCAFEBABEu);
    CHECK_EQ(bus.read32(0x301u), 0xCAFEBABEu);
    CHECK_EQ(bus.read32(0x302u), 0xCAFEBABEu);
    CHECK_EQ(bus.read32(0x303u), 0xCAFEBABEu);
}

TEST(dram_and_vram_are_separate) {
    Bus bus;
    bus.write32(0x400u, 0x1u);
    bus.write32(kVramBase + 0x400u, 0x2u);
    CHECK_EQ(bus.read32(0x400u), 0x1u);
    CHECK_EQ(bus.read32(kVramBase + 0x400u), 0x2u);
}

TEST(rom_ignores_writes) {
    Bus bus;
    const u8 image[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};
    CHECK(bus.load_bios(image, sizeof(image)));

    bus.write32(kRomBase, 0u);
    CHECK_EQ(bus.read32(kRomBase), 0xDEADBEEFu);
}

TEST(an_oversized_bios_is_refused) {
    Bus bus;
    std::vector<u8> oversized(kRomSize + 4, 0u);
    CHECK(!bus.load_bios(oversized.data(), oversized.size()));
    CHECK(!bus.bios_loaded());
}

TEST(a_reset_clears_ram_but_keeps_the_bios) {
    Bus bus;
    const u8 image[4] = {0x12, 0x34, 0x56, 0x78};
    CHECK(bus.load_bios(image, sizeof(image)));

    bus.write32(0x500u, 0xFFFFFFFFu);
    bus.reset();

    CHECK_EQ(bus.read32(0x500u), 0u);
    CHECK_EQ(bus.read32(kRomBase), 0x12345678u);
    CHECK(bus.bios_loaded());
}

TEST(a_console_without_a_bios_reports_why) {
    Console console;
    CHECK(!console.bios_loaded());
    CHECK(!console.load_bios("/definitely/not/a/real/path.rom"));
    CHECK(!console.last_error().empty());
}

TEST(the_frame_is_the_right_shape_for_the_region) {
    Console console;

    console.set_region(Region::Ntsc);
    Frame frame = console.framebuffer();
    CHECK_EQ(frame.width, 320);
    CHECK_EQ(frame.height, 240);
    CHECK(frame.pixels != nullptr);

    console.set_region(Region::Pal);
    frame = console.framebuffer();
    CHECK_EQ(frame.width, 320);
    CHECK_EQ(frame.height, 288);
}

TEST(the_nvram_comes_up_formatted_rather_than_blank) {
    // Real hardware is never blank: a console formats its NVRAM the first time
    // it is switched on and it stays that way. An emulator that comes up
    // zeroed is not modelling a new console, it is modelling one that has
    // never been switched on - and the OS only formats it when it reaches its
    // own shell, so a title booted straight from a disc finds no filesystem
    // and reports the NVRAM full.
    Bus bus;
    CHECK_EQ(bus.read32(kNvramBase + 0 * 4) & 0xffu, 0x01u);   // record type
    for (u32 i = 1; i <= 5; ++i) {
        CHECK_EQ(bus.read32(kNvramBase + i * 4) & 0xffu, 0x5au);   // sync
    }

    // Labelled, and sized as the whole 32 KiB.
    const char label[] = "nvram";
    for (u32 i = 0; i < 5; ++i) {
        CHECK_EQ(bus.read32(kNvramBase + (40 + i) * 4) & 0xffu,
                 static_cast<u32>(label[i]));
    }
    u32 blocks = 0;
    for (u32 i = 0; i < 4; ++i) {
        blocks = (blocks << 8) | (bus.read32(kNvramBase + (80 + i) * 4) & 0xffu);
    }
    CHECK_EQ(blocks, 32768u);

    // And a reset does not wipe it, any more than switching a console off does.
    bus.write32(kNvramBase + 200 * 4, 0x5au);
    bus.reset();
    CHECK_EQ(bus.read32(kNvramBase + 200 * 4) & 0xffu, 0x5au);
}
