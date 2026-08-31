#include "bus.h"

#include <algorithm>
#include <cstring>

#include "clio.h"
#include "madam.h"
#include "sport.h"

namespace retro3do {
namespace {

// A formatted, empty NVRAM.
//
// Real hardware is never blank: the machine formats its NVRAM the first time
// it is switched on and it stays that way for the rest of its life. An
// emulator that comes up zeroed is not modelling a new console, it is
// modelling one that has never been switched on at all - and the OS only
// formats it when it reaches its own shell, so a title booted straight from a
// disc finds no filesystem and reports the NVRAM FULL.
//
// This is what the boot ROM itself writes, captured from a run with no disc
// in the drive.
constexpr u8 kFormattedNvram[] = {
    0x01, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x6E, 0x76, 0x72, 0x61, 0x6D, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x00,
    0xFF, 0xFF, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x5A, 0x02, 0xB6, 0x00, 0x00, 0x00, 0x98, 0x00, 0x00, 0x00, 0x98,
    0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x14, 0x7A, 0xA5, 0x65, 0xBD,
    0x00, 0x00, 0x00, 0x84, 0x00, 0x00, 0x00, 0x84, 0x00, 0x00, 0x7F, 0x68,
    0x00, 0x00, 0x00, 0x14,
};

}  // namespace
namespace {

// The 3DO stores words big-endian. Host byte order is almost always little, so
// every word access swaps. These compile to a single REV / BSWAP.
inline u32 load_be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

inline void store_be32(u8* p, u32 value) {
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
}

inline u16 load_be16(const u8* p) {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
}

inline void store_be16(u8* p, u16 value) {
    p[0] = static_cast<u8>(value >> 8);
    p[1] = static_cast<u8>(value);
}

}  // namespace

Bus::Bus()
    : dram_(kDramSize, 0),
      vram_(kVramSize, 0),
      rom_(kRomSize, 0),
      nvram_(kNvramSize, 0) {
    format_nvram();
}

// Put the NVRAM back to formatted-but-empty, which is where a console that has
// been switched on before starts from.
void Bus::format_nvram() {
    std::fill(nvram_.begin(), nvram_.end(), 0);
    std::copy(std::begin(kFormattedNvram), std::end(kFormattedNvram),
              nvram_.begin());
    nvram_dirty_ = true;
}

bool Bus::restore_nvram(const u8* data, size_t size) {
    if (data == nullptr || size != nvram_.size()) {
        return false;
    }
    std::copy(data, data + size, nvram_.begin());
    // A restored image is what the user last had, so there is nothing new to
    // write back until the machine changes it.
    nvram_dirty_ = false;
    return true;
}

// Run the cel engine, if a store has asked for it.
//
// Called after each instruction rather than during the store that requests it,
// because the hardware does exactly that: writing the start port only sets the
// engine going, and it begins reading CCBs once the CPU's current work is
// done. Software is entitled to rely on that gap - and Need for Speed does. It
// starts the engine and then finishes writing the very CCB the engine is about
// to read, so a machine that walks the list inside the store reads the
// previous frame's version of it.
//
// The effect is not subtle. The road and terrain are drawn from the CCBs
// written in that gap, so they were missing entirely: a correct cockpit and
// mirror sitting in front of a flat field of the clear colour.
void Bus::run_pending_cel_engine() {
    if (!cel_pending_) {
        return;
    }
    cel_pending_ = false;
    if (madam_ != nullptr) {
        madam_->run_cel_engine();
    }
}

void Bus::reset() {
    std::fill(dram_.begin(), dram_.end(), u8{0});
    std::fill(vram_.begin(), vram_.end(), u8{0});
    write_watch_.clear();
    sport_accesses_ = 0;
    rom_overlay_ = true;
    cel_pending_ = false;
    // ROM and NVRAM deliberately survive a reset, as they do in hardware.
}

bool Bus::load_bios(const u8* data, size_t size) {
    if (data == nullptr || size == 0 || size > rom_.size()) {
        bios_loaded_ = false;
        return false;
    }
    std::fill(rom_.begin(), rom_.end(), u8{0});
    std::memcpy(rom_.data(), data, size);
    bios_loaded_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------
u32 Bus::read32(u32 address) {
    // ARM word accesses ignore the low two address bits.
    const u32 addr = address & ~u32{3};

    if (addr < kDramSize) {
        if (rom_overlay_) {
            return addr < kRomSize ? load_be32(&rom_[addr]) : 0u;
        }
        return load_be32(&dram_[addr]);
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        return load_be32(&vram_[addr - kVramBase]);
    }
    if (addr >= kRomBase && addr < kRomBase + kRomSize) {
        return load_be32(&rom_[addr - kRomBase]);
    }
    if (addr >= kNvramBase && addr < kNvramBase + kNvramSpan) {
        // One stored byte per word, on the low byte.
        return nvram_[((addr - kNvramBase) % kNvramMirror) / kNvramStride];
    }
    if (clio_ != nullptr && addr >= kClioBase && addr < kClioBase + kClioSize) {
        return clio_->read(addr - kClioBase);
    }
    if (addr >= kSportBase && addr < kSportBase + kSportSize) {
        ++sport_accesses_;
        return sport_ != nullptr ? sport_->read(addr - kSportBase) : 0;
    }
    if (madam_ != nullptr && addr >= kMadamBase && addr < kMadamBase + kMadamSize) {
        return madam_->read(addr - kMadamBase);
    }
    // Reading an unmapped region returns zero rather than aborting. That is
    // scaffolding: it should become a real data abort once enough of the
    // machine exists that an unmapped read is definitely a bug.
    return 0;
}

u16 Bus::read16(u32 address) {
    const u32 addr = address & ~u32{1};
    if (addr < kDramSize) {
        if (rom_overlay_) {
            return addr < kRomSize ? load_be16(&rom_[addr]) : u16{0};
        }
        return load_be16(&dram_[addr]);
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        return load_be16(&vram_[addr - kVramBase]);
    }
    if (addr >= kRomBase && addr < kRomBase + kRomSize) {
        return load_be16(&rom_[addr - kRomBase]);
    }
    // Device registers are word-wide, but software may reach them with a
    // halfword access. Reading the containing word and extracting is what the
    // hardware does; leaving these out entirely - as an earlier version did -
    // means such an access silently returns nothing and the write silently
    // vanishes, which is invisible and extremely hard to attribute.
    if (is_device(addr)) {
        const u32 word = read32(addr & ~u32{3});
        return static_cast<u16>((addr & 2u) ? (word & 0xffffu) : (word >> 16));
    }
    return 0;
}

u8 Bus::read8(u32 address) {
    if (address < kDramSize) {
        if (rom_overlay_) {
            return address < kRomSize ? rom_[address] : u8{0};
        }
        return dram_[address];
    }
    if (address >= kVramBase && address < kVramBase + kVramSize) {
        return vram_[address - kVramBase];
    }
    if (address >= kRomBase && address < kRomBase + kRomSize) {
        return rom_[address - kRomBase];
    }
    if (address >= kNvramBase && address < kNvramBase + kNvramSpan) {
        return (((address - kNvramBase) % kNvramMirror) % kNvramStride) == kNvramStride - 1
                   ? nvram_[((address - kNvramBase) % kNvramMirror) / kNvramStride]
                   : 0u;
    }
    if (is_device(address)) {
        const u32 word = read32(address & ~u32{3});
        const unsigned shift = (3u - (address & 3u)) * 8u;
        return static_cast<u8>((word >> shift) & 0xffu);
    }
    return 0;
}

u32 Bus::fetch32(u32 address) {
    const u32 addr = address & ~u32{3};
    if (addr >= kRomBase && addr < kRomBase + kRomSize) {
        return load_be32(&rom_[addr - kRomBase]);
    }
    if (addr < kDramSize) {
        if (rom_overlay_) {
            return addr < kRomSize ? load_be32(&rom_[addr]) : 0u;
        }
        return load_be32(&dram_[addr]);
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        return load_be32(&vram_[addr - kVramBase]);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------
void Bus::write32(u32 address, u32 value) {
    const u32 addr = address & ~u32{3};

    if (addr < kDramSize) {
        rom_overlay_ = false;
        store_be32(&dram_[addr], value);
        write_watch_.note(addr, 4);
        return;
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        store_be32(&vram_[addr - kVramBase], value);
        return;
    }
    if (addr >= kNvramBase && addr < kNvramBase + kNvramSpan) {
        nvram_[((addr - kNvramBase) % kNvramMirror) / kNvramStride] = static_cast<u8>(value & 0xffu);
        nvram_dirty_ = true;
        return;
    }
    if (clio_ != nullptr && addr >= kClioBase && addr < kClioBase + kClioSize) {
        clio_->write(addr - kClioBase, value);
        return;
    }
    if (addr >= kSportBase && addr < kSportBase + kSportSize) {
        ++sport_accesses_;
        if (sport_ != nullptr) sport_->write(addr - kSportBase, value);
        return;
    }
    if (madam_ != nullptr && addr >= kMadamBase && addr < kMadamBase + kMadamSize) {
        madam_->write(addr - kMadamBase, value);
        return;
    }
    // ROM is read-only.
}

void Bus::write16(u32 address, u16 value) {
    const u32 addr = address & ~u32{1};
    if (addr < kDramSize) {
        rom_overlay_ = false;
        store_be16(&dram_[addr], value);
        write_watch_.note(addr, 2);
        return;
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        store_be16(&vram_[addr - kVramBase], value);
        return;
    }
    if (is_device(addr)) {
        const u32 aligned = addr & ~u32{3};
        const u32 word = read32(aligned);
        const u32 merged = (addr & 2u) ? ((word & 0xffff0000u) | value)
                                       : ((word & 0x0000ffffu) |
                                          (static_cast<u32>(value) << 16));
        write32(aligned, merged);
        return;
    }
}

void Bus::write8(u32 address, u8 value) {
    if (address < kDramSize) {
        rom_overlay_ = false;
        dram_[address] = value;
        write_watch_.note(address, 1);
        return;
    }
    if (address >= kVramBase && address < kVramBase + kVramSize) {
        vram_[address - kVramBase] = value;
        return;
    }
    if (address >= kNvramBase && address < kNvramBase + kNvramSpan) {
        if ((((address - kNvramBase) % kNvramMirror) % kNvramStride) == kNvramStride - 1) {
            nvram_[((address - kNvramBase) % kNvramMirror) / kNvramStride] = value;
            nvram_dirty_ = true;
        }
        return;
    }
    if (is_device(address)) {
        const u32 aligned = address & ~u32{3};
        const unsigned shift = (3u - (address & 3u)) * 8u;
        const u32 word = read32(aligned);
        write32(aligned, (word & ~(0xffu << shift)) |
                             (static_cast<u32>(value) << shift));
        return;
    }
}

}  // namespace retro3do
