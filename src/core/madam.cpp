#include "madam.h"

#include "bus.h"
#include "vdlp.h"

namespace retro3do {
namespace {

// A cel list that does not terminate would hang the emulator on a bad pointer.
// Real lists are far shorter than this; the limit exists so that garbage in
// memory produces a dropped frame rather than a freeze.
constexpr u32 kMaxCels = 4096;

// A single cel larger than this is treated as corrupt. Without it, a bad size
// field asks the engine to draw billions of pixels and the app appears to hang.
constexpr u32 kMaxCelDimension = 2048;

// CCB word offsets. This is struct order, and it is the part of the CCB layout
// that is well established.
enum : u32 {
    kCcbFlagsWord   = 0,
    kCcbNextWord    = 1,
    kCcbSourceWord  = 2,
    kCcbPlutWord    = 3,
    kCcbXWord       = 4,
    kCcbYWord       = 5,
    kCcbHdxWord     = 6,
    kCcbHdyWord     = 7,
    kCcbVdxWord     = 8,
    kCcbVdyWord     = 9,
    kCcbHddxWord    = 10,
    kCcbHddyWord    = 11,
    kCcbPixcWord    = 12,
    kCcbPre0Word    = 13,
    kCcbPre1Word    = 14,
    kCcbWordCount   = 15,
};

// TODO(madam): the PRE0/PRE1 bit assignments below are the commonly published
// ones and have NOT been checked against the hardware documentation. They are
// isolated in these two functions precisely so that correcting them is a local
// change rather than a hunt through the renderer.
constexpr u32 kPre0FormatMask  = 0x00000007u;
constexpr u32 kPre0HeightShift = 6;
constexpr u32 kPre0HeightMask  = 0x000003ffu;
constexpr u32 kPre1WidthMask   = 0x000003ffu;

u32 cel_height_from_pre0(u32 pre0) {
    // The stored value is one less than the real height, as is usual for a
    // field that must be able to express a full-size cel.
    return ((pre0 >> kPre0HeightShift) & kPre0HeightMask) + 1u;
}

u32 cel_width_from_pre1(u32 pre1) {
    return (pre1 & kPre1WidthMask) + 1u;
}

// How many bits one source pixel occupies.
unsigned bits_per_pixel(CelFormat format) {
    switch (format) {
        case CelFormat::Indexed1: return 1;
        case CelFormat::Indexed2: return 2;
        case CelFormat::Indexed4: return 4;
        case CelFormat::Indexed6: return 6;
        case CelFormat::Indexed8: return 8;
        case CelFormat::Direct16: return 16;
        case CelFormat::Unknown:
        default:                  return 0;
    }
}

// How many destination samples one step vector needs so that consecutive
// samples land on adjacent pixels. A step of one pixel or less needs one; a
// step of three needs three, or the fill leaves gaps.
u32 footprint_steps(s32 dx, s32 dy) {
    const s32 ax = dx < 0 ? -dx : dx;
    const s32 ay = dy < 0 ? -dy : dy;
    const s32 longest = ax > ay ? ax : ay;
    // Round up to whole pixels; 16.16, so one pixel is 1 << 16.
    const u32 steps = static_cast<u32>((longest + 0xffff) >> 16);
    if (steps < 1) return 1;
    // A step this large means a corrupt CCB rather than a real magnification.
    if (steps > 256) return 256;
    return steps;
}

}  // namespace

CelFormat cel_format_from_pre0(u32 pre0) {
    switch (pre0 & kPre0FormatMask) {
        case 1:  return CelFormat::Indexed1;
        case 2:  return CelFormat::Indexed2;
        case 3:  return CelFormat::Indexed4;
        case 4:  return CelFormat::Indexed6;
        case 5:  return CelFormat::Indexed8;
        case 6:  return CelFormat::Direct16;
        default: return CelFormat::Unknown;
    }
}

Madam::Madam(Bus& bus) : bus_(bus) {
    reset();
}

void Madam::reset() {
    revision_ = 0;
    mem_config_ = kMadamMemConfigStock;
    vdl_address_ = 0;
    clip_width_ = 320;
    clip_height_ = 240;
    target_address_ = kVramBase;
    target_stride_bytes_ = 320 * 2;
    stats_ = MadamStats{};
}

void Madam::set_clip(u32 width, u32 height) {
    clip_width_ = width;
    clip_height_ = height;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
u32 Madam::read(u32 offset) {
    offset &= (kMadamWindowSize - 1);
    switch (offset) {
        case kMadamRevision:  return revision_;
        case kMadamMemConfig:  return mem_config_;
        case kMadamVdlAddress: return vdl_address_;
        default:              return 0;
    }
}


bool Madam::register_written(u32 offset) const {
    const u32 index = offset / 4;
    return index < kTrackedRegisters && written_flag_[index];
}

u32 Madam::register_last_write(u32 offset) const {
    const u32 index = offset / 4;
    return index < kTrackedRegisters ? written_value_[index] : 0;
}

void Madam::note_write(u32 offset, u32 value) {
    const u32 index = offset / 4;
    if (index < kTrackedRegisters) {
        written_value_[index] = value;
        written_flag_[index] = true;
    }
}

void Madam::write(u32 offset, u32 value) {
    offset &= (kMadamWindowSize - 1);
    note_write(offset, value);
    switch (offset) {
        case kMadamRevision:
            revision_ = value;
            break;
        case kMadamMemConfig:
            mem_config_ = value;
            break;
        case kMadamVdlAddress:
            vdl_address_ = value;
            break;
        case kMadamCelStart:
            // Writing the list address is what starts the engine. The real chip
            // runs asynchronously and raises an interrupt when it finishes;
            // this runs it to completion immediately, which is indistinguishable
            // to software that waits for the completion flag and wrong only for
            // software that races it.
            render_cel_list(value);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Reading a CCB
// ---------------------------------------------------------------------------
Ccb Madam::read_ccb(u32 address) const {
    Ccb ccb;

    u32 words[kCcbWordCount];
    for (u32 i = 0; i < kCcbWordCount; ++i) {
        words[i] = const_cast<Bus&>(bus_).read32(address + i * 4);
    }

    ccb.flags  = words[kCcbFlagsWord];

    // Pointers are stored either absolutely or relative to the word after the
    // field that holds them. Getting the relative base wrong shifts every cel
    // in a list by a constant, which looks like a mysterious offset rather than
    // a pointer bug, so the base is written out explicitly.
    const u32 next_base   = address + (kCcbNextWord + 1) * 4;
    const u32 source_base = address + (kCcbSourceWord + 1) * 4;
    const u32 plut_base   = address + (kCcbPlutWord + 1) * 4;

    ccb.next_address = (ccb.flags & kCcbNpAbs)
                           ? words[kCcbNextWord]
                           : next_base + words[kCcbNextWord];
    ccb.source_address = (ccb.flags & kCcbSpAbs)
                             ? words[kCcbSourceWord]
                             : source_base + words[kCcbSourceWord];
    ccb.plut_address = (ccb.flags & kCcbPpAbs)
                           ? words[kCcbPlutWord]
                           : plut_base + words[kCcbPlutWord];

    ccb.x = static_cast<s32>(words[kCcbXWord]);
    ccb.y = static_cast<s32>(words[kCcbYWord]);

    ccb.hdx  = static_cast<s32>(words[kCcbHdxWord]);
    ccb.hdy  = static_cast<s32>(words[kCcbHdyWord]);
    ccb.vdx  = static_cast<s32>(words[kCcbVdxWord]);
    ccb.vdy  = static_cast<s32>(words[kCcbVdyWord]);
    ccb.hddx = static_cast<s32>(words[kCcbHddxWord]);
    ccb.hddy = static_cast<s32>(words[kCcbHddyWord]);

    ccb.pixc = words[kCcbPixcWord];
    ccb.pre0 = words[kCcbPre0Word];
    ccb.pre1 = words[kCcbPre1Word];

    ccb.format = cel_format_from_pre0(ccb.pre0);
    ccb.width  = cel_width_from_pre1(ccb.pre1);
    ccb.height = cel_height_from_pre0(ccb.pre0);

    return ccb;
}

// ---------------------------------------------------------------------------
// Sampling source pixels
// ---------------------------------------------------------------------------
u16 Madam::sample(const Ccb& ccb, u32 sx, u32 sy) const {
    Bus& bus = const_cast<Bus&>(bus_);
    const unsigned bpp = bits_per_pixel(ccb.format);
    if (bpp == 0) {
        return 0;
    }

    if (ccb.format == CelFormat::Direct16) {
        const u32 offset = (sy * ccb.width + sx) * 2u;
        const u32 address = ccb.source_address + offset;
        return static_cast<u16>((static_cast<u16>(bus.read8(address)) << 8) |
                                bus.read8(address + 1));
    }

    // Indexed formats. Rows are packed continuously; a pixel is bpp bits into
    // the stream, most significant bits first.
    const u64 bit_index = static_cast<u64>(sy) * ccb.width * bpp +
                          static_cast<u64>(sx) * bpp;
    const u32 byte_index = static_cast<u32>(bit_index / 8);
    const unsigned bit_in_byte = static_cast<unsigned>(bit_index % 8);

    // Read two bytes so a pixel straddling a byte boundary still works. Six bits
    // per pixel makes that the common case, not the exception.
    const u32 pair = (static_cast<u32>(bus.read8(ccb.source_address + byte_index)) << 8) |
                     bus.read8(ccb.source_address + byte_index + 1);
    const unsigned shift = 16u - bit_in_byte - bpp;
    const u32 index = (pair >> shift) & ((1u << bpp) - 1u);

    // Through the palette. Entries are 16-bit RGB555.
    const u32 entry = ccb.plut_address + index * 2u;
    return static_cast<u16>((static_cast<u16>(bus.read8(entry)) << 8) |
                            bus.read8(entry + 1));
}

void Madam::put_pixel(s32 x, s32 y, u16 pixel) {
    if (x < 0 || y < 0 || static_cast<u32>(x) >= clip_width_ ||
        static_cast<u32>(y) >= clip_height_) {
        return;
    }
    // The same layout the display reads. MADAM writing linearly while the VDLP
    // read interleaved produced a picture that was wrong in both chips' favour
    // depending on which you suspected, so the offset is computed in one place.
    const u32 address =
        target_address_ + framebuffer_offset(x, y, static_cast<int>(clip_width_));
    bus_.write16(address, pixel);
    ++stats_.pixels_written;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void Madam::draw_cel(const Ccb& ccb) {
    if (ccb.format == CelFormat::Unknown) {
        return;
    }
    if (ccb.width == 0 || ccb.height == 0) {
        return;
    }
    if (ccb.width > kMaxCelDimension || ccb.height > kMaxCelDimension) {
        return;
    }

    // Row origin, 16.16. Everything below is incremental: the source position
    // advances by adding deltas, never by multiplying per pixel. That is what
    // keeps the inner loop a straight walk the compiler can vectorise, and it
    // is the single most important difference from the interpreter-style
    // rasteriser this core replaces.
    s32 row_x = ccb.x;
    s32 row_y = ccb.y;

    // The horizontal step itself changes as rows advance, which is what turns
    // the mapped rectangle into a general quad.
    s32 step_x = ccb.hdx;
    s32 step_y = ccb.hdy;

    // How far one source row advances the destination. Constant for the whole
    // cel, so it is computed once.
    const u32 vertical_span = footprint_steps(ccb.vdx, ccb.vdy);

    for (u32 sy = 0; sy < ccb.height; ++sy) {
        s32 px = row_x;
        s32 py = row_y;

        // Each source pixel covers a parallelogram of destination pixels, given
        // by the horizontal and vertical step vectors. When a cel is magnified
        // that area is larger than one pixel, and writing a single destination
        // pixel per source pixel leaves the picture full of holes — a magnified
        // sprite comes out as a grid of dots rather than a solid shape.
        //
        // So the footprint is filled. The step counts are derived from the step
        // vectors, which means a 1:1 cel costs exactly one write per pixel and
        // only magnified cels pay more — and what they pay is proportional to
        // the destination area, which is the work that actually has to happen.
        const u32 horizontal_span = footprint_steps(step_x, step_y);

        for (u32 sx = 0; sx < ccb.width; ++sx) {
            const u16 pixel = sample(ccb, sx, sy);

            // Colour zero is transparent unless the cel says otherwise. This is
            // why sprites have holes in them rather than black boxes.
            // TODO(madam): confirm which flag overrides this.
            if (pixel != 0) {
                if (horizontal_span == 1 && vertical_span == 1) {
                    put_pixel(px >> 16, py >> 16, pixel);
                } else {
                    for (u32 j = 0; j < vertical_span; ++j) {
                        const s32 oy = static_cast<s32>(
                            (static_cast<s64>(ccb.vdy) * j) / vertical_span);
                        const s32 ox = static_cast<s32>(
                            (static_cast<s64>(ccb.vdx) * j) / vertical_span);
                        for (u32 i = 0; i < horizontal_span; ++i) {
                            const s32 ix = static_cast<s32>(
                                (static_cast<s64>(step_x) * i) / horizontal_span);
                            const s32 iy = static_cast<s32>(
                                (static_cast<s64>(step_y) * i) / horizontal_span);
                            put_pixel((px + ix + ox) >> 16, (py + iy + oy) >> 16,
                                      pixel);
                        }
                    }
                }
            }

            px += step_x;
            py += step_y;
        }

        row_x += ccb.vdx;
        row_y += ccb.vdy;
        step_x += ccb.hddx;
        step_y += ccb.hddy;
    }

    ++stats_.cels_drawn;
}

void Madam::render_cel_list(u32 address) {
    stats_ = MadamStats{};

    if (address == 0) {
        return;
    }

    u32 current = address;
    while (stats_.cels_walked < kMaxCels) {
        const Ccb ccb = read_ccb(current);
        ++stats_.cels_walked;

        if ((ccb.flags & kCcbSkip) == 0) {
            draw_cel(ccb);
        }

        if (ccb.flags & kCcbLast) {
            return;
        }
        if (ccb.next_address == 0 || ccb.next_address == current) {
            return;
        }
        current = ccb.next_address;
    }

    // Ran out of patience rather than reaching the end. Recorded rather than
    // silently ignored: a truncated list is the difference between "this game
    // draws nothing" and "this game draws most of a frame".
    stats_.list_truncated = 1;
}

}  // namespace retro3do
