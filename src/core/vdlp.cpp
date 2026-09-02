#include "vdlp.h"

#include <cstring>

#include "bus.h"

namespace retro3do {
namespace {

// A display list that does not terminate would hang the emulator on a bad
// pointer, so following it is bounded. A real field needs far fewer entries
// than this.
constexpr u32 kMaxVdlEntries = 1024;

// Offsets within a VDL entry, in words.
// Header order from the official VDL structure: control, current buffer,
// previous buffer, next entry, then optional control/CLUT words.
constexpr u32 kVdlControlWord      = 0;
constexpr u32 kVdlCurrentBuffer    = 1;
constexpr u32 kVdlNextEntry        = 3;

}  // namespace

Vdlp::Vdlp(Bus& bus) : bus_(bus) { reset_clut(); }

void Vdlp::reset() {
    list_address_ = 0;
    reset_clut();
    entries_walked_ = 0;
    field_entry_ = 0;
    field_buffer_ = 0;
    field_persist_ = 0;
    field_width_ = 0;
    field_height_ = 0;
    field_have_buffer_ = false;
    field_dma_enabled_ = false;
    field_list_ended_ = true;
    field_active_ = false;
}

void Vdlp::render_line(u32* out, int width, u32 framebuffer_address, int line) {
    const u8* vram = bus_.vram();

    // The VDLP exposes only the fitted VRAM address lines. Software
    // occasionally leaves high address bits set; fitted address lines wrap
    // them rather than turning an otherwise valid framebuffer black.
    const u32 base = framebuffer_address & (kVramSize - 1u);

    for (int x = 0; x < width; ++x) {
        const u32 at = (base + framebuffer_offset(x, line, width)) &
                       (kVramSize - 1u);
        const u32 next = (at + 1u) & (kVramSize - 1u);
        const u16 pixel =
            static_cast<u16>((static_cast<u16>(vram[at]) << 8) | vram[next]);
        out[x] = shade(pixel);
    }
}

void Vdlp::render_linear(u32* out, int width, int height, u32 vram_offset) {
    const u8* vram = bus_.vram();
    u32 offset = vram_offset;

    for (int y = 0; y < height; ++y) {
        u32* row = out + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            if (offset + 1 >= kVramSize) {
                row[x] = 0xff000000u;
                continue;
            }
            const u16 pixel = static_cast<u16>(
                (static_cast<u16>(vram[offset]) << 8) | vram[offset + 1]);
            row[x] = expand_rgb555(pixel);
            offset += 2;
        }
    }
}

// The table software finds when it starts: a straight ramp from five bits to
// eight. A title that never touches the table gets exactly the expansion a
// naive implementation would do, which is why ignoring the table looks right
// until something fades.
void Vdlp::reset_clut() {
    for (u32 i = 0; i < kClutEntries; ++i) {
        const u8 value = static_cast<u8>((i * 255u + 15u) / 31u);
        clut_red_[i] = value;
        clut_green_[i] = value;
        clut_blue_[i] = value;
    }
    background_ = 0;
    clut_bypass_ = false;
    horizontal_interp_ = false;
    vertical_interp_ = false;
    disable_vertical_once_ = false;
}

// The words that follow an entry's header: palette entries, the background
// colour, and display control.
void Vdlp::process_control_words(u32 address, u32 count) {
    bool colours_only = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 word = bus_.read32(address + i * 4);
        switch (word >> kVdlWordSelectorShift) {
            case 0: case 1: case 2: case 3: {
                const u32 slot = (word >> kVdlColourAddrShift) & kVdlColourAddrMask;
                const u8 red   = static_cast<u8>(word >> kVdlColourRedShift);
                const u8 green = static_cast<u8>(word >> kVdlColourGreenShift);
                const u8 blue  = static_cast<u8>(word >> kVdlColourBlueShift);
                // The two enable bits say which channels this word carries.
                // Zero means all three, which is not what the numbering
                // suggests and is the easy thing to get backwards.
                switch ((word >> kVdlColourEnableShift) & kVdlColourEnableMask) {
                    case 0:
                        clut_red_[slot] = red;
                        clut_green_[slot] = green;
                        clut_blue_[slot] = blue;
                        break;
                    case 1: clut_blue_[slot] = blue; break;
                    case 2: clut_green_[slot] = green; break;
                    default: clut_red_[slot] = red; break;
                }
                break;
            }
            case kVdlWordAvControl:
            case 5:
                break;   // for the audio/video output stage, not for us
            case kVdlWordDisplay:
                if (colours_only) {
                    continue;
                }
                clut_bypass_ = (word & kVdlDisplayClutBypass) != 0;
                horizontal_interp_ =
                    (word & kVdlDisplayHorizontalInterp) != 0;
                vertical_interp_ = (word & kVdlDisplayVerticalInterp) != 0;
                disable_vertical_once_ =
                    (word & kVdlDisplayDisableVerticalOnce) != 0;
                colours_only = (word & kVdlDisplayColoursOnly) != 0;
                break;
            default:
                background_ = word;
                break;
        }
    }
}

