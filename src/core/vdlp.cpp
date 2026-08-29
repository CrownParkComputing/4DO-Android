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
    int line = 0;

    while (line < height && entries_walked_ < kMaxVdlEntries) {
        const u32 control  = bus_.read32(entry + kVdlControlWord * 4);
        const u32 buffer   = bus_.read32(entry + kVdlCurrentBuffer * 4);
        const u32 next     = bus_.read32(entry + kVdlNextEntry * 4);

        u32 lines = (control & kVdlLineCountMask) >> kVdlLineCountShift;
        if (lines == 0) {
            // An entry covering no lines would spin forever. Treat it as
            // covering the rest of the field, which at least produces a picture.
            lines = static_cast<u32>(height - line);
        }

        const int last = line + static_cast<int>(lines) > height
                             ? height
                             : line + static_cast<int>(lines);
        for (int y = line; y < last; ++y) {
            render_line(out + static_cast<size_t>(y) * width, width, buffer,
                        y - line);
        }
        line = last;

        ++entries_walked_;
        if (next == 0 || next == entry) {
            break;
        }
        entry = next;
    }

    // Anything the list did not cover stays black.
    for (int y = line; y < height; ++y) {
        u32* row = out + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            row[x] = 0xff000000u;
        }
    }
}

}  // namespace retro3do
