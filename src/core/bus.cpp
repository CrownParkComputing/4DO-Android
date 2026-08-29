#include "bus.h"

#include <algorithm>
#include <cstring>

namespace retro3do {
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
      nvram_(kNvramSize, 0) {}

void Bus::reset() {
    std::fill(dram_.begin(), dram_.end(), u8{0});
    std::fill(vram_.begin(), vram_.end(), u8{0});
    write_watch_.clear();
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
        return load_be32(&dram_[addr]);
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        return load_be32(&vram_[addr - kVramBase]);
    }
    if (addr >= kRomBase && addr < kRomBase + kRomSize) {
        return load_be32(&rom_[addr - kRomBase]);
    }
    if (addr >= kNvramBase && addr < kNvramBase + kNvramSize) {
        return load_be32(&nvram_[addr - kNvramBase]);
    }
    // MADAM and CLIO registers are not wired up yet; reading an unmapped
    // region returns zero rather than aborting, which keeps early boot alive
    // while the chips are still being written.
    return 0;
}

u16 Bus::read16(u32 address) {
    const u32 addr = address & ~u32{1};
    if (addr < kDramSize) {
        return load_be16(&dram_[addr]);
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        return load_be16(&vram_[addr - kVramBase]);
    }
    if (addr >= kRomBase && addr < kRomBase + kRomSize) {
        return load_be16(&rom_[addr - kRomBase]);
    }
    return 0;
}

u8 Bus::read8(u32 address) {
    if (address < kDramSize) {
        return dram_[address];
    }
    if (address >= kVramBase && address < kVramBase + kVramSize) {
        return vram_[address - kVramBase];
    }
    if (address >= kRomBase && address < kRomBase + kRomSize) {
        return rom_[address - kRomBase];
    }
    if (address >= kNvramBase && address < kNvramBase + kNvramSize) {
        return nvram_[address - kNvramBase];
    }
    return 0;
}

u32 Bus::fetch32(u32 address) {
    const u32 addr = address & ~u32{3};
    if (addr >= kRomBase && addr < kRomBase + kRomSize) {
        return load_be32(&rom_[addr - kRomBase]);
    }
    if (addr < kDramSize) {
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
        store_be32(&dram_[addr], value);
        write_watch_.note(addr, 4);
        return;
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        store_be32(&vram_[addr - kVramBase], value);
        return;
    }
    if (addr >= kNvramBase && addr < kNvramBase + kNvramSize) {
        store_be32(&nvram_[addr - kNvramBase], value);
        return;
    }
    // ROM is read-only; MADAM/CLIO register writes are dropped until those
    // chips exist.
}

void Bus::write16(u32 address, u16 value) {
    const u32 addr = address & ~u32{1};
    if (addr < kDramSize) {
        store_be16(&dram_[addr], value);
        write_watch_.note(addr, 2);
        return;
    }
    if (addr >= kVramBase && addr < kVramBase + kVramSize) {
        store_be16(&vram_[addr - kVramBase], value);
        return;
    }
}

void Bus::write8(u32 address, u8 value) {
    if (address < kDramSize) {
        dram_[address] = value;
        write_watch_.note(address, 1);
        return;
    }
    if (address >= kVramBase && address < kVramBase + kVramSize) {
        vram_[address - kVramBase] = value;
        return;
    }
    if (address >= kNvramBase && address < kNvramBase + kNvramSize) {
        nvram_[address - kNvramBase] = value;
        return;
    }
}

}  // namespace retro3do
