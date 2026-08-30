// VDLP tests: pixel conversion and display-list walking.
//
// The colour expansion is asserted hardest because it is the kind of thing that
// looks right in a screenshot and is quietly wrong: a plain left-shift from five
// bits to eight makes white come out as 0xF8F8F8, which nobody notices until
// they compare against hardware.
#include "core/bus.h"
#include "core/console.h"
#include "core/madam.h"
#include "core/vdlp.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

// Write one RGB555 pixel into VRAM, big-endian, as the hardware stores it.
void put_pixel(Bus& bus, u32 vram_offset, u16 pixel) {
    bus.write16(kVramBase + vram_offset, pixel);
}

// Write a pixel into a framebuffer at (x, y), through the interleaved layout.
void put_fb(Bus& bus, u32 fb_vram_offset, int x, int y, int width, u16 pixel) {
    bus.write16(kVramBase + fb_vram_offset + framebuffer_offset(x, y, width), pixel);
}

constexpr u16 rgb555(unsigned r, unsigned g, unsigned b) {
    return static_cast<u16>((r << 10) | (g << 5) | b);
}

}  // namespace

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

TEST(black_and_white_reach_the_ends_of_the_range) {
    CHECK_EQ(expand_rgb555(rgb555(0, 0, 0)), 0xff000000u);
    // Full scale in must be full scale out. This is the assertion that catches
    // a naive five-to-eight-bit shift.
    CHECK_EQ(expand_rgb555(rgb555(31, 31, 31)), 0xffffffffu);
}

TEST(the_channels_do_not_get_swapped) {
    CHECK_EQ(expand_rgb555(rgb555(31, 0, 0)), 0xffff0000u);
    CHECK_EQ(expand_rgb555(rgb555(0, 31, 0)), 0xff00ff00u);
    CHECK_EQ(expand_rgb555(rgb555(0, 0, 31)), 0xff0000ffu);
}

TEST(mid_grey_lands_near_the_middle) {
    const u32 grey = expand_rgb555(rgb555(16, 16, 16));
    const u32 channel = grey & 0xffu;
    CHECK(channel > 0x7fu);
    CHECK(channel < 0x90u);
    // All three channels must agree, or greys will have a colour cast.
    CHECK_EQ((grey >> 16) & 0xffu, channel);
    CHECK_EQ((grey >> 8) & 0xffu, channel);
}

// ---------------------------------------------------------------------------
// Reading VRAM
// ---------------------------------------------------------------------------

TEST(a_linear_framebuffer_reads_out_in_the_right_order) {
    Bus bus;
    Vdlp vdlp(bus);

    // Four distinct pixels in the first two words.
    put_pixel(bus, 0, rgb555(31, 0, 0));
    put_pixel(bus, 2, rgb555(0, 31, 0));
    put_pixel(bus, 4, rgb555(0, 0, 31));
    put_pixel(bus, 6, rgb555(31, 31, 31));

    u32 out[4] = {};
    vdlp.render_linear(out, 4, 1, 0);

    CHECK_EQ(out[0], 0xffff0000u);
    CHECK_EQ(out[1], 0xff00ff00u);
    CHECK_EQ(out[2], 0xff0000ffu);
    CHECK_EQ(out[3], 0xffffffffu);
}

TEST(reading_past_the_end_of_vram_gives_black_not_a_crash) {
    Bus bus;
    Vdlp vdlp(bus);

    u32 out[8] = {};
    // Start two pixels short of the end of VRAM and ask for eight.
    vdlp.render_linear(out, 8, 1, kVramSize - 4);
    CHECK_EQ(out[7], 0xff000000u);
}

// ---------------------------------------------------------------------------
// The display list
// ---------------------------------------------------------------------------

TEST(no_display_list_gives_a_black_field_not_noise) {
    Bus bus;
    Vdlp vdlp(bus);

    // Put something bright in VRAM at offset zero. Without a list it must not
    // appear: an unconfigured machine should look unconfigured, not broken.
    for (u32 i = 0; i < 64; i += 2) {
        put_pixel(bus, i, rgb555(31, 0, 31));
    }

    u32 out[16] = {};
    vdlp.render_field(out, 4, 4);
    for (int i = 0; i < 16; ++i) {
        CHECK_EQ(out[i], 0xff000000u);
    }
    CHECK_EQ(vdlp.entries_walked(), 0u);
}

