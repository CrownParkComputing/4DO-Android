#include <cstdlib>
#include "console.h"

#include <cstdio>

#include "demo_rom.h"

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
    clio_.attach_dsp(&dsp_);
    dsp_.set_host(this);

    // Enabling a channel is CLIO's register but the channel is MADAM's.
    clio_.set_channel_handler(
        [](void* context, u32 enable_mask, u32 clear_mask) {
            Console* console = static_cast<Console*>(context);
            console->madam_.set_dma_channel_enable(enable_mask);
            for (u32 channel = 0; channel < 13; ++channel) {
                if ((clear_mask & (1u << channel)) != 0) {
                    console->madam_.clear_fifo(channel, false);
                }
            }
            for (u32 channel = 0; channel < 4; ++channel) {
                if ((clear_mask & (1u << (channel + 16))) != 0) {
                    console->madam_.clear_fifo(channel, true);
                }
            }
        },
        this);

    // A channel that runs dry interrupts, and input and output channels use
    // different bits for it.
    madam_.set_fifo_done_handler(
        [](void* context, u32 channel, bool output) {
            Console* console = static_cast<Console*>(context);
            console->clio_.raise(1u << (output ? (channel + 12) : (channel + 16)));
        },
        this);
    // The expansion transfer runs inside the store that triggers it.
    clio_.set_xbus_dma_handler(
        [](void* context) {
            static_cast<Console*>(context)->service_expansion_dma();
        },
        this);
    clio_.set_scanline_handler(
        [](void* context, u32 line) {
            static_cast<Console*>(context)->vdlp_.process_scanline(line);
        },
        this);
    madam_.set_pbus_handler(
        [](void* context) {
            static_cast<Console*>(context)->service_pbus_dma();
        },
        this);
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
    demo_loaded_ = false;
    cpu_.invalidate_decode_cache();
    last_error_.clear();
    return true;
}

bool Console::load_builtin_demo() {
    const DemoRom& rom = builtin_demo_rom();
    if (!bus_.load_bios(rom.data, rom.size)) {
        last_error_ = "The built-in demonstration ROM could not be loaded";
        demo_loaded_ = false;
        return false;
    }
    demo_loaded_ = true;
    cpu_.invalidate_decode_cache();
    last_error_.clear();
    return true;
}

bool Console::load_disc(const std::string& path) {
    // A disc replacement is an eject/insert transition, not a machine reset.
    // Software waiting at a "please insert disc" prompt observes the same
    // media-change interrupt that physical hardware raises.
    if (disc_.is_open()) clio_.cdrom().set_disc_present(false);
    if (!disc_.open(path)) {
        clio_.cdrom().attach_disc(nullptr);
        last_error_ = disc_.last_error();
        return false;
    }
    // Tell the drive. Opening the image and reporting an empty tray to the
    // machine are two different things, and only the second one is visible to
    // the software running on it.
    clio_.cdrom().attach_disc(&disc_);
    clio_.cdrom().set_disc_present(true);
    last_error_.clear();
    return true;
}

bool Console::load_disc_fd(int fd, const std::string& display_name) {
    if (disc_.is_open()) clio_.cdrom().set_disc_present(false);
    if (!disc_.open_fd(fd, display_name)) {
        clio_.cdrom().attach_disc(nullptr);
        last_error_ = disc_.last_error();
        return false;
    }
    clio_.cdrom().attach_disc(&disc_);
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

    demo_loaded_ = false;
    cpu_.invalidate_decode_cache();
    last_error_.clear();
    return true;
}

void Console::eject_disc() {
    disc_.close();
    clio_.cdrom().attach_disc(nullptr);
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
    // Where the visible window sits inside the field. The display list runs
    // from well before it, and the framebuffer address advances the whole
    // time, so the picture that reaches the screen starts some way into the
    // buffer rather than at the top of it.
    vdlp_.set_field_shape(21, region == Region::Pal ? 312u : 262u);
    // CLIO's display generator always turns the logical 320-wide framebuffer
    // into a 640-wide video signal (and doubles the height likewise), either
    // by interpolation or by pixel replication.
    framebuffer_.assign(static_cast<size_t>(frame_width_ * 2) *
                            (frame_height_ * 2),
                        0xff000000u);
}

void Console::reset() {
    bus_.reset();
    cpu_.reset();
    clio_.reset();
    vdlp_.reset();
    madam_.reset();
    // Region is front-end configuration, not volatile chip state. MADAM's
    // hardware reset restores its own NTSC-sized clip defaults, so put the
    // selected machine geometry back afterwards (notably 288-line PAL).
    madam_.set_clip(static_cast<u32>(frame_width_),
                    static_cast<u32>(frame_height_));
    madam_.set_target(kVramBase, static_cast<u32>(frame_width_) * 2u);
    sport_.reset();
    pads_.reset();
    audio_.reset();
    sample_accumulator_ = 0;
    last_sample_ = 0;
    samples_.clear();
    expansion_dma_count_ = 0;
    frame_count_ = 0;
    std::fill(framebuffer_.begin(), framebuffer_.end(), 0xff000000u);
}

