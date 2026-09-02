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
// Specification basis
// -------------------
// CCB and preamble fields come from the official SDK hardware.h and Graphics
// Programming Guide. Projection, corner calculation, pixel processing and CEL
// bus state are grounded in WO 94/10643, WO 94/10644 and WO 94/10641. The
// current matrix, visibility, active-edge raster and LR row-pair algorithms are
// project implementations; the historical source audit is in PROVENANCE.md.
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

    // CEL engine state. The official hardware description calls these SPRON
    // and SPRPAU: running sets bit 4, and a list suspended at a safe bus-yield
    // point sets bits 4 and 5.
    kMadamCelStatus  = 0x0028,
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
    // The PBUS channel lives on the DMA-enable register above: writing it with
    // bit 15 set starts a controller scan, and the hardware clears the bit
    // when it is done and interrupts.
    kMadamPbusStart   = 0x8000,
    kMadamPbusAddress = 0x0570,
    kMadamPbusLength  = 0x0574,
    kMadamPbusPointer = 0x0578,

    // The DSP's DMA channels. Thirteen carry samples INTO the DSP and four
    // carry them out, and each has a current address and length plus a reload
    // pair so a channel can run continuously without the CPU in the loop.
    //
    //   0x400 + channel*16   input  channels 0..12
    //   0x500 + channel*16   output channels 0..3
    //     +0x00 address   +0x04 length   +0x08 next address   +0x0C next length
    kMadamFifoBase   = 0x0400,
    kMadamFifoEnd    = 0x0540,
    kMadamFifoOutput = 0x0500,

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
    kMadamCelResume  = 0x0108,   // SPRCNTU in the hardware documentation
    kMadamCelPause   = 0x010c,

    // Where the walk is, and where it goes next. Software both reads and
    // writes these, so they are real state rather than loop variables.
    // Where the cel engine draws, and how wide the buffer is.
    //
    //   0x130  the row stride, encoded, separately for reads and writes
    //   0x134  the clip rectangle
    //   0x138  the buffer cels are read FROM
    //   0x13C  the buffer cels are drawn INTO
    //
    // These are not optional. A machine that assumes the framebuffer is at the
    // base of VRAM draws every cel of every title into one buffer, which is
    // right for whichever title happens to use that one and invisible for the
    // rest.
    // The engine's own control word: which bits of a written pixel carry the
    // horizontal and vertical sub-position, and whether they are swapped.
    kMadamCcbCtl0    = 0x0110,

    kMadamRegCtl0    = 0x0130,
    kMadamRegCtl1    = 0x0134,
    kMadamRegCtl2    = 0x0138,
    kMadamRegCtl3    = 0x013c,

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
    // The hardware matrix unit.
    //
    // MADAM can multiply a 4x4 matrix of 16.16 values by a vector, and divide
    // through by z on the way out. It is the machine's transform-and-project
    // stage, and a 3D title uses it for every vertex it draws - Need for
    // Speed puts its whole road through it. A machine without one hands back
    // zeros, and the title dutifully draws the degenerate geometry that
    // implies: a correct cockpit in front of an empty field.
    //
    // The registers are the matrix, then the vector, then the outputs, then
    // the numerator used by the projecting form. Writing to the control port
    // performs the operation; the value written selects which.
    kMadamMatrixIn    = 0x0600,   // 4x4, row-major, 16.16
    kMadamMatrixVec   = 0x0640,   // the vector, four words
    kMadamMatrixOut   = 0x0660,   // four words
    kMadamMatrixNumHi = 0x0680,   // 64-bit numerator for the divide
    kMadamMatrixNumLo = 0x0684,
    kMadamMatrixCtl   = 0x07fc,
    kMadamWindowSize = 0x10000,
};

enum : u32 {
    kMadamCelRunning = 0x10,
    kMadamCelPaused  = 0x20,
};