TEST(a_single_entry_covers_the_whole_field) {
    Bus bus;
    Vdlp vdlp(bus);

    const u32 list = 0x1000u;              // the VDL lives in DRAM
    const u32 framebuffer = kVramBase;     // and points at the start of VRAM

    bus.write32(list + 0, kVdlCurrOverride | 1);   // sets the buffer, two lines
    bus.write32(list + 4, framebuffer);    // current buffer
    bus.write32(list + 8, framebuffer);    // previous buffer
    bus.write32(list + 12, 0);             // no next entry

    // Two lines of two pixels, placed through the interleaved layout.
    put_fb(bus, 0, 0, 0, 2, rgb555(31, 0, 0));
    put_fb(bus, 0, 1, 0, 2, rgb555(0, 31, 0));
    put_fb(bus, 0, 0, 1, 2, rgb555(0, 0, 31));
    put_fb(bus, 0, 1, 1, 2, rgb555(31, 31, 31));

    vdlp.set_list_address(list);
    u32 out[4] = {};
    vdlp.render_field(out, 2, 2);

    CHECK_EQ(vdlp.entries_walked(), 1u);
    CHECK_EQ(out[0], 0xffff0000u);
    CHECK_EQ(out[1], 0xff00ff00u);
    CHECK_EQ(out[2], 0xff0000ffu);
    CHECK_EQ(out[3], 0xffffffffu);
}

TEST(the_list_is_followed_to_a_second_entry) {
    Bus bus;
    Vdlp vdlp(bus);

    const u32 first  = 0x1000u;
    const u32 second = 0x1100u;

    bus.write32(first + 0, kVdlCurrOverride | 0);   // one line
    bus.write32(first + 4, kVramBase);
    bus.write32(first + 12, second);

    bus.write32(second + 0, kVdlCurrOverride | 0);  // one line, further into VRAM
    bus.write32(second + 4, kVramBase + 0x100);
    bus.write32(second + 12, 0);

    put_fb(bus, 0, 0, 0, 1, rgb555(31, 0, 0));
    put_fb(bus, 0x100, 0, 0, 1, rgb555(0, 0, 31));

    vdlp.set_list_address(first);
    u32 out[2] = {};
    vdlp.render_field(out, 1, 2);

    CHECK_EQ(vdlp.entries_walked(), 2u);
    CHECK_EQ(out[0], 0xffff0000u);
    CHECK_EQ(out[1], 0xff0000ffu);
}

TEST(a_list_that_points_at_itself_terminates) {
    // A malformed list must not hang the emulator. Games do write garbage here
    // during startup, and an infinite walk would look like a freeze.
    Bus bus;
    Vdlp vdlp(bus);

    const u32 list = 0x1000u;
    bus.write32(list + 0, kVdlCurrOverride | 0);
    bus.write32(list + 4, kVramBase);
    bus.write32(list + 12, list);   // next points back at itself

    vdlp.set_list_address(list);
    u32 out[4] = {};
    vdlp.render_field(out, 2, 2);
    CHECK_EQ(vdlp.entries_walked(), 1u);
}

TEST(an_entry_claiming_no_lines_does_not_spin) {
    Bus bus;
    Vdlp vdlp(bus);

    const u32 list = 0x1000u;
    bus.write32(list + 0, kVdlCurrOverride | 0);   // one line, then the next entry
    bus.write32(list + 4, kVramBase);
    bus.write32(list + 12, list);    // and loops

    vdlp.set_list_address(list);
    u32 out[16] = {};
    vdlp.render_field(out, 4, 4);
    CHECK_EQ(vdlp.entries_walked(), 1u);
}

TEST(a_frame_from_the_console_is_black_before_anything_configures_video) {
    Console console;
    console.reset();
    console.run_frame();

    const Frame frame = console.framebuffer();
    CHECK(frame.pixels != nullptr);
    CHECK_EQ(frame.pixels[0], 0xff000000u);
}

// ---------------------------------------------------------------------------
// Whole-path integration
// ---------------------------------------------------------------------------

