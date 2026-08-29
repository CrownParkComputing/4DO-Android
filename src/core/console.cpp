#include "console.h"

#include <cstdio>

#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <vector>

namespace retro3do {
namespace {

// ARM60 clock. Cycles per frame follow from this and the field rate.
constexpr u32 kCpuHz = 12500000;

u32 cycles_per_frame(Region region) {
    return region == Region::Pal ? kCpuHz / 50 : kCpuHz / 60;
}

u32 cycles_per_scanline(Region region) {
    const u32 lines = region == Region::Pal ? 313u : 263u;
    const u32 per_line = cycles_per_frame(region) / lines;
    return per_line < 2 ? 2 : per_line;
}

}  // namespace

Console::Console() : cpu_(bus_), clio_(cpu_), vdlp_(bus_), madam_(bus_), sport_(bus_) {
    bus_.attach_clio(&clio_);
    bus_.attach_madam(&madam_);
    bus_.attach_sport(&sport_);
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

bool Console::load_disc(const std::string& path) {
    if (!disc_.open(path)) {
        last_error_ = disc_.last_error();
        return false;
    }
    // Tell the drive. Opening the image and reporting an empty tray to the
    // machine are two different things, and only the second one is visible to
    // the software running on it.
    clio_.cdrom().set_disc_present(true);
    last_error_.clear();
    return true;
}

bool Console::load_disc_fd(int fd, const std::string& display_name) {
    if (!disc_.open_fd(fd, display_name)) {
        last_error_ = disc_.last_error();
        return false;
    }
    clio_.cdrom().set_disc_present(true);
    last_error_.clear();
    return true;
}

bool Console::load_bios_fd(int fd, const std::string& display_name) {
    if (fd < 0) {
        last_error_ = "Could not open " + display_name;
        return false;
    }

    // fdopen adopts the descriptor, so closing the FILE* closes it too.
    std::FILE* file = ::fdopen(fd, "rb");
    if (file == nullptr) {
        last_error_ = "Could not read " + display_name;
        ::close(fd);
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (size <= 0) {
        std::fclose(file);
        last_error_ = "BIOS file is empty: " + display_name;
        return false;
    }

    std::vector<u8> data(static_cast<size_t>(size));
    const size_t read = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);

    if (read != data.size()) {
        last_error_ = "BIOS file could not be read in full: " + display_name;
        return false;
    }
    if (!bus_.load_bios(data.data(), data.size())) {
        last_error_ = "BIOS image is larger than the 3DO's ROM window";
        return false;
    }

    cpu_.invalidate_decode_cache();
    last_error_.clear();
    return true;
}

void Console::eject_disc() {
    disc_.close();
    clio_.cdrom().set_disc_present(false);
}

void Console::set_region(Region region) {
    region_ = region;
    frame_width_  = 320;
    frame_height_ = region == Region::Pal ? 288 : 240;
    // A field carries more lines than are visible: the rest is vertical blank,
    // which is when the machine does its display-list work.
    clio_.set_scanlines_per_field(region == Region::Pal ? 313 : 263);
    madam_.set_clip(static_cast<u32>(frame_width_),
                    static_cast<u32>(frame_height_));
    madam_.set_target(kVramBase, static_cast<u32>(frame_width_) * 2u);
    framebuffer_.assign(static_cast<size_t>(frame_width_) * frame_height_, 0xff000000u);
}

void Console::reset() {
    bus_.reset();
    cpu_.reset();
    clio_.reset();
    vdlp_.reset();
    madam_.reset();
    sport_.reset();
    pads_.reset();
    audio_.reset();
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

    // Run the CPU in slices, and keep a slice SHORTER THAN A SCANLINE.
    //
    // Two reasons. The obvious one is that self-modifying code and DMA need
    // their decode-cache invalidation applied promptly rather than only at the
    // frame boundary; the 3DO's OS does relocate code, so that is not
    // theoretical.
    //
    // The one that actually bites: the CPU and CLIO advance in alternating
    // chunks, so the CPU can only observe a line number if CLIO stops on it.
    // The boot ROM waits for *exact* line values, and with a slice several
    // lines long it simply never sees them and waits forever. Half a scanline
    // guarantees every line is observable.
    const u32 kSliceCycles = cycles_per_scanline(region_) / 2;
    u32 spent = 0;
    while (spent < budget) {
        const u32 slice = (budget - spent) < kSliceCycles ? (budget - spent)
                                                          : kSliceCycles;
        const u32 ran = cpu_.run(slice);
        spent += ran;
        clio_.tick(ran);
        apply_write_watch();

        // A field boundary ends the frame even if the cycle budget has not run
        // out. The video hardware, not the arithmetic, decides when a frame is
        // finished; running past it would drift the two apart.
        if (clio_.field_complete()) {
            clio_.clear_field_complete();
            break;
        }
    }

    // Audio for this frame. The DSP does not exist yet, so the machine is
    // silent — but the ring is filled with the right number of samples anyway,
    // so that the pacing and underrun behaviour are exercised from the start
    // rather than appearing for the first time when sound is switched on.
    {
        const u32 fields_per_second = region_ == Region::Pal ? 50u : 60u;
        const u32 samples_this_frame = kAudioSampleRate / fields_per_second;
        StereoSample silence[kAudioSampleRate / 50];
        for (u32 i = 0; i < samples_this_frame; ++i) {
            silence[i] = StereoSample{};
        }
        audio_.push(silence, samples_this_frame);
    }

    // The machine tells the display where its list is by writing a MADAM
    // register; the display reads it each field rather than being pushed to.
    vdlp_.set_list_address(madam_.vdl_address());

    // The field has ended, so draw it. This is the only place the framebuffer
    // is produced, and it happens after emulation rather than during it — the
    // console still does not present, it only fills a buffer.
    vdlp_.render_field(framebuffer_.data(), frame_width_, frame_height_);

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
