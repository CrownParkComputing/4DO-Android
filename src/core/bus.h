// System bus and memory map.
//
// Everything the CPU can address goes through here. The map below is written
// from published 3DO hardware documentation and is deliberately data-driven, so
// that correcting a region is a one-line change rather than a hunt through
// scattered address comparisons.
//
// Region bases still to be confirmed against the Portfolio hardware manuals are
// marked TODO(map). They are not guesses in the sense of being invented — they
// are the commonly published values — but this core's rule is that nothing is
// asserted as fact until it has been checked against a source we can cite.
#pragma once

#include <vector>

#include "types.h"

namespace retro3do {

class Madam;
class Clio;
class Sport;

// ---------------------------------------------------------------------------
// Memory map
// ---------------------------------------------------------------------------
enum : u32 {
    // The machine's real map: two megabytes of DRAM in one run - DRAM1 at
    // 0x00000000, DRAM2 at 0x00100000 - with a megabyte of VRAM above at
    // 0x00200000.
    //
    // This was halved for a long time, with VRAM moved down to 0x00100000,
    // because the correct layout made the ROM panic. It turned out not to be
    // the map at all: MADAM's memory-configuration register was writable, the
    // ROM writes zero to it during start-up, and its own sizing routine then
    // read back "no memory fitted" and gave up. See kMadamMemConfigStock.
    //
    kDramBase = 0x00000000,
    kDramSize = 2u * 1024 * 1024,

    kVramBase = 0x00200000,
    kVramSize = 1u * 1024 * 1024,

    kRomBase  = 0x03000000,         // BIOS. Also the reset vector.
    kRomSize  = 1u * 1024 * 1024,

    // 32 KiB of storage occupying a 128 KiB window.
    //
    // The ARM60 has no bus address translator, so the NVRAM's data lines sit on
    // the low byte of the word and each stored byte is addressed as a whole
    // 32-bit word - byte n lives at base + n*4. Mapping it flat instead makes
    // the machine see four times as much NVRAM as exists, with every byte in
    // the wrong place.
    kNvramBase   = 0x03140000,
    kNvramSize   = 32u * 1024,          // real storage
    kNvramStride = 4u,                  // bytes of address space per stored byte
    kNvramWindow = kNvramSize * kNvramStride,

    // SPORT: the VRAM serial port, which does fast page copies and clears.
    //
    // Found by tracing the boot ROM's memory test, which builds addresses as
    // `0x03200000 | (address >> 9)` and writes to them - the classic
    // "the address IS the command" encoding. The test fills memory with an
    // arithmetic pattern, waits for video lines 10..13, drives SPORT, then
    // reads back and compares; the compare fails here because nothing is
    // mapped at this address, so the writes go nowhere.
    //
    // Not implemented: the page size and exactly which operation a given
    // address selects are not yet established. The region is mapped so that
    // accesses are counted and visible rather than vanishing silently, which
    // is what made this hard to find in the first place.
    kSportBase = 0x03200000,
    // SPORT decodes only the windows it actually has: the copy window at
    // +0x0000, the fill-value register at +0x2000, and the fill window at
    // +0x4000. Claiming the whole megabyte the region is listed as means every
    // stray access anywhere in it is treated as a page copy - and the machine
    // does make them, repeatedly, near the top of the region. Under the real
    // memory map that turns into 142 million page copies and looks like a hang.
    kSportSize = 0x00006000,

    kMadamBase = 0x03300000,        // confirmed by the boot ROM
    kMadamSize = 0x00100000,

    kClioBase  = 0x03400000,        // TODO(map): confirm
    kClioSize  = 0x00100000,
};

// A write that lands in memory the CPU might later execute has to invalidate
// the decode cache. Rather than have the bus know about the CPU, it records
// that something happened and the console asks.
// Which pages of memory have been written since the CPU's decode cache was last
// brought up to date.
//
// This used to be a single low/high range, which is wrong in a way that only
// shows up as slowness: two writes at opposite ends of memory in the same slice
// make the range cover everything between them, and the whole span then gets
// invalidated page by page. The OS legitimately writes across the whole of DRAM,
// so the range was usually enormous, and the cost scaled with how much memory
// the machine had rather than with how much was actually written.
//
// A set of dirty pages costs the same to record and makes invalidation
// proportional to what was really touched.
struct WriteWatch {
    static constexpr u32 kPageBytes = 4096;
    static constexpr u32 kPages     = (2u * 1024 * 1024) / kPageBytes;
    static constexpr u32 kWords     = (kPages + 63u) / 64u;

    bool dirty = false;
    u64  bits[kWords] = {};

    void note(u32 address, u32 length) {
        if (length == 0) return;
        const u32 first = address / kPageBytes;
        const u32 last  = (address + length - 1) / kPageBytes;
        for (u32 page = first; page <= last && page < kPages; ++page) {
            bits[page >> 6] |= (u64{1} << (page & 63));
        }
        dirty = true;
    }

    bool is_dirty(u32 page) const {
        return page < kPages && (bits[page >> 6] >> (page & 63) & 1) != 0;
    }

    void clear() {
        dirty = false;
        for (u64& word : bits) word = 0;
    }
};

class Bus {
public:
    Bus();

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    void reset();

    void attach_madam(Madam* madam) { madam_ = madam; }
    void attach_clio(Clio* clio) { clio_ = clio; }
    void attach_sport(Sport* sport) { sport_ = sport; }

    // Load a BIOS image. Returns false if the image is missing or too large.
    bool load_bios(const u8* data, size_t size);
    bool bios_loaded() const { return bios_loaded_; }

    // --- CPU-visible access ----------------------------------------------
    // The 3DO is big-endian as the CPU sees it; these do the byte order.
    u32  read32(u32 address);
    u16  read16(u32 address);
    u8   read8(u32 address);

    void write32(u32 address, u32 value);
    void write16(u32 address, u16 value);
    void write8(u32 address, u8 value);

    // Instruction fetch. Separate from read32 because it is the hottest path
    // in the machine and because a prefetch abort is a different exception
    // from a data abort.
    u32 fetch32(u32 address);

    // --- direct access for the chips --------------------------------------
    // MADAM and the VDLP walk VRAM far too often to pay bus dispatch for it.
    u8* dram() { return dram_.data(); }
    u8* vram() { return vram_.data(); }
    const u8* dram() const { return dram_.data(); }
    const u8* vram() const { return vram_.data(); }

    WriteWatch& write_watch() { return write_watch_; }

    // True for any address that belongs to a chip rather than to memory. Byte
    // and halfword accesses to those go through a read-modify-write of the
    // containing word, because the registers are word-wide.
    static bool is_device(u32 address) {
        return (address >= kSportBase && address < kSportBase + kSportSize) ||
               (address >= kMadamBase && address < kMadamBase + kMadamSize) ||
               (address >= kClioBase && address < kClioBase + kClioSize);
    }

    // Accesses to regions that are recognised but not implemented. A silent
    // drop is the hardest kind of gap to find, so they are counted.
    u64 sport_accesses() const { return sport_accesses_; }

private:
    std::vector<u8> dram_;
    std::vector<u8> vram_;
    std::vector<u8> rom_;
    std::vector<u8> nvram_;

    bool bios_loaded_ = false;

    Madam* madam_ = nullptr;
    Clio*  clio_  = nullptr;
    Sport* sport_ = nullptr;

    WriteWatch write_watch_;
    u64 sport_accesses_ = 0;
};

}  // namespace retro3do
