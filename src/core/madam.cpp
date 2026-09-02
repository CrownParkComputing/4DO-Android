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
};

// PRE0/PRE1 fields from the official 3DO hardware.h and Graphics Programming
// Guide. Keep the extraction here so the DMA-facing layout stays separate from
// the renderer's internal Ccb representation.
constexpr u32 kPre0FormatMask  = 0x00000007u;
// Set when a pixel carries its colour directly instead of an index into the
// palette. Bit FOUR, not the top bit - the top bit is something else entirely.
constexpr u32 kPre0Linear      = 0x00000010u;
constexpr u32 kPre0HeightShift = 6;
constexpr u32 kPre0HeightMask  = 0x000003ffu;
constexpr u32 kPre1WidthMask   = 0x000007ffu;

// Set when a cel's source is stored in the framebuffer's own interleaved
// left/right layout rather than as a flat bitstream.
constexpr u32 kPre1Lrform = 0x00000800u;

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
    revision_ = kMadamRevisionGreen;
    mem_config_ = kMadamMemConfigStock;
    dma_enable_ = 0;
    dma_channel_enable_ = 0;
    xbus_dma_address_ = 0;
    xbus_dma_length_ = 0;
    xbus_dma_pending_ = false;
    vdl_address_ = 0;
    for (u32 i = 0; i < kMadamDmaChannels; ++i) {
        dma_address_[i] = 0;
        dma_length_[i] = 0;
    }
    clip_width_ = 320;
    clip_height_ = 240;
    target_address_ = kVramBase;
    framebuffer_configured_ = false;
    reg_ctl0_ = 0;
    reg_ctl1_ = 0;
    read_base_ = 0;
    write_base_ = 0;
    read_stride_ = 320 * 4;
    write_stride_ = 320 * 4;
    target_stride_bytes_ = 320 * 2;
    pbus_address_ = 0;
    pbus_length_ = 0;
    pbus_pointer_ = 0;
    current_ccb_ = 0;
    next_ccb_ = 0;
    cel_engine_state_ = CelEngineState::Idle;
    cel_walk_started_ = false;
    ccb_ctl0_ = 0;
    cel_flags_ = 0;
    cel_pixc_ = 0;
    cel_pre1_ = 0;
    cel_pxor1_ = 0xffffffffu;
    cel_pxor2_ = 0;
    cel_pmode_or_ = 0;
    cel_pmode_and_ = 0xffffu;
    cel_origin_vh_ = 0;
    cel_bpp_ = 0;
    cel_linear_ = false;
    cel_pluta_ = 0;
    cel_pixel_mask_ = 0;
    cel_transparent_mask_ = true;
    ccb_x_register_ = 0;
    ccb_y_register_ = 0;
    ccb_hdx_register_ = 0;
    ccb_hdy_register_ = 0;
    ccb_vdx_register_ = 0;
    ccb_vdy_register_ = 0;
    ccb_hddx_register_ = 0;
    ccb_hddy_register_ = 0;
    ccb_pixc_register_ = 0;
    pre0_register_ = 0;
    pre1_register_ = 0;
    for (Fifo& fifo : input_fifo_) fifo = Fifo{};
    for (Fifo& fifo : output_fifo_) fifo = Fifo{};
    for (u16& entry : plut_) entry = 0;
    for (s32& value : matrix_in_) value = 0;
    for (s32& value : matrix_vec_) value = 0;
    for (s32& value : matrix_out_) value = 0;
    for (s64& value : matrix_pending_) value = 0;
    matrix_num_hi_ = 0;
    matrix_num_lo_ = 0;
    engine_runs_ = 0;
    total_cels_drawn_ = 0;
    stats_ = MadamStats{};
}

