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

// A VDL control word.
//
// The line count is how many lines this entry PERSISTS for, and zero is a
// perfectly ordinary value: it means the next entry is picked up on the very
// next line. A list with one entry per line is normal, and treating a zero as
// "the rest of the field" renders the whole screen out of the first entry's
// buffer - which for a title that puts a blank buffer first is a black screen.
enum : u32 {
    kVdlPersistMask     = 0x000001ffu,
    kVdlControlCountShift = 9,
    kVdlControlCountMask  = 0x3fu,
    kVdlPrevOverride    = 1u << 15,
    kVdlCurrOverride    = 1u << 16,
    kVdlPrevTick        = 1u << 17,
    kVdlNextRelative    = 1u << 18,
    kVdlVerticalMode    = 1u << 19,
    kVdlEnableDma       = 1u << 21,
    kVdlModuloShift     = 23,
    kVdlModuloMask      = 0x7u,
};

// An optional control word that follows an entry's four-word header. The top
// three bits say which kind it is.
enum : u32 {
    kVdlWordSelectorShift = 29,
    kVdlWordColour        = 0,   // 0..3, with the low two bits enabling channels
    kVdlWordAvControl     = 4,   // 4..5
    kVdlWordDisplay       = 6,
    kVdlWordBackground    = 7,
};

// Fields of a colour word.
enum : u32 {
    kVdlColourBlueShift  = 0,
    kVdlColourGreenShift = 8,
    kVdlColourRedShift   = 16,
    kVdlColourAddrShift  = 24,
    kVdlColourAddrMask   = 0x1fu,
    kVdlColourEnableShift = 29,
    kVdlColourEnableMask  = 3u,
};

// Fields of a display-control word.
enum : u32 {
    kVdlDisplayColoursOnly = 1u << 1,
    kVdlDisplayClutBypass  = 1u << 25,
};

// How far apart the lines of a framebuffer are, selected by the control word.
constexpr u32 kVdlLineModulo[8] = {320, 384, 512, 640, 1024, 320, 320, 320};

class Vdlp {
public:
    // The line the display list starts running on. Not the first visible one:
    // it runs through the vertical blank as well.
    static constexpr u32 kListStartLine = 5;

    explicit Vdlp(Bus& bus);

    void reset();

    // Where the display list starts. Written by MADAM in the real machine; for
    // now the console sets it directly.
    void set_list_address(u32 address) { list_address_ = address; }

    // Where the visible part of a field sits within it.
    //
    // The display list starts running well before the first visible line, and
    // the framebuffer address advances on every line it runs for - blanking
    // included. So the picture that reaches the screen begins some way into
    // the buffer, and a machine that starts at the top of the buffer shows
    // everything shifted down by however many lines it skipped.
    void set_field_shape(u32 first_visible, u32 total_lines) {
        first_visible_line_ = first_visible;
        total_lines_ = total_lines;
    }
    u32  list_address() const { return list_address_; }

    // Walk the display list and produce a whole field into `out`, which must
    // hold width * height pixels in XRGB8888.
    void render_field(u32* out, int width, int height);

    // Read VRAM as a plain RGB555 framebuffer at `vram_offset`, ignoring the
    // display list entirely. This is not how the hardware works; it is how we
    // get a picture on screen while the VDL control bits are still unconfirmed,
    // and it is what the tests use to check the pixel conversion in isolation.
    void render_linear(u32* out, int width, int height, u32 vram_offset);

    // Which line of the framebuffer ends up at the top of the screen. The list
    // runs for this many lines before the first visible one, advancing the
    // address the whole time, so it is not zero.
    u32 buffer_start_line() const {
        return first_visible_line_ > kListStartLine
                   ? first_visible_line_ - kListStartLine
                   : 0u;
    }

    // The output colour table as it stands. For diagnosis: a wrong table
    // looks exactly like a wrong palette in the game data.
    u8 clut_red(u32 slot) const { return clut_red_[slot & 31]; }
    u8 clut_green(u32 slot) const { return clut_green_[slot & 31]; }
    u8 clut_blue(u32 slot) const { return clut_blue_[slot & 31]; }

    // Number of VDL entries followed on the last field. Zero means the list was
    // empty or did not look like a list, which is the symptom to look for when
    // a game shows nothing.
    u32 entries_walked() const { return entries_walked_; }

private:
    // One scanline's worth of pixels, from a framebuffer address in VRAM.
    // Step a framebuffer address on by one line. The layout interleaves rows
    // in pairs, so the step alternates between two bytes and the rest of the
    // pair - it is not a constant stride.
    u32 advance_line(u32 address) const;

    void render_line(u32* out, int width, u32 framebuffer_address, int line);

    Bus& bus_;
    u32 list_address_ = 0;
    u32 modulo_index_ = 0;

    // The output colour table. Not an indexed palette: each of the three
    // five-bit channels is looked up separately to eight bits, so it is a per
    // channel ramp. A title reprograms it to fade, tint, or flash without
    // touching a single pixel of its framebuffer - so a machine that ignores
    // it renders every fade as a hard cut.
    static constexpr u32 kClutEntries = 32;
    u8 clut_red_[kClutEntries] = {};
    u8 clut_green_[kClutEntries] = {};
    u8 clut_blue_[kClutEntries] = {};

    // What a pixel of zero shows. Also programmable, and also not black by
    // default in every title.
    u32 background_ = 0;
    bool clut_bypass_ = false;

    void reset_clut();
    void process_control_words(u32 address, u32 count);
    u32  shade(u16 pixel) const;

    // Defaults to no blanking at all - the first line the list runs for is the
    // first line shown, and the field is however tall the output is. That is
    // what a test wants. The console replaces it with the real region shape.
    u32 first_visible_line_ = kListStartLine;
    u32 total_lines_ = 0;
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
// The top bit of a framebuffer pixel. It means nothing at all unless the
// display list has asked for CLUT bypass, and then it means "this pixel is
// already a colour, do not look it up".
constexpr u16 kVdlPixelLiteral = 0x8000;

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
