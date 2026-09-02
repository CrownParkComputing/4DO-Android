// MADAM tests: CCB decoding, the fixed-point mapping, and the pixel formats.
//
// The mapping tests matter most. A cel is placed by 16.16 fixed-point deltas,
// and an error of one bit in the fraction is invisible on a still image and
// obvious once anything moves — so position, scaling and the row-to-row bend
// are each asserted separately rather than through one composite picture.
#include "core/bus.h"
#include "core/console.h"
#include "core/madam.h"
#include "core/vdlp.h"
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
// The step vectors are given here in 16.16 throughout, because that is how it
// is natural to think about them. The horizontal ones are stored as 12.20,
// which is what the hardware reads, so this shifts them on the way in.
void write_ccb(Bus& bus, u32 at, u32 flags, u32 source, u32 plut,
               s32 x, s32 y, s32 hdx, s32 hdy, s32 vdx, s32 vdy,
               u32 pre0, u32 pre1, s32 hddx = 0, s32 hddy = 0) {
    hdx  <<= 4;
    hdy  <<= 4;
    hddx <<= 4;
    hddy <<= 4;
    // A cel that wants a palette has to say so; one that does not draws with
    // whatever the last cel left loaded.
    //
    // CCBPRE, because these helpers write the preamble into the CCB. Without
    // it the hardware takes the preamble from the front of the source data
    // instead, and the cel would be described by whatever the pixels happen to
    // begin with.
    //
    // Both winding flags, meaning either way round is acceptable. A CCB with
    // neither is not drawn by the hardware at all, so a test cel without them
    // would be testing nothing - and every cel in every title measured here
    // sets at least one.
    bus.write32(at + 0,  flags | kCcbLast | kCcbNpAbs | kCcbSpAbs | kCcbPpAbs |
                             kCcbLdSize | kCcbLdPrs | kCcbLdPpmp | kCcbLdPlut |
                             kCcbCcbPre | kCcbYoxy | kCcbAcw | kCcbAccw);
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
// A cel whose pixels carry their colour directly rather than an index has to
// say so: that is the LINEAR bit, and without it even a sixteen-bit pixel goes
// through the palette.
constexpr u32 kPre0Linear = 0x00000010u;

u32 pre0_for(u32 format_code, u32 height) {
    const u32 linear = format_code == 6 ? kPre0Linear : 0u;
    return format_code | linear | ((height - 1u) << 6);
}
// A cel's rows are strided by a word count carried in PRE1, and the smallest
// stride the field can express is two words. So a row is never shorter than
// eight bytes however narrow the cel is - which is why the tests below space
// their rows out rather than packing them.
u32 row_words(u32 width, u32 bpp) {
    const u32 needed = (width * bpp + 31u) / 32u;
    return needed < 2u ? 2u : needed;
}
u32 row_bytes(u32 width, u32 bpp) { return row_words(width, bpp) * 4u; }

u32 pre1_for(u32 width, u32 bpp = 16) {
    const u32 offset = row_words(width, bpp) - 2u;
    const u32 field = bpp < 8 ? (offset << 24) : (offset << 16);
    return (width - 1u) | field;
}

constexpr u32 kFormatDirect16 = 6;
constexpr u32 kFormatIndexed8 = 5;
constexpr u32 kFormatIndexed4 = 3;

// The framebuffer is interleaved, so a pixel is not at y*width + x. Using the
// same helper the chips use keeps the tests honest about the real layout.
u16 read_target(Bus& bus, int x, int y, int stride_pixels = 320) {
    return bus.read16(kVramBase + framebuffer_offset(x, y, stride_pixels));
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

    // Absolute flags cleared, so the source offset is relative. CCBPRE set, so
    // the preamble comes from the CCB and does not eat the first words of the
    // source - which is a separate behaviour with its own test.
    bus.write32(kCcbAt + 0, kCcbLast | kCcbCcbPre);
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
    bus.write16(kSourceAt + row_bytes(2, 16) + 0, rgb555(0, 0, 31));
    bus.write16(kSourceAt + row_bytes(2, 16) + 2, rgb555(31, 31, 31));

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

TEST(an_lrform_cel_reads_the_two_rows_from_each_source_word) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    // The official LR layout interleaves a pair of rows by pixel:
    // [row0 x0, row1 x0], [row0 x1, row1 x1].
    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    bus.write16(kSourceAt + 2, rgb555(0, 0, 31));
    bus.write16(kSourceAt + 4, rgb555(0, 31, 0));
    bus.write16(kSourceAt + 6, rgb555(31, 31, 31));

    constexpr u32 kLrform = 1u << 11;
    // VCNT zero means one pair, hence two output rows in LR form.
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(10), fixed(20),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(2) | kLrform);
    madam.render_cel_list(kCcbAt);

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
    bus.write16(kVramBase + framebuffer_offset(0, 0, 320), rgb555(0, 31, 0));
    bus.write16(kVramBase + framebuffer_offset(1, 0, 320), rgb555(0, 31, 0));

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

    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            bus.write16(kSourceAt + y * row_bytes(4, 16) + x * 2,
                        rgb555(31, 31, 31));
        }
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
    bus.write16(kSourceAt + row_bytes(1, 16), rgb555(0, 0, 31));

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

    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            bus.write16(kSourceAt + y * row_bytes(2, 16) + x * 2,
                        rgb555(31, 31, 31));
        }
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
              pre0_for(kFormatIndexed8, 1), pre1_for(2, 8));
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
              pre0_for(kFormatIndexed4, 1), pre1_for(2, 4));
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
    bus.write32(first + 0, bus.read32(first) & ~kCcbLast);
    bus.write32(first + 4, second);

    // Second cel: blue, same place, and it is last.
    write_ccb(bus, second, 0, kSourceAt + 2, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));

    madam.render_cel_list(first);

    CHECK_EQ(madam.stats().cels_walked, 2u);
    CHECK_EQ(read_target(bus, 0, 0), rgb555(0, 0, 31));
}

