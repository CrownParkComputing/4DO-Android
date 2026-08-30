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
    u32 persist = 0;
    bool have_buffer = false;
    bool list_ended = false;

    for (int line = 0; line < height; ++line) {
        while (persist == 0 && !list_ended && entries_walked_ < kMaxVdlEntries) {
            const u32 control = bus_.read32(entry);
            if (control == 0) {
                list_ended = true;
                break;
            }
            ++entries_walked_;

            // The buffer only changes when the entry says so; otherwise it
            // carries on from where the previous entry left it.
            if ((control & kVdlCurrOverride) != 0) {
                buffer = bus_.read32(entry + 4);
                have_buffer = true;
            }

            modulo_index_ = (control >> kVdlModuloShift) & kVdlModuloMask;
            persist = (control & kVdlPersistMask) + 1;

            u32 next = bus_.read32(entry + 12);
            if ((control & kVdlNextRelative) != 0) {
                next += entry + 16;
            }
            if (next == entry || next == 0) {
                list_ended = true;
                break;
            }
            entry = next;
        }

        u32* row = out + static_cast<size_t>(line) * width;
        if (!have_buffer) {
            for (int x = 0; x < width; ++x) {
                row[x] = 0xff000000u;
            }
            continue;
        }

        render_line(row, width, buffer, 0);
        buffer = advance_line(buffer);
        if (persist > 0) {
            --persist;
        }
    }
}


}  // namespace retro3do
