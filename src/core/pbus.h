// PBUS - the player bus, which is where the controllers live.
//
// It is not a bus the CPU talks to directly. Software builds no request and
// reads no port: it points MADAM's PBUS channel at a buffer, sets a transfer
// going, and the hardware writes the state of every attached device into
// memory and raises an interrupt when it has finished.
//
// That interrupt is the point. The OS's input task blocks on it every frame,
// so a machine with no PBUS does not merely lack controllers - it has a task
// that never wakes, and anything waiting on input behind it stops with it.
//
// Devices report themselves as a stream of bytes, each opening with an
// identifying byte, and the stream is terminated by padding of 0xFF. A machine
// with nothing plugged in therefore transfers padding alone, which is a
// perfectly good answer and still completes.
#pragma once

#include <cstddef>
#include <vector>

#include "types.h"

namespace retro3do {

// The standard control pad. Every field is a single bit as far as the wire is
// concerned.
struct Joypad {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool a = false;
    bool b = false;
    bool c = false;
    bool play = false;      // the button marked P
    bool stop = false;      // the button marked X
    bool left_shift = false;
    bool right_shift = false;
};

class Pbus {
public:
    // Device identifiers, sent as the first byte of a device's report.
    enum : u8 {
        kJoypadId = 0x80,
    };

    // Start a new report. Called once per transfer, before devices are added.
    void begin() { data_.clear(); }

    void add_joypad(const Joypad& pad);

    // Close the report. Padding marks the end of the device chain, and a
    // report consisting of nothing else is how "no controllers" is expressed.
    void pad();

    const u8* data() const { return data_.data(); }
    size_t size() const { return data_.size(); }

private:
    static constexpr size_t kCapacity = 256;
    static constexpr size_t kPadBytes = 8;
    std::vector<u8> data_;
};

}  // namespace retro3do