TEST(a_skipped_cel_can_load_the_palette_for_the_cel_after_it) {
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    const u32 first = kCcbAt;
    const u32 second = kCcbAt + 0x100;
    bus.write8(kSourceAt, 1u);
    bus.write8(kSourceAt + 8, 1u);
    bus.write16(kPlutAt + 2, rgb555(31, 0, 0));

    write_ccb(bus, first, kCcbSkip, kSourceAt, kPlutAt,
              fixed(1), fixed(1), fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatIndexed8, 1), pre1_for(1, 8));
    bus.write32(first, bus.read32(first) & ~kCcbLast);
    bus.write32(first + 4, second);

    write_ccb(bus, second, 0, kSourceAt + 8, kPlutAt + 0x100,
              fixed(5), fixed(6), fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatIndexed8, 1), pre1_for(1, 8));
    bus.write32(second, bus.read32(second) & ~kCcbLdPlut);

    madam.render_cel_list(first);
    CHECK_EQ(madam.stats().cels_walked, 2u);
    CHECK_EQ(madam.stats().cels_drawn, 1u);
    CHECK_EQ(read_target(bus, 5, 6), rgb555(31, 0, 0));
}

TEST(a_self_referencing_list_terminates) {
    Bus bus;
    Madam madam(bus);
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    bus.write32(kCcbAt + 0, bus.read32(kCcbAt) & ~kCcbLast);
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

    // Source pixels for the only part that can be visible. The rows are as
    // far apart as the cel's own stride field claims, which with every bit set
    // is a very long way indeed.
    const u32 huge_row = (0x3ffu + 2u) * 4u;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            bus.write16(kSourceAt + y * huge_row + x * 2u, rgb555(31, 31, 31));
        }
    }

    // Every bit set in both size fields.
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    // Every bit of the size fields, but no skip: the point here is the size
    // handling, and a skip of fifteen would quietly move which source pixels
    // are the visible ones.
    bus.write32(kCcbAt + 52, kFormatDirect16 | kPre0Linear | 0x0000ffc0u);
    // Every bit except LRFORM. This test is about the size fields, and bit 11
    // says the source is stored in the framebuffer's interleaved layout - which
    // is a different question and would change which bytes count as pixels.
    bus.write32(kCcbAt + 56, 0xffffffffu & ~0x800u);

    const Ccb ccb = madam.read_ccb(kCcbAt);
    // Eleven bits of width and ten of height, so these are the largest values
    // the fields can hold rather than round numbers.
    CHECK(ccb.width <= 2048u);
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
    // The list head goes in NEXTCCB; the start port only says "go".
    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);
    bus.write32(kMadamBase + kMadamCelStart, 0);

    // Writing the start port does not run the engine - it runs once the CPU
    // finishes the instruction that asked, which is what lets software write
    // the CCB after starting it. There is no CPU here, so the test reaches
    // that boundary itself.
    CHECK_EQ(console.madam().stats().cels_drawn, 0u);
    bus.run_pending_cel_engine();

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
    // Drawn far enough down the buffer to land on the visible part of the
    // screen: the display list runs through the vertical blank first, so the
    // top of the screen is some way into the buffer.
    const int cel_row = 7 + 16;
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(3), fixed(cel_row),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    // The list head goes in NEXTCCB; the start port only says "go".
    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);
    bus.write32(kMadamBase + kMadamCelStart, 0);

    const u32 list = 0x8000u;
    bus.write32(list + 0, kVdlEnableDma | kVdlCurrOverride | 261u);
    bus.write32(list + 4, kVramBase);
    bus.write32(list + 8, kVramBase);
    bus.write32(list + 12, 0);
    // Through MADAM's register, as the machine does: the console reads the
    // display-list address from there each field.
    bus.write32(kMadamBase + kMadamVdlAddress, list);

    console.run_frame();

    // The screen starts some way down the buffer - the display list runs
    // through the vertical blank first - so a cel drawn at row seven appears
    // that many rows higher on screen.
    const Frame frame = console.framebuffer();
    const int row = cel_row - static_cast<int>(console.vdlp().buffer_start_line());
    CHECK(row >= 0);
    CHECK_EQ(frame.pixels[(row * 2) * frame.width + 3 * 2], 0xffff0000u);
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

    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            bus.write16(kSourceAt + y * row_bytes(2, 16) + x * 2,
                        rgb555(31, 31, 31));
        }
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

    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            bus.write16(kSourceAt + y * row_bytes(4, 16) + x * 2,
                        rgb555(31, 31, 31));
        }
    }

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, 0, 0, fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 4), pre1_for(4));
    madam.render_cel_list(kCcbAt);

    CHECK_EQ(madam.stats().pixels_written, 16u);
}

