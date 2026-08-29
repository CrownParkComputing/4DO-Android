// MADAM tests: CCB decoding, the fixed-point mapping, and the pixel formats.
//
// The mapping tests matter most. A cel is placed by 16.16 fixed-point deltas,
// and an error of one bit in the fraction is invisible on a still image and
// obvious once anything moves — so position, scaling and the row-to-row bend
// are each asserted separately rather than through one composite picture.
#include "core/bus.h"
#include "core/console.h"
#include "core/madam.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

constexpr s32 fixed(int whole) { return static_cast<s32>(whole) << 16; }

constexpr u16 rgb555(unsigned r, unsigned g, unsigned b) {
    return static_cast<u16>((r << 10) | (g << 5) | b);
}

// Somewhere in DRAM to build test structures.
constexpr u32 kCcbAt    = 0x1000;
constexpr u32 kSourceAt = 0x2000;
constexpr u32 kPlutAt   = 0x3000;

// Build a CCB with absolute pointers, which keeps the tests about the renderer
// rather than about relative-pointer arithmetic (which has its own test).
void write_ccb(Bus& bus, u32 at, u32 flags, u32 source, u32 plut,
               s32 x, s32 y, s32 hdx, s32 hdy, s32 vdx, s32 vdy,
               u32 pre0, u32 pre1, s32 hddx = 0, s32 hddy = 0) {
    bus.write32(at + 0,  flags | kCcbLast | kCcbNpAbs | kCcbSpAbs | kCcbPpAbs);
    bus.write32(at + 4,  0);
    bus.write32(at + 8,  source);
    bus.write32(at + 12, plut);
    bus.write32(at + 16, static_cast<u32>(x));
    bus.write32(at + 20, static_cast<u32>(y));
    bus.write32(at + 24, static_cast<u32>(hdx));
    bus.write32(at + 28, static_cast<u32>(hdy));
    bus.write32(at + 32, static_cast<u32>(vdx));
    bus.write32(at + 36, static_cast<u32>(vdy));
    bus.write32(at + 40, static_cast<u32>(hddx));
    bus.write32(at + 44, static_cast<u32>(hddy));
    bus.write32(at + 48, 0);
    bus.write32(at + 52, pre0);
    bus.write32(at + 56, pre1);
}

// PRE0 carries the format in the low bits and (height - 1) above it; PRE1
// carries (width - 1).
u32 pre0_for(u32 format_code, u32 height) {
    return format_code | ((height - 1u) << 6);
}
u32 pre1_for(u32 width) { return width - 1u; }

constexpr u32 kFormatDirect16 = 6;
constexpr u32 kFormatIndexed8 = 5;
constexpr u32 kFormatIndexed4 = 3;

u16 read_target(Bus& bus, int x, int y, int stride_pixels = 320) {
    return bus.read16(kVramBase + static_cast<u32>(y * stride_pixels + x) * 2u);
}

}  // namespace

// ---------------------------------------------------------------------------
// Format decoding
// ---------------------------------------------------------------------------

TEST(the_pixel_format_is_decoded_from_pre0) {
    CHECK(cel_format_from_pre0(1) == CelFormat::Indexed1);
    CHECK(cel_format_from_pre0(3) == CelFormat::Indexed4);
    CHECK(cel_format_from_pre0(5) == CelFormat::Indexed8);
    CHECK(cel_format_from_pre0(6) == CelFormat::Direct16);
    CHECK(cel_format_from_pre0(0) == CelFormat::Unknown);
    CHECK(cel_format_from_pre0(7) == CelFormat::Unknown);
}

TEST(a_ccb_reports_its_dimensions) {
    Bus bus;
    Madam madam(bus);
    write_ccb(bus, kCcbAt, 0, kSourceAt, kPlutAt, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 12), pre1_for(20));

    const Ccb ccb = madam.read_ccb(kCcbAt);
    CHECK_EQ(ccb.width, 20u);
    CHECK_EQ(ccb.height, 12u);
    CHECK(ccb.format == CelFormat::Direct16);
}

TEST(a_relative_pointer_is_taken_from_the_word_after_it) {
    // Relative pointers are measured from the word following the field that
    // holds them. Getting that base wrong shifts every cel by a constant, which
    // reads as a mysterious offset rather than as a pointer bug.
    Bus bus;
    Madam madam(bus);

    // Absolute flags cleared, so the source offset is relative.
    bus.write32(kCcbAt + 0, kCcbLast);
    bus.write32(kCcbAt + 8, 0x40);   // source offset

    const Ccb ccb = madam.read_ccb(kCcbAt);
    CHECK_EQ(ccb.source_address, kCcbAt + 12u + 0x40u);
}