void Madam::set_clip(u32 width, u32 height) {
    clip_width_ = width;
    clip_height_ = height;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
// The matrix unit.
//
// A 4x4 multiply of 16.16 fixed point, with an optional perspective divide.
// Everything is done in 64 bits and shifted back down, because the products of
// two 16.16 values overflow 32 bits long before the result does.
//
// The outputs are double buffered. An operation computes into a holding set,
// and it is the PREVIOUS result that moves into the readable outputs as it
// does so. Software reads the answer to the operation before the one it just
// asked for - which is how a title keeps the unit fed without ever waiting on
// it, and is not an optimisation anyone would guess at.
void Madam::matrix_execute(u32 operation) {
    // Publish the previous result first, whatever this operation turns out to
    // be. Operation zero is that and nothing else.
    for (int i = 0; i < 4; ++i) {
        matrix_out_[i] = static_cast<s32>(matrix_pending_[i]);
    }

    // The SDK defines these operations as row/vector dot products on signed
    // 16.16 values. Accumulate at full precision, then discard the low sixteen
    // fractional bits once, after the sum. Shifting each product separately
    // loses up to one unit per column.
    const auto dot = [this](int row, int columns) {
        s64 sum = 0;
        for (int column = 0; column < columns; ++column) {
            sum += static_cast<s64>(matrix_in_[row * 4 + column]) *
                   static_cast<s64>(matrix_vec_[column]);
        }
        return sum >> 16;
    };

    switch (operation) {
        case kMatrixCopyOnly:
            return;

        case kMatrixMultiply4x4:
            for (int row = 0; row < 4; ++row) {
                matrix_pending_[row] = dot(row, 4);
            }
            return;

        case kMatrixMultiply3x3:
            for (int row = 0; row < 3; ++row) {
                matrix_pending_[row] = dot(row, 3);
            }
            return;

        case kMatrixMultiply3x3DivideZ: {
            // Transform, then divide x and y by z - the perspective divide,
            // done in hardware. The numerator is a 64-bit value software loads
            // separately, so it controls the field of view.
            s64 numerator = (static_cast<s64>(matrix_num_hi_) << 32) |
                            static_cast<u32>(matrix_num_lo_);

            matrix_pending_[2] = dot(2, 3);
            // A vertex exactly on the eye plane would divide by zero. The
            // hardware leaves the numerator alone rather than trapping.
            if (matrix_pending_[2] != 0) {
                numerator /= matrix_pending_[2];
            }

            matrix_pending_[0] = dot(0, 3);
            matrix_pending_[1] = dot(1, 3);
            matrix_pending_[0] = (matrix_pending_[0] * numerator) >> 32;
            matrix_pending_[1] = (matrix_pending_[1] * numerator) >> 32;
            return;
        }

        default:
            return;
    }
}

u32 Madam::read(u32 offset) {
    offset &= (kMadamWindowSize - 1);
    u32 channel = 0;
    bool is_length = false;
    if (dma_slot(offset, &channel, &is_length)) {
        return is_length ? dma_length_[channel] : dma_address_[channel];
    }

    if (offset >= kMadamMatrixIn && offset < kMadamMatrixIn + 16 * 4) {
        return static_cast<u32>(matrix_in_[(offset - kMadamMatrixIn) / 4]);
    }
    if (offset >= kMadamMatrixVec && offset < kMadamMatrixVec + 4 * 4) {
        return static_cast<u32>(matrix_vec_[(offset - kMadamMatrixVec) / 4]);
    }
    if (offset >= kMadamMatrixOut && offset < kMadamMatrixOut + 4 * 4) {
        return static_cast<u32>(matrix_out_[(offset - kMadamMatrixOut) / 4]);
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
        case kMadamCelStatus:
            switch (cel_engine_state_) {
                case CelEngineState::Idle:       return 0;
                case CelEngineState::InProcess:  return kMadamCelRunning;
                case CelEngineState::Suspended:  return kMadamCelRunning |
                                                        kMadamCelPaused;
            }
            return 0;
        case kMadamXbusDmaAddress:   return xbus_dma_address_;
        case kMadamXbusDmaLength:    return xbus_dma_length_;
        case kMadamVdlAddress: return vdl_address_;
        case kMadamPbusAddress: return pbus_address_;
        case kMadamPbusLength:  return pbus_length_;
        case kMadamPbusPointer: return pbus_pointer_;
        case kMadamCcbCtl0:    return ccb_ctl0_;
        case kMadamMatrixNumHi: return matrix_num_hi_;
        case kMadamMatrixNumLo: return matrix_num_lo_;
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

    if (offset >= kMadamMatrixIn && offset < kMadamMatrixIn + 16 * 4) {
        matrix_in_[(offset - kMadamMatrixIn) / 4] = static_cast<s32>(value);
        return;
    }
    if (offset >= kMadamMatrixVec && offset < kMadamMatrixVec + 4 * 4) {
        matrix_vec_[(offset - kMadamMatrixVec) / 4] = static_cast<s32>(value);
        return;
    }
    if (offset >= kMadamMatrixOut && offset < kMadamMatrixOut + 4 * 4) {
        matrix_out_[(offset - kMadamMatrixOut) / 4] = static_cast<s32>(value);
        return;
    }

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
            // Read-only chip identification. Some boot code probes low MADAM
            // ports with writes; accepting one would make the machine change
            // revision while it is running.
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
        case kMadamCelStatus:
            // Read-only engine state.
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
        // the instruction.
        //
        // Starting the engine does not run it. It runs once the CPU's current
        // instruction is finished, which is what the hardware does and what
        // software is entitled to rely on - Need for Speed starts the engine
        // and then finishes writing the CCB the engine is about to read.
        case kMadamCelStart:
            if (cel_engine_state_ == CelEngineState::Idle) {
                cel_engine_state_ = CelEngineState::InProcess;
                cel_walk_started_ = false;
                bus_.request_cel_engine();
            }
            break;
        case kMadamCelStop:
            cel_engine_state_ = CelEngineState::Idle;
            cel_walk_started_ = false;
            bus_.cancel_cel_engine();
            next_ccb_ = 0;
            break;
        case kMadamCelResume:
            if (cel_engine_state_ == CelEngineState::Suspended) {
                cel_engine_state_ = CelEngineState::InProcess;
                bus_.request_cel_engine();
            }
            break;
        case kMadamCelPause:
            if (cel_engine_state_ == CelEngineState::InProcess) {
                cel_engine_state_ = CelEngineState::Suspended;
                // This catches a pause in the same multi-store instruction as
                // START. Once rendering has actually seized the data bus, the
                // CPU cannot issue this write until the engine yields.
                bus_.cancel_cel_engine();
            }
            break;

        case kMadamCcbCtl0:
            ccb_ctl0_ = value;
            break;
        case kMadamMatrixNumHi:
            matrix_num_hi_ = value;
            break;
        case kMadamMatrixNumLo:
            matrix_num_lo_ = value;
            break;
        case kMadamMatrixCtl:
            matrix_execute(value);
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


Ccb Madam::read_ccb(u32 address) {
    Ccb ccb;
    Bus& bus = bus_;

    ccb.flags = bus.read32(address + kCcbFlagsWord * 4);

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
    const u32 next_raw = bus.read32(address + kCcbNextWord * 4) &
                         kCcbAddressMask;
    ccb.next_address = next_raw == 0
                           ? 0u
                           : ((ccb.flags & kCcbNpAbs) ? next_raw
                                                      : next_base + next_raw);

    const u32 source_raw = bus.read32(address + kCcbSourceWord * 4) &
                           kCcbAddressMask;
    ccb.source_address = (ccb.flags & kCcbSpAbs) ? source_raw
                                                 : source_base + source_raw;

    const u32 plut_raw = bus.read32(address + kCcbPlutWord * 4) &
                         kCcbAddressMask;
    ccb.plut_address = (ccb.flags & kCcbPpAbs) ? plut_raw
                                               : plut_base + plut_raw;

    // The first six words always occupy space, but the rest of a CCB is a
    // compact stream feeding load-controlled hardware registers. When a load
    // flag is clear the field is absent and the preceding cel's register value
    // remains live. Treating every CCB as a fixed 15-word structure consumes
    // PRE0/PIXC data as transform deltas and shreds linked sprites into strips.
    u32 cursor = address + (kCcbYWord + 1) * 4;
    if ((ccb.flags & kCcbSkip) == 0 && (ccb.flags & kCcbYoxy) != 0) {
        ccb_x_register_ = static_cast<s32>(bus.read32(address + kCcbXWord * 4));
        ccb_y_register_ = static_cast<s32>(bus.read32(address + kCcbYWord * 4));
    }

    // Horizontal vectors and their derivatives are stored as 12.20 and become
    // 16.16 here. Vertical vectors are already 16.16.
    if ((ccb.flags & kCcbLdSize) != 0) {
        ccb_hdx_register_ = static_cast<s32>(bus.read32(cursor)) >> 4;
        ccb_hdy_register_ = static_cast<s32>(bus.read32(cursor + 4)) >> 4;
        ccb_vdx_register_ = static_cast<s32>(bus.read32(cursor + 8));
        ccb_vdy_register_ = static_cast<s32>(bus.read32(cursor + 12));
        cursor += 16;
    }
    if ((ccb.flags & kCcbLdPrs) != 0) {
        ccb_hddx_register_ = static_cast<s32>(bus.read32(cursor)) >> 4;
        ccb_hddy_register_ = static_cast<s32>(bus.read32(cursor + 4)) >> 4;
        cursor += 8;
    }
    if ((ccb.flags & kCcbLdPpmp) != 0) {
        ccb_pixc_register_ = bus.read32(cursor);
        cursor += 4;
    }

    ccb.x = ccb_x_register_;
    ccb.y = ccb_y_register_;
    ccb.hdx = ccb_hdx_register_;
    ccb.hdy = ccb_hdy_register_;
    ccb.vdx = ccb_vdx_register_;
    ccb.vdy = ccb_vdy_register_;
    ccb.hddx = ccb_hddx_register_;
    ccb.hddy = ccb_hddy_register_;
    ccb.pixc = ccb_pixc_register_;

    // Where the preamble comes from, which is not always the CCB.
    //
    // CCBPRE says the two preamble words are carried in the CCB. Cleared, they
    // are the first words of the SOURCE DATA instead, and the pixels begin
    // after them - which is how a title stores a cel as one self-describing
    // blob and points a bare CCB at it. Reading the CCB's own words in that
    // case picks up whatever follows the structure in memory, so the cel gets
    // an invented format and size.
    //
    // PRE1 is a register rather than a per-cel value: a packed cel never
    // supplies one, from either place, and keeps whatever the last unpacked
    // cel left there. Its own rows say how long they are, so it has no use for
    // a width - but the register still has to survive, because the next
    // unpacked cel may not reload it either.
    if ((ccb.flags & kCcbCcbPre) != 0) {
        pre0_register_ = bus.read32(cursor);
        cursor += 4;
        if ((ccb.flags & kCcbPacked) == 0) {
            pre1_register_ = bus.read32(cursor);
            cursor += 4;
        }
    } else {
        pre0_register_ = bus.read32(ccb.source_address);
        ccb.source_address += 4;
        if ((ccb.flags & kCcbPacked) == 0) {
            pre1_register_ = bus.read32(ccb.source_address);
            ccb.source_address += 4;
        }
    }
    ccb.pre0 = pre0_register_;
    ccb.pre1 = pre1_register_;

    ccb.format = cel_format_from_pre0(ccb.pre0);
    ccb.width  = cel_width_from_pre1(ccb.pre1);
    ccb.height = cel_height_from_pre0(ccb.pre0);
    ccb.fetch_end_address = cursor;

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

    // Which mapper. The hardware picks by the same test it uses to decide
    // visibility: a cel with no perspective stepping that is square-on to one
    // axis is scaled, and everything else is a four-sided figure.
    if (ccb.hddx == 0 && ccb.hddy == 0 &&
        ((ccb.hdx == 0 && ccb.vdy == 0) || (ccb.hdy == 0 && ccb.vdx == 0))) {
        cel_mapper_ = cel_one_to_one_ ? Mapper::Line : Mapper::Scale;
    } else {
        cel_mapper_ = Mapper::Arbitrary;
    }

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
void Madam::plot_texel(const Ccb& ccb, s32 px, s32 py, s32 step_x, s32 step_y,
                       s32 down_x, s32 down_y, s32 next_step_x, s32 next_step_y,
                       u16 pixel, u16 shade) {
    if (cel_mapper_ != Mapper::Arbitrary) {
        plot_footprint(ccb, px, py, step_x, step_y, pixel, shade);
        return;
    }
    // The four corners, in order round the figure: this pixel, the next one
    // along this row, the matching one on the next row, and the one below this.
    plot_quad(ccb, px, py, px + step_x, py + step_y,
              down_x + next_step_x, down_y + next_step_y, down_x, down_y,
              pixel, shade);
}

// One source pixel of a rotated or perspective-stepped cel, which lands as a
// four-sided figure rather than a rectangle.
//
// Filled by a conventional active-edge scan conversion. Each non-horizontal
// edge contributes on [min_y,max_y), the usual top-inclusive/bottom-exclusive
// rule that lets adjacent texels share an edge without both painting it. Edge
// direction retains the CCB's clockwise/counter-clockwise face selection. A
// folded quad can yield four crossings, so sorted crossings are consumed in
// pairs rather than assuming every row has exactly one span.
void Madam::plot_quad(const Ccb& ccb, s32 ax, s32 ay, s32 bx, s32 by,
                      s32 cx, s32 cy, s32 dx, s32 dy, u16 pixel, u16 shade) {
    ax >>= 16; bx >>= 16; cx >>= 16; dx >>= 16;
    ay >>= 16; by >>= 16; cy >>= 16; dy >>= 16;

    // Collapsed to a line: no area, nothing to fill.
    if (ax == bx && bx == cx && cx == dx) {
        return;
    }

    const s32 max_x = static_cast<s32>(clip_width_);

    s32 lowest = ay;
    s32 highest = ay;
    for (s32 v : {by, cy, dy}) {
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
    }
    const s32 first_y = lowest < 0 ? 0 : lowest;
    const s32 last_y = highest < static_cast<s32>(clip_height_)
                           ? highest
                           : static_cast<s32>(clip_height_);

    const s32 xs[4] = {ax, bx, cx, dx};
    const s32 ys[4] = {ay, by, cy, dy};

    struct Crossing { s32 x; bool downward; };
    for (s32 y = first_y; y < last_y; ++y) {
        Crossing crossing[4] = {};
        int count = 0;

        for (int edge = 0; edge < 4; ++edge) {
            const int from = edge;
            const int to = (edge + 1) & 3;
            if (ys[from] == ys[to]) continue;
            const s32 min_y = ys[from] < ys[to] ? ys[from] : ys[to];
            const s32 max_edge_y = ys[from] < ys[to] ? ys[to] : ys[from];
            if (y < min_y || y >= max_edge_y) continue;

            const s64 numerator = static_cast<s64>(xs[to] - xs[from]) *
                                  static_cast<s64>(y - ys[from]);
            crossing[count++] = {
                xs[from] + static_cast<s32>(numerator / (ys[to] - ys[from])),
                ys[to] > ys[from],
            };
        }

        for (int i = 1; i < count; ++i) {
            const Crossing value = crossing[i];
            int j = i;
            while (j > 0 && crossing[j - 1].x > value.x) {
                crossing[j] = crossing[j - 1];
                --j;
            }
            crossing[j] = value;
        }

        const auto permitted = [&](int direction) {
            return (((ccb.flags & kCcbAcw) != 0 && direction == 0) ||
                    ((ccb.flags & kCcbAccw) != 0 && direction == 1));
        };
        const auto fill = [&](s32 from, s32 to) {
            if (from < 0) from = 0;
            if (to > max_x) to = max_x;
            for (s32 x = from; x < to; ++x) {
                put_pixel(x, y, pixel, shade);
            }
        };

        for (int i = 0; i + 1 < count; i += 2) {
            if (permitted(crossing[i].downward ? 1 : 0)) {
                fill(crossing[i].x, crossing[i + 1].x);
            }
        }
    }
}

void Madam::plot_footprint(const Ccb& ccb, s32 px, s32 py, s32 step_x,
                           s32 step_y, u16 pixel, u16 shade) {
    if (cel_one_to_one_) {
        put_pixel(px >> 16, py >> 16, pixel, shade);
        return;
    }

    // How far one source pixel reaches, and it is not the horizontal step.
    //
    // A magnified cel covers the ground between where this pixel lands and
    // where the NEXT one would - but "the next one" is displaced by BOTH step
    // vectors together, not just the horizontal one. The hardware spans
    // hdx+vdx across and hdy+vdy down.
    //
    // Using the horizontal step alone works for an ordinary upright sprite,
    // where the vertical step contributes nothing across, and fails completely
    // for the case that matters most: a 1x1 cel stretched into a quad. Need
    // for Speed paints its road and scenery that way, a hundred stretched 1x1
    // cels a frame, and many of them have hdx of exactly zero - the quad's
    // whole width comes from vdx. Every one of those covered a rectangle zero
    // pixels wide and drew nothing at all, which is why the road was a flat
    // green field with a correct dashboard sitting on it.
    s32 span_x = step_x + ccb.vdx;
    s32 span_y = step_y + ccb.vdy;

    // MARIA limits how far one source pixel may be stretched downwards. It is
    // there so a cel scaled to the horizon does not smear a single row over
    // the whole screen.
    if ((ccb.flags & kCcbMaria) != 0 && span_y > 0x10000) {
        span_y = 0x10000;
    }

    s32 x0 = px >> 16;
    s32 x1 = (px + span_x) >> 16;
    s32 y0 = py >> 16;
    s32 y1 = (py + span_y) >> 16;
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
        // Where the matching pixel of the NEXT row lands, and the step that
        // row will use. The rotated mapper needs both to know the shape one
        // source pixel covers; the others ignore them.
        const s32 next_step_x = step_x + ccb.hddx;
        const s32 next_step_y = step_y + ccb.hddy;
        s32 down_x = row_x + ccb.vdx;
        s32 down_y = row_y + ccb.vdy;

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
                // The arbitrary mapper tracks both the top and bottom edge of
                // each source texel. A transparent packet consumes source
                // columns just like a literal packet, so both edges must move
                // past them. Advancing only the top edge makes the next
                // visible texel span backwards across the transparent run;
                // rotated packed sprites then fan out into horizontal strips
                // (the leaned rider in Road Rash is a direct example).
                down_x += next_step_x * static_cast<s32>(count);
                down_y += next_step_y * static_cast<s32>(count);
                continue;
            }
            if (type == 3) {
                u16 shade = 0;
                bool clear = false;
                const u16 pixel = decode_pixel(reader.read(bpp), &shade, &clear);
                for (u32 i = 0; i < count; ++i) {
                    if (!clear) {
                        plot_texel(ccb, px, py, step_x, step_y, down_x, down_y,
                                   next_step_x, next_step_y, pixel, shade);
                    }
                    px += step_x;
                    py += step_y;
                    down_x += next_step_x;
                    down_y += next_step_y;
                }
                continue;
            }
            for (u32 i = 0; i < count; ++i) {
                u16 shade = 0;
                bool clear = false;
                const u16 pixel = decode_pixel(reader.read(bpp), &shade, &clear);
                if (!clear) {
                    plot_texel(ccb, px, py, step_x, step_y, down_x, down_y,
                               next_step_x, next_step_y, pixel, shade);
                }
                px += step_x;
                py += step_y;
                down_x += next_step_x;
                down_y += next_step_y;
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
// An LR-form cel, whose source is stored the way the FRAMEBUFFER is stored.
//
// "LR" is left/right: the source rows are interleaved in pairs, two vertically
// adjacent lines sharing each 32-bit word, exactly as the display's own
// framebuffer is laid out. That is the whole point of the format - it lets a
// title hand the cel engine a buffer it has just rendered or decoded into,
// with no rearranging in between.
//
// Which is why it is not an exotic corner of the hardware. Need for Speed
// draws 1,047 of its 1,051 unpacked cels this way, because its video decoder
// writes framebuffer-shaped output and then blits it. Alone in the Dark uses
// it too. Sending these down the ordinary literal path reads the source as a
// flat bitstream, so every row comes out of the wrong place.
//
// The official guide defines the storage directly: VCNT counts row pairs, and
// each 32-bit source word is [row 2n pixel x, row 2n+1 pixel x]. Work in those
// pairs here instead of adapting the ordinary row-stream decoder.
void Madam::draw_lr_cel(const Ccb& ccb) {
    const u32 stride_words = (ccb.pre1 >> 16) & 0x3ffu;
    const u32 pair_stride = (stride_words + 2u) * sizeof(u32);
    const u32 pair_count =
        ((ccb.pre0 >> kPre0HeightShift) & kPre0HeightMask) + 1u;
    if (pair_count * 2u > kMaxCelDimension) {
        return;
    }

    s32 row_x = ccb.x;
    s32 row_y = ccb.y;
    s32 step_x = ccb.hdx;
    s32 step_y = ccb.hdy;

    for (u32 pair = 0; pair < pair_count; ++pair) {
        const u32 pair_base = ccb.source_address + pair * pair_stride;
        for (u32 half = 0; half < 2; ++half) {
            s32 px = row_x;
            s32 py = row_y;
            const s32 next_step_x = step_x + ccb.hddx;
            const s32 next_step_y = step_y + ccb.hddy;
            s32 down_x = row_x + ccb.vdx;
            s32 down_y = row_y + ccb.vdy;

            for (u32 column = 0; column < ccb.width; ++column) {
                const u32 at = pair_base + column * sizeof(u32) + half * sizeof(u16);
                const u16 source_pixel = bus_.read16(at);

                u16 shade = 0;
                bool clear = false;
                const u16 pixel = decode_pixel(source_pixel, &shade, &clear);
                if (!clear) {
                    plot_texel(ccb, px, py, step_x, step_y, down_x, down_y,
                               next_step_x, next_step_y, pixel, shade);
                }
                px += step_x;
                py += step_y;
                down_x += next_step_x;
                down_y += next_step_y;
            }

            row_x += ccb.vdx;
            row_y += ccb.vdy;
            step_x += ccb.hddx;
            step_y += ccb.hddy;
        }
    }

    ++stats_.cels_drawn;
}

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
        // Where the matching pixel of the NEXT row lands, and the step that
        // row will use. The rotated mapper needs both to know the shape one
        // source pixel covers; the others ignore them.
        const s32 next_step_x = step_x + ccb.hddx;
        const s32 next_step_y = step_y + ccb.hddy;
        s32 down_x = row_x + ccb.vdx;
        s32 down_y = row_y + ccb.vdy;

        for (u32 column = 0; column < width; ++column) {
            u16 shade = 0;
            bool clear = false;
            const u16 pixel = decode_pixel(reader.read(bpp), &shade, &clear);
            if (!clear) {
                plot_texel(ccb, px, py, step_x, step_y, down_x, down_y,
                           next_step_x, next_step_y, pixel, shade);
            }
            px += step_x;
            py += step_y;
            down_x += next_step_x;
            down_y += next_step_y;
        }

        row_x += ccb.vdx;
        row_y += ccb.vdy;
        step_x += ccb.hddx;
        step_y += ccb.hddy;
    }

    ++stats_.cels_drawn;
}

// Which way round a quad is wound, given its two step vectors.
//
// Returns the flag that names that winding, so it can be compared against the
// CCB's own pair of flags directly.
namespace {
u32 texel_winding(s64 hdx, s64 hdy, s64 vdx, s64 vdy) {
    // Sign of the 2-D cross product. In screen coordinates +Y points down, so
    // a positive determinant is the hardware's counter-clockwise permission.
    return (hdx * vdy - hdy * vdx) > 0 ? kCcbAccw : kCcbAcw;
}
}  // namespace

// Whether a cel is thrown away before any of it is drawn.
//
// The hardware decides this once, from the CCB alone, and there are four
// separate reasons it can say no. Drawing anyway is not just wasted work: a
// title that leans on the winding tests to hide the back faces of its geometry
// gets those faces drawn over the front ones.
//
//   1. A cel that permits neither winding is not drawn at all. Across this
//      library that never happens - measured, not assumed - but it is the
//      hardware's first question and costs one test.
//   2. A bounding box entirely off one edge of the clip rectangle, where the
//      steps only lead further off it.
//   3. For a cel that is axis-aligned and not perspective-stepped, the winding
//      follows from the signs of the steps, and the matching flag must be set.
//   4. Otherwise the four corners are wound and compared.
bool Madam::cel_is_invisible(const Ccb& ccb, bool packed) const {
    if ((ccb.flags & (kCcbAcw | kCcbAccw)) == 0) {
        return true;
    }

    // Inclusive, as the register is: the last pixel drawn, not the first
    // dropped.
    const s32 clip_x = static_cast<s32>(clip_width_) - 1;
    const s32 clip_y = static_cast<s32>(clip_height_) - 1;
    const s32 wide = static_cast<s32>(ccb.width);
    const s32 high = static_cast<s32>(ccb.height);

    // Wrapping arithmetic on purpose: a cel positioned far outside the screen
    // overflows these products on the hardware too, and the test is only ever
    // used to reject, so a wrapped value cannot cause anything to be dropped
    // that the machine would have drawn.
    const auto step = [](s32 base, s32 delta, s32 count) {
        return static_cast<s32>(static_cast<u32>(base) +
                                static_cast<u32>(delta) * static_cast<u32>(count));
    };

    if (packed) {
        // A packed cel's rows are variable length, so only the vertical span
        // is known ahead of time.
        const s32 x0 = ccb.x >> 16;
        const s32 x1 = step(ccb.x, ccb.vdx, high) >> 16;
        if (x0 < 0 && x1 < 0 && ccb.hdx <= 0 && ccb.hddx <= 0) return true;
        if (x0 > clip_x && x1 > clip_x && ccb.hdx >= 0 && ccb.hddx >= 0) return true;

        const s32 y0 = ccb.y >> 16;
        const s32 y1 = step(ccb.y, ccb.vdy, high) >> 16;
        if (y0 < 0 && y1 < 0 && ccb.hdy <= 0 && ccb.hddy <= 0) return true;
        if (y0 > clip_y && y1 > clip_y && ccb.hdy >= 0 && ccb.hddy >= 0) return true;
    } else {
        const s32 xs[4] = {
            ccb.x >> 16,
            step(ccb.x, ccb.hdx, wide) >> 16,
            step(ccb.x, ccb.vdx, high) >> 16,
            step(step(ccb.x, ccb.vdx, high), step(ccb.hdx, ccb.hddx, high), wide) >> 16,
        };
        if (xs[0] < 0 && xs[1] < 0 && xs[2] < 0 && xs[3] < 0) return true;
        if (xs[0] > clip_x && xs[1] > clip_x && xs[2] > clip_x && xs[3] > clip_x) {
            return true;
        }

        const s32 ys[4] = {
            ccb.y >> 16,
            step(ccb.y, ccb.hdy, wide) >> 16,
            step(ccb.y, ccb.vdy, high) >> 16,
            step(step(ccb.y, ccb.vdy, high), step(ccb.hdy, ccb.hddy, high), wide) >> 16,
        };
        if (ys[0] < 0 && ys[1] < 0 && ys[2] < 0 && ys[3] < 0) return true;
        if (ys[0] > clip_y && ys[1] > clip_y && ys[2] > clip_y && ys[3] > clip_y) {
            return true;
        }
    }

    if (ccb.hddx == 0 && ccb.hddy == 0) {
        // Rotated a quarter turn: the horizontal step is vertical and the
        // vertical step is horizontal.
        if (ccb.hdx == 0 && ccb.vdy == 0) {
            const bool clockwise = (ccb.hdy < 0 && ccb.vdx > 0) ||
                                   (ccb.hdy > 0 && ccb.vdx < 0);
            return (ccb.flags & (clockwise ? kCcbAcw : kCcbAccw)) == 0;
        }
        // Upright, possibly mirrored on either axis.
        if (ccb.hdy == 0 && ccb.vdx == 0) {
            const bool counter = (ccb.hdx < 0 && ccb.vdy > 0) ||
                                 (ccb.hdx > 0 && ccb.vdy < 0);
            return (ccb.flags & (counter ? kCcbAccw : kCcbAcw)) == 0;
        }
    }

    return quad_is_wrong_way_round(ccb, packed ? 2048 : wide);
}

// The four corners of a perspective-stepped cel, wound and compared.
//
// If the four disagree the quad is twisted and is drawn regardless - only a
// consistently wound one can be rejected.
bool Madam::quad_is_wrong_way_round(const Ccb& ccb, s32 wide) const {
    const u32 allowed = ccb.flags & (kCcbAcw | kCcbAccw);
    if (allowed == (kCcbAcw | kCcbAccw)) {
        return false;
    }

    // P(x,y) = origin + x*(H + y*DD) + y*V. The local horizontal and
    // vertical tangents therefore change as H+y*DD and V+x*DD. Test their
    // cross product at all four corners; if the signs disagree the surface is
    // folded and cannot be rejected as one back-facing quad.
    const s64 w = wide;
    const s64 h = ccb.height;
    const auto add = [](s32 base, s32 delta, s64 count) {
        return static_cast<s64>(base) + static_cast<s64>(delta) * count;
    };

    const u32 first = texel_winding(ccb.hdx, ccb.hdy, ccb.vdx, ccb.vdy);
    if (first != texel_winding(ccb.hdx, ccb.hdy,
                               add(ccb.vdx, ccb.hddx, w),
                               add(ccb.vdy, ccb.hddy, w))) {
        return false;
    }
    if (first != texel_winding(add(ccb.hdx, ccb.hddx, h),
                               add(ccb.hdy, ccb.hddy, h),
                               ccb.vdx, ccb.vdy)) {
        return false;
    }
    if (first != texel_winding(add(ccb.hdx, ccb.hddx, h),
                               add(ccb.hdy, ccb.hddy, h),
                               add(ccb.vdx, ccb.hddx, w),
                               add(ccb.vdy, ccb.hddy, w))) {
        return false;
    }

    return first == allowed;
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
    if (cel_is_invisible(ccb, (ccb.flags & kCcbPacked) != 0)) {
        return;
    }
    // A cel drawn backwards along an axis is shifted back by half a pixel on
    // that axis.
    //
    // It is a rounding correction. Stepping forwards, a pixel's address is
    // truncated towards the pixel it starts on; stepping backwards, the same
    // truncation lands on the pixel BEFORE the one the hardware means, so a
    // mirrored cel would sit one pixel off from its unmirrored twin. The half
    // pixel puts the rounding back where it belongs, and only the axis-aligned
    // mappers need it - a rotated cel is resolved a different way.
    //
    // Honestly: this changes nothing measurable in the twelve titles here. One
    // extra distinct frame in Flashback out of two thousand, and both
    // pixel-identical titles stay identical. It is in because the hardware
    // does it and a mirrored cel elsewhere will need it, not because anything
    // here was visibly wrong without it.
    Ccb biased = ccb;
    if (ccb.hddx == 0 && ccb.hddy == 0 &&
        ((ccb.hdx == 0 && ccb.vdy == 0) || (ccb.hdy == 0 && ccb.vdx == 0))) {
        if (ccb.hdx < 0 || ccb.vdx < 0) biased.x -= 0x8000;
        if (ccb.hdy < 0 || ccb.vdy < 0) biased.y -= 0x8000;
    }
    const Ccb& use = biased;
    if ((ccb.flags & kCcbPacked) != 0) {
        draw_packed_cel(use);
    } else if ((ccb.pre1 & kPre1Lrform) != 0 && cel_bpp_ == 16) {
        draw_lr_cel(use);
    } else {
        draw_unpacked_cel(use);
    }
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

void Madam::begin_cel_walk(u32 address) {
    stats_ = MadamStats{};
    ++engine_runs_;
    cel_walk_started_ = true;
    next_ccb_ = address;
    if (g_cel_log != nullptr) {
        std::fprintf(g_cel_log, "RUN %llu head=%06X wbase=%06X rbase=%06X wstride=%d\n",
                     (unsigned long long)engine_runs_, address,
                     write_base_, read_base_, write_stride_);
    }
}

// Consume one CCB. Keeping this as a single-list-element primitive makes the
// DMA cursor, inheritance and termination rules testable without treating a
// CCB boundary as permission for the ARM to take the shared data bus.
bool Madam::step_cel_walk() {
    if (next_ccb_ == 0) {
        return false;
    }
    if (stats_.cels_walked >= kMaxCels) {
        stats_.list_truncated = 1;
        return false;
    }

    const u32 ccb_address = next_ccb_;
    current_ccb_ = ccb_address;
    const Ccb ccb = read_ccb(ccb_address);
    current_ccb_ = ccb.fetch_end_address;
    ++stats_.cels_walked;
    next_ccb_ = ccb.next_address;
    if (g_cel_log != nullptr) {
        if (stats_.cels_walked <= 3) {
            std::fprintf(g_cel_log, "    RAW");
            for (u32 w = 0; w < 17; ++w) {
                std::fprintf(g_cel_log, " %08X", bus_.read32(ccb_address + w * 4));
            }
            std::fprintf(g_cel_log, "\n");
        }
        std::fprintf(g_cel_log,
                     "CCB %06X flags=%08X pre0=%08X pre1=%08X next=%06X "
                     "src=%06X %ux%u\n",
                     ccb_address, ccb.flags, ccb.pre0, ccb.pre1, next_ccb_,
                     ccb.source_address, ccb.width, ccb.height);
    }

    // SKIP suppresses projection, not CCB loading. The official CCB load bits
    // update the shared decoder/projector state first, allowing an invisible
    // CCB to prepare a compact follower.
    const bool valid_cel = ccb.format != CelFormat::Unknown &&
                           ccb.width != 0 && ccb.height != 0 &&
                           ccb.width <= kMaxCelDimension &&
                           ccb.height <= kMaxCelDimension;
    if (valid_cel) {
        begin_cel(ccb);
    }

    if ((ccb.flags & kCcbSkip) == 0) {
        const u32 before = stats_.cels_drawn;
        const u64 before_pixels = stats_.pixels_written;
        const bool culled = cel_is_invisible(ccb, (ccb.flags & kCcbPacked) != 0);
        draw_cel(ccb);
        total_cels_drawn_ += stats_.cels_drawn - before;
        if (g_cel_log != nullptr) {
            std::fprintf(g_cel_log,
                         "    drew=%u pixels=%llu packed=%d fmt=%d cull=%d "
                         "at=%d,%d hd=%d,%d vd=%d,%d hdd=%d,%d\n",
                         stats_.cels_drawn - before,
                         (unsigned long long)(stats_.pixels_written - before_pixels),
                         (ccb.flags & kCcbPacked) != 0 ? 1 : 0,
                         static_cast<int>(ccb.format),
                         culled ? 1 : 0,
                         ccb.x >> 16, ccb.y >> 16,
                         ccb.hdx, ccb.hdy, ccb.vdx, ccb.vdy,
                         ccb.hddx, ccb.hddy);
        }
    }

    if ((ccb.flags & kCcbLast) != 0 || next_ccb_ == 0 || next_ccb_ == ccb_address) {
        return false;
    }
    if (stats_.cels_walked >= kMaxCels) {
        stats_.list_truncated = 1;
        return false;
    }
    return true;
}

// Walk a complete list for focused rendering tests.
void Madam::render_cel_list(u32 address) {
    begin_cel_walk(address);
    while (step_cel_walk()) {}
    cel_walk_started_ = false;
}

// Walk from NEXTCCB while the CEL owns the data bus. The start itself is
// deferred until the triggering ARM instruction completes, but once DMA has
// begun the ARM cannot interleave ordinary memory instructions between CCBs.
void Madam::run_cel_engine() {
    if (cel_engine_state_ != CelEngineState::InProcess) return;
    if (!cel_walk_started_) begin_cel_walk(next_ccb_);

    while (step_cel_walk()) {}
    cel_engine_state_ = CelEngineState::Idle;
    cel_walk_started_ = false;
    bus_.cancel_cel_engine();
}

}  // namespace retro3do
