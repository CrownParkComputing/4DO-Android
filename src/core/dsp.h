// DSPP - the 3DO's audio digital signal processor.
//
// It is a 16-bit fixed-point machine with its own instruction set, its own two
// memories, and its own idea of when it is running. The CPU loads a program
// into it, points its DMA channels at buffers, and then leaves it alone; the
// DSPP runs one complete pass of that program per audio sample, mixes and
// filters whatever the channels hand it, writes two words to the DACs and goes
// back to sleep until the next sample.
//
// The reason a machine without one does not merely run silent
// -----------------------------------------------------------
// The OS's audio folio drives the frame clock everything else is sequenced
// against, and the DSPP is what interrupts it. A title with no DSPP plays its
// opening logo and then stops, with the CPU busy and every other subsystem
// healthy - which is a remarkably convincing way to look broken.
//
// Shape of the machine
// --------------------
//   NMem   1024 words of program. Filled with "sleep" at reset, so a DSPP that
//          has been started but never loaded halts immediately instead of
//          running whatever was left in memory.
//   IMem   1024 words of data, with the top of the range mapped to hardware:
//          the DACs, the semaphore pair, the interrupt register, and windows
//          onto the thirteen input and four output DMA channels.
//   PC     ten bits. A pass starts at zero and ends at the first SLEEP.
//
// Instructions are one word. The top bit separates arithmetic from control;
// arithmetic instructions are followed by operand words that say where their
// inputs come from and where the result goes.
#pragma once

#include <array>

#include "types.h"

namespace retro3do {

// What the DSPP needs from the rest of the machine. The DMA channels belong to
// MADAM and the interrupt belongs to CLIO, so neither lives here.
class DspHost {
public:
    virtual ~DspHost() = default;

    // Take the next word from an input channel, advancing it. Refills from the
    // channel's reload registers when it runs out, and interrupts if it
    // cannot.
    virtual u16 dsp_input_next(u16 channel) = 0;

    // The word an input channel is currently pointing at, without advancing.
    virtual u16 dsp_input_peek(u16 channel) = 0;

    virtual u16 dsp_input_status(u16 channel) = 0;
    virtual u16 dsp_output_status(u16 channel) = 0;
    virtual void dsp_output(u16 channel, u16 value) = 0;

    // The DSPP has asked for the audio interrupt.
    virtual void dsp_audio_interrupt() = 0;
};

class Dsp {
public:
    Dsp();

    void set_host(DspHost* host) { host_ = host; }

    // Run one pass of the loaded program, up to its SLEEP. Returns the two DAC
    // words packed as (right << 16) | left. Does nothing unless started.
    u32 run();

    void reset();

    // Started and stopped by the CPU through CLIO. A stopped DSPP is not
    // merely quiet - it never reaches the instruction that raises the audio
    // interrupt, so the machine above it stops with it.
    void set_running(bool running) { running_ = running; }
    bool running() const { return running_; }

    // Program memory, written by the CPU two words at a time.
    void write_program(u16 address, u16 value);

    // Data memory. Only the low half is writable by the CPU, and the window
    // from 0x70 to 0x7C is special: a write there is a value handed TO the
    // DSPP, which reads it once and then falls back to the DMA channel.
    void write_data(u16 address, u16 value);
    u16  read_data(u16 address) const;

    // Bring-up diagnostics: the raw program and data words, and where the
    // last pass ended. Reading them takes no hardware path and changes nothing.
    u16 peek_program(u16 address) const { return program_[address & 0x3ff]; }
    u16 peek_raw_data(u16 address) const { return data_[address & 0x3ff]; }
    void poke_raw_data(u16 address, u16 value) { data_[address & 0x3ff] = value; }
    u32 peek_pc() const { return pc_; }

    // The semaphore the CPU and the DSPP use to agree who last wrote what.
    void  write_semaphore(u32 value);
    u32   read_semaphore() const;

private:
    // Where a register number lands in data memory, given the current register
    // map. Precomputed for all eight maps.
    static u16 register_base(u32 map, u32 reg);

    u16  read(u32 address);
    void write(u32 address, u16 value);

    struct OperandGroup {
        std::array<u16, 3> values{};
        u8 count = 0;
        u16 destination = 0;
        u16 marked_destination = 0;
    };

    u16 next_program_word();
    u16 mapped_register(u32 reg) const;
    u16 operand_value(u16 address, bool indirect);
    u16 move_source();
    OperandGroup decode_operand_group(u16 word);
    void gather_arithmetic_operands(unsigned requested);

    // A pass runs until SLEEP, so the program's own structure bounds it - but
    // a program with a loop and no SLEEP would spin for ever inside one audio
    // sample. This is that backstop.
    static constexpr u32 kMaxStepsPerPass = 1u << 20;

    // Ticks of the DSPP's own clock in one audio sample, at 25 MHz and 44.1
    // kHz. Its internal counter is expressed in these.
    static constexpr s16 kSystemTicks = 568;

    DspHost* host_ = nullptr;

    std::array<u16, 1024> program_{};
    std::array<u16, 1024> data_{};

    // Which operands an arithmetic instruction wants, precomputed for every
    // instruction word - the decode is pure and the table is small.
    struct Decoded {
        u8 requests = 0;
        u8 shift = 0;
    };
    std::array<Decoded, 0x8000> decoded_{};

    // Whether a conditional branch is taken, for every combination of branch
    // bits and flag state.
    std::array<std::array<u8, 32>, 32> branch_taken_{};

    // Register number to data address, for each of the eight register maps.
    std::array<std::array<u16, 16>, 8> register_map_{};

    // One value handed to the DSPP by the CPU, per channel, consumed once.
    std::array<bool, 16> cpu_supplied_{};

    u32 pc_ = 0;
    u32 register_base_x4_ = 0;
    u32 register_map_index_ = 0;

    u16 audio_out_status_ = 0;
    u16 semaphore_status_ = 0;
    u16 semaphore_data_ = 0;
    u16 interrupt_word_ = 0;
    s16 counter_ = kSystemTicks;
    s16 reload_ = kSystemTicks;

    u16 mult1_ = 0;
    u16 mult2_ = 0;
    u16 alu1_ = 0;
    u16 alu2_ = 0;
    s32 barrel_shift_ = 0;
    u16 operand_mask_ = 0xffff;
    u16 writeback_ = 0;
    u8  requests_ = 0;

    bool running_ = false;
    bool generate_interrupt_ = false;

    u32 noise_state_ = 0xdeadbeefu;
    u16 noise();
};

}  // namespace retro3do
