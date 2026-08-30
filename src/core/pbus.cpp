#include "pbus.h"

namespace retro3do {
namespace {

// Bit positions within the pad's two report bytes. They are not in any obvious
// order - the wire layout is what it is - so they are named rather than
// computed.
enum : unsigned {
    kShiftA  = 0,
    kShiftL  = 1,
    kShiftR  = 2,
    kShiftU  = 3,
    kShiftD  = 4,

    kShiftLt = 2,
    kShiftRt = 3,
    kShiftX  = 4,
    kShiftP  = 5,
    kShiftC  = 6,
    kShiftB  = 7,
};

u8 bit(bool set, unsigned shift) {
    return static_cast<u8>(set ? (1u << shift) : 0u);
}

}  // namespace

void Pbus::add_joypad(const Joypad& pad) {
    if (data_.size() + 2 > kCapacity) {
        return;
    }
    data_.push_back(static_cast<u8>(kJoypadId |
                                    bit(pad.down, kShiftD) |
                                    bit(pad.up, kShiftU) |
                                    bit(pad.right, kShiftR) |
                                    bit(pad.left, kShiftL) |
                                    bit(pad.a, kShiftA)));
    data_.push_back(static_cast<u8>(bit(pad.b, kShiftB) |
                                    bit(pad.c, kShiftC) |
                                    bit(pad.play, kShiftP) |
                                    bit(pad.stop, kShiftX) |
                                    bit(pad.right_shift, kShiftRt) |
                                    bit(pad.left_shift, kShiftLt)));
}

void Pbus::pad() {
    for (size_t i = 0; i < kPadBytes && data_.size() < kCapacity; ++i) {
        data_.push_back(0xff);
    }
}

}  // namespace retro3do
