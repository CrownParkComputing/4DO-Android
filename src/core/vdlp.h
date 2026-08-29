// VDLP — the Video Display List Processor.
//
// The 3DO does not have a fixed framebuffer register. The display is described
// by a linked list in VRAM: each VDL entry names the framebuffer to show, how
// many scanlines it applies to, a colour lookup table, and the address of the
// next entry. Walking that list is how a field gets drawn, and it is how games
// change palette or buffer partway down the screen.
//
// Confidence
// ----------
// The pixel format (16-bit RGB555 in VRAM, two pixels per 32-bit big-endian
// word) and the shape of the list are implemented here. The exact bit
// assignments inside a VDL control word are marked TODO(vdl) — they are the
// part still to be checked against the published documentation, and until they
// are, `render_linear` exists as a way to see VRAM on screen without trusting
// them. Nothing here derives from another emulator; see docs/CLEANROOM.md.
#pragma once

#include "types.h"

namespace retro3do {

class Bus;

// A VDL control word packs the line count and a set of enables. Only the fields
// this implementation acts on are named.
enum : u32 {
    // TODO(vdl): confirm. The line count is believed to sit in the low bits of
    // the control word, with the upper bits carrying DMA and CLUT enables.
    kVdlLineCountMask   = 0x000001ffu,
    kVdlLineCountShift  = 0,
};

class Vdlp {
public:
    explicit Vdlp(Bus& bus);

    void reset();

    // Where the display list starts. Written by MADAM in the real machine; for
    // now the console sets it directly.
    void set_list_address(u32 address) { list_address_ = address; }
    u32  list_address() const { return list_address_; }

    // Walk the display list and produce a whole field into `out`, which must
    // hold width * height pixels in XRGB8888.
    void render_field(u32* out, int width, int height);

    // Read VRAM as a plain RGB555 framebuffer at `vram_offset`, ignoring the
    // display list entirely. This is not how the hardware works; it is how we
    // get a picture on screen while the VDL control bits are still unconfirmed,
    // and it is what the tests use to check the pixel conversion in isolation.
    void render_linear(u32* out, int width, int height, u32 vram_offset);

    // Number of VDL entries followed on the last field. Zero means the list was
    // empty or did not look like a list, which is the symptom to look for when
    // a game shows nothing.
    u32 entries_walked() const { return entries_walked_; }

private:
    // One scanline's worth of pixels, from a framebuffer address in VRAM.
    void render_line(u32* out, int width, u32 framebuffer_address, int line);

    Bus& bus_;
    u32 list_address_ = 0;
    u32 entries_walked_ = 0;
};

// Where a pixel lives in the framebuffer, as a byte offset from its base.
//
// The framebuffer is INTERLEAVED, not linear: each 32-bit word holds two pixels
// at the same x from two adjacent scanlines, the even line in the high half and
// the odd line in the low half. A pair of lines therefore occupies `width`
// words, and consecutive pixels on one line are four bytes apart.
//
// This lives in one place on purpose. MADAM writes the framebuffer and the VDLP
// reads it, and if the two disagree about the layout the picture is wrong in a
// way that looks like a rendering bug in whichever one you happen to suspect.
// They were briefly inconsistent, and the test that caught it was the one that
// draws a cel and then checks it appears in the frame - the seam between them.
constexpr u32 framebuffer_offset(int x, int y, int width) {
    const u32 pair = static_cast<u32>(y) / 2u;
    const u32 pair_bytes = static_cast<u32>(width) * 4u;
    return pair * pair_bytes + static_cast<u32>(x) * 4u +
           ((y & 1) != 0 ? 2u : 0u);
}

// Expand a 3DO pixel to XRGB8888.
//
// VRAM holds 16 bits per pixel as 0RRRRRGGGGGBBBBB. Five bits per channel are
// widened to eight by replicating the top three bits into the low ones, so that
// full-scale in maps to full-scale out — a plain left-shift would make white
// come out as 0xF8 and tint every bright area of every game.
constexpr u32 expand_rgb555(u16 pixel) {
    const u32 r5 = (pixel >> 10) & 0x1fu;
    const u32 g5 = (pixel >> 5) & 0x1fu;
    const u32 b5 = pixel & 0x1fu;

    const u32 r8 = (r5 << 3) | (r5 >> 2);
    const u32 g8 = (g5 << 3) | (g5 >> 2);
    const u32 b8 = (b5 << 3) | (b5 >> 2);

    return 0xff000000u | (r8 << 16) | (g8 << 8) | b8;
}

}  // namespace retro3do