// One framebuffer pixel to one output pixel.
// Turn a pixel out of the framebuffer into a colour on the screen.
//
// Zero is not a colour. Whatever the palette says about entry zero, a zero
// pixel shows the background colour - that is how a title gets a coloured
// border or a flat backdrop without writing it into the framebuffer at all.
//
// "Bypass CLUT" does not bypass the CLUT.
// -------------------------------------
// The name is a trap. Turning it on does not send every pixel straight to the
// screen; it makes bit 15 of each pixel decide, PER PIXEL, which way that
// pixel goes. Set, and the remaining fifteen bits are already a colour and are
// used as one. Clear, and the pixel goes through the palette exactly as it
// would with the bit switched off entirely.
//
// So the mode is not a bypass, it is a per-pixel escape hatch: it lets a title
// mix true-colour pixels into a palettised image. Treating it as a whole-frame
// bypass sends the palettised half of that mixture to the screen as raw
// colour, which puts a coloured speckle over the picture in exactly the places
// the title was relying on the palette. Wing Commander III's opening video
// plays under one.
u32 Vdlp::shade(u16 pixel) const {
    if (pixel == 0) {
        return 0xff000000u | (background_ & 0x00ffffffu);
    }
    if (clut_bypass_ && (pixel & kVdlPixelLiteral) != 0) {
        return expand_rgb555(pixel);
    }
    return 0xff000000u |
           (static_cast<u32>(clut_red_[(pixel >> 10) & 0x1f]) << 16) |
           (static_cast<u32>(clut_green_[(pixel >> 5) & 0x1f]) << 8) |
           static_cast<u32>(clut_blue_[pixel & 0x1f]);
}

u32 Vdlp::advance_line(u32 address) const {
    const u32 modulo = kVdlLineModulo[modulo_index_ & 7];
    return address + ((address & 2) != 0 ? ((modulo << 2) - 2) : 2);
}

// Walk the display list a LINE at a time rather than an entry at a time.
//
// An entry says how many lines it persists for, and the framebuffer address
// advances every line whether or not a new entry arrives. That is the whole
// mechanism, and it is why a persist count of zero is ordinary rather than
// degenerate: it means the next entry arrives on the very next line. A list
// with one entry per line is normal.
//
// Treating a zero as "the rest of the field" renders the whole screen out of
// the first entry's buffer, which for a title that puts a blank buffer first
// is a black screen - and looks for all the world like nothing was drawn.
// Walk the display list a LINE at a time rather than an entry at a time, and
// run it across the WHOLE field rather than just the visible part.
//
// An entry says how many lines it persists for, and the framebuffer address
// advances every line whether or not a new entry arrives. A persist count of
// zero is ordinary rather than degenerate: it means the entry covers no lines
// at all and the next one is picked up on the same line. Treating zero as "the
// rest of the field" renders the whole screen out of the first entry's buffer,
// which for a title that puts a blank buffer first is a black screen.
//
// The list also starts running well before the first visible line, and the
// address advances the whole time - blanking included. So the picture that
// reaches the screen begins some way into the buffer. Starting at the top of
// it instead shifts everything down by however many lines were skipped, which
// looks like a badly centred television rather than a bug.
void Vdlp::begin_field(int width, int height) {
    entries_walked_ = 0;
    field_width_ = width;
    field_height_ = height;
    field_entry_ = list_address_;
    field_buffer_ = 0;
    field_persist_ = 0;
    field_have_buffer_ = false;
    field_dma_enabled_ = false;
    field_list_ended_ = list_address_ == 0;
    field_active_ = width > 0 && height > 0;

    logical_frame_.assign(static_cast<size_t>(width > 0 ? width : 0) *
                              static_cast<size_t>(height > 0 ? height : 0),
                          0xff000000u);
    line_interp_modes_.assign(static_cast<size_t>(height > 0 ? height : 0), 0);
}

