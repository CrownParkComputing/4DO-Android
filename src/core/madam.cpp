#include "madam.h"
#include <cstdlib>
#include <cstdio>

#include "bus.h"
#include "vdlp.h"

namespace retro3do {
namespace {

// The stride is not a plain number of bytes. Three separate fields of the
// control register contribute, at three different weights, and the result is
// the distance between one PAIR of rows and the next.
u32 decode_stride(u32 value) {
    return ((value & 0x01u) << 7) + ((value & 0x0cu) << 8) + ((value & 0x70u) << 4);
}

// Where a pixel sits, given a stride. Rows are interleaved in pairs, so the
// vertical step covers two rows and the odd row is two bytes along from the
// even one.
// The multiply/divide table the pixel processor scales a channel through. A
// channel is multiplied by one of eight factors and divided by one of four,
// and both are small enough that the whole thing is a lookup.
u8 g_scale[8][4][32];
bool g_scale_built = false;

void build_scale_table() {
    if (g_scale_built) {
        return;
    }
    // The divider is not the field value. Zero means a shift of FOUR, not of
    // none, so a cel asking for what looks like no division is asking for the
    // largest one - and a processor that reads it as none makes every cel
    // sixteen times too bright.
    const auto divisor_shift = [](u32 field) { return ((field - 1u) & 3u) + 1u; };
    for (u32 value = 0; value < 32; ++value) {
        for (u32 multiplier = 0; multiplier < 8; ++multiplier) {
            for (u32 divider = 0; divider < 4; ++divider) {
                g_scale[multiplier][divider][value] = static_cast<u8>(
                    (value * (multiplier + 1u)) >> divisor_shift(divider));
            }
        }
    }
    g_scale_built = true;
}

s32 clamp_channel(s32 value) {
    if (value < 0) return 0;
    if (value > 31) return 31;
    return value;
}

u32 pixel_offset(s32 x, s32 y, s32 stride) {
    return static_cast<u32>(((y >> 1) * stride) + ((y & 1) << 1) + (x << 2));
}

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
// Set when a pixel carries its colour directly instead of an index into the
// palette. Bit FOUR, not the top bit - the top bit is something else entirely.
constexpr u32 kPre0Linear      = 0x00000010u;
constexpr u32 kPre0HeightShift = 6;
constexpr u32 kPre0HeightMask  = 0x000003ffu;
constexpr u32 kPre1WidthMask   = 0x000007ffu;

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

    void skip(unsigned bits) { bit_ += bits; }

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
    framebuffer_configured_ = false;
    read_base_ = 0;
    write_base_ = 0;
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