TEST(a_rotated_cel_lands_as_a_solid_figure_not_a_stack_of_rectangles) {
    // A rotated cel does not go through the scaling mapper at all. Each source
    // pixel lands as a four-sided figure, and the hardware fills it by
    // scanlines - which is the only way to tile the plane without either gaps
    // between the pixels or overlap along the diagonal.
    //
    // Approximating that with an upright rectangle per pixel, as this once
    // did, is not a small error. It cannot produce the shape below at all.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            bus.write16(kSourceAt + y * row_bytes(4, 16) + x * 2,
                        rgb555(31, 31, 31));
        }
    }

    // Forty-five degrees, scaled by two.
    const s32 step = fixed(2);
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(40), fixed(40),
              step, step, -step, step,
              pre0_for(kFormatDirect16, 4), pre1_for(4));
    madam.render_cel_list(kCcbAt);

    // Sixteen source pixels, each covering the parallelogram its two step
    // vectors span: |hd x vd| = |2*2 - 2*-2| = 8 pixels apiece.
    CHECK_EQ(madam.stats().pixels_written, 16u * 8u);

    // Solid: every row it touches is one unbroken run. A gap anywhere is the
    // failure this test exists to catch.
    int rows_touched = 0;
    int widest = 0;
    for (int y = 0; y < 240; ++y) {
        int first = -1;
        int last = -1;
        int painted = 0;
        for (int x = 0; x < 320; ++x) {
            if (read_target(bus, x, y) != rgb555(31, 31, 31)) continue;
            if (first < 0) first = x;
            last = x;
            ++painted;
        }
        if (first < 0) continue;
        ++rows_touched;
        CHECK_EQ(painted, last - first + 1);   // no holes in the run
        if (painted > widest) widest = painted;
    }

    // A diamond fifteen rows tall and sixteen wide at its middle.
    CHECK_EQ(rows_touched, 15);
    CHECK_EQ(widest, 16);
}