void Vdlp::take_field_entry() {
    if (field_list_ended_ || entries_walked_ >= kMaxVdlEntries) {
        return;
    }

    const u32 entry = field_entry_;
    const u32 control = bus_.read32(entry + kVdlControlWord * 4u);
    if (control == 0) {
        field_list_ended_ = true;
        return;
    }
    ++entries_walked_;

    if ((control & kVdlCurrOverride) != 0) {
        field_buffer_ = bus_.read32(entry + kVdlCurrentBuffer * 4u);
        field_have_buffer_ = true;
    }
    modulo_index_ = (control >> kVdlModuloShift) & kVdlModuloMask;
    field_dma_enabled_ = (control & kVdlEnableDma) != 0;
    field_persist_ = static_cast<s32>(control & kVdlPersistMask);

    const u32 words = (control >> kVdlControlCountShift) & kVdlControlCountMask;
    if (words != 0) {
        process_control_words(entry + (kVdlNextEntry + 1u) * 4u, words);
    }

    u32 next = bus_.read32(entry + kVdlNextEntry * 4u);
    if ((control & kVdlNextRelative) != 0) {
        next += entry + (kVdlNextEntry + 1u) * 4u;
    }
    if (next == entry || next == 0) {
        field_list_ended_ = true;
        return;
    }
    field_entry_ = next;
}

void Vdlp::process_scanline(u32 line) {
    if (!field_active_ || line < kListStartLine) {
        return;
    }

    const u32 last_line = total_lines_ != 0
                              ? total_lines_
                              : first_visible_line_ + static_cast<u32>(field_height_);
    if (line >= last_line) {
        return;
    }

    // The entry is read in horizontal blank before this line is displayed.
    if (line == kListStartLine) {
        take_field_entry();
    }
    if (field_persist_ == 0) {
        take_field_entry();
    }

    if (line >= first_visible_line_) {
        const u32 output_line = line - first_visible_line_;
        if (output_line < static_cast<u32>(field_height_)) {
            line_interp_modes_[output_line] =
                static_cast<u8>((horizontal_interp_ ? 1u : 0u) |
                                ((vertical_interp_ && !disable_vertical_once_)
                                     ? 2u
                                     : 0u));
            u32* row = logical_frame_.data() +
                       static_cast<size_t>(output_line) * field_width_;
            if (!field_have_buffer_ || !field_dma_enabled_) {
                for (int x = 0; x < field_width_; ++x) {
                    row[x] = 0xff000000u;
                }
            } else {
                render_line(row, field_width_, field_buffer_, 0);
            }
        }
    }

    // The SDK's documented line order ticks the bitmap pointer after display
    // and before the next entry is read.
    if (field_have_buffer_) {
        field_buffer_ = advance_line(field_buffer_);
    }
    disable_vertical_once_ = false;
    --field_persist_;
}

void Vdlp::render_field(u32* out, int width, int height) {
    if (out == nullptr || width <= 0 || height <= 0) {
        return;
    }
    begin_field(width, height);
    const u32 last_line = total_lines_ != 0
                              ? total_lines_
                              : first_visible_line_ + static_cast<u32>(height);
    for (u32 line = kListStartLine; line < last_line; ++line) {
        process_scanline(line);
    }
    std::memcpy(out, logical_frame_.data(),
                static_cast<size_t>(width) * height * sizeof(u32));
}

