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
    kDramBase = 0x00000000,
    kDramSize = 1u * 1024 * 1024,   // EXPERIMENT

    kVramBase = 0x00100000,
    kVramSize = 1u * 1024 * 1024,

    kRomBase  = 0x03000000,         // BIOS. Also the reset vector.
    kRomSize  = 1u * 1024 * 1024,

    kNvramBase = 0x03140000,        // TODO(map): confirm base and stride
    kNvramSize = 32u * 1024,

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
    kSportSize = 0x00100000,

    kMadamBase = 0x03300000,        // confirmed by the boot ROM
    kMadamSize = 0x00100000,

    kClioBase  = 0x03400000,        // TODO(map): confirm
    kClioSize  = 0x00100000,
};

// A write that lands in memory the CPU might later execute has to invalidate
// the decode cache. Rather than have the bus know about the CPU, it records
// that something happened and the console asks.
struct WriteWatch {
    bool     dirty = false;
    u32      low   = 0;
    u32      high  = 0;

    void note(u32 address, u32 length) {
        const u32 end = address + length;
        if (!dirty) {
            dirty = true;
            low   = address;
            high  = end;
            return;
        }
        if (address < low) low = address;
        if (end > high)    high = end;
    }

    void clear() { dirty = false; low = 0; high = 0; }
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
