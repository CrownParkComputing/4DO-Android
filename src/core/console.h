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
#include "audio_ring.h"
#include "disc.h"
#include "clio.h"
#include "madam.h"
#include "dsp.h"
#include "pbus.h"
#include "pad.h"
#include "sport.h"
#include "vdlp.h"
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

class Console : public DspHost {
public:
    Console();
    ~Console();

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Load a BIOS image from disk. Without one the machine has nothing to run.
    bool load_bios(const std::string& path);
    bool bios_loaded() const { return bus_.bios_loaded(); }

    // Insert a disc image. The disc is opened and its table of contents read;
    // nothing is executed until the machine asks for a sector.
    bool load_disc(const std::string& path);

    // Insert a disc from an already-open descriptor, taking ownership of it.
    // Android's scoped storage hands out descriptors rather than paths;
    // `display_name` supplies the filename the descriptor does not carry.
    bool load_disc_fd(int fd, const std::string& display_name);

    // Load a BIOS from a descriptor, same reason.
    bool load_bios_fd(int fd, const std::string& display_name);
    void eject_disc();
    bool disc_loaded() const { return disc_.is_open(); }
    const Disc& disc() const { return disc_; }

    void set_region(Region region);
    Region region() const { return region_; }

    void reset();

    // Emulate one video frame. Returns the number of CPU cycles spent.
    u32 run_frame();

    // How many expansion-bus DMA transfers have been served. A bring-up
    // diagnostic: "is the disc actually being read" is otherwise invisible.
    u64 expansion_dma_count() const { return expansion_dma_count_; }

private:
    void service_expansion_dma();
    void service_pbus_dma();
    void tick_dsp(u32 cycles);

public:
    // --- DspHost ----------------------------------------------------------
    // The DSP's DMA channels are MADAM's registers and its interrupt is
    // CLIO's, so the machine wires them together rather than the DSP reaching
    // for either.
    u16  dsp_input_next(u16 channel) override { return madam_.fifo_input_next(channel); }
    u16  dsp_input_peek(u16 channel) override { return madam_.fifo_input_peek(channel); }
    u16  dsp_input_status(u16 channel) override { return madam_.fifo_input_status(channel); }
    u16  dsp_output_status(u16 channel) override { return madam_.fifo_output_status(channel); }
    void dsp_output(u16 channel, u16 value) override { madam_.fifo_output(channel, value); }
    void dsp_audio_interrupt() override { clio_.raise(kIrqAudioTimer); }

    const Dsp& dsp() const { return dsp_; }

private:

public:
    // The controller state the machine will report on its next input scan.
    void set_joypad(const Joypad& pad) { joypad_ = pad; }
    const Joypad& joypad() const { return joypad_; }

private:
    u64 expansion_dma_count_ = 0;

public:

    Frame framebuffer() const;

    Arm60& cpu() { return cpu_; }
    Bus& bus() { return bus_; }
    Clio& clio() { return clio_; }
    Vdlp& vdlp() { return vdlp_; }
    Madam& madam() { return madam_; }
    Sport& sport() { return sport_; }
    PadState& pads() { return pads_; }
    AudioRing& audio() { return audio_; }

    const std::string& last_error() const { return last_error_; }

private:
    void apply_write_watch();

    Bus   bus_;
    Arm60 cpu_;
    Clio  clio_;
    Vdlp  vdlp_;
    Madam madam_;
    Dsp dsp_;
    Pbus pbus_;
    Joypad joypad_;
    // The DSP runs one pass of its program per audio sample. At 44.1 kHz
    // that is about every 283 CPU cycles.
    static constexpr u32 kSamplePeriod = 12500000u / 44100u;
    u32 sample_accumulator_ = 0;

    // The last pair of DAC words the DSP produced, as (right << 16) | left.
    u32 last_sample_ = 0;
    Sport sport_;
    Disc  disc_;
    PadState  pads_;
    AudioRing audio_;

    Region region_ = Region::Ntsc;
    int frame_width_  = 320;
    int frame_height_ = 240;

    std::vector<u32> framebuffer_;
    std::string last_error_;

    u64 frame_count_ = 0;
};

}  // namespace retro3do
