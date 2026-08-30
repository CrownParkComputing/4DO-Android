#include "vdlp.h"

#include "bus.h"

namespace retro3do {
namespace {

// A display list that does not terminate would hang the emulator on a bad
// pointer, so following it is bounded. A real field needs far fewer entries
// than this.
constexpr u32 kMaxVdlEntries = 1024;

// Offsets within a VDL entry, in words.
// TODO(vdl): confirm against the published documentation. The order below —
// control, current buffer, previous buffer, next entry, then the CLUT — is the
// commonly described layout, but it has not been verified here.
constexpr u32 kVdlControlWord      = 0;
constexpr u32 kVdlCurrentBuffer    = 1;
constexpr u32 kVdlPreviousBuffer   = 2;
constexpr u32 kVdlNextEntry        = 3;

}  // namespace

Vdlp::Vdlp(Bus& bus) : bus_(bus) { reset_clut(); }

void Vdlp::reset() {
    list_address_ = 0;
    reset_clut();
    entries_walked_ = 0;
}

void Vdlp::render_line(u32* out, int width, u32 framebuffer_address, int line) {
    const u8* vram = bus_.vram();

    const u32 base = framebuffer_address - kVramBase;

    for (int x = 0; x < width; ++x) {
        // Bounds-check every pixel rather than the line: a display list can
        // legitimately point near the end of VRAM, and a game that sets a bad
        // pointer should show corruption, not crash the emulator.
        const u32 at = base + framebuffer_offset(x, line, width);
        if (at + 1 >= kVramSize) {
            out[x] = 0xff000000u;
            continue;
        }
        const u16 pixel =
            static_cast<u16>((static_cast<u16>(vram[at]) << 8) | vram[at + 1]);
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
                colours_only = (word & kVdlDisplayColoursOnly) != 0;
                break;
            default:
                background_ = word;
                break;
        }
    }
}

// One framebuffer pixel to one output pixel.
u32 Vdlp::shade(u16 pixel) const {
    if (pixel == 0) {
        return 0xff000000u | (background_ & 0x00ffffffu);
    }
    if (clut_bypass_) {
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
// advances every line whether or not a new entry arrives. That is why a
// persist count of zero is ordinary rather than degenerate: it means the next
// entry arrives on the very next line, and a list with one entry per line is
// normal. Treating zero as "the rest of the field" renders the whole screen
// out of the first entry's buffer, which for a title that puts a blank buffer
// first is a black screen.
//
// The list also starts running well before the first visible line, and the
// address advances on every line it runs for - blanking included. So the
// picture that reaches the screen begins some way into the buffer. Starting at
// the top of the buffer instead shifts everything down by however many lines
// were skipped, which on this machine is nineteen and looks like a badly
// centred television rather than a bug.
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
void Vdlp::render_field(u32* out, int width, int height) {
    entries_walked_ = 0;

    if (list_address_ == 0) {
        // No display list yet. Leave the frame black rather than showing
        // whatever happens to be at VRAM offset zero, which would look like a
        // rendering bug rather than an unconfigured machine.
        for (int i = 0; i < width * height; ++i) {
            out[i] = 0xff000000u;
        }
        return;
    }

    u32 entry = list_address_;
    u32 buffer = 0;
    s32 persist = 0;
    bool have_buffer = false;
    bool list_ended = false;

    // Reads one entry and leaves the walk pointing at the next.
    const auto take_entry = [&]() {
        if (list_ended || entries_walked_ >= kMaxVdlEntries) {
            return;
        }
        const u32 control = bus_.read32(entry);
        if (control == 0) {
            list_ended = true;
            return;
        }
        ++entries_walked_;

        // The buffer only changes when the entry says so; otherwise it carries
        // on from where the previous entry left it.
        if ((control & kVdlCurrOverride) != 0) {
            buffer = bus_.read32(entry + 4);
            have_buffer = true;
        }
        modulo_index_ = (control >> kVdlModuloShift) & kVdlModuloMask;
        persist = static_cast<s32>(control & kVdlPersistMask);

        // The palette and display control ride along behind the header.
        const u32 words = (control >> kVdlControlCountShift) & kVdlControlCountMask;
        if (words != 0) {
            process_control_words(entry + 16, words);
        }

        u32 next = bus_.read32(entry + 12);
        if ((control & kVdlNextRelative) != 0) {
            next += entry + 16;
        }
        if (next == entry || next == 0) {
            list_ended = true;
            return;
        }
        entry = next;
    };

    const u32 last_line = total_lines_ != 0
                              ? total_lines_
                              : first_visible_line_ + static_cast<u32>(height);

    for (u32 line = kListStartLine; line < last_line; ++line) {
        // The first line takes an entry unconditionally; every line takes one
        // if the last has run out. Both can happen on the first line, which is
        // why an entry that persists for nothing still costs a line's worth of
        // walking.
        if (line == kListStartLine) {
            take_entry();
        }
        if (persist == 0) {
            take_entry();
        }

        if (line >= first_visible_line_) {
            const u32 output_line = line - first_visible_line_;
            if (output_line >= static_cast<u32>(height)) {
                break;
            }
            u32* row = out + static_cast<size_t>(output_line) * width;
            if (!have_buffer) {
                for (int x = 0; x < width; ++x) {
                    row[x] = 0xff000000u;
                }
            } else {
                render_line(row, width, buffer, 0);
            }
        }

        if (have_buffer) {
            buffer = advance_line(buffer);
        }
        --persist;
    }
}


}  // namespace retro3do