void Console::apply_write_watch() {
    WriteWatch& watch = bus_.write_watch();
    if (!watch.dirty) {
        return;
    }
    for (u32 page = 0; page < WriteWatch::kPages; ++page) {
        if (watch.is_dirty(page)) {
            cpu_.invalidate_decode_cache(page * WriteWatch::kPageBytes,
                                         WriteWatch::kPageBytes);
        }
    }
    watch.clear();
}

// Move a sector from the drive into memory.
//
// The CPU never reads the drive's data port - across a whole disc mount it
// reads it exactly zero times. It programmes MADAM's expansion-bus DMA with an
// address and a length and then waits for the transfer-complete interrupt, so
// without this the machine issues a perfectly good READ and then waits forever
// for data that has nowhere to go.
// Write the state of every attached controller into memory and say so.
//
// The length register counts DOWN and holds bytes-minus-four, so a negative
// value means there is no buffer to fill and the transfer is skipped entirely
// rather than run with a wrapped count.
//
// The first word of the buffer is stepped over without being written. That is
// not an off-by-one: the transfer begins by advancing past it, and software
// lays its buffer out expecting that.
// Run the DSP, once per audio sample.
//
// One pass of its program is one sample: it reads whatever its DMA channels
// hand it, mixes and filters, writes two words to the DACs and sleeps. The
// audio interrupt comes out of the program itself rather than from a timer -
// a title decides when it wants to be woken, and titles differ.
void Console::tick_dsp(u32 cycles) {
    sample_accumulator_ += cycles;
    while (sample_accumulator_ >= kSamplePeriod) {
        sample_accumulator_ -= kSamplePeriod;
        last_sample_ = dsp_.run();

        // The low half is the left channel and the high half the right, both
        // signed. A pass that wrote nothing leaves the previous pair, which is
        // what the hardware's DACs hold too.
        StereoSample pair;
        pair.left  = static_cast<s16>(last_sample_ & 0xffffu);
        pair.right = static_cast<s16>(last_sample_ >> 16);
        if (samples_.size() < kMaxSamplesPerFrame) {
            samples_.push_back(pair);
        }
    }
}

// One pad's buttons, in the shape the PBUS wants them.
void Console::set_joypad(const Joypad& pad) {
    const auto hold = [this](PadButton button, bool down) {
        pads_.press(0, button, down);
    };
    hold(PadButton::Up, pad.up);
    hold(PadButton::Down, pad.down);
    hold(PadButton::Left, pad.left);
    hold(PadButton::Right, pad.right);
    hold(PadButton::A, pad.a);
    hold(PadButton::B, pad.b);
    hold(PadButton::C, pad.c);
    hold(PadButton::Play, pad.play);
    hold(PadButton::Stop, pad.stop);
    hold(PadButton::LeftShift, pad.left_shift);
    hold(PadButton::RightShift, pad.right_shift);
}

Joypad Console::joypad_for(u32 slot) const {
    const u32 held = pads_.buttons(slot);
    const auto down = [held](PadButton button) {
        return (held & (1u << static_cast<u32>(button))) != 0;
    };
    Joypad pad;
    pad.up          = down(PadButton::Up);
    pad.down        = down(PadButton::Down);
    pad.left        = down(PadButton::Left);
    pad.right       = down(PadButton::Right);
    pad.a           = down(PadButton::A);
    pad.b           = down(PadButton::B);
    pad.c           = down(PadButton::C);
    pad.play        = down(PadButton::Play);
    pad.stop        = down(PadButton::Stop);
    pad.left_shift  = down(PadButton::LeftShift);
    pad.right_shift = down(PadButton::RightShift);
    return pad;
}

void Console::service_pbus_dma() {
    if (static_cast<s32>(madam_.pbus_length()) < 0) {
        return;
    }

    pbus_.begin();
    // Every attached pad, in order, onto the one serial stream. This is where
    // the host's buttons finally reach the machine, and for a long time it was
    // not: the front end wrote into PadState and this read a separate Joypad
    // that only the headless harness ever set, so a game saw nothing at all
    // from the on-screen controls or from a controller. There is one source of
    // truth now, and it is PadState.
    const u32 attached = pads_.connected_count();
    for (u32 slot = 0; slot < attached; ++slot) {
        pbus_.add_joypad(joypad_for(slot));
    }
    pbus_.pad();

    u32 address = madam_.pbus_address();
    s32 remaining = static_cast<s32>(madam_.pbus_length());

    remaining -= 4;
    address += 4;
    madam_.advance_pbus_pointer();

    const u8* source = pbus_.data();
    s32 available = static_cast<s32>(pbus_.size());
    while (remaining > 0 && available > 0) {
        // Four bytes, most significant first - the order they were reported
        // in, not the order the host happens to store words in.
        const u32 word = (static_cast<u32>(source[0]) << 24) |
                         (static_cast<u32>(source[1]) << 16) |
                         (static_cast<u32>(source[2]) << 8) |
                          static_cast<u32>(source[3]);
        bus_.write32(address, word);
        source += 4;
        available -= 4;
        remaining -= 4;
        address += 4;
        madam_.advance_pbus_pointer();
    }

    // Anything the devices did not fill reads as absent.
    while (remaining > 0) {
        bus_.write32(address, 0xffffffffu);
        remaining -= 4;
        address += 4;
        madam_.advance_pbus_pointer();
    }

    madam_.set_pbus_address(address);
    madam_.set_pbus_length(0xfffffffcu);
    madam_.finish_pbus();
    clio_.raise_secondary(kIrq1PbusComplete);
}