// What a write to the matrix control port asks for.
enum : u32 {
    kMatrixCopyOnly          = 0,   // publish the previous result, compute nothing
    kMatrixMultiply4x4       = 1,
    kMatrixMultiply3x3       = 2,
    kMatrixMultiply3x3DivideZ = 3,  // transform and project
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

// Production Panasonic consumer machines use the Green MADAM revision with
// the hardware matrix engine. Software uses this ID to choose the matching
// matrix-engine driver; zero (or an invented revision) is not a harmless
// placeholder because Need for Speed then selects a layout whose result ports
// do not match the chip.
constexpr u32 kMadamRevisionGreen = 0x01020000u;

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
    kCcbPlutPos  = 1u << 6,
    kCcbBgnd     = 1u << 5,
    kCcbNoBlk    = 1u << 4,
    kCcbPoverMask = 0x180u,
};

// CCBCTL0. Only the fields the projector acts on.
enum : u32 {
    kCtl0B15Mask = 0xc0000000u,
    kCtl0B15Zero = 0x00000000u,
    kCtl0B15One  = 0x40000000u,
    kCtl0B15Pdc  = 0xc0000000u,
    kCtl0B0Mask  = 0x30000000u,
    kCtl0B0Zero  = 0x00000000u,
    kCtl0B0One   = 0x10000000u,
    kCtl0B0Ppmp  = 0x20000000u,
    kCtl0B0Pdc   = 0x30000000u,
    kCtl0SwapHv  = 0x08000000u,
};