// ---------------------------------------------------------------------------
// Direct 16-bit cels
// ---------------------------------------------------------------------------

TEST(a_direct_cel_lands_where_it_is_told) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    // A 2x2 cel of four distinct colours.
    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    bus.write16(kSourceAt + 2, rgb555(0, 31, 0));
    bus.write16(kSourceAt + 4, rgb555(0, 0, 31));
    bus.write16(kSourceAt + 6, rgb555(31, 31, 31));

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(10), fixed(20),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 2), pre1_for(2));

    madam.render_cel_list(kCcbAt);

    CHECK_EQ(madam.stats().cels_drawn, 1u);
    CHECK_EQ(read_target(bus, 10, 20), rgb555(31, 0, 0));
    CHECK_EQ(read_target(bus, 11, 20), rgb555(0, 31, 0));
    CHECK_EQ(read_target(bus, 10, 21), rgb555(0, 0, 31));
    CHECK_EQ(read_target(bus, 11, 21), rgb555(31, 31, 31));
}

TEST(colour_zero_is_transparent) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    // Pre-fill the destination so a transparent pixel is visible as "unchanged".
    bus.write16(kVramBase + (0 * 320 + 0) * 2, rgb555(0, 31, 0));
    bus.write16(kVramBase + (0 * 320 + 1) * 2, rgb555(0, 31, 0));

    bus.write16(kSourceAt + 0, 0);                 // transparent
    bus.write16(kSourceAt + 2, rgb555(31, 0, 0));  // opaque

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(2));
    madam.render_cel_list(kCcbAt);

    CHECK_EQ(read_target(bus, 0, 0), rgb555(0, 31, 0));   // survived
    CHECK_EQ(read_target(bus, 1, 0), rgb555(31, 0, 0));   // overwritten
}

TEST(a_cel_is_clipped_rather_than_writing_out_of_bounds) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(4, 4);
    madam.set_target(kVramBase, 4 * 2);

    for (u32 i = 0; i < 16; ++i) {
        bus.write16(kSourceAt + i * 2, rgb555(31, 31, 31));
    }

    // Placed so that most of it falls off the right and bottom edges.
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(2), fixed(2),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 4), pre1_for(4));
    madam.render_cel_list(kCcbAt);

    CHECK_EQ(read_target(bus, 2, 2, 4), rgb555(31, 31, 31));
    CHECK_EQ(read_target(bus, 3, 3, 4), rgb555(31, 31, 31));
    // Only the four visible pixels were written.
    CHECK_EQ(madam.stats().pixels_written, 4u);
}

// ---------------------------------------------------------------------------
// The mapping
// ---------------------------------------------------------------------------

TEST(a_doubled_horizontal_step_stretches_the_cel) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    bus.write16(kSourceAt + 2, rgb555(0, 0, 31));

    // Two source pixels stepping two destination pixels each.
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(2), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(2));
    madam.render_cel_list(kCcbAt);

    CHECK_EQ(read_target(bus, 0, 0), rgb555(31, 0, 0));
    CHECK_EQ(read_target(bus, 2, 0), rgb555(0, 0, 31));
}

TEST(fractional_steps_accumulate_rather_than_rounding_each_time) {
    // With a half-pixel step, two source pixels must land on destination 0 and
    // 0 again — the fraction accumulates. Rounding per pixel instead would put
    // them at 0 and 1, which is the classic off-by-a-fraction scaling bug.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    bus.write16(kSourceAt + 2, rgb555(0, 0, 31));

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1) / 2, 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(2));
    madam.render_cel_list(kCcbAt);

    // The second pixel overwrote the first at the same destination.
    CHECK_EQ(read_target(bus, 0, 0), rgb555(0, 0, 31));
    CHECK_EQ(read_target(bus, 1, 0), 0u);
}

TEST(a_vertical_delta_can_place_rows_diagonally) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    bus.write16(kSourceAt + 2, rgb555(0, 0, 31));

    // One pixel per row, with each row stepping right as well as down. This is
    // a shear, and it is the thing a plain blitter cannot do.
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0,
              fixed(1), fixed(1),
              pre0_for(kFormatDirect16, 2), pre1_for(1));
    madam.render_cel_list(kCcbAt);

    CHECK_EQ(read_target(bus, 0, 0), rgb555(31, 0, 0));
    CHECK_EQ(read_target(bus, 1, 1), rgb555(0, 0, 31));
}

