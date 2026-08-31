// SPORT — the VRAM serial port.
//
// SPORT copies a whole page of memory in one operation, without the CPU moving
// the words. It is how the machine clears the screen and moves large blocks
// quickly, and the boot ROM's memory test uses it, so nothing boots without it.
//
// How it is driven, and how that was established
// ----------------------------------------------
// There is no command register. The *address* is the command: a page is named
// by `0x03200000 | (byte_address >> 9)`, a read from that address latches the
// source page, and a write to another such address copies into the destination
// page. The value written is a bit mask saying which bits to copy.
//
// All of that comes from the boot ROM's own use of it, which is worth writing
// down because it is the evidence:
//
//   ORR r1, r2, r1, LSR #9      ; r2 = 0x03200000  -> source page address
//   ORR r4, r5, r4, LSR #9      ;                  -> destination page address
//   ...
//   LDR r3, [r1]                ; prime: latch the source
//   STR r6, [r4]                ; commit: copy into the destination, mask in r6
//
// with a literal pool holding source `0x00056800`, destination `0x000B3000` and
// mask `0xFFFFFFFF`. Both addresses are 2048-byte aligned, and the test then
// reads back and compares exactly 512 words — 2048 bytes — which is what fixes
// the page size. The `>> 9` in the address is finer than the page it moves.
//
// The three windows, and what each does — all read off the ROM's own use:
//
//   +0x0000 + page   copy. A READ latches the source page; a WRITE copies the
//                    latched page into this one, under the mask written.
//   +0x2000          the fill value. A single register, not paged.
//   +0x4000 + page   fill. A WRITE fills this page with the fill value, under
//                    the mask written.
//
// The second memory test is what shows the fill path, and it is the reason a
// copy-only implementation is not enough:
//
//   LDR r1, =0x03202000        ; the fill-value register
//   LDR r2, =0x66676869
//   STR r2, [r1]               ; set the value
//   ORR r1, 0x03204000, r1, LSR #9   ; the page to fill
//   STR r2, [r1]               ; r2 = 0xFFFFFFFF, the mask -> do it
//
// A page index is the VRAM offset shifted right by nine, so `page << 9` is a
// VRAM-relative byte offset. That "VRAM-relative" is load-bearing and was the
// thing that took longest to see: the ROM builds these addresses from bare
// literals with no memory base added, while filling and verifying at
// `r0 + literal`. The two only agree if SPORT's addresses are offsets into
// VRAM rather than absolute.

#pragma once

#include "types.h"

namespace retro3do {

class Bus;

// A SPORT page. Established from the ROM's memory test, which verifies exactly
// this many bytes after a copy.
constexpr u32 kSportPageBytes = 2048;

// The address shift the hardware applies. Note this is NOT the page size: the
// address is finer-grained than the unit that gets moved.
constexpr u32 kSportAddressShift = 9;

// Window offsets within the SPORT region.
constexpr u32 kSportCopyWindow = 0x0000;
constexpr u32 kSportValueReg   = 0x2000;
constexpr u32 kSportFillWindow = 0x4000;
constexpr u32 kSportWindowSpan = 0x2000;

// Which window an offset names is decided by three bits of it, not by a range.
// Everything the value and fill windows do not claim is a copy.
constexpr u32 kSportWindowMask = 0xe000;

// The page index wraps within the VRAM the SPORT can reach rather than running
// off the end of it.
constexpr u32 kSportIndexMask = 0x7ff;

// A mask of all ones protects nothing, so it is the same as an unmasked copy.
constexpr u32 kSportNoMask = 0xffffffffu;

class Sport {
public:
    explicit Sport(Bus& bus) : bus_(bus) {}

    void reset();

    // A read in the copy window latches the source page.
    u32 read(u32 offset);

    // A write does whatever the window says: set the fill value, copy a page,
    // or fill a page. `value` is the mask for copy and fill.
    void write(u32 offset, u32 value);

    u64 copies() const { return copies_; }
    u64 fills() const { return fills_; }
    u32 latched_source() const { return source_address_; }
    u32 fill_value() const { return fill_value_; }

private:
    static u32 page_address(u32 offset);
    static u32 apply_mask(u32 destination, u32 source, u32 mask);
    void copy_page(u32 destination, u32 mask);
    void fill_page(u32 destination, u32 mask);

    Bus& bus_;
    u32 source_address_ = 0;
    u32 fill_value_ = 0;
    u64 copies_ = 0;
    u64 fills_ = 0;
};

}  // namespace retro3do
