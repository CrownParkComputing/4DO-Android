// MADAM — the 3DO's graphics and DMA engine.
//
// MADAM draws by walking a linked list of Cel Control Blocks. A CCB describes
// one "cel": where its pixels live, how they are encoded, and a 2x2 matrix of
// 16.16 fixed-point deltas that maps the source rectangle onto the destination.
// Because the mapping is a general affine step rather than a blit, a cel can be
// scaled, rotated and sheared, which is how the machine draws its
// pseudo-3D — every textured polygon in a 3DO game is a cel.
//
// Performance, and the one thing the obvious plan gets wrong
// ---------------------------------------------------------
// This is the hot chip: it is where a phone spends its time, and it is the
// reason this core exists. Two decisions are baked in from the start rather
// than retrofitted:
//
//   * The inner loop steps incrementally. Source coordinates advance by adding
//     the deltas, never by multiplying per pixel, so a row of destination
//     pixels is a straight walk that vectorises.
//
//   * Parallelism is *within* a cel, not across cels. Cels are drawn in list
//     order and later ones paint over earlier ones, so handing each CCB to a
//     different thread is simply wrong — it produces a race on the destination
//     and, worse, an intermittently wrong picture rather than a crash. The
//     safe split is destination row bands inside one large cel. This is written
//     down here because "just thread the cel list" is the natural mistake and
//     it would be very hard to diagnose afterwards.
//
// Confidence
// ----------
// The CCB layout, the fixed-point mapping and the pixel formats implemented
// here come from published 3DO developer documentation. Bit assignments not yet
// checked are marked TODO(madam) rather than asserted. Nothing derives from
// another emulator; see docs/CLEANROOM.md.
#pragma once

#include "types.h"

namespace retro3do {

class Bus;

// MADAM register offsets, relative to the base of its window.
enum : u32 {
    kMadamRevision   = 0x0000,

    // Memory configuration: how much DRAM and VRAM the machine is fitted with.
    //
    // The boot ROM decodes this field by field, and the decode is what tells us
    // the layout. At 0x03000518 it reads this register into r3 and then:
    //   VRAM megabytes  = (r3 & 7)
    //   a               = (r3 >> 5) & 3, capped at 4
    //   b               = (r3 >> 3) & 3, capped at 4
    //   DRAM megabytes  = a + b   (or 16 if b > 2)
    // So a stock machine - 2 MB of DRAM in two banks of one, and 1 MB of VRAM -
    // is 0x29. Reporting zero says the machine has no memory at all, and the
    // ROM then fails its memory test and jumps to a panic handler that stores
    // registers to MADAM in an infinite loop.
    kMadamMemConfig  = 0x0004,
    kMadamClipXY     = 0x0008,   // TODO(madam): confirm
    // DMA channels. Each is eight bytes: an address then a length.
    //
    // Read off the driver that programs them, at DRAM 0x1A7FC:
    //
    //   LDR r3, [r0], #4
    //   STR r3, [r1, #0x18]     ; r1 = MADAM + 0x200  -> channel 3 address
    //   LDR r0, [r0]
    //   STR r0, [r1, #0x1C]     ;                     -> channel 3 length
    //   ...
    //   STR r2, [r1, #0x38]     ;                     -> channel 7 address
    //   STR r0, [r1, #0x3C]!    ;                     -> channel 7 length
    //
    // 0x218 and 0x238 are therefore channels 3 and 7 of an array based at
    // 0x200 with an eight-byte stride. The expansion bus uses these two: a
    // device's reply is DMA'd into memory rather than read back through a
    // port, which is why no amount of adjusting the status port moved the
    // machine.
    kMadamDmaBase    = 0x0200,
    kMadamDmaStride  = 8,
    kMadamDmaChannels = 32,

    kMadamCelStart   = 0x0100,   // writing here starts the engine on a list

