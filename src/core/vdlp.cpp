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

Vdlp::Vdlp(Bus& bus) : bus_(bus) {}

void Vdlp::reset() {
    list_address_ = 0;
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
        out[x] = expand_rgb555(pixel);
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