TEST(a_transparent_run_advances_both_edges_of_a_rotated_packed_cel) {
    // A packed transparent packet still consumes source columns. The
    // arbitrary mapper carries both the current row edge and the matching
    // edge on the next row; if only the first is advanced, the next visible
    // texel becomes a long quad spanning backwards across the transparent
    // run. Road Rash's leaned rider exposed this as repeated horizontal
    // strips while steering.
    Bus bus;
    Madam madam(bus);
    madam.set_clip(320, 240);
    madam.set_target(kVramBase, 320 * 2);

    // One eight-byte packed row: three transparent pixels, then one literal
    // palette index, then end-of-row. The zero length field names an
    // eight-byte row because packed lengths are stored as words-minus-two.
    bus.write8(kSourceAt + 0, 0x00);
    bus.write8(kSourceAt + 1, 0x00);
    bus.write8(kSourceAt + 2, 0x82);  // transparent, count 3
    bus.write8(kSourceAt + 3, 0x40);  // literal, count 1
    bus.write8(kSourceAt + 4, 0x01);  // PLUT entry 1
    bus.write8(kSourceAt + 5, 0x00);  // end of row
    bus.write16(kPlutAt + 2, rgb555(31, 31, 31));

    const s32 step = fixed(2);
    write_ccb(bus, kCcbAt, kCcbPacked, kSourceAt, kPlutAt,
              fixed(20), fixed(20), step, step, -step, step,
              pre0_for(kFormatIndexed8, 1), pre1_for(4, 8));
    madam.render_cel_list(kCcbAt);

    // Only the final source texel is visible, so it covers one 2x2-rotated
    // diamond (determinant eight), not a bridge back to the row origin.
    CHECK_EQ(madam.stats().pixels_written, 8u);
    CHECK_EQ(read_target(bus, 20, 23), 0u);
    CHECK_EQ(read_target(bus, 26, 27), rgb555(31, 31, 31));
}

// ---------------------------------------------------------------------------
// DMA channel registers
// ---------------------------------------------------------------------------

TEST(madam_reports_the_green_hardware_revision_and_keeps_it_read_only) {
    Bus bus;
    Madam madam(bus);
    bus.attach_madam(&madam);

    CHECK_EQ(bus.read32(kMadamBase + kMadamRevision), 0x01020000u);
    bus.write32(kMadamBase + kMadamRevision, 0);
    CHECK_EQ(bus.read32(kMadamBase + kMadamRevision), 0x01020000u);
    madam.reset();
    CHECK_EQ(bus.read32(kMadamBase + kMadamRevision), 0x01020000u);
}

TEST(dma_channels_are_address_and_length_pairs_eight_bytes_apart) {
    // The layout comes from the ROM's own driver, which writes channel 3 at
    // MADAM+0x218/0x21C and channel 7 at +0x238/0x23C from a base of 0x200.
    Bus bus;
    Madam madam(bus);
    bus.attach_madam(&madam);

    bus.write32(kMadamBase + 0x218, 0x00020C00u);
    bus.write32(kMadamBase + 0x21C, 0x00000040u);
    bus.write32(kMadamBase + 0x238, 0x00030000u);
    bus.write32(kMadamBase + 0x23C, 0x00000010u);

    CHECK_EQ(madam.dma_address(3), 0x00020C00u);
    CHECK_EQ(madam.dma_length(3), 0x00000040u);
    CHECK_EQ(madam.dma_address(7), 0x00030000u);
    CHECK_EQ(madam.dma_length(7), 0x00000010u);

    // And they read back through the bus.
    CHECK_EQ(bus.read32(kMadamBase + 0x218), 0x00020C00u);
    CHECK_EQ(bus.read32(kMadamBase + 0x23C), 0x00000010u);
}

TEST(dma_channels_do_not_disturb_each_other) {
    Bus bus;
    Madam madam(bus);
    bus.attach_madam(&madam);

    bus.write32(kMadamBase + 0x200, 0x11111111u);   // channel 0 address
    bus.write32(kMadamBase + 0x208, 0x22222222u);   // channel 1 address

    CHECK_EQ(madam.dma_address(0), 0x11111111u);
    CHECK_EQ(madam.dma_address(1), 0x22222222u);
    CHECK_EQ(madam.dma_length(0), 0u);
}