void Console::service_expansion_dma() {
    if (!clio_.xbus_dma_requested()) {
        return;
    }
    clio_.clear_xbus_dma_request();

    u32 address = madam_.xbus_dma_address();
    // The length register holds bytes-minus-four and the loop runs while it is
    // non-negative, so a transfer moves length + 4 bytes.
    const s32 length = static_cast<s32>(madam_.read(kMadamXbusDmaLength));

    for (s32 left = length; left >= 0; left -= 4) {
        // Each word is assembled from four bytes taken MOST significant first,
        // then stored in ascending address order. Reading them straight through
        // instead reverses every word - which does not fail, it quietly
        // produces a sector of plausible-looking rubbish.
        const u8 b3 = clio_.cdrom().read_data();
        const u8 b2 = clio_.cdrom().read_data();
        const u8 b1 = clio_.cdrom().read_data();
        const u8 b0 = clio_.cdrom().read_data();
        bus_.write8(address + 0, b3);
        bus_.write8(address + 1, b2);
        bus_.write8(address + 2, b1);
        bus_.write8(address + 3, b0);
        address += 4;
    }

    // The drive reports the transfer finished by parking the length and raising
    // the completion interrupt the OS enabled at start-up.
    madam_.write(kMadamXbusDmaLength, 0xfffffffcu);
    madam_.clear_xbus_dma();
    clio_.set_xbus_ready(true);
    cpu_.invalidate_decode_cache(madam_.xbus_dma_address(), u32(length) + 4u);
    ++expansion_dma_count_;
    clio_.raise(kIrqXbusDmaComplete);
}

u32 Console::run_frame() {
    const u32 budget = cycles_per_frame(region_);
    const u32 cpu_scale = cpu_scale_percent_.load(std::memory_order_acquire);

    // The VDL root is latched at the field boundary. Each subsequent line is
    // sampled by the CLIO scanline callback during horizontal blank, matching
    // the order documented by the official 3DO Graphics Programming Guide.
    vdlp_.set_list_address(madam_.vdl_address());
    vdlp_.begin_field(frame_width_, frame_height_);

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
    u32 cpu_spent = 0;
    if (cpu_scale == 100u) {
        // Keep the hardware-accurate path exactly as it was. In particular,
        // peripherals see the few cycles by which the last ARM instruction in
        // a slice overshoots its target.
        while (cpu_spent < budget) {
            const u32 slice = (budget - cpu_spent) < kSliceCycles
                                  ? (budget - cpu_spent)
                                  : kSliceCycles;
            const u32 ran = cpu_.run(slice);
            cpu_spent += ran;
            clio_.tick(ran);
            tick_dsp(ran);
            apply_write_watch();

            if (clio_.field_complete()) {
                clio_.clear_field_complete();
                break;
            }
        }
    } else {
        // CPU boost is deliberately a second clock domain. `hardware_spent`
        // advances video, timers and the DSP at the stock 12.5 MHz cadence;
        // only the ARM receives the scaled instruction budget. Cumulative
        // targets carry rounding and instruction overshoot into the next slice
        // instead of compounding an error hundreds of times per field.
        u32 hardware_spent = 0;
        while (hardware_spent < budget) {
            const u32 slice = (budget - hardware_spent) < kSliceCycles
                                  ? (budget - hardware_spent)
                                  : kSliceCycles;
            hardware_spent += slice;
            const u32 cpu_target = static_cast<u32>(
                (static_cast<u64>(hardware_spent) * cpu_scale) / 100u);
            if (cpu_spent < cpu_target) {
                cpu_spent += cpu_.run(cpu_target - cpu_spent);
            }

            clio_.tick(slice);
            tick_dsp(slice);
            apply_write_watch();

            if (clio_.field_complete()) {
                clio_.clear_field_complete();
                break;
            }
        }
    }

    // Audio for this frame. These are the DSP's own DAC words, one pair per
    // pass of its program.
    // If it is stopped, or its program never writes them, the pairs are zero
    // and the machine is silent - which is the truth rather than a stand-in.
    if (!samples_.empty()) {
        audio_.push(samples_.data(), static_cast<u32>(samples_.size()));
        samples_.clear();
    }

    // Scanlines are already latched. The field boundary only expands the
    // logical signal to the hardware's 2x output; it does not reread VRAM.
    vdlp_.finish_field_2x(framebuffer_.data(), frame_width_, frame_height_);

    ++frame_count_;
    return cpu_spent;
}

Frame Console::framebuffer() const {
    Frame frame;
    frame.pixels = framebuffer_.data();
    frame.width  = frame_width_ * 2;
    frame.height = frame_height_ * 2;
    return frame;
}

}  // namespace retro3do
