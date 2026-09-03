// Memory map and byte-order tests.
//
// The 3DO is big-endian as the CPU sees it, and this host is not. Every word
// access therefore swaps, and a swap that is right in one direction and wrong
// in the other produces graphics that are almost right — the most expensive
// kind of bug to chase later. So it is pinned down here.
#include <vector>

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

TEST(the_builtin_demo_is_an_original_executable_rom) {
    Console console;
    CHECK(console.load_builtin_demo());
    CHECK(console.bios_loaded());
    CHECK(console.demo_loaded());
    CHECK_EQ(console.bus().read32(kRomBase), 0xea000006u);

    // The ROM really executes through the ARM and hardware bus. Within a few
    // fields it must install a VDL, start the DSP, and render non-black video.
    for (int i = 0; i < 30; ++i) console.run_frame();
    CHECK(console.cpu().total_instructions() > 1000u);
    CHECK(console.madam().vdl_address() != 0u);
    CHECK(console.dsp().running());
    CHECK(console.vdlp().entries_walked() > 0u);

    const Frame frame = console.framebuffer();
    bool has_colour = false;
    for (int y = 0; y < frame.height && !has_colour; y += 16) {
        for (int x = 0; x < frame.width; x += 16) {
            if ((frame.pixels[static_cast<size_t>(y) * frame.width + x] &
                 0x00ffffffu) != 0u) {
                has_colour = true;
                break;
            }
        }
    }
    CHECK(has_colour);

    // The DSP program must produce an actual signal, not merely set its run
    // bit. This is sampled through the same audio ring used by the app.
    console.audio().reset();
    console.run_frame();
    StereoSample samples[1024] = {};
    const u32 count = console.audio().pull(samples, 1024);
    bool has_audio = false;
    int peak = 0;
    bool audio_changes = false;
    for (u32 i = 0; i < count; ++i) {
        if (samples[i].left != 0 || samples[i].right != 0) {
            has_audio = true;
        }
        const int magnitude = samples[i].left < 0
                                  ? -static_cast<int>(samples[i].left)
                                  : static_cast<int>(samples[i].left);
        if (magnitude > peak) peak = magnitude;
        if (i > 0 && samples[i].left != samples[i - 1].left) {
            audio_changes = true;
        }
        CHECK_EQ(samples[i].left, samples[i].right);
    }
    CHECK(has_audio);
    CHECK(audio_changes);
    CHECK(peak <= 4096);

    // PBUS input is consumed by the ROM itself. Holding Right must move the
    // marker in VRAM, which proves the host pad reached emulated software.
    const u32 before = console.bus().read32(kVramBase +
        framebuffer_offset(152, 204, 320));
    Joypad pad;
    pad.right = true;
    console.set_joypad(pad);
    for (int i = 0; i < 3; ++i) console.run_frame();
    const u32 after = console.bus().read32(kVramBase +
        framebuffer_offset(152, 204, 320));
    CHECK(before != after);
}

TEST(the_frame_is_the_right_shape_for_the_region) {
    Console console;

    console.set_region(Region::Ntsc);
    Frame frame = console.framebuffer();
    CHECK_EQ(frame.width, 640);
    CHECK_EQ(frame.height, 480);
    CHECK(frame.pixels != nullptr);

    console.set_region(Region::Pal);
    frame = console.framebuffer();
    CHECK_EQ(frame.width, 640);
    CHECK_EQ(frame.height, 576);
}

TEST(a_reset_preserves_the_selected_pal_geometry_in_madam) {
    Console console;
    console.set_region(Region::Pal);
    console.reset();

    CHECK_EQ(console.madam().clip_width(), 320u);
    CHECK_EQ(console.madam().clip_height(), 288u);
}

TEST(nvram_survives_a_round_trip_through_a_saved_image) {
    Bus bus;
    // Nothing to save until the machine writes something. Formatting counts,
    // so the very first ask is expected to hand back the formatted image.
    CHECK(bus.nvram_dirty());
    bus.clear_nvram_dirty();
    CHECK(!bus.nvram_dirty());

    bus.write32(kNvramBase + 300 * 4, 0xa5u);
    CHECK(bus.nvram_dirty());

    const std::vector<u8> saved(bus.nvram(), bus.nvram() + bus.nvram_size());

    Bus fresh;
    CHECK_EQ(fresh.read32(kNvramBase + 300 * 4) & 0xffu, 0x00u);
    CHECK(fresh.restore_nvram(saved.data(), saved.size()));
    CHECK_EQ(fresh.read32(kNvramBase + 300 * 4) & 0xffu, 0xa5u);
    // Nothing has changed since the restore, so there is nothing to write back.
    CHECK(!fresh.nvram_dirty());
}

TEST(a_wrong_sized_nvram_image_is_refused_rather_than_padded) {
    // A half-restored NVRAM looks corrupt to a title, which then offers to
    // reformat it - so a short read must lose nothing rather than lose all of
    // it. Refusing leaves the formatted image in place.
    Bus bus;
    const std::vector<u8> too_short(16, 0xffu);
    CHECK(!bus.restore_nvram(too_short.data(), too_short.size()));
    CHECK(!bus.restore_nvram(nullptr, bus.nvram_size()));
    CHECK_EQ(bus.read32(kNvramBase + 0 * 4) & 0xffu, 0x01u);
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