TEST(the_horizontal_step_bends_as_rows_advance) {
    // hddx is what makes a cel a general quad rather than a parallelogram: the
    // horizontal step itself changes from row to row.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    for (u32 i = 0; i < 4; ++i) {
        bus.write16(kSourceAt + i * 2, rgb555(31, 31, 31));
    }

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0,
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 2), pre1_for(2),
              /*hddx=*/fixed(1), /*hddy=*/0);
    madam.render_cel_list(kCcbAt);

    // Row 0 steps by one; row 1 steps by two, so its second pixel is at x = 2.
    CHECK_EQ(read_target(bus, 1, 0), rgb555(31, 31, 31));
    CHECK_EQ(read_target(bus, 2, 1), rgb555(31, 31, 31));
}

// ---------------------------------------------------------------------------
// Indexed formats
// ---------------------------------------------------------------------------

TEST(an_eight_bit_indexed_cel_goes_through_the_palette) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    // Palette: entry 1 red, entry 2 blue.
    bus.write16(kPlutAt + 1 * 2, rgb555(31, 0, 0));
    bus.write16(kPlutAt + 2 * 2, rgb555(0, 0, 31));

    bus.write8(kSourceAt + 0, 1);
    bus.write8(kSourceAt + 1, 2);

    write_ccb(bus, kCcbAt, 0, kSourceAt, kPlutAt, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatIndexed8, 1), pre1_for(2));
    madam.render_cel_list(kCcbAt);

    CHECK_EQ(read_target(bus, 0, 0), rgb555(31, 0, 0));
    CHECK_EQ(read_target(bus, 1, 0), rgb555(0, 0, 31));
}

TEST(a_four_bit_indexed_cel_unpacks_two_pixels_per_byte) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    bus.write16(kPlutAt + 1 * 2, rgb555(31, 0, 0));
    bus.write16(kPlutAt + 2 * 2, rgb555(0, 0, 31));

    // High nibble first: index 1 then index 2.
    bus.write8(kSourceAt + 0, 0x12);

    write_ccb(bus, kCcbAt, 0, kSourceAt, kPlutAt, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatIndexed4, 1), pre1_for(2));
    madam.render_cel_list(kCcbAt);

    CHECK_EQ(read_target(bus, 0, 0), rgb555(31, 0, 0));
    CHECK_EQ(read_target(bus, 1, 0), rgb555(0, 0, 31));
}

// ---------------------------------------------------------------------------
// Walking the list
// ---------------------------------------------------------------------------

TEST(the_list_stops_at_the_last_flag) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    madam.render_cel_list(kCcbAt);
    CHECK_EQ(madam.stats().cels_walked, 1u);
    CHECK_EQ(madam.stats().list_truncated, 0u);
}

TEST(a_second_cel_paints_over_the_first) {
    // Order matters, and this is why cels cannot simply be handed to separate
    // threads: the later one must win.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    const u32 first = kCcbAt;
    const u32 second = kCcbAt + 0x100;

    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    bus.write16(kSourceAt + 2, rgb555(0, 0, 31));

    // First cel: red at the origin, chaining to the second.
    write_ccb(bus, first, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    bus.write32(first + 0, kCcbNpAbs | kCcbSpAbs | kCcbPpAbs);  // clear LAST
    bus.write32(first + 4, second);

    // Second cel: blue, same place, and it is last.
    write_ccb(bus, second, 0, kSourceAt + 2, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));

    madam.render_cel_list(first);

    CHECK_EQ(madam.stats().cels_walked, 2u);
    CHECK_EQ(read_target(bus, 0, 0), rgb555(0, 0, 31));
}

TEST(a_self_referencing_list_terminates) {
    Bus bus;
    Madam madam(bus);
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    bus.write32(kCcbAt + 0, kCcbNpAbs | kCcbSpAbs | kCcbPpAbs);  // not LAST
    bus.write32(kCcbAt + 4, kCcbAt);                             // points at itself

    madam.render_cel_list(kCcbAt);
    CHECK_EQ(madam.stats().cels_walked, 1u);
    CHECK_EQ(madam.stats().list_truncated, 0u);
}

