// The machine as a whole: the parts, and the frame loop that drives them.
//
// The console never presents. It emulates a frame into `framebuffer()` and
// returns; whoever owns the window decides when to draw it. That separation is
// the whole reason this rewrite exists — the previous core called into the GPU
// from inside its emulation thread, so a slow present stalled emulation and the
// machine appeared to run slow when it was really being throttled.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "arm60.h"
#include "bus.h"
#include "types.h"

namespace retro3do {

enum class Region {
    Ntsc,   // 320x240, 60 fields per second
    Pal,    // 320x288, 50 fields per second
};

// A frame handed to the front end. Pixels are XRGB8888 in host order, ready to
// upload without further conversion.
struct Frame {
    const u32* pixels = nullptr;
    int width  = 0;
    int height = 0;
};

class Console {
public:
    Console();
    ~Console();

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Load a BIOS image from disk. Without one the machine has nothing to run.
    bool load_bios(const std::string& path);
    bool bios_loaded() const { return bus_.bios_loaded(); }

    void set_region(Region region);
    Region region() const { return region_; }

    void reset();

    // Emulate one video frame. Returns the number of CPU cycles spent.
    u32 run_frame();

    Frame framebuffer() const;

    Arm60& cpu() { return cpu_; }
    Bus& bus() { return bus_; }

    const std::string& last_error() const { return last_error_; }

private:
    void apply_write_watch();

    Bus   bus_;
    Arm60 cpu_;

    Region region_ = Region::Ntsc;
    int frame_width_  = 320;
    int frame_height_ = 240;

    std::vector<u32> framebuffer_;
    std::string last_error_;

    u64 frame_count_ = 0;
};

}  // namespace retro3do