TEST(a_reset_clears_the_dma_channels) {
    Bus bus;
    Madam madam(bus);
    bus.attach_madam(&madam);

    bus.write32(kMadamBase + 0x218, 0xDEADBEEFu);
    madam.reset();
    CHECK_EQ(madam.dma_address(3), 0u);
}

TEST(a_reset_clears_madams_operational_registers_and_inherited_cel_state) {
    Bus bus;
    Madam madam(bus);
    bus.attach_madam(&madam);

    bus.write32(kMadamBase + kMadamDmaEnable, 1u);
    bus.write32(kMadamBase + kMadamXbusDmaAddress, 0x11110000u);
    bus.write32(kMadamBase + kMadamXbusDmaLength, 0x200u);
    bus.write32(kMadamBase + kMadamVdlAddress, 0x22220000u);
    bus.write32(kMadamBase + kMadamPbusAddress, 0x33330000u);
    bus.write32(kMadamBase + kMadamPbusLength, 0x80u);
    bus.write32(kMadamBase + kMadamPbusPointer, 0x20u);
    bus.write32(kMadamBase + kMadamCurrentCcb, 0x44440000u);
    bus.write32(kMadamBase + kMadamNextCcb, 0x55550000u);
    bus.write32(kMadamBase + kMadamCcbCtl0, 0x1234u);
    bus.write32(kMadamBase + kMadamRegCtl0, 0x0101u);
    bus.write32(kMadamBase + kMadamRegCtl1, 0x00ef013fu);
    bus.write32(kMadamBase + kMadamRegCtl2, 0x66660000u);
    bus.write32(kMadamBase + kMadamRegCtl3, 0x77770000u);
    bus.write32(kMadamBase + kMadamFifoBase, 0x88880000u);
    bus.write32(kMadamBase + kMadamFifoBase + 4, 0x40u);
    bus.write32(kMadamBase + kMadamMatrixIn, 0x1111u);
    bus.write32(kMadamBase + kMadamMatrixVec, 0x2222u);
    bus.write32(kMadamBase + kMadamMatrixNumHi, 0x3333u);
    madam.reset();

    CHECK_EQ(bus.read32(kMadamBase + kMadamDmaEnable), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamXbusDmaAddress), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamXbusDmaLength), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamVdlAddress), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamPbusAddress), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamPbusLength), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamPbusPointer), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamCurrentCcb), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamNextCcb), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamCcbCtl0), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamRegCtl0), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamRegCtl1), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamRegCtl2), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamRegCtl3), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamFifoBase), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamFifoBase + 4), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamMatrixIn), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamMatrixVec), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamMatrixNumHi), 0u);
}

TEST(the_start_port_ignores_the_value_written_to_it) {
    // None of the engine's control ports carries an address. Treating the
    // written value as the list head makes the engine draw from wherever the
    // last store happened to land, which is usually nothing at all.
    Console console;
    console.reset();
    Bus& bus = console.bus();
    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(5), fixed(5),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));

    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);
    bus.write32(kMadamBase + kMadamCelStart, 0xdeadbeef);
    bus.run_pending_cel_engine();
    CHECK_EQ(console.madam().stats().cels_drawn, 1u);
}

TEST(the_cel_status_distinguishes_running_suspended_and_idle) {
    Console console;
    console.reset();
    Bus& bus = console.bus();

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(5), fixed(5),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);

    CHECK_EQ(bus.read32(kMadamBase + kMadamCelStatus), 0u);
    bus.write32(kMadamBase + kMadamCelStart, 0);
    CHECK_EQ(bus.read32(kMadamBase + kMadamCelStatus), kMadamCelRunning);

    bus.write32(kMadamBase + kMadamCelPause, 0);
    CHECK_EQ(bus.read32(kMadamBase + kMadamCelStatus),
             kMadamCelRunning | kMadamCelPaused);
    CHECK(!bus.cel_engine_pending());

    bus.write32(kMadamBase + kMadamCelResume, 0);
    CHECK_EQ(bus.read32(kMadamBase + kMadamCelStatus), kMadamCelRunning);
    CHECK(bus.cel_engine_pending());
    bus.run_pending_cel_engine();
    CHECK_EQ(bus.read32(kMadamBase + kMadamCelStatus), 0u);
}