TEST(the_largest_expressible_cel_is_still_bounded) {
    // The size fields are ten bits each, so a cel cannot claim to be larger
    // than 1024 in either direction however corrupt the CCB is. That is what
    // actually protects against a "draw billions of pixels" hang; the explicit
    // dimension guard in the renderer is defence in case the field widths --
    // which are still TODO(madam) -- turn out to be wider than this.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(4, 4);
    madam.set_target(kVramBase, 4 * 2);

    // Source pixels for the only part that can be visible. The cel claims to be
    // 1024 wide, so its rows are 1024 pixels apart.
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            bus.write16(kSourceAt + (y * 1024u + x) * 2u, rgb555(31, 31, 31));
        }
    }

    // Every bit set in both size fields.
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    bus.write32(kCcbAt + 52, kFormatDirect16 | 0xffffffc0u);
    bus.write32(kCcbAt + 56, 0xffffffffu);

    const Ccb ccb = madam.read_ccb(kCcbAt);
    CHECK(ccb.width <= 1024u);
    CHECK(ccb.height <= 1024u);

    madam.render_cel_list(kCcbAt);
    // It draws, but clipping means only the visible 4x4 is ever written.
    CHECK_EQ(madam.stats().pixels_written, 16u);
}

TEST(an_unknown_pixel_format_draws_nothing) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(0, 4), pre1_for(4));
    madam.render_cel_list(kCcbAt);
    CHECK_EQ(madam.stats().cels_drawn, 0u);
}

// ---------------------------------------------------------------------------
// Through the machine
// ---------------------------------------------------------------------------

TEST(writing_the_start_register_runs_the_list) {
    Console console;
    console.reset();
    Bus& bus = console.bus();

    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(5), fixed(5),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));

    // The CPU would do this; here the test writes the register directly.
    bus.write32(kMadamBase + kMadamCelStart, kCcbAt);

    CHECK_EQ(console.madam().stats().cels_drawn, 1u);
    CHECK_EQ(read_target(bus, 5, 5), rgb555(31, 0, 0));
}

TEST(a_cel_drawn_into_vram_appears_in_the_frame) {
    // MADAM writes VRAM, VDLP reads it. This asserts the two agree about where
    // a pixel lives — the seam between them is exactly where an off-by-a-row
    // bug would hide.
    Console console;
    console.reset();
    Bus& bus = console.bus();

    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(3), fixed(7),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    bus.write32(kMadamBase + kMadamCelStart, kCcbAt);

    const u32 list = 0x8000u;
    bus.write32(list + 0, 240);
    bus.write32(list + 4, kVramBase);
    bus.write32(list + 8, kVramBase);
    bus.write32(list + 12, 0);
    console.vdlp().set_list_address(list);

    console.run_frame();

    const Frame frame = console.framebuffer();
    CHECK_EQ(frame.pixels[7 * frame.width + 3], 0xffff0000u);
}

// ---------------------------------------------------------------------------
// Magnification
// ---------------------------------------------------------------------------

TEST(a_magnified_cel_is_solid_rather_than_a_grid_of_dots) {
    // Forward mapping walks SOURCE pixels, so writing one destination pixel per
    // source pixel leaves holes as soon as a cel is scaled up — a magnified
    // sprite comes out as a dot grid. Each source pixel therefore fills its
    // whole destination footprint. This is a regression test for that, because
    // it is the kind of thing that looks like a texture effect rather than a
    // bug.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    for (u32 i = 0; i < 4; ++i) {
        bus.write16(kSourceAt + i * 2, rgb555(31, 31, 31));
    }

    // A 2x2 cel blown up four times in each direction: an 8x8 solid block.
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0,
              fixed(4), 0, 0, fixed(4),
              pre0_for(kFormatDirect16, 2), pre1_for(2));
    madam.render_cel_list(kCcbAt);

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK_EQ(read_target(bus, x, y), rgb555(31, 31, 31));
        }
    }
}

TEST(a_one_to_one_cel_still_writes_exactly_one_pixel_each) {
    // The footprint fill must not cost anything when there is no magnification,
    // or every unscaled cel in every game pays for a feature it does not use.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    for (u32 i = 0; i < 16; ++i) {
        bus.write16(kSourceAt + i * 2, rgb555(31, 31, 31));
    }

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 4), pre1_for(4));
    madam.render_cel_list(kCcbAt);

    CHECK_EQ(madam.stats().pixels_written, 16u);
}

TEST(a_rotated_magnified_cel_has_no_holes_along_its_diagonal) {
    // The footprint follows the step vectors, so it stays correct when the cel
    // is rotated as well as scaled — a rectangular fill would leave gaps here.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    for (u32 i = 0; i < 16; ++i) {
        bus.write16(kSourceAt + i * 2, rgb555(31, 31, 31));
    }

    // Roughly 45 degrees, scaled by three.
    const s32 step = fixed(2);
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(40), fixed(40),
              step, step, -step, step,
              pre0_for(kFormatDirect16, 4), pre1_for(4));
    madam.render_cel_list(kCcbAt);

    // Walk the first source row's destination path; every step must be painted.
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(read_target(bus, 40 + i, 40 + i), rgb555(31, 31, 31));
    }
}
