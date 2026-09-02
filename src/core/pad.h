// The 3DO control pad.
//
// Pads daisy-chain: the machine sees one serial stream with every connected pad
// in it, which is why a 3DO needs no multitap. Up to eight can be attached.
#pragma once

#include <atomic>

#include "types.h"

namespace retro3do {

enum class PadButton {
    Up, Down, Left, Right,
    A, B, C,
    Play,   // the "P" button
    Stop,   // the "X" button
    LeftShift,
    RightShift,
    Count,
};

constexpr u32 kMaxPads = 8;

// Button state for the attached pads. Written by whatever is reading the host's
// controllers and read by the emulation thread, so each pad's bits live in one
// atomic word: a pad is then always seen as a coherent set rather than halfway
// through an update, which is what stops a diagonal registering as a stutter.
class PadState {
public:
    void reset();

    void set_connected(u32 pad, bool connected);
    bool connected(u32 pad) const;
    u32  connected_count() const;

    void press(u32 pad, PadButton button, bool down);
    bool pressed(u32 pad, PadButton button) const;

    // The whole pad at once, as a bitmask indexed by PadButton.
    u32 buttons(u32 pad) const;

private:
    std::atomic<u32> buttons_[kMaxPads] = {};
    std::atomic<u32> connected_{1};  // one pad, as a machine ships
};

}  // namespace retro3do
