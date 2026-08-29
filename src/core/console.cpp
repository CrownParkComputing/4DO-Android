#include "console.h"

#include <cstdio>
#include <vector>

namespace retro3do {
namespace {

// ARM60 clock. Cycles per frame follow from this and the field rate.
constexpr u32 kCpuHz = 12500000;

u32 cycles_per_frame(Region region) {
    return region == Region::Pal ? kCpuHz / 50 : kCpuHz / 60;
}

}  // namespace

Console::Console() : cpu_(bus_) {
    set_region(Region::Ntsc);
    reset();
}

Console::~Console() = default;

bool Console::load_bios(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        last_error_ = "Could not open BIOS file: " + path;
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (size <= 0) {
        std::fclose(file);
        last_error_ = "BIOS file is empty: " + path;
        return false;
    }

    std::vector<u8> data(static_cast<size_t>(size));
    const size_t read = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);

    if (read != data.size()) {
        last_error_ = "BIOS file could not be read in full: " + path;
        return false;
    }

    if (!bus_.load_bios(data.data(), data.size())) {
        last_error_ = "BIOS image is larger than the 3DO's ROM window";
        return false;
    }

    // New code underneath the CPU: anything already decoded is stale.
    cpu_.invalidate_decode_cache();
    last_error_.clear();
    return true;
}

void Console::set_region(Region region) {
    region_ = region;
    frame_width_  = 320;
    frame_height_ = region == Region::Pal ? 288 : 240;
    framebuffer_.assign(static_cast<size_t>(frame_width_) * frame_height_, 0xff000000u);
}

void Console::reset() {
    bus_.reset();
    cpu_.reset();
    frame_count_ = 0;
    std::fill(framebuffer_.begin(), framebuffer_.end(), 0xff000000u);
}

void Console::apply_write_watch() {
    WriteWatch& watch = bus_.write_watch();
    if (!watch.dirty) {
        return;
    }
    cpu_.invalidate_decode_cache(watch.low, watch.high - watch.low);
    watch.clear();
}

u32 Console::run_frame() {
    const u32 budget = cycles_per_frame(region_);

    // Run the CPU in slices so that self-modifying code and DMA get their
    // decode-cache invalidation applied promptly, rather than only at the frame
    // boundary. The 3DO's OS does relocate code, so this is not theoretical.
    constexpr u32 kSliceCycles = 4096;
    u32 spent = 0;
    while (spent < budget) {
        const u32 slice = (budget - spent) < kSliceCycles ? (budget - spent)
                                                          : kSliceCycles;
        spent += cpu_.run(slice);
        apply_write_watch();
    }

    ++frame_count_;
    return spent;
}

Frame Console::framebuffer() const {
    Frame frame;
    frame.pixels = framebuffer_.data();
    frame.width  = frame_width_;
    frame.height = frame_height_;
    return frame;
}

}  // namespace retro3do
