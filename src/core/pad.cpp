#include "pad.h"

namespace retro3do {

void PadState::reset() {
    for (u32 i = 0; i < kMaxPads; ++i) {
        buttons_[i].store(0, std::memory_order_relaxed);
    }
    connected_.store(1, std::memory_order_relaxed);
}

void PadState::set_connected(u32 pad, bool connected) {
    if (pad >= kMaxPads) return;
    u32 mask = connected_.load(std::memory_order_relaxed);
    if (connected) {
        mask |= (1u << pad);
    } else {
        mask &= ~(1u << pad);
        buttons_[pad].store(0, std::memory_order_relaxed);
    }
    connected_.store(mask, std::memory_order_relaxed);
}

bool PadState::connected(u32 pad) const {
    if (pad >= kMaxPads) return false;
    return (connected_.load(std::memory_order_relaxed) & (1u << pad)) != 0;
}

u32 PadState::connected_count() const {
    u32 mask = connected_.load(std::memory_order_relaxed);
    u32 count = 0;
    while (mask != 0) {
        count += mask & 1u;
        mask >>= 1;
    }
    return count;
}

void PadState::press(u32 pad, PadButton button, bool down) {
    if (pad >= kMaxPads) return;
    const u32 bit = 1u << static_cast<u32>(button);

    // Read-modify-write rather than a plain store: the host may report several
    // buttons from different sources (keyboard and gamepad both mapped to pad
    // one) and a store would lose whichever arrived first.
    u32 current = buttons_[pad].load(std::memory_order_relaxed);
    u32 updated;
    do {
        updated = down ? (current | bit) : (current & ~bit);
    } while (!buttons_[pad].compare_exchange_weak(current, updated,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed));
}

bool PadState::pressed(u32 pad, PadButton button) const {
    if (pad >= kMaxPads) return false;
    return (buttons_[pad].load(std::memory_order_acquire) &
            (1u << static_cast<u32>(button))) != 0;
}

u32 PadState::buttons(u32 pad) const {
    if (pad >= kMaxPads) return 0;
    return buttons_[pad].load(std::memory_order_acquire);
}

}  // namespace retro3do
