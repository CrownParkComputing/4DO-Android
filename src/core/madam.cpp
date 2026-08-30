#include "madam.h"
#include <cstdlib>
#include <cstdio>

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
// Reads a big-endian bitstream out of memory, most significant bit first,
// which is the order the cel packer writes it in.
class BitReader {
public:
    BitReader(Bus& bus, u32 base) : bus_(bus), base_(base) {}

    u32 read(unsigned bits) {
        u32 value = 0;
        while (bits-- > 0) {
            const u32 byte = bus_.read8(base_ + (bit_ >> 3));
            const unsigned shift = 7u - (bit_ & 7u);
            value = (value << 1) | ((byte >> shift) & 1u);
            ++bit_;
        }
        return value;
    }

    // Where the reader has reached, as an address. Rounded down, which is what
    // the end-of-row test wants: a packet that starts past the end is past it.
    u32 address() const { return base_ + (bit_ >> 3); }

private:
    Bus& bus_;
    u32 base_;
    u32 bit_ = 0;
};

#if RETRO3DO_TRACING
// A register trace, off unless MADAMLOG names a file. Same idea as the CLIO
// one: comparing the sequence against a machine known to work is the quickest
// way to find a register nobody is driving.
std::FILE* const g_madam_log = [] {
    const char* path = std::getenv("MADAMLOG");
    return path != nullptr ? std::fopen(path, "w") : nullptr;
}();
long g_madam_log_count = 0;
const long g_madam_log_limit = [] {
    const char* limit = std::getenv("MADAMLOGMAX");
    return limit != nullptr ? std::strtol(limit, nullptr, 10) : 200000L;
}();
#endif

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