constexpr u32 kPre1NoSwap = 0x00004000u;

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

    // The hardware CURRENTCCB DMA cursor after loading this CCB. This differs
    // from the CCB's base: the first six words are always fetched, followed by
    // only the optional register/preamble words selected by FLAGS.
    u32 fetch_end_address = 0;
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
    void draw_packed_cel(const Ccb& ccb);
    void draw_unpacked_cel(const Ccb& ccb);
    void draw_lr_cel(const Ccb& ccb);
    bool cel_is_invisible(const Ccb& ccb, bool packed) const;
    bool quad_is_wrong_way_round(const Ccb& ccb, s32 wide) const;
    void plot_quad(const Ccb& ccb, s32 ax, s32 ay, s32 bx, s32 by,
                   s32 cx, s32 cy, s32 dx, s32 dy, u16 pixel, u16 shade);
    // A cel's rows are walked with two cursors, not one: where this pixel
    // lands, and where the matching pixel of the NEXT row lands. The second is
    // dead weight for a scaled cel and is the other half of the figure for a
    // rotated one.
    void plot_texel(const Ccb& ccb, s32 px, s32 py, s32 step_x, s32 step_y,
                    s32 down_x, s32 down_y, s32 next_step_x, s32 next_step_y,
                    u16 pixel, u16 shade);
    void plot_footprint(const Ccb& ccb, s32 px, s32 py, s32 step_x, s32 step_y,
                        u16 pixel, u16 shade);
    // Turn one source value into a colour, and say what multiplier it carries
    // and whether it is transparent.
    //
    // A coded pixel is NOT an index. Only its low five bits select a palette
    // entry; the bits above carry a per-channel multiplier the pixel processor
    // uses for shading. Treating all eight bits of an eight-bit pixel as an
    // index reads two hundred and fifty-six entries out of a palette that has
    // thirty-two, which is most of a cel's colours coming from whatever
    // follows it in memory.
    u16  decode_pixel(u32 value, u16* multiplier, bool* transparent) const;

    // The pixel processor. Every pixel the engine writes goes through it: it
    // scales the source, optionally mixes it with what is already in the
    // framebuffer, and decides the two sub-position bits. Writing the source
    // straight out instead is not a subtle difference - it is the whole of a
    // cel's brightness and all of its blending.
    void process_pixel(s32 x, s32 y, u16 source, u16 amv);
    void begin_cel(const Ccb& ccb);
    void run_cel_engine();

    // The rectangle cels are clipped to. Defaults to the whole visible field.
    void set_clip(u32 width, u32 height);
    u32 clip_width() const { return clip_width_; }
    u32 clip_height() const { return clip_height_; }

    // The PBUS transfer happens inside the store that starts it, and it is
    // MADAM that owns the registers but the machine that owns memory, so the
    // work itself is handed back out.
    void set_pbus_handler(void (*handler)(void*), void* context) {
        pbus_handler_ = handler;
        pbus_context_ = context;
    }
    u32  pbus_address() const { return pbus_address_; }
    u32  pbus_length() const { return pbus_length_; }
    void set_pbus_address(u32 value) { pbus_address_ = value; }
    void set_pbus_length(u32 value) { pbus_length_ = value; }
    void advance_pbus_pointer() { pbus_pointer_ += 4; }
    void finish_pbus() { dma_enable_ &= ~kMadamPbusStart; }

    // --- the DSP's DMA channels ------------------------------------------
    //
    // The channels are MADAM's registers but they move data for the DSP, so
    // the DSP drives them through here rather than owning them.
    u16  fifo_input_next(u16 channel);
    u16  fifo_input_peek(u16 channel);
    u16  fifo_input_status(u16 channel) const;
    u16  fifo_output_status(u16 channel) const;
    void fifo_output(u16 channel, u16 value);

    // Software enables and disables channels through CLIO, not MADAM, so the
    // mask is handed over rather than decoded here.
    void set_dma_channel_enable(u32 mask) { dma_channel_enable_ = mask; }
    void clear_fifo(u32 channel, bool output);

    // Raised when a channel runs out and cannot reload. The interrupt bit
    // differs between input and output channels, so the caller is told which.
    using FifoDoneHandler = void (*)(void* context, u32 channel, bool output);
    void set_fifo_done_handler(FifoDoneHandler handler, void* context) {
        fifo_done_ = handler;
        fifo_done_context_ = context;
    }

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
    Ccb read_ccb(u32 address);

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
    enum class CelEngineState { Idle, InProcess, Suspended };

    void note_write(u32 offset, u32 value);
    void draw_cel(const Ccb& ccb);
    void begin_cel_walk(u32 address);
    bool step_cel_walk();

    // Fetch one source pixel, already expanded to RGB555. Handles the indexed
    // formats via the PLUT and the direct format without it.
    u16 sample(const Ccb& ccb, u32 sx, u32 sy) const;

    void put_pixel(s32 x, s32 y, u16 pixel, u16 shade = 0);

    Bus& bus_;

    u32 revision_ = kMadamRevisionGreen;
    u32 mem_config_ = kMadamMemConfigStock;
    u32 dma_enable_ = 0;
    u32 xbus_dma_address_ = 0;
    u32 xbus_dma_length_ = 0;
    bool xbus_dma_pending_ = false;
    u32 vdl_address_ = 0;
    u64 engine_runs_ = 0;
    u64 total_cels_drawn_ = 0;
    // A DSP DMA channel. `index` is how far through the current buffer it has
    // read; a length of zero with no reload set means the channel is idle.
    struct Fifo {
        s32 index = 0;
        u32 address = 0;
        s32 length = 0;
        u32 next_address = 0;
        s32 next_length = 0;
    };
    static constexpr u32 kInputFifos = 13;
    static constexpr u32 kOutputFifos = 4;
    Fifo input_fifo_[kInputFifos];
    Fifo output_fifo_[kOutputFifos];
    u32 dma_channel_enable_ = 0;

    u32 pbus_address_ = 0;
    u32 pbus_length_ = 0;
    u32 pbus_pointer_ = 0;
    void (*pbus_handler_)(void*) = nullptr;
    FifoDoneHandler fifo_done_ = nullptr;
    void* fifo_done_context_ = nullptr;
    void* pbus_context_ = nullptr;
    u32 current_ccb_ = 0;
    u32 next_ccb_ = 0;
    CelEngineState cel_engine_state_ = CelEngineState::Idle;
    bool cel_walk_started_ = false;
    u32 dma_address_[kMadamDmaChannels] = {};
    u32 dma_length_[kMadamDmaChannels] = {};
    u32 clip_width_ = 320;
    u32 clip_height_ = 240;

    // The framebuffer control registers, and what they decode to. Both stride
    // fields sit in one register, the write one eight bits up.
    u32 reg_ctl0_ = 0;
    u32 reg_ctl1_ = 0;
    u32 read_base_ = 0;
    u32 write_base_ = 0;
    s32 read_stride_ = 320 * 4;
    s32 write_stride_ = 320 * 4;
    bool framebuffer_configured_ = false;

    // Per-cel pixel-processor state, set up once when a cel starts.
    u32 ccb_ctl0_ = 0;
    u32 cel_flags_ = 0;
    u32 cel_pixc_ = 0;
    u32 cel_pre1_ = 0;
    u32 cel_pxor1_ = 0xffffffffu;
    u32 cel_pxor2_ = 0;
    u32 cel_pmode_or_ = 0;
    u32 cel_pmode_and_ = 0xffffu;
    u32 cel_origin_vh_ = 0;
    u32 cel_bpp_ = 0;
    bool cel_linear_ = false;
    u32 cel_pluta_ = 0;
    u32 cel_pixel_mask_ = 0;
    bool cel_transparent_mask_ = true;

    // The tail of a cel is a bank of load-controlled registers, not a fixed
    // structure. A CCB with YOXY/LDSIZE/LDPRS/LDPPMP clear omits that group and
    // inherits the value the preceding cel left behind. PRE0/PRE1 persist too.
    s32 ccb_x_register_ = 0;
    s32 ccb_y_register_ = 0;
    s32 ccb_hdx_register_ = 0;
    s32 ccb_hdy_register_ = 0;
    s32 ccb_vdx_register_ = 0;
    s32 ccb_vdy_register_ = 0;
    s32 ccb_hddx_register_ = 0;
    s32 ccb_hddy_register_ = 0;
    u32 ccb_pixc_register_ = 0;
    u32 pre0_register_ = 0;
    u32 pre1_register_ = 0;

    // Whether this cel maps one source pixel to one screen pixel. The common
    // case by a very long way, and worth knowing per cel rather than deciding
    // per pixel.
    // The matrix unit's registers. The outputs are DOUBLE BUFFERED: an
    // operation computes into a holding set, and the previous result is what
    // moves into the readable outputs. Software therefore reads the answer to
    // the operation BEFORE the one it just asked for, which is how a title
    // keeps the unit busy without ever waiting for it.
    s32 matrix_in_[16] = {};
    s32 matrix_vec_[4] = {};
    s32 matrix_out_[4] = {};
    s64 matrix_pending_[4] = {};
    u32 matrix_num_hi_ = 0;
    u32 matrix_num_lo_ = 0;

    void matrix_execute(u32 operation);

    bool cel_one_to_one_ = true;

    // Which of the hardware's three texel mappers this cel uses. They are not
    // variations on one another: a scaled cel covers a rectangle between one
    // pixel and the next, while a rotated one covers a four-sided figure that
    // no rectangle approximates. Using one routine for both draws the scaled
    // case with gaps or the rotated case with none of its shear.
    enum class Mapper { Line, Scale, Arbitrary };
    Mapper cel_mapper_ = Mapper::Line;

    // The cel engine's own palette, loaded from a cel that says to and kept
    // for the ones that do not.
    static constexpr u32 kPlutEntries = 32;
    u16 plut_[kPlutEntries] = {};

    u32 target_address_ = 0;
    u32 target_stride_bytes_ = 320 * 2;

    MadamStats stats_;
    u32 written_value_[kTrackedRegisters] = {};
    bool written_flag_[kTrackedRegisters] = {};
};

// Decode the pixel format from a CCB's PRE0 word.
CelFormat cel_format_from_pre0(u32 pre0);

}  // namespace retro3do