TEST(current_ccb_tracks_the_hardware_fetch_cursor) {
    Console console;
    console.reset();
    Bus& bus = console.bus();

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(5), fixed(5),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);
    bus.write32(kMadamBase + kMadamCelStart, 0);
    bus.run_pending_cel_engine();

    // Six fixed words + four size + two perspective + PIXC + two literal
    // preamble words = fifteen fetched words.
    CHECK_EQ(bus.read32(kMadamBase + kMadamCurrentCcb), kCcbAt + 15u * 4u);
}

TEST(stop_leaves_the_engine_idle_and_discards_a_suspended_walk) {
    Console console;
    console.reset();
    Bus& bus = console.bus();

    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);
    bus.write32(kMadamBase + kMadamCelStart, 0);
    bus.write32(kMadamBase + kMadamCelPause, 0);
    bus.write32(kMadamBase + kMadamCelStop, 0);

    CHECK_EQ(bus.read32(kMadamBase + kMadamCelStatus), 0u);
    CHECK_EQ(bus.read32(kMadamBase + kMadamNextCcb), 0u);
    CHECK(!bus.cel_engine_pending());
}

TEST(stopping_the_engine_clears_where_it_would_go_next) {
    Console console;
    console.reset();
    Bus& bus = console.bus();
    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(5), fixed(5),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));

    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);
    bus.write32(kMadamBase + kMadamCelStop, 0);
    bus.write32(kMadamBase + kMadamCelStart, 0);
    CHECK_EQ(console.madam().stats().cels_drawn, 0u);
}

TEST(a_cel_without_ccbpre_takes_its_preamble_from_the_front_of_its_source) {
    // CCBPRE says the two preamble words are carried in the CCB. Cleared, they
    // are the first words of the SOURCE DATA and the pixels begin after them,
    // which is how a title stores a cel as one self-describing blob and points
    // a bare CCB at it. Reading the CCB's own words in that case picks up
    // whatever follows the structure in memory, and the cel gets an invented
    // format and size.
    Bus bus;
    Madam madam(bus);

    bus.write32(kCcbAt + 0, kCcbLast | kCcbSpAbs);
    bus.write32(kCcbAt + 8, kSourceAt);
    // Deliberately different from the preamble in the source, so a reader that
    // looks in the wrong place cannot accidentally agree.
    bus.write32(kCcbAt + 52, pre0_for(kFormatDirect16, 99));
    bus.write32(kCcbAt + 56, pre1_for(99));

    bus.write32(kSourceAt + 0, pre0_for(kFormatDirect16, 12));
    bus.write32(kSourceAt + 4, pre1_for(20));

    const Ccb ccb = madam.read_ccb(kCcbAt);
    CHECK_EQ(ccb.width, 20u);
    CHECK_EQ(ccb.height, 12u);
    // And the pixels start after the two words it just consumed.
    CHECK_EQ(ccb.source_address, kSourceAt + 8u);
}

TEST(a_packed_cel_keeps_the_width_the_last_unpacked_cel_left_behind) {
    // PRE1 is a register, not a per-cel value. A packed cel never supplies one
    // - from the CCB or from its source - because its rows say how long they
    // are. The register still has to survive, because the next unpacked cel
    // may not reload it either.
    Bus bus;
    Madam madam(bus);

    bus.write32(kCcbAt + 0, kCcbLast | kCcbSpAbs | kCcbCcbPre);
    bus.write32(kCcbAt + 8, kSourceAt);
    bus.write32(kCcbAt + 24, pre0_for(kFormatDirect16, 4));
    bus.write32(kCcbAt + 28, pre1_for(37));
    CHECK_EQ(madam.read_ccb(kCcbAt).width, 37u);

    // Now a packed cel with a different PRE1 word sitting in the CCB, which it
    // must ignore.
    bus.write32(kCcbAt + 0, kCcbLast | kCcbSpAbs | kCcbCcbPre | kCcbPacked);
    bus.write32(kCcbAt + 28, pre1_for(5));
    CHECK_EQ(madam.read_ccb(kCcbAt).width, 37u);
}