    // The address of the video display list, which is what the VDLP walks to
    // produce a field.
    //
    // Found by recording every register the boot ROM programs: it writes
    // exactly one word-aligned value pointing into VRAM, `0x001B0000`, and
    // this is where. It is an absolute bus address, not a VRAM offset - as a
    // VRAM offset it would be past the end of a 1 MB VRAM.
    kMadamVdlAddress = 0x0580,
    kMadamPipStart   = 0x0104,   // TODO(madam): confirm
    kMadamMatrixBase = 0x7000,   // the hardware matrix unit
    kMadamWindowSize = 0x10000,
};

// The stock configuration: 2 MB DRAM, 1 MB VRAM, derived from the ROM's own
// decode above.
constexpr u32 kMadamMemConfigStock = 0x21;

// Flags in the CCB's first word. Only the ones acted on are named.
enum : u32 {
    kCcbLast     = 1u << 31,  // this is the final cel in the list
    kCcbNpAbs    = 1u << 30,  // next pointer is absolute, not relative
    kCcbSpAbs    = 1u << 29,  // source pointer is absolute
    kCcbPpAbs    = 1u << 28,  // PLUT pointer is absolute
    kCcbLdSize   = 1u << 27,  // TODO(madam): confirm
    kCcbLdPrs    = 1u << 26,  // TODO(madam): confirm
    kCcbLdPpmp   = 1u << 25,  // TODO(madam): confirm
    kCcbLdPlut   = 1u << 24,  // TODO(madam): confirm
    kCcbCcbPre   = 1u << 23,  // TODO(madam): confirm
    kCcbYoxy     = 1u << 22,  // TODO(madam): confirm
    kCcbSkip     = 1u << 15,  // TODO(madam): confirm — do not draw this cel
    kCcbPacked   = 1u << 9,   // TODO(madam): confirm — source is run-length coded
};

// How a cel's source pixels are encoded.
enum class CelFormat {
    Unknown,
    Indexed1,    // 1 bit per pixel, through the PLUT
    Indexed2,
    Indexed4,
    Indexed6,
    Indexed8,
    Direct16,    // 16-bit RGB555 straight from the source
};

// What the engine did on the last run. Not decoration: the split between cels,
// pixels and rows is what tells us whether a slow game is slow because of many
// small cels or a few enormous ones, and those want different optimisations.
struct MadamStats {
    u32 cels_walked = 0;
    u32 cels_drawn  = 0;
    u64 pixels_written = 0;
    u32 list_truncated = 0;   // non-zero if the walk hit its safety limit
};

// One CCB, unpacked into something the renderer can work with.
struct Ccb {
    u32 flags = 0;
    u32 next_address = 0;
    u32 source_address = 0;
    u32 plut_address = 0;

    // Destination origin, 16.16 fixed point.
    s32 x = 0;
    s32 y = 0;

    // The mapping. hdx/hdy step per source column, vdx/vdy per source row, and
    // hddx/hddy bend the horizontal step as rows advance — which is what makes
    // a cel a quad rather than a parallelogram.
    s32 hdx = 0, hdy = 0;
    s32 vdx = 0, vdy = 0;
    s32 hddx = 0, hddy = 0;

    u32 pixc = 0;   // blend control
    u32 pre0 = 0;
    u32 pre1 = 0;

    // Decoded from pre0/pre1.
    CelFormat format = CelFormat::Unknown;
    u32 width = 0;
    u32 height = 0;
};

class Madam {
public:
    explicit Madam(Bus& bus);

    void reset();

    u32  read(u32 offset);
    void write(u32 offset, u32 value);

    // Draw the cel list starting at `address` into VRAM. Normally triggered by
    // a register write; exposed directly so tests can drive it without going
    // through the register interface.
    void render_cel_list(u32 address);

    // The rectangle cels are clipped to. Defaults to the whole visible field.
    void set_clip(u32 width, u32 height);

    const MadamStats& stats() const { return stats_; }

    // Where the machine has told the display to read its list from. Zero until
    // the software sets it, which is why a freshly reset machine is black.
    u32 vdl_address() const { return vdl_address_; }

    // A DMA channel's programmed address and length. Stored and readable, but
    // no transfer is performed: what a device would put there depends on the
    // expansion-bus command set, which is not established yet. Keeping them
    // means the setup is visible instead of vanishing.
    u32 dma_address(u32 channel) const;
    u32 dma_length(u32 channel) const;

    // Read one CCB. Public because it is worth testing on its own: a
    // misread CCB produces garbage that is very hard to attribute afterwards.
    Ccb read_ccb(u32 address) const;

    // Where cels are drawn. In the real machine this comes from the current
    // framebuffer; until MADAM's own register set is confirmed the console
    // points it at VRAM directly.
    void set_target(u32 address, u32 stride_bytes) {
        target_address_ = address;
        target_stride_bytes_ = stride_bytes;
    }

    // Bring-up diagnostics: the last value written to each low register, and
    // whether it was ever written at all. Knowing WHICH registers a boot ROM
    // programs, and with what, is most of the work of finding out what it
    // expects; guessing that from behaviour alone is very slow.
    static constexpr u32 kTrackedRegisters = 2048;
    bool register_written(u32 offset) const;
    u32  register_last_write(u32 offset) const;

private:
    void note_write(u32 offset, u32 value);
    void draw_cel(const Ccb& ccb);

    // Fetch one source pixel, already expanded to RGB555. Handles the indexed
    // formats via the PLUT and the direct format without it.
    u16 sample(const Ccb& ccb, u32 sx, u32 sy) const;

    void put_pixel(s32 x, s32 y, u16 pixel);

    Bus& bus_;

    u32 revision_ = 0;
    u32 mem_config_ = kMadamMemConfigStock;
    u32 vdl_address_ = 0;
    u32 dma_address_[kMadamDmaChannels] = {};
    u32 dma_length_[kMadamDmaChannels] = {};
    u32 clip_width_ = 320;
    u32 clip_height_ = 240;

    u32 target_address_ = 0;
    u32 target_stride_bytes_ = 320 * 2;

    MadamStats stats_;
    u32 written_value_[kTrackedRegisters] = {};
    bool written_flag_[kTrackedRegisters] = {};
};

// Decode the pixel format from a CCB's PRE0 word.
CelFormat cel_format_from_pre0(u32 pre0);

}  // namespace retro3do
