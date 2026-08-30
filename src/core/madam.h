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

    // MCTL: one bit per DMA channel. The boot ROM writes it repeatedly during
    // start-up and reads it back, so it has to hold its value.
    kMadamDmaEnable  = 0x0008,
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

    // The expansion-bus DMA. This is how a sector actually reaches memory: the
    // CPU never reads the drive's data port, it programmes these and waits for
    // the transfer-complete interrupt.
    //
    // The length is written as bytes-minus-four - the boot ROM asks for 0x7FC
    // when it wants one 2048-byte sector - so it reads as the offset of the
    // last word rather than a count.
    kMadamXbusDmaAddress = 0x0540,
    kMadamXbusDmaLength  = 0x0544,
    kMadamDmaStride  = 8,
    kMadamDmaChannels = 32,

    // The engine's control ports. NONE of them carries an address: writing to
    // SPRSTRT starts a walk from whatever NEXTCCB already holds, and the value
    // written is discarded. Treating the written value as the list head means
    // the engine draws from wherever the last store happened to land.
    kMadamCelStart   = 0x0100,
    kMadamCelStop    = 0x0104,
    kMadamCelResume  = 0x0108,
    kMadamCelPause   = 0x010c,

    // Where the walk is, and where it goes next. Software both reads and
    // writes these, so they are real state rather than loop variables.
    kMadamCurrentCcb = 0x05a0,
    kMadamNextCcb    = 0x05a4,

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

// The stock configuration for a consumer machine: 2 MB of DRAM and 1 MB of
// VRAM.
//
// The encoding is not guesswork - the ROM decodes this register itself at
// 0x00000124, and the shifts say what each field is:
//
//     and r1, r3, #7    ; bits 0-2  VRAM size in MB
//     and r2, r3, #24   ; bits 3-4  DRAM1 size
//     and r2, r3, #96   ; bits 5-6  DRAM2 size
//
// So VRAM 1 MB, DRAM1 1 MB, DRAM2 1 MB is 0x01 | 0x08 | 0x20 = 0x29, which is
// also the value the community register map gives for consumer units.
//
// This register is READ ONLY. It reports how much memory is fitted, which
// software cannot change, and the boot ROM writes zero to it during start-up.
// Honouring that write made the machine tell itself it had no memory: the
// sizing routine above read back zero, decoded no DRAM, and panicked with
// error 7 before doing anything else. That one writable register is what kept
// this emulator on a halved, wrong memory map.
constexpr u32 kMadamMemConfigStock = 0x29;

// Flags in the CCB's first word.
//
// SKIP is the TOP bit and LAST is the one below it, not the other way round.
// Getting that pair the wrong way up is not a cosmetic error: a cel meant to
// be skipped is drawn, and the list runs on past the cel that was supposed to
// end it, off into whatever memory follows.
enum : u32 {
    kCcbSkip     = 1u << 31,  // do not draw this cel
    kCcbLast     = 1u << 30,  // this is the final cel in the list
    kCcbNpAbs    = 1u << 29,  // next pointer is absolute, not relative
    kCcbSpAbs    = 1u << 28,  // source pointer is absolute
    kCcbPpAbs    = 1u << 27,  // PLUT pointer is absolute
    kCcbLdSize   = 1u << 26,
    kCcbLdPrs    = 1u << 25,
    kCcbLdPpmp   = 1u << 24,
    kCcbLdPlut   = 1u << 23,
    kCcbCcbPre   = 1u << 22,
    kCcbYoxy     = 1u << 21,
    kCcbAcsc     = 1u << 20,
    kCcbAlsc     = 1u << 19,
    kCcbAcw      = 1u << 18,
    kCcbAccw     = 1u << 17,
    kCcbTwd      = 1u << 16,
    kCcbLce      = 1u << 15,
    kCcbAce      = 1u << 14,
    kCcbMaria    = 1u << 12,
    kCcbPxor     = 1u << 11,
    kCcbUseAv    = 1u << 10,
    kCcbPacked   = 1u << 9,   // source is run-length coded
    kCcbBgnd     = 1u << 5,
    kCcbNoBlk    = 1u << 4,
};

// A CCB pointer is a 24-bit address. The upper byte is not part of it and must
// be dropped BEFORE any relative base is added, or a stray high bit turns a
// short offset into an address on the other side of memory.
constexpr u32 kCcbAddressMask = 0x00ffffffu;

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
    void run_cel_engine();

    // The rectangle cels are clipped to. Defaults to the whole visible field.
    void set_clip(u32 width, u32 height);

    const MadamStats& stats() const { return stats_; }

    // Lifetime totals. `stats()` describes the LAST list only, which reads as
    // "the engine barely runs" whenever the final list of a frame happens to
    // be a short one.
    u64 engine_runs() const { return engine_runs_; }
    u64 total_cels_drawn() const { return total_cels_drawn_; }

    // An expansion-bus DMA the host has programmed and not yet been served.
    bool xbus_dma_pending() const { return xbus_dma_pending_; }
    u32  xbus_dma_address() const { return xbus_dma_address_; }
    u32  xbus_dma_bytes() const { return xbus_dma_length_ + 4; }
    void clear_xbus_dma() { xbus_dma_pending_ = false; }

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
    u32 dma_enable_ = 0;
    u32 xbus_dma_address_ = 0;
    u32 xbus_dma_length_ = 0;
    bool xbus_dma_pending_ = false;
    u32 vdl_address_ = 0;
    u64 engine_runs_ = 0;
    u64 total_cels_drawn_ = 0;
    u32 current_ccb_ = 0;
    u32 next_ccb_ = 0;
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