namespace {

u32 average2(u32 a, u32 b) {
    // Average channels independently so a carry out of blue cannot leak into
    // green. The display generator works on the 24-bit colours after the CLUT.
    const u32 rb = (((a & 0x00ff00ffu) + (b & 0x00ff00ffu)) >> 1) &
                   0x00ff00ffu;
    const u32 g = (((a & 0x0000ff00u) + (b & 0x0000ff00u)) >> 1) &
                  0x0000ff00u;
    return 0xff000000u | rb | g;
}

u32 average4(u32 a, u32 b, u32 c, u32 d) {
    // Red and blue are far enough apart to sum four eight-bit channels in
    // parallel without either carrying into the other. This is bit-exact with
    // averaging each channel separately, but substantially cheaper in the
    // display generator's per-pixel loop.
    constexpr u32 kRedBlue = 0x00ff00ffu;
    constexpr u32 kGreen = 0x0000ff00u;
    const u32 rb = (((a & kRedBlue) + (b & kRedBlue) +
                     (c & kRedBlue) + (d & kRedBlue)) >> 2) & kRedBlue;
    const u32 g = (((a & kGreen) + (b & kGreen) +
                    (c & kGreen) + (d & kGreen)) >> 2) & kGreen;
    return 0xff000000u | rb | g;
}

}  // namespace

void Vdlp::render_field_2x(u32* out, int width, int height) {
    if (out == nullptr || width <= 0 || height <= 0) {
        return;
    }

    begin_field(width, height);
    const u32 last_line = total_lines_ != 0
                              ? total_lines_
                              : first_visible_line_ + static_cast<u32>(height);
    for (u32 line = kListStartLine; line < last_line; ++line) {
        process_scanline(line);
    }
    finish_field_2x(out, width, height);
}

void Vdlp::finish_field_2x(u32* out, int width, int height) {
    if (out == nullptr || width <= 0 || height <= 0 ||
        width != field_width_ || height != field_height_) {
        return;
    }

    const int output_width = width * 2;
    for (int y = 0; y < height; ++y) {
        const int below_y = y + 1 < height ? y + 1 : y;
        const bool horizontal = (line_interp_modes_[y] & 1u) != 0;
        const bool vertical = (line_interp_modes_[y] & 2u) != 0;
        const u32* row = logical_frame_.data() + static_cast<size_t>(y) * width;
        const u32* below_row =
            logical_frame_.data() + static_cast<size_t>(below_y) * width;
        u32* upper = out + static_cast<size_t>(y * 2) * output_width;
        u32* lower = upper + output_width;

        // Interpolation mode is constant for the whole scanline. Splitting the
        // four cases here avoids testing both axes and loading unused neighbour
        // pixels for every output pixel. The last source pixel is handled
        // separately because its right neighbour clamps to itself.
        if (!horizontal && !vertical) {
            for (int x = 0; x < width; ++x) {
                const u32 here = row[x];
                upper[x * 2] = here;
                upper[x * 2 + 1] = here;
                lower[x * 2] = here;
                lower[x * 2 + 1] = here;
            }
        } else if (horizontal && !vertical) {
            for (int x = 0; x + 1 < width; ++x) {
                const u32 here = row[x];
                const u32 half = average2(here, row[x + 1]);
                upper[x * 2] = here;
                upper[x * 2 + 1] = half;
                lower[x * 2] = here;
                lower[x * 2 + 1] = half;
            }
            const int x = width - 1;
            upper[x * 2] = upper[x * 2 + 1] = row[x];
            lower[x * 2] = lower[x * 2 + 1] = row[x];
        } else if (!horizontal && vertical) {
            for (int x = 0; x < width; ++x) {
                const u32 here = row[x];
                const u32 half = average2(here, below_row[x]);
                upper[x * 2] = upper[x * 2 + 1] = here;
                lower[x * 2] = lower[x * 2 + 1] = half;
            }
        } else {
            for (int x = 0; x + 1 < width; ++x) {
                const u32 here = row[x];
                const u32 right = row[x + 1];
                const u32 below = below_row[x];
                const u32 diagonal = below_row[x + 1];
                upper[x * 2] = here;
                upper[x * 2 + 1] = average2(here, right);
                lower[x * 2] = average2(here, below);
                lower[x * 2 + 1] = average4(here, right, below, diagonal);
            }
            const int x = width - 1;
            const u32 here = row[x];
            const u32 half = average2(here, below_row[x]);
            upper[x * 2] = upper[x * 2 + 1] = here;
            lower[x * 2] = lower[x * 2 + 1] = half;
        }
    }
}


}  // namespace retro3do