    if (offset >= kMadamFifoBase && offset < kMadamFifoEnd) {
        const bool output = offset >= kMadamFifoOutput;
        const u32 channel = (offset >> 4) & 0x0f;
        const Fifo& fifo = output ? output_fifo_[channel % kOutputFifos]
                                  : input_fifo_[channel % kInputFifos];
        switch (offset & 0x0f) {
            // The address and length read back as the channel's CURRENT
            // position, not as what software wrote. That is how software finds
            // out how far a sound has played.
            case 0x00: return fifo.address + static_cast<u32>(fifo.index);
            case 0x04: return static_cast<u32>(fifo.length - fifo.index);
            case 0x08: return fifo.next_address;
            default:   return static_cast<u32>(fifo.next_length);
        }
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
        case kMadamCcbCtl0:    return ccb_ctl0_;
        case kMadamRegCtl0:    return reg_ctl0_;
        case kMadamRegCtl1:    return reg_ctl1_;
        case kMadamRegCtl2:    return read_base_;
        case kMadamRegCtl3:    return write_base_;
        case kMadamCurrentCcb: return current_ccb_;
        case kMadamNextCcb:    return next_ccb_;
        default:
            return 0;
    }
}


// ---------------------------------------------------------------------------
// The DSP's DMA channels
// ---------------------------------------------------------------------------
//
// A channel walks a buffer in memory a word at a time. When it reaches the
// end it interrupts and, if software has left a reload address behind, picks
// that up and carries on - which is how a sound stays continuous without the
// CPU being woken for every buffer.
//
// An address of zero means the channel is idle. That is not a sentinel this
// code invented: software clears the address to stop a channel.

u16 Madam::fifo_input_peek(u16 channel) {
    if (channel >= kInputFifos) {
        return 0;
    }
    const Fifo& fifo = input_fifo_[channel];
    const u32 address = fifo.address + static_cast<u32>(fifo.index);
    return static_cast<u16>((static_cast<u16>(bus_.read8(address)) << 8) |
                            bus_.read8(address + 1));
}

u16 Madam::fifo_input_next(u16 channel) {
    if (channel >= kInputFifos) {
        return 0;
    }
    Fifo& fifo = input_fifo_[channel];
    if (fifo.address == 0) {
        return 0;
    }

    if ((fifo.length - fifo.index) > 0) {
        const u16 value = fifo_input_peek(channel);
        fifo.index += 2;
        return value;
    }

    fifo.index = 0;
    if (fifo_done_ != nullptr) {
        fifo_done_(fifo_done_context_, channel, false);
    }

    // Reload only if software armed one AND left the channel enabled.
    if (fifo.next_address != 0 && (dma_channel_enable_ & (1u << channel)) != 0) {
        fifo.address = fifo.next_address;
        fifo.length = fifo.next_length;
        const u16 value = fifo_input_peek(channel);
        fifo.index += 2;
        return value;
    }

    fifo.address = 0;
    return 0;
}

void Madam::fifo_output(u16 channel, u16 value) {
    if (channel >= kOutputFifos) {
        return;
    }
    Fifo& fifo = output_fifo_[channel];
    if (fifo.address == 0) {
        return;
    }

    if ((fifo.length - fifo.index) > 0) {
        const u32 address = fifo.address + static_cast<u32>(fifo.index);
        bus_.write8(address, static_cast<u8>(value >> 8));
        bus_.write8(address + 1, static_cast<u8>(value));
        fifo.index += 2;
        return;
    }

    fifo.index = 0;
    if (fifo_done_ != nullptr) {
        fifo_done_(fifo_done_context_, channel, true);
    }
    if (fifo.next_address != 0 && (dma_channel_enable_ & (1u << channel)) != 0) {
        fifo.address = fifo.next_address;
        fifo.length = fifo.next_length;
    } else {
        fifo.address = 0;
    }
}

u16 Madam::fifo_input_status(u16 channel) const {
    if (channel >= kInputFifos) {
        return 0;
    }
    return input_fifo_[channel].address != 0 ? 2u : 0u;
}

u16 Madam::fifo_output_status(u16 channel) const {
    if (channel >= kOutputFifos) {
        return 0;
    }
    return output_fifo_[channel].address != 0 ? 1u : 0u;
}

void Madam::clear_fifo(u32 channel, bool output) {
    if (output) {
        if (channel < kOutputFifos) output_fifo_[channel] = Fifo{};
    } else {
        if (channel < kInputFifos) input_fifo_[channel] = Fifo{};
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

    if (offset >= kMadamFifoBase && offset < kMadamFifoEnd) {
        const bool output = offset >= kMadamFifoOutput;
        const u32 channel = (offset >> 4) & 0x0f;
        Fifo& fifo = output ? output_fifo_[channel % kOutputFifos]
                            : input_fifo_[channel % kInputFifos];
        switch (offset & 0x0f) {
            case 0x00:
                fifo.address = value;
                // Setting the address abandons any reload that was armed.
                if (!output) fifo.next_address = 0;
                break;
            case 0x04:
                // A length of zero means an idle channel rather than a
                // zero-length one, and everything else is four bytes longer
                // than software asked for.
                fifo.length = value != 0 ? static_cast<s32>(value + 4) : 0;
                if (!output) fifo.next_length = 0;
                break;
            case 0x08:
                fifo.next_address = value;
                break;
            default:
                fifo.next_length = value != 0 ? static_cast<s32>(value + 4) : 0;
                break;
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

        case kMadamCcbCtl0:
            ccb_ctl0_ = value;
            break;
        case kMadamRegCtl0:
            reg_ctl0_ = value;
            read_stride_ = static_cast<s32>(decode_stride(value));
            write_stride_ = static_cast<s32>(decode_stride(value >> 8));
            framebuffer_configured_ = true;
            break;
        case kMadamRegCtl1:
            reg_ctl1_ = value;
            // Inclusive: the clip values are the last pixel drawn, not the
            // first one dropped.
            clip_width_ = ((value >> 0) & 0x3ffu) + 1u;
            clip_height_ = ((value >> 16) & 0x3ffu) + 1u;
            break;
        case kMadamRegCtl2:
            read_base_ = value;
            framebuffer_configured_ = true;
            break;
        case kMadamRegCtl3:
            write_base_ = value;
            framebuffer_configured_ = true;
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
// Turn one source value into a colour.
//
// A coded pixel is not an index. Only its low five bits select a palette
// entry; the bits above carry a per-channel multiplier that the pixel
// processor uses for shading. Treating all eight bits of an eight-bit pixel as
// an index reads two hundred and fifty-six entries out of a palette that holds
// thirty-two - so most of a cel's colours come from whatever happens to follow
// the palette in memory.
u16 Madam::decode_pixel(u32 value, u16* multiplier, bool* transparent) const {
    // The default multiplier is one in each of three channels, which is what
    // an uncoded pixel wants.
    u16 amv = 0x49;
    u16 result = 0;

    switch (cel_bpp_) {
        case 1: case 2: case 4:
            // The low nibble of the CCB's flags picks which block of the
            // palette a shallow cel draws from, so several cels can share one
            // loaded palette.
            result = plut_[((cel_pluta_ + (value & cel_pixel_mask_) * 2) >> 1) &
                           (kPlutEntries - 1)];
            break;

        case 6:
            result = plut_[value & 0x1f];
            result = static_cast<u16>((result & 0x7fffu) |
                                      ((value & 0x20u) != 0 ? 0x8000u : 0u));
            break;

        case 8:
            if (cel_linear_) {
                // Not coded at all: the byte IS the colour, two bits of blue
                // and three each of green and red, widened to five.
                const u32 blue  = value & 3u;
                const u32 green = (value >> 2) & 7u;
                const u32 red   = (value >> 5) & 7u;
                result = static_cast<u16>(
                    ((((red << 2) + (red >> 1)) & 0x1fu) << 10) |
                    ((((green << 2) + (green >> 1)) & 0x1fu) << 5) |
                    (((blue << 3) + (blue << 1) + (blue >> 1)) & 0x1fu));
            } else {
                result = plut_[value & 0x1f];
                // Three bits of multiplier, replicated across all three
                // channels.
                const u32 shade = ((value >> 6) & 3u) * 2u + ((value >> 5) & 1u);
                amv = static_cast<u16>((shade << 6) + (shade << 3) + shade);
            }
            break;

        default:
            if (cel_linear_) {
                result = static_cast<u16>(value);
            } else {
                result = plut_[value & 0x1f];
                result = static_cast<u16>((result & 0x7fffu) | (value & 0x8000u));
                // A separate three-bit multiplier per channel.
                amv = static_cast<u16>((((value >> 11) & 7u) << 6) |
                                       (((value >> 8) & 7u) << 3) |
                                       ((value >> 5) & 7u));
            }
            break;
    }

    if (multiplier != nullptr) {
        *multiplier = amv;
    }
    if (transparent != nullptr) {
        *transparent = ((result & 0x7fffu) == 0) && cel_transparent_mask_;
    }
    return result;
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

// Set up the per-cel state the pixel processor needs. None of it changes while
// a cel is being drawn, so it is done once rather than per pixel.
void Madam::begin_cel(const Ccb& ccb) {
    build_scale_table();
    cel_flags_ = ccb.flags;
    cel_pixc_ = ccb.pixc;
    cel_pre1_ = ccb.pre1;

    // PXOR chooses whether the first operand is carried through whole or the
    // second is taken as a constant. Two masks rather than a branch, because
    // they apply per channel.
    if ((ccb.flags & kCcbPxor) != 0) {
        cel_pxor1_ = 0;
        cel_pxor2_ = 0x1f1f1f1fu;
    } else {
        cel_pxor1_ = 0xffffffffu;
        cel_pxor2_ = 0;
    }

    const u32 pmode = ccb.flags & kCcbPoverMask;
    cel_pmode_or_ = (pmode == 0x180u) ? 0x8000u : 0x0000u;
    cel_pmode_and_ = (pmode != 0x100u) ? 0xffffu : 0x7fffu;

    cel_origin_vh_ = (static_cast<u32>(ccb.x) & 1u) |
                     ((static_cast<u32>(ccb.y) & 1u) << 15);

    // One source pixel to one screen pixel when the cel is neither scaled nor
    // rotated and lands on whole pixels. Anything else covers the rectangle
    // out to the next pixel's position instead.
    cel_one_to_one_ = (ccb.hdy == 0) && (ccb.vdx == 0) &&
                      (ccb.hdx == 0x10000 || ccb.hdx == -0x10000) &&
                      (ccb.vdy == 0x10000 || ccb.vdy == -0x10000) &&
                      ((ccb.x | ccb.y) & 0xffff) == 0;

    cel_bpp_ = bits_per_pixel(ccb.format);
    cel_linear_ = (ccb.pre0 & kPre0Linear) != 0;

    // A cel with the background flag clear treats colour zero as transparent.
    cel_transparent_mask_ = (ccb.flags & kCcbBgnd) == 0;

    switch (cel_bpp_) {
        case 1:
            cel_pluta_ = (ccb.flags & 0x0fu) * 4u;
            cel_pixel_mask_ = 1;
            break;
        case 2:
            cel_pluta_ = (ccb.flags & 0x0eu) * 4u;
            cel_pixel_mask_ = 3;
            break;
        case 4:
            cel_pluta_ = (ccb.flags & 0x08u) * 4u;
            cel_pixel_mask_ = 15;
            break;
        default:
            cel_pluta_ = 0;
            cel_pixel_mask_ = 0x1f;
            break;
    }

    // The palette is loaded only by a cel that asks for it. One that does not
    // draws with whatever the last cel left behind, which is how a run of
    // cels shares a palette without reloading it every time.
    if ((ccb.flags & kCcbLdPlut) != 0) {
        u32 entries = kPlutEntries;
        switch (cel_bpp_) {
            case 1: entries = 2; break;
            case 2: entries = 4; break;
            case 4: entries = 16; break;
            default: break;
        }
        const u32 base = ccb.plut_address & ~1u;
        for (u32 i = 0; i < entries; ++i) {
            plut_[i] = static_cast<u16>(
                (static_cast<u16>(bus_.read8(base + i * 2)) << 8) |
                bus_.read8(base + i * 2 + 1));
        }
    }
}

// One pixel, through the processor and into the framebuffer.
//
// This is the stage that decides what a cel actually looks like. It scales the
// source, optionally mixes it with what is already in the framebuffer, and
// sets the two sub-position bits. Writing the source straight out instead is
// not a subtle difference: it is the whole of a cel's brightness and all of
// its blending.
void Madam::process_pixel(s32 x, s32 y, u16 source, u16 amv) {
    if (x < 0 || y < 0 || static_cast<u32>(x) >= clip_width_ ||
        static_cast<u32>(y) >= clip_height_) {
        return;
    }

    const u32 read_at = read_base_ + pixel_offset(x, y, read_stride_);
    const u32 write_at = write_base_ + pixel_offset(x, y, write_stride_);
    const u16 frame = static_cast<u16>((static_cast<u16>(bus_.read8(read_at)) << 8) |
                                       bus_.read8(read_at + 1));

    const u32 pixel = (source | cel_pmode_or_) & cel_pmode_and_;

    // Two independent control words, chosen by the pixel's top bit - which is
    // how one cel gets two different blends without being two cels.
    const u32 control = (pixel & 0x8000u) ? (cel_pixc_ >> 16) : (cel_pixc_ & 0xffffu);
    const u32 dv2 = control & 1u;
    const u32 av  = (control >> 1) & 0x1fu;
    const u32 s2  = (control >> 6) & 3u;
    const u32 dv1 = (control >> 8) & 3u;
    const u32 mxf = (control >> 10) & 7u;
    const u32 ms  = (control >> 13) & 3u;
    const u32 s1  = (control >> 15) & 1u;

    u32 negate = 0, extend = 0, no_clip = 0, dv3 = 0;
    if ((cel_flags_ & kCcbUseAv) != 0) {
        negate  = av & 1u;
        extend  = (av >> 1) & 1u;
        no_clip = (av >> 2) & 1u;
        dv3     = (av >> 3) & 3u;
    }

    const u32 input = (s1 != 0) ? frame : pixel;
    const auto red   = [](u32 p) { return (p >> 10) & 0x1fu; };
    const auto green = [](u32 p) { return (p >> 5) & 0x1fu; };
    const auto blue  = [](u32 p) { return p & 0x1fu; };

    s32 second_r = 0, second_g = 0, second_b = 0;
    switch (s2) {
        case 0:
            break;
        case 1:
            second_r = second_g = second_b = static_cast<s32>(av >> dv3);
            break;
        case 2:
            second_r = static_cast<s32>(red(frame) >> dv3);
            second_g = static_cast<s32>(green(frame) >> dv3);
            second_b = static_cast<s32>(blue(frame) >> dv3);
            break;
        default:
            second_r = static_cast<s32>(red(pixel) >> dv3);
            second_g = static_cast<s32>(green(pixel) >> dv3);
            second_b = static_cast<s32>(blue(pixel) >> dv3);
            break;
    }

    s32 first_r = 0, first_g = 0, first_b = 0;
    switch (ms) {
        case 0:
            first_r = g_scale[mxf][dv1][red(input)];
            first_g = g_scale[mxf][dv1][green(input)];
            first_b = g_scale[mxf][dv1][blue(input)];
            break;
        case 1:
            // The multiplier comes from the pixel itself, three bits a
            // channel, which is how an eight-bit coded cel carries shading.
            first_r = g_scale[(amv >> 6) & 7][dv1][red(input)];
            first_g = g_scale[(amv >> 3) & 7][dv1][green(input)];
            first_b = g_scale[amv & 7][dv1][blue(input)];
            break;
        case 2: {
            const u32 pr = red(pixel), pg = green(pixel), pb = blue(pixel);
            first_r = g_scale[pr >> 2][pr & 3][red(input)];
            first_g = g_scale[pg >> 2][pg & 3][green(input)];
            first_b = g_scale[pb >> 2][pb & 3][blue(input)];
            break;
        }
        default:
            first_r = g_scale[4][dv1][red(input)];
            first_g = g_scale[4][dv1][green(input)];
            first_b = g_scale[4][dv1][blue(input)];
            break;
    }

    // The masks are applied a byte at a time on the hardware, and every byte
    // of each mask is the same, so one byte of it is enough here.
    const s32 mask1 = static_cast<s32>(cel_pxor1_ & 0xffu);
    const s32 mask2 = static_cast<s32>(cel_pxor2_ & 0xffu);
    const s32 keep_r = first_r & mask1;
    const s32 keep_g = first_g & mask1;
    const s32 keep_b = first_b & mask1;
    first_r &= mask2;
    first_g &= mask2;
    first_b &= mask2;

    s32 other_r, other_g, other_b;
    if (negate != 0) {
        other_r = second_r ^ 0xff;
        other_g = second_g ^ 0xff;
        other_b = second_b ^ 0xff;
    } else {
        other_r = second_r ^ first_r;
        other_g = second_g ^ first_g;
        other_b = second_b ^ first_b;
    }
    if (extend != 0) {
        // Sign-extend from five bits, so a difference can come out negative.
        const auto sign5 = [](s32 v) {
            return static_cast<s32>(static_cast<s8>(v << 3)) >> 3;
        };
        other_r = sign5(other_r);
        other_g = sign5(other_g);
        other_b = sign5(other_b);
    }

    s32 out_r = (keep_r + other_r + static_cast<s32>(negate)) >> dv2;
    s32 out_g = (keep_g + other_g + static_cast<s32>(negate)) >> dv2;
    s32 out_b = (keep_b + other_b + static_cast<s32>(negate)) >> dv2;
    if (no_clip == 0) {
        out_r = clamp_channel(out_r);
        out_g = clamp_channel(out_g);
        out_b = clamp_channel(out_b);
    }

    u32 result = (static_cast<u32>(out_r & 0x1f) << 10) |
                 (static_cast<u32>(out_g & 0x1f) << 5) |
                 static_cast<u32>(out_b & 0x1f);

    // A cel not allowed to write pure black writes the darkest non-black
    // instead, so it still covers what was underneath.
    if ((cel_flags_ & kCcbNoBlk) == 0 && result == 0) {
        result = 1u << 10;
    }

    // The projector decides the two sub-position bits.
    u32 vh = ((cel_flags_ & kCcbPlutPos) != 0) ? (source & 0x8001u) : cel_origin_vh_;
    if ((ccb_ctl0_ & kCtl0SwapHv) != 0 && (cel_pre1_ & kPre1NoSwap) == 0) {
        vh = (vh >> 15) | ((vh & 1u) << 15);
    }
    switch (ccb_ctl0_ & kCtl0B15Mask) {
        case kCtl0B15Zero: vh &= ~0x8000u; break;
        case kCtl0B15One:  vh |= 0x8000u;  break;
        default: break;
    }
    switch (ccb_ctl0_ & kCtl0B0Mask) {
        case kCtl0B0Zero: vh &= ~1u; break;
        case kCtl0B0One:  vh |= 1u;  break;
        case kCtl0B0Ppmp: vh = (vh & ~1u) | (result & 1u); break;
        default: break;
    }

    const u32 final_pixel = (result & 0x7ffeu) | vh;
    bus_.write8(write_at, static_cast<u8>(final_pixel >> 8));
    bus_.write8(write_at + 1, static_cast<u8>(final_pixel));
    ++stats_.pixels_written;
}

void Madam::put_pixel(s32 x, s32 y, u16 pixel, u16 shade) {
    // Once software has programmed the framebuffer registers, every pixel goes
    // through the processor. Before that - which is only ever a test - fall
    // back to a plain store so the pixel conversion can be checked alone.
    if (framebuffer_configured_) {
        process_pixel(x, y, pixel, shade);
        return;
    }
    if (pixel == 0) {
        return;
    }
    if (x < 0 || y < 0 || static_cast<u32>(x) >= clip_width_ ||
        static_cast<u32>(y) >= clip_height_) {
        return;
    }
    // The same layout the display reads. MADAM writing linearly while the VDLP
    // read interleaved produced a picture that was wrong in both chips' favour
    // depending on which you suspected, so the offset is computed in one place.
    // Once software has programmed the framebuffer registers they are the
    // truth. Before that - which is only ever a test, since the OS programs
    // them long before it draws anything - fall back to where the console
    // pointed us.
    const u32 address =
        framebuffer_configured_
            ? write_base_ + pixel_offset(x, y, write_stride_)
            : target_address_ + framebuffer_offset(x, y, static_cast<int>(clip_width_));
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
// One source pixel onto the screen.
//
// A cel that is neither scaled nor rotated puts one source pixel on one screen
// pixel, and that is the common case by a long way - upwards of ninety-eight
// per cent of the cels in most titles.
//
// A scaled cel instead covers the rectangle from where this pixel lands to
// where the NEXT one will. Taking the bounds from the next position rather
// than from the size of the step is what makes it exact: derive a span from
// the step magnitude and a fractional step leaves gaps on some pixels and
// double-writes others.
//
// An empty rectangle draws nothing. That is not a degenerate case to guard
// against - it is how a cel drawn smaller than its source drops the pixels
// that do not fit.
void Madam::plot_footprint(const Ccb& ccb, s32 px, s32 py, s32 step_x,
                           s32 step_y, u16 pixel, u16 shade) {
    if (cel_one_to_one_) {
        put_pixel(px >> 16, py >> 16, pixel, shade);
        return;
    }

    s32 x0 = px >> 16;
    s32 x1 = (px + step_x) >> 16;
    s32 y0 = py >> 16;
    s32 y1 = (py + ccb.vdy) >> 16;
    if (x1 < x0) { const s32 t = x0; x0 = x1 + 1; x1 = t + 1; }
    if (y1 < y0) { const s32 t = y0; y0 = y1 + 1; y1 = t + 1; }

    // Clip the rectangle before walking it, not inside the loop.
    //
    // A single source pixel of a heavily magnified cel can cover more of the
    // plane than the screen holds, and most of that is off it. Testing each
    // one against the clip as it comes still costs a loop iteration per
    // invisible pixel - which on one title turned twenty thousand frames from
    // eighty-eight seconds into four hundred and thirteen.
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > static_cast<s32>(clip_width_)) x1 = static_cast<s32>(clip_width_);
    if (y1 > static_cast<s32>(clip_height_)) y1 = static_cast<s32>(clip_height_);

    for (s32 y = y0; y < y1; ++y) {
        for (s32 x = x0; x < x1; ++x) {
            put_pixel(x, y, pixel, shade);
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

    for (u32 row = 0; row < ccb.height; ++row) {
        BitReader reader(bus, row_data);
        const u32 length = reader.read(length_bits);

        // The field counts words and excludes itself and one more, hence +2.
        const u32 row_end = row_data + ((length + 2u) << 2);

        s32 px = row_x;
        s32 py = row_y;

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
                u16 shade = 0;
                bool clear = false;
                const u16 pixel = decode_pixel(reader.read(bpp), &shade, &clear);
                for (u32 i = 0; i < count; ++i) {
                    if (!clear) {
                        plot_footprint(ccb, px, py, step_x, step_y, pixel, shade);
                    }
                    px += step_x;
                    py += step_y;
                }
                continue;
            }
            for (u32 i = 0; i < count; ++i) {
                u16 shade = 0;
                bool clear = false;
                const u16 pixel = decode_pixel(reader.read(bpp), &shade, &clear);
                if (!clear) {
                    plot_footprint(ccb, px, py, step_x, step_y, pixel, shade);
                }
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

// An unpacked cel, read a row at a time.
//
// Its rows are NOT tightly packed. Each starts a fixed number of WORDS after
// the last, and that stride is carried in PRE1 rather than derived from the
// width - so a cel narrower than its stride has slack at the end of every row
// that belongs to nobody. Deriving the stride from the width instead walks
// diagonally into the picture, one row further out of step each line, which
// looks like a palette fault rather than an addressing one.
//
// A row may also begin with pixels to be skipped, and those come off the
// width as well as off the front.
void Madam::draw_unpacked_cel(const Ccb& ccb) {
    const unsigned bpp = bits_per_pixel(ccb.format);
    if (bpp == 0 || ccb.height == 0 || ccb.height > kMaxCelDimension) {
        return;
    }

    // The stride field is eight bits at low colour depths and ten above,
    // because a deeper row needs a longer reach.
    const u32 stride_words = bpp < 8 ? ((ccb.pre1 >> 24) & 0xffu)
                                     : ((ccb.pre1 >> 16) & 0x3ffu);
    const u32 row_bytes = (stride_words + 2u) << 2;

    const u32 skip = (ccb.pre0 >> 24) & 0x0fu;
    if (skip >= ccb.width) {
        return;
    }
    const u32 width = ccb.width - skip;

    s32 row_x = ccb.x;
    s32 row_y = ccb.y;
    s32 step_x = ccb.hdx;
    s32 step_y = ccb.hdy;

    for (u32 row = 0; row < ccb.height; ++row) {
        BitReader reader(bus_, ccb.source_address + row * row_bytes);
        reader.skip(bpp * skip);

        s32 px = row_x;
        s32 py = row_y;

        for (u32 column = 0; column < width; ++column) {
            u16 shade = 0;
            bool clear = false;
            const u16 pixel = decode_pixel(reader.read(bpp), &shade, &clear);
            if (!clear) {
                plot_footprint(ccb, px, py, step_x, step_y, pixel, shade);
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

// One cel. Which of the two source formats it is decides everything else, so
// the choice is made once here rather than threaded through the drawing.
void Madam::draw_cel(const Ccb& ccb) {
    if (ccb.format == CelFormat::Unknown || ccb.width == 0 || ccb.height == 0) {
        return;
    }
    if (ccb.width > kMaxCelDimension || ccb.height > kMaxCelDimension) {
        return;
    }
    begin_cel(ccb);
    if ((ccb.flags & kCcbPacked) != 0) {
        draw_packed_cel(ccb);
    } else {
        draw_unpacked_cel(ccb);
    }
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