// Is this offset inside the DMA channel array, and if so which channel and
// which half of the pair?
bool dma_slot(u32 offset, u32* channel, bool* is_length) {
    if (offset < kMadamDmaBase) return false;
    const u32 relative = offset - kMadamDmaBase;
    const u32 span = kMadamDmaChannels * kMadamDmaStride;
    if (relative >= span) return false;
    *channel = relative / kMadamDmaStride;
    *is_length = (relative % kMadamDmaStride) >= 4;
    return true;
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
    for (u32 i = 0; i < kMadamDmaChannels; ++i) {
        dma_address_[i] = 0;
        dma_length_[i] = 0;
    }
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
    u32 channel = 0;
    bool is_length = false;
    if (dma_slot(offset, &channel, &is_length)) {
        return is_length ? dma_length_[channel] : dma_address_[channel];
    }

    switch (offset) {
        case kMadamRevision:  return revision_;
        case kMadamMemConfig:  return mem_config_;
        case kMadamDmaEnable:        return dma_enable_;
        case kMadamXbusDmaAddress:   return xbus_dma_address_;
        case kMadamXbusDmaLength:    return xbus_dma_length_;
        case kMadamVdlAddress: return vdl_address_;
        case kMadamPbusAddress: return pbus_address_;
        case kMadamPbusLength:  return pbus_length_;
        case kMadamPbusPointer: return pbus_pointer_;
        case kMadamCurrentCcb: return current_ccb_;
        case kMadamNextCcb:    return next_ccb_;
        default:
            return 0;
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
#if RETRO3DO_TRACING
    if (g_madam_log != nullptr && g_madam_log_count < g_madam_log_limit) {
        std::fprintf(g_madam_log, "W %04X %08X\n", offset, value);
        ++g_madam_log_count;
    }
#endif
    note_write(offset, value);
    u32 channel = 0;
    bool is_length = false;
    if (dma_slot(offset, &channel, &is_length)) {
        if (is_length) {
            dma_length_[channel] = value;
        } else {
            dma_address_[channel] = value;
        }
        return;
    }

    switch (offset) {
        case kMadamRevision:
            revision_ = value;
            break;
        case kMadamMemConfig:
            // Read-only: it reports how much memory is FITTED, which software
            // cannot change. The boot ROM writes zero to it during start-up,
            // and honouring that makes the machine tell itself it has no
            // memory - its own sizing routine reads this register, decodes
            // zero DRAM, and panics.
            break;
        case kMadamDmaEnable:
            // Bit 15 asks for a controller scan. The transfer runs here,
            // inside the store, because the OS starts it and then blocks on
            // the interrupt it raises - there is no later point at which it
            // would be convenient to notice.
            dma_enable_ = value;
            if ((dma_enable_ & kMadamPbusStart) != 0 && pbus_handler_ != nullptr) {
                pbus_handler_(pbus_context_);
            }
            break;
        case kMadamXbusDmaAddress:
            xbus_dma_address_ = value;
            break;
        case kMadamXbusDmaLength:
            // Writing the length is what arms the transfer: the address is set
            // first and means nothing on its own.
            xbus_dma_length_ = value;
            xbus_dma_pending_ = true;
            break;
        case kMadamVdlAddress:
            vdl_address_ = value;
            break;

        case kMadamPbusAddress:
            pbus_address_ = value;
            break;
        case kMadamPbusLength:
            pbus_length_ = value;
            break;
        case kMadamPbusPointer:
            pbus_pointer_ = value;
            break;
        // The value written to any of these is discarded; the port itself is
        // the instruction. The real chip runs the walk asynchronously and
        // raises an interrupt when it finishes; this runs it to completion
        // immediately, which is indistinguishable to software that waits for
        // the completion flag and wrong only for software that races it.
        case kMadamCelStart:
            run_cel_engine();
            break;
        case kMadamCelStop:
            next_ccb_ = 0;
            break;
        case kMadamCelResume:
            run_cel_engine();
            break;
        case kMadamCelPause:
            break;

        case kMadamCurrentCcb:
            current_ccb_ = value;
            break;
        case kMadamNextCcb:
            next_ccb_ = value;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Reading a CCB
// ---------------------------------------------------------------------------
u32 Madam::dma_address(u32 channel) const {
    return channel < kMadamDmaChannels ? dma_address_[channel] : 0;
}

u32 Madam::dma_length(u32 channel) const {
    return channel < kMadamDmaChannels ? dma_length_[channel] : 0;
}


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

    // A next-pointer of zero is the end of the list, whatever the addressing
    // mode says. Adding a relative base to it produces a perfectly plausible
    // address, and the walk marches off through whatever memory happens to
    // follow - four thousand one-pixel cels out of cleared RAM before the
    // safety limit stops it.
    const u32 next_raw = words[kCcbNextWord] & kCcbAddressMask;
    ccb.next_address = next_raw == 0
                           ? 0u
                           : ((ccb.flags & kCcbNpAbs) ? next_raw
                                                      : next_base + next_raw);

    const u32 source_raw = words[kCcbSourceWord] & kCcbAddressMask;
    ccb.source_address = (ccb.flags & kCcbSpAbs) ? source_raw
                                                 : source_base + source_raw;

    const u32 plut_raw = words[kCcbPlutWord] & kCcbAddressMask;
    ccb.plut_address = (ccb.flags & kCcbPpAbs) ? plut_raw
                                               : plut_base + plut_raw;

    ccb.x = static_cast<s32>(words[kCcbXWord]);
    ccb.y = static_cast<s32>(words[kCcbYWord]);

    // The two step vectors are stored in DIFFERENT fixed-point formats, which
    // is the sort of thing that reads as a scaling bug rather than a parsing
    // one. The horizontal steps and their derivatives are 12.20; the vertical
    // pair is already 16.16. Using the horizontal ones raw magnifies every cel
    // sixteen times across, so a full-screen background lands as a smear a
    // sixteenth of the way through its own artwork.
    //
    // The shift is arithmetic: these are signed, and a cel drawn right-to-left
    // has a negative step.
    ccb.hdx  = static_cast<s32>(words[kCcbHdxWord]) >> 4;
    ccb.hdy  = static_cast<s32>(words[kCcbHdyWord]) >> 4;
    ccb.vdx  = static_cast<s32>(words[kCcbVdxWord]);
    ccb.vdy  = static_cast<s32>(words[kCcbVdyWord]);
    ccb.hddx = static_cast<s32>(words[kCcbHddxWord]) >> 4;
    ccb.hddy = static_cast<s32>(words[kCcbHddyWord]) >> 4;

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
// Turn one source value into a colour. Direct cels carry the colour already;
// everything else is an index into the cel's own palette.
u16 Madam::decode_pixel(const Ccb& ccb, u32 value) const {
    if (ccb.format == CelFormat::Direct16) {
        return static_cast<u16>(value);
    }
    Bus& bus = const_cast<Bus&>(bus_);
    const u32 entry = ccb.plut_address + value * 2u;
    return static_cast<u16>((static_cast<u16>(bus.read8(entry)) << 8) |
                            bus.read8(entry + 1));
}

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
// One source pixel covers a parallelogram of destination pixels, given by the
// horizontal and vertical step vectors. When a cel is magnified that area is
// larger than one pixel, and writing a single destination pixel per source
// pixel leaves the picture full of holes - a magnified sprite comes out as a
// grid of dots rather than a solid shape.
//
// A 1:1 cel costs exactly one write per pixel; only magnified cels pay more,
// and what they pay is proportional to the destination area, which is the work
// that actually has to happen.
void Madam::plot_footprint(const Ccb& ccb, s32 px, s32 py, s32 step_x,
                           s32 step_y, u32 horizontal_span, u32 vertical_span,
                           u16 pixel) {
    // Colour zero is transparent unless the cel says otherwise. This is why
    // sprites have holes in them rather than black boxes.
    if (pixel == 0) {
        return;
    }
    if (horizontal_span == 1 && vertical_span == 1) {
        put_pixel(px >> 16, py >> 16, pixel);
        return;
    }
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
            put_pixel((px + ix + ox) >> 16, (py + iy + oy) >> 16, pixel);
        }
    }
}

// A packed cel does not have a width. Its rows are a bitstream of packets and
// each row ends where the packets say it does, so there is nothing to index
// into and no rectangle to walk - which is why this cannot share the sampler.
//
// Each row opens with a length field giving the row's size in words, then runs
// packets of a 2-bit type and a 6-bit count-less-one:
//
//   0  end of row
//   1  literal    - `count` pixels follow, bpp bits each
//   2  transparent- skip `count` pixels
//   3  repeat     - one pixel follows and covers `count` positions
//
// Reading this as if it were raw pixels produces a screen of coloured noise
// that looks like a palette fault rather than a format one.
void Madam::draw_packed_cel(const Ccb& ccb) {
    const unsigned bpp = bits_per_pixel(ccb.format);
    if (bpp == 0 || ccb.height == 0 || ccb.height > kMaxCelDimension) {
        return;
    }

    // The row-length field is one byte at low colour depths and two above,
    // because a deeper row cannot describe its length in eight bits.
    const unsigned length_bits = bpp < 8 ? 8u : 16u;

    Bus& bus = bus_;
    u32 row_data = ccb.source_address;

    s32 row_x = ccb.x;
    s32 row_y = ccb.y;
    s32 step_x = ccb.hdx;
    s32 step_y = ccb.hdy;
    const u32 vertical_span = footprint_steps(ccb.vdx, ccb.vdy);

    for (u32 row = 0; row < ccb.height; ++row) {
        BitReader reader(bus, row_data);
        const u32 length = reader.read(length_bits);

        // The field counts words and excludes itself and one more, hence +2.
        const u32 row_end = row_data + ((length + 2u) << 2);

        s32 px = row_x;
        s32 py = row_y;
        const u32 horizontal_span = footprint_steps(step_x, step_y);

        for (;;) {
            u32 type = reader.read(2);

            // Running past the row's declared end ends the row, whatever the
            // bits happen to say. Without this a corrupt length runs the
            // decoder through the whole of memory.
            if (reader.address() >= row_end) {
                type = 0;
            }
            const u32 count = reader.read(6) + 1u;

            if (type == 0) {
                break;
            }
            if (type == 2) {
                px += step_x * static_cast<s32>(count);
                py += step_y * static_cast<s32>(count);
                continue;
            }
            if (type == 3) {
                const u16 pixel = decode_pixel(ccb, reader.read(bpp));
                for (u32 i = 0; i < count; ++i) {
                    plot_footprint(ccb, px, py, step_x, step_y,
                                   horizontal_span, vertical_span, pixel);
                    px += step_x;
                    py += step_y;
                }
                continue;
            }
            for (u32 i = 0; i < count; ++i) {
                const u16 pixel = decode_pixel(ccb, reader.read(bpp));
                plot_footprint(ccb, px, py, step_x, step_y,
                               horizontal_span, vertical_span, pixel);
                px += step_x;
                py += step_y;
            }
        }

        row_data = row_end;
        row_x += ccb.vdx;
        row_y += ccb.vdy;
        step_x += ccb.hddx;
        step_y += ccb.hddy;
    }

    ++stats_.cels_drawn;
}

void Madam::draw_cel(const Ccb& ccb) {
    if ((ccb.flags & kCcbPacked) != 0) {
        draw_packed_cel(ccb);
        return;
    }
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
            plot_footprint(ccb, px, py, step_x, step_y,
                           horizontal_span, vertical_span, pixel);

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

// Walk from NEXTCCB, which is where the hardware starts and where it leaves
// off. The walk consumes NEXTCCB as it goes, so software that starts the
// engine twice without reloading it draws nothing the second time - which is
// what the hardware does.
void Madam::run_cel_engine() {
    render_cel_list(next_ccb_);
}

namespace {

// The walk, one line per cel, off unless CELLOG names a file.
// Cheap enough to leave in: a cel is thousands of pixels, so one branch per
// cel does not show up next to the drawing. A list that
// stops one cel in looks identical from the outside to a list that only had
// one cel in it, and only the flags tell them apart.
std::FILE* const g_cel_log = [] {
    const char* path = std::getenv("CELLOG");
    return path != nullptr ? std::fopen(path, "w") : nullptr;
}();
}  // namespace

void Madam::render_cel_list(u32 address) {
    stats_ = MadamStats{};
    ++engine_runs_;
    if (g_cel_log != nullptr) {
        std::fprintf(g_cel_log, "RUN %llu head=%06X\n",
                     (unsigned long long)engine_runs_, address);
    }

    if (address == 0) {
        return;
    }

    next_ccb_ = address;
    while (stats_.cels_walked < kMaxCels) {
        current_ccb_ = next_ccb_;
        const Ccb ccb = read_ccb(current_ccb_);
        ++stats_.cels_walked;
        next_ccb_ = ccb.next_address;
        if (g_cel_log != nullptr) {
            std::fprintf(g_cel_log, "CCB %06X flags=%08X next=%06X src=%06X %ux%u\n",
                         current_ccb_, ccb.flags, next_ccb_, ccb.source_address,
                         ccb.width, ccb.height);
        }

        if ((ccb.flags & kCcbSkip) == 0) {
            const u32 before = stats_.cels_drawn;
            const u64 before_pixels = stats_.pixels_written;
            draw_cel(ccb);
            total_cels_drawn_ += stats_.cels_drawn - before;
            if (g_cel_log != nullptr) {
                std::fprintf(g_cel_log,
                             "    drew=%u pixels=%llu packed=%d fmt=%d "
                             "at=%d,%d hd=%d,%d vd=%d,%d\n",
                             stats_.cels_drawn - before,
                             (unsigned long long)(stats_.pixels_written - before_pixels),
                             (ccb.flags & kCcbPacked) != 0 ? 1 : 0,
                             static_cast<int>(ccb.format),
                             ccb.x >> 16, ccb.y >> 16,
                             ccb.hdx, ccb.hdy, ccb.vdx, ccb.vdy);
            }
        }

        if (ccb.flags & kCcbLast) {
            return;
        }
        if (next_ccb_ == 0 || next_ccb_ == current_ccb_) {
            return;
        }
    }

    // Ran out of patience rather than reaching the end. Recorded rather than
    // silently ignored: a truncated list is the difference between "this game
    // draws nothing" and "this game draws most of a frame".
    stats_.list_truncated = 1;
}

}  // namespace retro3do
