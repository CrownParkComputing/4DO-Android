#include "sport.h"

#include "bus.h"

namespace retro3do {

void Sport::reset() {
    source_address_ = 0;
    fill_value_ = 0;
    copies_ = 0;
    fills_ = 0;
}

u32 Sport::read(u32 offset) {
    // Only the copy window latches. A read anywhere else is not something the
    // ROM does, and inventing a meaning for it would be guessing.
    if (offset < kSportValueReg) {
        source_address_ = kVramBase + (offset << kSportAddressShift);
    }
    return 0;
}

void Sport::write(u32 offset, u32 value) {
    if (offset < kSportValueReg) {
        copy_page(kVramBase + (offset << kSportAddressShift), value);
        return;
    }
    if (offset < kSportFillWindow) {
        // The fill-value register. Paged addressing does not apply here: the
        // ROM writes to exactly 0x03202000.
        fill_value_ = value;
        return;
    }
    fill_page(kVramBase + ((offset - kSportFillWindow) << kSportAddressShift),
              value);
}

void Sport::copy_page(u32 destination, u32 mask) {
    ++copies_;
    if (destination == source_address_) {
        return;
    }

    // Word at a time through the bus, so this needs no knowledge of the memory
    // map and the write-watch sees it - a page copied into memory may be code.
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        const u32 source_word = bus_.read32(source_address_ + i);
        if (mask == 0xffffffffu) {
            bus_.write32(destination + i, source_word);
        } else {
            // The mask selects which bits move; the rest of the destination is
            // left alone, which is what makes SPORT useful for writing one
            // bitplane without disturbing the others.
            const u32 existing = bus_.read32(destination + i);
            bus_.write32(destination + i, (existing & ~mask) | (source_word & mask));
        }
    }
}

void Sport::fill_page(u32 destination, u32 mask) {
    ++fills_;
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        if (mask == 0xffffffffu) {
            bus_.write32(destination + i, fill_value_);
        } else {
            const u32 existing = bus_.read32(destination + i);
            bus_.write32(destination + i, (existing & ~mask) | (fill_value_ & mask));
        }
    }
}

}  // namespace retro3do
