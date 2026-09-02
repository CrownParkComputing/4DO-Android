#include "sport.h"

#include "bus.h"

namespace retro3do {

// What a SPORT mask bit means.
//
// For a masked operation, a one preserves the destination bit and a zero takes
// the source bit. Written as two explicitly disjoint fields, this is the logic
// the BIOS's SPORT memory test establishes. 0xffffffff is the port's separate
// "unmasked transfer" command and is handled before this helper is called.
//
// Having this backwards writes the complement of the bits the title asked for.
// The structure of the image survives, because the pixels that should move
// still move - it is the ones that should have been left alone that get
// clobbered - so it looks like a decoder that is nearly working rather than a
// blitter that is inverted. Wing Commander III's video played all the way
// through its opening under a blizzard of coloured speckle because of this.
u32 Sport::apply_mask(u32 destination, u32 source, u32 mask) {
    return (destination & mask) | (source & ~mask);
}

void Sport::reset() {
    source_address_ = 0;
    fill_value_ = 0;
    copies_ = 0;
    fills_ = 0;
}

// The page an offset selects. The index wraps within the VRAM the SPORT can
// reach rather than running off the end of it, and the step between pages is
// a quarter of the page - consecutive indices name overlapping pages, which is
// how a copy can be made to shift an image sideways.
u32 Sport::page_address(u32 offset) {
    return kVramBase + ((offset & kSportIndexMask) << kSportAddressShift);
}

u32 Sport::read(u32 offset) {
    // A read latches where the next copy comes FROM. Only the copy window does
    // that; a read anywhere else is not something the ROM does, and inventing
    // a meaning for it would be guessing.
    if ((offset & kSportWindowMask) == kSportCopyWindow) {
        source_address_ = page_address(offset);
    }
    return 0;
}

void Sport::write(u32 offset, u32 value) {
    // The window is selected by three bits of the offset, not by a range: the
    // copy window is the one that catches everything the other two do not.
    switch (offset & kSportWindowMask) {
        case kSportValueReg:
            // The fill-value register. Paged addressing does not apply.
            fill_value_ = value;
            return;
        case kSportFillWindow:
            fill_page(page_address(offset), value);
            return;
        default:
            copy_page(page_address(offset), value);
            return;
    }
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
        if (mask == kSportNoMask) {
            bus_.write32(destination + i, source_word);
        } else {
            const u32 existing = bus_.read32(destination + i);
            bus_.write32(destination + i, apply_mask(existing, source_word, mask));
        }
    }
}

void Sport::fill_page(u32 destination, u32 mask) {
    ++fills_;
    for (u32 i = 0; i < kSportPageBytes; i += 4) {
        if (mask == kSportNoMask) {
            bus_.write32(destination + i, fill_value_);
        } else {
            const u32 existing = bus_.read32(destination + i);
            bus_.write32(destination + i, apply_mask(existing, fill_value_, mask));
        }
    }
}

}  // namespace retro3do