TEST(a_pattern_written_through_the_bus_comes_back_out_of_the_frame) {
    // The same thing the app's "Test pattern" button does, minus the window.
    // It crosses every part of the display path — bus write, VRAM, display
    // list, VDLP walk, pixel expansion — so a break anywhere fails here rather
    // than showing up as a black screen on a device.
    Console console;
    console.reset();

    const Frame shape = console.framebuffer();
    Bus& bus = console.bus();

    for (int y = 0; y < shape.height; ++y) {
        for (int x = 0; x < shape.width; ++x) {
            const unsigned r = static_cast<unsigned>(x * 31 / (shape.width - 1));
            const unsigned g = static_cast<unsigned>(y * 31 / (shape.height - 1));
            const unsigned b = 31u - r;
            bus.write16(kVramBase + framebuffer_offset(x, y, shape.width),
                        static_cast<u16>((r << 10) | (g << 5) | b));
        }
    }

    const u32 list = 0x1000u;
    bus.write32(list + 0, kVdlCurrOverride | static_cast<u32>(shape.height - 1));
    bus.write32(list + 4, kVramBase);
    bus.write32(list + 8, kVramBase);
    bus.write32(list + 12, 0);
    // Through MADAM's register, as the machine does: the console reads the
    // display-list address from there each field.
    bus.write32(kMadamBase + kMadamVdlAddress, list);

    console.run_frame();

    const Frame frame = console.framebuffer();
    CHECK_EQ(frame.width, 320);
    CHECK_EQ(frame.height, 240);

    // Top-left is red-free and full blue; top-right is full red and no blue.
    CHECK_EQ(frame.pixels[0], 0xff0000ffu);
    CHECK_EQ(frame.pixels[frame.width - 1], 0xffff0000u);

    // Green rises down the screen, so the bottom row must be brighter in green
    // than the top. This is the assertion that catches a flipped image.
    const u32 top_green = (frame.pixels[0] >> 8) & 0xffu;
    const u32 bottom_green =
        (frame.pixels[static_cast<size_t>(frame.height - 1) * frame.width] >> 8) & 0xffu;
    CHECK(bottom_green > top_green);
}

// ---------------------------------------------------------------------------
// The framebuffer layout
// ---------------------------------------------------------------------------

TEST(the_framebuffer_is_interleaved_by_line_pairs) {
    // Each 32-bit word holds two pixels at the same x from two adjacent lines:
    // the even line in the high half, the odd line in the low half.
    const int width = 320;

    // Same x, adjacent lines: two halves of one word, two bytes apart.
    CHECK_EQ(framebuffer_offset(0, 0, width), 0u);
    CHECK_EQ(framebuffer_offset(0, 1, width), 2u);

    // Adjacent x on one line: four bytes apart, not two.
    CHECK_EQ(framebuffer_offset(1, 0, width), 4u);
    CHECK_EQ(framebuffer_offset(1, 1, width), 6u);

    // A pair of lines occupies width words.
    CHECK_EQ(framebuffer_offset(0, 2, width), static_cast<u32>(width) * 4u);
    CHECK_EQ(framebuffer_offset(0, 3, width), static_cast<u32>(width) * 4u + 2u);
}

TEST(reading_the_framebuffer_linearly_is_the_bug_that_looks_like_a_stride_error) {
    // This is a regression test for a real failure, kept because the symptom is
    // so distinctive: treating the framebuffer as linear made the boot logo come
    // out squashed to half width, duplicated across the screen, and with every
    // other line black. It reads as a stride bug, and it is a pixel-order one.
    Bus bus;
    Vdlp vdlp(bus);

    const int width = 4;
    const int height = 2;

    // A single red pixel at (2, 1) - an odd line, so the low half of a word.
    put_pixel(bus, framebuffer_offset(2, 1, width), rgb555(31, 0, 0));

    const u32 list = 0x1000u;
    bus.write32(list + 0, kVdlCurrOverride | static_cast<u32>(height - 1));
    bus.write32(list + 4, kVramBase);
    bus.write32(list + 12, 0);
    vdlp.set_list_address(list);

    u32 out[width * height] = {};
    vdlp.render_field(out, width, height);

    CHECK_EQ(out[1 * width + 2], 0xffff0000u);
    // And nowhere else.
    int lit = 0;
    for (int i = 0; i < width * height; ++i) {
        if ((out[i] & 0xffffffu) != 0) ++lit;
    }
    CHECK_EQ(lit, 1);
}

TEST(madam_and_the_display_agree_about_where_a_pixel_goes) {
    // The seam between the two chips. They compute the framebuffer offset with
    // the same function precisely so this cannot drift; when it did drift, this
    // shape of test is what caught it.
    Console console;
    console.reset();
    Bus& bus = console.bus();

    const Frame shape = console.framebuffer();
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            bus.write16(kVramBase + framebuffer_offset(x, y, shape.width),
                        rgb555(31, 31, 31));
        }
    }

    const u32 list = 0x2000u;
    bus.write32(list + 0, kVdlCurrOverride | static_cast<u32>(shape.height - 1));
    bus.write32(list + 4, kVramBase);
    bus.write32(list + 12, 0);
    bus.write32(kMadamBase + kMadamVdlAddress, list);

    console.run_frame();
    const Frame frame = console.framebuffer();
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            CHECK_EQ(frame.pixels[y * frame.width + x], 0xffffffffu);
        }
    }
}