TEST(a_compact_ccb_inherits_the_load_controlled_cel_registers) {
    Bus bus;
    Madam madam(bus);

    write_ccb(bus, kCcbAt, 0, kSourceAt, kPlutAt,
              fixed(13), fixed(17), fixed(2), fixed(3),
              fixed(4), fixed(5), pre0_for(kFormatDirect16, 6),
              pre1_for(7), fixed(8), fixed(9));
    bus.write32(kCcbAt + 48, 0x12345678u);

    const Ccb loaded = madam.read_ccb(kCcbAt);
    CHECK_EQ(loaded.x, fixed(13));
    CHECK_EQ(loaded.hdx, fixed(2));
    CHECK_EQ(loaded.pixc, 0x12345678u);

    // With all four load flags clear, the tail begins immediately after the
    // always-present X/Y slots. Those slots are ignored because YOXY is clear;
    // PRE0/PRE1 at +24/+28 still describe this cel, while every transform and
    // PIXC register remains from the preceding one.
    constexpr u32 compact = 0x1100;
    bus.write32(compact + 0, kCcbLast | kCcbNpAbs | kCcbSpAbs | kCcbPpAbs |
                                 kCcbCcbPre | kCcbAcw | kCcbAccw);
    bus.write32(compact + 4, 0);
    bus.write32(compact + 8, kSourceAt);
    bus.write32(compact + 12, kPlutAt);
    bus.write32(compact + 16, 0x7fffffffu);
    bus.write32(compact + 20, 0x7fffffffu);
    bus.write32(compact + 24, pre0_for(kFormatDirect16, 10));
    bus.write32(compact + 28, pre1_for(11));

    const Ccb inherited = madam.read_ccb(compact);
    CHECK_EQ(inherited.x, fixed(13));
    CHECK_EQ(inherited.y, fixed(17));
    CHECK_EQ(inherited.hdx, fixed(2));
    CHECK_EQ(inherited.hdy, fixed(3));
    CHECK_EQ(inherited.vdx, fixed(4));
    CHECK_EQ(inherited.vdy, fixed(5));
    CHECK_EQ(inherited.hddx, fixed(8));
    CHECK_EQ(inherited.hddy, fixed(9));
    CHECK_EQ(inherited.pixc, 0x12345678u);
    CHECK_EQ(inherited.width, 11u);
    CHECK_EQ(inherited.height, 10u);
}

TEST(the_engine_reads_the_ccb_as_it_stands_when_the_instruction_ends) {
    // Software is entitled to start the engine and then finish writing the
    // very CCB it is about to read, because on the hardware the engine does
    // not begin until the CPU's current instruction is done. Need for Speed
    // does exactly this, and a machine that walks the list inside the store
    // draws the previous frame's geometry - which for that title meant no road
    // and no terrain at all, just a correct cockpit over the clear colour.
    Console console;
    console.reset();
    Bus& bus = console.bus();

    bus.write16(kSourceAt + 0, rgb555(31, 0, 0));
    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(5), fixed(5),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));

    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);
    bus.write32(kMadamBase + kMadamCelStart, 0);

    // Still inside the instruction that asked: nothing has been read yet, so
    // moving the cel now must move where it lands.
    bus.write32(kCcbAt + 16, static_cast<u32>(fixed(9)));
    bus.write32(kCcbAt + 20, static_cast<u32>(fixed(9)));

    bus.run_pending_cel_engine();

    CHECK_EQ(console.madam().stats().cels_drawn, 1u);
    CHECK_EQ(read_target(bus, 9, 9), rgb555(31, 0, 0));
    CHECK_EQ(read_target(bus, 5, 5), 0u);
}

TEST(stopping_the_engine_also_cancels_a_start_that_has_not_happened_yet) {
    Console console;
    console.reset();
    Bus& bus = console.bus();

    write_ccb(bus, kCcbAt, 0, kSourceAt, 0, fixed(5), fixed(5),
              fixed(1), 0, 0, fixed(1),
              pre0_for(kFormatDirect16, 1), pre1_for(1));
    bus.write32(kMadamBase + kMadamNextCcb, kCcbAt);
    bus.write32(kMadamBase + kMadamCelStart, 0);
    bus.write32(kMadamBase + kMadamCelStop, 0);

    bus.run_pending_cel_engine();
    CHECK_EQ(console.madam().stats().cels_drawn, 0u);
}
