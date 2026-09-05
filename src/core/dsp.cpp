#include "dsp.h"
#include <cstdio>
#include <cstdlib>

namespace retro3do {
namespace {

// Instruction fields. Decoded with shifts rather than bitfields: the layout is
// fixed by the hardware, not by whatever the compiler decides to do with a
// packed struct.
constexpr u32 field(u32 word, unsigned shift, unsigned bits) {
    return (word >> shift) & ((1u << bits) - 1u);
}

// Arithmetic instruction.
constexpr u32 aif_bs(u32 w)     { return field(w, 0, 4); }
constexpr u32 aif_alu(u32 w)    { return field(w, 4, 4); }
constexpr u32 aif_muxb(u32 w)   { return field(w, 8, 2); }
constexpr u32 aif_muxa(u32 w)   { return field(w, 10, 2); }
constexpr bool aif_m2sel(u32 w) { return field(w, 12, 1) != 0; }
constexpr u32 aif_numops(u32 w) { return field(w, 13, 2); }
constexpr bool is_control(u32 w) { return field(w, 15, 1) != 0; }

// Control instruction: the low ten bits are an address or a small operand.
constexpr u32 cif_address(u32 w) { return field(w, 0, 10); }

// Conditional branch: five bits select the condition.
constexpr u32 branch_bits(u32 w) { return field(w, 10, 5); }

// Operand words.
constexpr u32 op_type(u32 w)    { return field(w, 13, 3); }
constexpr u32 nrof_address(u32 w) { return field(w, 0, 10); }
constexpr bool nrof_indirect(u32 w) { return field(w, 10, 1) != 0; }
constexpr bool nrof_wb1(u32 w)  { return field(w, 11, 1) != 0; }

constexpr u32 r2_r1(u32 w)      { return field(w, 0, 4); }
constexpr bool r2_r1_di(u32 w)  { return field(w, 4, 1) != 0; }
constexpr u32 r2_r2(u32 w)      { return field(w, 5, 4); }
constexpr bool r2_r2_di(u32 w)  { return field(w, 9, 1) != 0; }
constexpr bool r2_numregs(u32 w){ return field(w, 10, 1) != 0; }
constexpr bool r2_wb1(u32 w)    { return field(w, 11, 1) != 0; }
constexpr bool r2_wb2(u32 w)    { return field(w, 12, 1) != 0; }

constexpr u32 r3_r1(u32 w)      { return field(w, 0, 4); }
constexpr bool r3_r1_di(u32 w)  { return field(w, 4, 1) != 0; }
constexpr u32 r3_r2(u32 w)      { return field(w, 5, 4); }
constexpr bool r3_r2_di(u32 w)  { return field(w, 9, 1) != 0; }
constexpr u32 r3_r3(u32 w)      { return field(w, 10, 4); }
constexpr bool r3_r3_di(u32 w)  { return field(w, 14, 1) != 0; }

// An immediate is thirteen bits, signed, and is justified by either nothing or
// three bits. The justify field is one bit wide and SIGNED, so it reads as
// zero or minus one - and minus one masked to two bits is three, not one.
// Shifting by one instead of three puts every immediate a factor of four out.
s32 immediate_value(u32 word) {
    const s32 value = static_cast<s32>(field(word, 0, 13) << 19) >> 19;
    const unsigned justify = field(word, 13, 1) != 0 ? 3u : 0u;
    return value << justify;
}

// Which operand slots an arithmetic instruction is asking to be filled. The
// order of these bits is the order they are consumed in.
enum : u8 {
    kNeedShift = 1u << 0,
    kNeedAlu2  = 1u << 1,
    kNeedAlu1  = 1u << 2,
    kNeedMult2 = 1u << 3,
    kNeedMult1 = 1u << 4,
};

constexpr u32 kTopBit = 0x80000000u;

bool add_carry(u32 a, u32 b, u32 y) {
    return ((a & b & kTopBit) != 0) || ((a & ~y & kTopBit) != 0) ||
           ((b & ~y & kTopBit) != 0);
}
bool sub_carry(u32 a, u32 b, u32 y) {
    return ((a & ~b & kTopBit) != 0) || ((a & ~y & kTopBit) != 0) ||
           ((~b & ~y & kTopBit) != 0);
}
bool add_overflow(u32 a, u32 b, u32 y) {
    return ((a & b & ~y & kTopBit) != 0) || ((~a & ~b & y & kTopBit) != 0);
}
bool sub_overflow(u32 a, u32 b, u32 y) {
    return ((a & ~b & ~y & kTopBit) != 0) || ((~a & b & y & kTopBit) != 0);
}

// The status latch described in the DSPP patent. Keep the flags as flags: the
// hardware condition modes are much easier to audit in these terms than when
// encoded into host byte lanes and recovered with a multiplication trick.
struct Flags {
    bool z = false;
    bool n = false;
    bool c = false;
    bool v = false;
    void set_zero(bool value)     { z = value; }
    void set_negative(bool value) { n = value; }
    void set_carry(bool value)    { c = value; }
    void set_overflow(bool value) { v = value; }
    bool zero() const     { return z; }
    bool negative() const { return n; }
    bool carry() const    { return c; }
    bool overflow() const { return v; }
};

u32 flag_index(const Flags& flags, bool exact) {
    return (flags.zero() ? 1u : 0u) |
           (flags.negative() ? 2u : 0u) |
           (flags.carry() ? 4u : 0u) |
           (flags.overflow() ? 8u : 0u) |
           (exact ? 16u : 0u);
}

// Decode the patent's three conditional modes directly.
//
//   mode 1: every selected flag must be one
//   mode 2: every selected flag must be zero
//   mode 3: signed N/V/Z, unsigned C/Z, or exactness comparisons
//
// In modes 1/2 mask bit 1 controls status 0 (N or C) and mask bit 0
// controls status 1 (V or Z). Mode 1 with neither mask bit is the documented
// all-zero / not-all-zero pair, selected by S.
bool branch_condition(u32 condition, const Flags& flags, bool exact) {
    const bool mask0 = (condition & 0x01u) != 0;
    const bool mask1 = (condition & 0x02u) != 0;
    const bool select_cz = (condition & 0x04u) != 0;
    const u32 mode = (condition >> 3) & 3u;

    if (mode == 0) return false;

    if (mode == 1 && !mask0 && !mask1) {
        const bool all_twenty_bits_zero = flags.zero() && exact;
        return select_cz ? !all_twenty_bits_zero : all_twenty_bits_zero;
    }

    if (mode == 1 || mode == 2) {
        const bool wanted = mode == 1;
        const bool status0 = select_cz ? flags.carry() : flags.negative();
        const bool status1 = select_cz ? flags.zero() : flags.overflow();
        return (!mask1 || status0 == wanted) &&
               (!mask0 || status1 == wanted);
    }

    if (!select_cz) {
        // Signed comparisons: N xor V means less-than; mask0 includes equal,
        // and mask1 complements the result.
        return (((flags.negative() != flags.overflow()) ||
                 (flags.zero() && mask0)) != mask1);
    }
    if (!mask1) {
        // Unsigned high is C && !Z; mask0 selects it or its complement.
        return ((flags.carry() && !flags.zero()) != mask0);
    }
    // Exact / not-exact.
    return exact != mask0;
}

struct AluResult {
    u32 value = 0;
    bool carry = false;
    bool overflow = false;
};

// The patent's sixteen ASEL operations, expressed as an ordinary arithmetic
// unit. Carry and overflow are meaningful only for the arithmetic half; the
// logical operations clear both latches.
AluResult evaluate_alu(u32 operation, u32 a, u32 b) {
    AluResult result;
    switch (operation & 0x0fu) {
        case 0: result.value = a; break;
        case 1:
            result.value = 0u - b;
            result.carry = sub_carry(0, b, result.value);
            result.overflow = sub_overflow(0, b, result.value);
            break;
        case 2: case 3:
            result.value = a + b;
            result.carry = add_carry(a, b, result.value);
            result.overflow = add_overflow(a, b, result.value);
            break;
        case 4: case 5:
            result.value = a - b;
            result.carry = sub_carry(a, b, result.value);
            result.overflow = sub_overflow(a, b, result.value);
            break;
        case 6:
            result.value = a + 0x1000u;
            result.carry = add_carry(a, 0x1000u, result.value);
            result.overflow = add_overflow(a, 0x1000u, result.value);
            break;
        case 7:
            result.value = a - 0x1000u;
            result.carry = sub_carry(a, 0x1000u, result.value);
            result.overflow = sub_overflow(a, 0x1000u, result.value);
            break;
        case 8:  result.value = a; break;
        case 9:  result.value = ~a; break;
        case 10: result.value = a & b; break;
        case 11: result.value = ~(a & b); break;
        case 12: result.value = a | b; break;
        case 13: result.value = ~(a | b); break;
        case 14: result.value = a ^ b; break;
        case 15: result.value = ~(a ^ b); break;
    }
    return result;
}

u32 apply_barrel_shift(u8 operation, u32 value, Flags& flags) {
    static constexpr u8 left_amount[7]  = {0, 1, 2, 3, 4, 5, 8};
    static constexpr u8 right_amount[7] = {16, 8, 5, 4, 3, 2, 1};

    const u8 low = operation & 0x0fu;
    if ((operation < 16 || operation >= 17) && low >= 1 && low <= 6) {
        return value << left_amount[low];
    }
    if (operation == 7 || operation == 23) {
        if (flags.overflow()) {
            return flags.negative() ? 0x7ffff000u : 0x80000000u;
        }
        return value;
    }
    if (operation == 8 || operation == 24) {
        const bool shifted_out = (value & 0x80000000u) != 0;
        flags.set_carry(shifted_out);
        return ((value << 1) & 0xfffe0000u) |
               (shifted_out ? (1u << 16) : 0u) | (value & 0xf000u);
    }
    if (operation >= 9 && operation <= 15) {
        return static_cast<u32>(static_cast<s32>(value) >>
                                right_amount[operation - 9]);
    }
    if (operation >= 25 && operation <= 31) {
        return value >> right_amount[operation - 25];
    }
    return value;
}

}  // namespace

Dsp::Dsp() {
    // Register maps. A register number does not name a fixed address: the
    // current map decides which of two banks it lands in, which is how the
    // same program body works on either half of a stereo pair.
    for (u32 map = 0; map < 8; ++map) {
        for (u32 reg = 0; reg < 16; ++reg) {
            register_map_[map][reg] = register_base(map, reg);
        }
    }

    // Which operands each arithmetic instruction wants. Pure decode, so it is
    // done once here rather than per instruction.
    for (u32 word = 0; word < 0x8000; ++word) {
        u8 requests = 0;
        if (aif_bs(word) == 0x8) {
            requests |= kNeedShift;
        }
        switch (aif_muxa(word)) {
            case 3:
                requests |= kNeedMult1;
                if (aif_m2sel(word)) requests |= kNeedMult2;
                break;
            case 1: requests |= kNeedAlu1; break;
            case 2: requests |= kNeedAlu2; break;
            default: break;
        }
        switch (aif_muxb(word)) {
            case 1: requests |= kNeedAlu1; break;
            case 2: requests |= kNeedAlu2; break;
            case 3:
                requests |= kNeedMult1;
                if (aif_m2sel(word)) requests |= kNeedMult2;
                break;
            default: break;
        }
        decoded_[word].requests = requests;
        // The shift is five bits: four from the instruction and a fifth
        // borrowed from the top of the ALU field, which is what separates the
        // arithmetic shifts from the logical ones.
        decoded_[word].shift =
            static_cast<u8>(aif_bs(word) | ((aif_alu(word) & 8) << 1));
    }

    // The real chip evaluates this as combinational logic. Precompute the same
    // 32 conditions by 32 status states from the readable decoder above.
    for (u32 condition = 0; condition < 32; ++condition) {
        for (u32 packed = 0; packed < 32; ++packed) {
            Flags flags;
            flags.set_zero((packed & 1) != 0);
            flags.set_negative((packed & 2) != 0);
            flags.set_carry((packed & 4) != 0);
            flags.set_overflow((packed & 8) != 0);
            const bool exact = (packed & 16u) != 0;
            branch_taken_[condition][packed] =
                static_cast<u8>(branch_condition(condition, flags, exact));
        }
    }

    // A DSPP that has been started but never loaded must halt, not run
    // whatever was left in memory.
    program_.fill(0x8380);   // sleep
    reset();
}

u16 Dsp::register_base(u32 map, u32 reg) {
    reg &= 0x0f;
    const u32 x = (reg >> 2) & 1;
    const u32 y = (reg >> 3) & 1;
    u32 twist = 0;
    switch (map) {
        case 0: case 1: case 2: case 3: twist = x; break;
        case 4: twist = y; break;
        case 5: twist = y ^ 1; break;
        case 6: twist = x & y; break;
        default: twist = x | y; break;
    }
    return static_cast<u16>((reg & 7) | (twist << 8) | ((reg >> 3) << 9));
}

void Dsp::reset() {
    counter_ = reload_;
    pc_ = 0;
    register_base_x4_ = 0;
    register_map_index_ = 0;
    operand_mask_ = 0xffff;
}

u16 Dsp::noise() {
    noise_state_ += 0xfc15u;
    const u32 hashed = noise_state_ * 0x02abu;
    return static_cast<u16>(((hashed >> 16) ^ hashed) & 0xffffu);
}

// Reading data memory. The top of the range is not memory at all: it is the
// DACs, the semaphores, and the windows onto the DMA channels.
u16 Dsp::read(u32 address) {
    switch (address) {
        case 0xea: return noise();
        case 0xeb: return audio_out_status_;
        case 0xec: return semaphore_status_;
        case 0xed: return semaphore_data_;
        case 0xee: return static_cast<u16>(pc_);
        case 0xef: return static_cast<u16>(counter_);
        default: break;
    }

    // A value the CPU handed over directly is read once and then the channel
    // takes over again.
    if (address >= 0xf0 && address <= 0xfc) {
        if (cpu_supplied_[address - 0xf0]) {
            cpu_supplied_[address - 0xf0] = false;
            return noise();
        }
        return host_ != nullptr
                   ? host_->dsp_input_next(static_cast<u16>(address & 0x0f))
                   : 0;
    }
    if (address >= 0x70 && address <= 0x7c) {
        if (cpu_supplied_[address - 0x70]) {
            cpu_supplied_[address - 0x70] = false;
            return data_[address];
        }
        return host_ != nullptr
                   ? host_->dsp_input_peek(static_cast<u16>(address & 0x0f))
                   : 0;
    }
    if (address >= 0xd0 && address <= 0xde) {
        if (cpu_supplied_[address & 0x0f]) {
            return 2;
        }
        return host_ != nullptr
                   ? host_->dsp_input_status(static_cast<u16>(address & 0x0f))
                   : 0;
    }
    if (address >= 0xe0 && address <= 0xe3) {
        return host_ != nullptr
                   ? host_->dsp_output_status(static_cast<u16>(address & 0x0f))
                   : 0;
    }

    const u32 shifted = address - 0x100;
    if (shifted < 0x200) {
        return data_[shifted | 0x100];
    }
    return data_[shifted & 0x7f];
}

void Dsp::write(u32 address, u16 value) {
    address &= 0x3ff;
    switch (address) {
        case 0x3eb:
            audio_out_status_ = value;
            return;
        case 0x3ec:
            semaphore_status_ |= 0x01;   // the DSPP acknowledges
            return;
        case 0x3ed:
            semaphore_data_ = value;
            semaphore_status_ = 0x04;    // the DSPP wrote last
            return;
        case 0x3ee:
            // This is the audio interrupt. It is raised at the end of the
            // pass rather than here, because a program may write it and then
            // keep running.
            interrupt_word_ = value;
            generate_interrupt_ = true;
            return;
        case 0x3ef:
            reload_ = static_cast<s16>(value);
            return;
        case 0x3fd:
            return;   // flush the output FIFO
        case 0x3fe:   // left DAC
        case 0x3ff:   // right DAC
            data_[address] = value;
            return;
        default:
            break;
    }
    if (address >= 0x3f0 && address <= 0x3f3) {
        if (host_ != nullptr) {
            host_->dsp_output(static_cast<u16>(address & 0x0f), value);
        }
        return;
    }
    if (address < 0x100) {
        return;   // the low half is the CPU's to write, not the DSPP's
    }
    const u32 shifted = address - 0x100;
    data_[shifted < 0x200 ? (shifted | 0x100) : (shifted + 0x100)] = value;
}

u16 Dsp::next_program_word() {
    return program_[pc_++ & 0x3ff];
}

u16 Dsp::mapped_register(u32 reg) const {
    return static_cast<u16>(register_map_[register_map_index_][reg & 0x0f] ^
                            register_base_x4_);
}

u16 Dsp::operand_value(u16 address, bool indirect) {
    const u16 first = read(address);
    return indirect ? read(first) : first;
}

// A MOVE consumes one source even when the following word uses the packed
// two- or three-register spelling.  In those spellings the source is the last
// register field, as specified by the control-instruction operand diagram.
u16 Dsp::move_source() {
    const u16 word = next_program_word();
    switch (op_type(word)) {
        case 0: case 1: case 2: case 3:
            return operand_value(mapped_register(r3_r3(word)), r3_r3_di(word));
        case 4:
            return operand_value(static_cast<u16>(nrof_address(word)),
                                 nrof_indirect(word));
        case 5:
            return operand_value(mapped_register(r2_r1(word)), r2_r1_di(word));
        default:
            return static_cast<u16>(immediate_value(word));
    }
}

// Turn one operand word into the ordered values it contributes.  Destination
// selection is kept alongside the group instead of mutating the execution
// state while each field is decoded; this mirrors the hardware packet and
// makes the write-back rule explicit at the caller.
Dsp::OperandGroup Dsp::decode_operand_group(u16 word) {
    OperandGroup group;

    auto append_register = [&](u32 reg, bool indirect) {
        const u16 address = mapped_register(reg);
        group.values[group.count++] = operand_value(address, indirect);
        return address;
    };

    switch (op_type(word)) {
        case 0: case 1: case 2: case 3:
            append_register(r3_r3(word), r3_r3_di(word));
            append_register(r3_r2(word), r3_r2_di(word));
            group.destination = append_register(r3_r1(word), r3_r1_di(word));
            break;

        case 4: {
            const u16 address = static_cast<u16>(nrof_address(word));
            group.values[group.count++] = operand_value(address, nrof_indirect(word));
            group.destination = address;
            if (nrof_wb1(word)) group.marked_destination = address;
            break;
        }

        case 5: {
            auto append_writable_register = [&](u32 reg, bool indirect, bool marked) {
                u16 address = mapped_register(reg);
                if (indirect) address = read(address);
                group.values[group.count++] = read(address);
                group.destination = address;
                if (marked) group.marked_destination = address;
            };
            if (r2_numregs(word)) {
                append_writable_register(r2_r2(word), r2_r2_di(word), r2_wb2(word));
            }
            append_writable_register(r2_r1(word), r2_r1_di(word), r2_wb1(word));
            break;
        }

        default:
            group.values[0] = static_cast<u16>(immediate_value(word));
            group.count = 1;
            group.destination = group.values[0];
            break;
    }
    return group;
}

void Dsp::gather_arithmetic_operands(unsigned requested) {
    writeback_ = 0;
    if (requested == 0) {
        if (requests_ == 0) return;
        requested = 4;
    }

    std::array<u16, 8> values{};
    unsigned value_count = 0;
    u16 last_destination = 0;
    u16 marked_destination = 0;

    do {
        const OperandGroup group = decode_operand_group(next_program_word());
        for (u8 i = 0; i < group.count; ++i) {
            values[value_count++] = group.values[i];
        }
        last_destination = group.destination;
        if (group.marked_destination != 0) {
            marked_destination = group.marked_destination;
        }
    } while (value_count < requested && value_count < 6);

    // Requested slots have a fixed consumption order in the arithmetic data
    // path.  MASK suppresses a slot without changing the packed value order.
    requests_ &= static_cast<u8>(operand_mask_);
    unsigned consumed = 0;
    if (requests_ & kNeedMult1) mult1_ = values[consumed++];
    if (requests_ & kNeedMult2) mult2_ = values[consumed++];
    if (requests_ & kNeedAlu1)  alu1_  = values[consumed++];
    if (requests_ & kNeedAlu2)  alu2_  = values[consumed++];
    if (requests_ & kNeedShift) barrel_shift_ = values[consumed++];

    // An explicit WB bit wins.  Without one, an unused trailing value makes
    // the packet's natural destination writable; a fully consumed packet has
    // no destination.
    writeback_ = marked_destination != 0
                     ? marked_destination
                     : (value_count != consumed ? last_destination : 0);
}

u32 Dsp::run() {
    if (!running_) {
        return (static_cast<u32>(data_[0x3ff]) << 16) | data_[0x3fe];
    }

    reset();

    u32 accumulator = 0;
    u32 return_address = 0;
    bool exact = false;
    Flags flags;
    bool working = true;
    u32 steps = 0;

    while (working && steps++ < kMaxStepsPerPass) {
        const u32 instruction = program_[pc_++ & 0x3ff];
#if RETRO3DO_TRACING
        // One line per executed DSPP instruction, for diffing a pass against
        // another interpreter's walk of the same program.
        static std::FILE* step_log = [] {
            const char* path = std::getenv("DSPSTEP");
            return path != nullptr ? std::fopen(path, "w") : nullptr;
        }();
        if (step_log != nullptr) {
            if (is_control(instruction) && ((instruction >> 7) & 0xff) >= 64) {
                const u32 packed = (flags.zero() ? 1u : 0) | (flags.negative() ? 2u : 0) |
                                   (flags.carry() ? 4u : 0) | (flags.overflow() ? 8u : 0) |
                                   (exact ? 16u : 0);
                std::fprintf(step_log, "%03X %04X F=%02X Y=%08X\n",
                             (pc_ - 1) & 0x3ff, instruction, packed, accumulator);
            } else {
                std::fprintf(step_log, "%03X %04X\n", (pc_ - 1) & 0x3ff, instruction);
            }
        }
#endif

        if (is_control(instruction)) {
            const u32 opcode = (instruction >> 7) & 0xff;
            switch (opcode) {
                case 0:   // nop
                    break;
                case 1:   // branch to the accumulator
                    pc_ = (accumulator >> 16) & 0x3ff;
                    break;
                case 2:   // set the register base
                    register_base_x4_ = (cif_address(instruction) & 0x3f) << 2;
                    break;
                case 3:   // select a register map
                    register_map_index_ = cif_address(instruction) & 7;
                    break;
                case 4:   // return
                    pc_ = return_address;
                    break;
                case 5:   // set the operand mask
                    operand_mask_ = static_cast<u16>(~(cif_address(instruction) & 0x1f));
                    break;
                case 6:
                    break;
                case 7:   // sleep - the pass ends here
                    working = false;
                    break;
                case 8: case 9: case 10: case 11:
                case 12: case 13: case 14: case 15:
                    pc_ = cif_address(instruction);
                    break;
                case 16: case 17: case 18: case 19:
                case 20: case 21: case 22: case 23:
                    return_address = pc_;
                    pc_ = cif_address(instruction);
                    break;
                case 24: case 25: case 26: case 27:
                case 28: case 29: case 30: case 31:
                    pc_ = cif_address(instruction);
                    break;
                default:
                    if (opcode >= 32 && opcode <= 47) {
                        // Move to a register-addressed destination.
                        const u16 value = move_source();
                        u16 address = static_cast<u16>(
                            register_map_[register_map_index_][r2_r1(instruction)] ^
                            register_base_x4_);
                        if (r2_r1_di(instruction)) address = read(address);
                        write(address, value);
                    } else if (opcode >= 48 && opcode <= 63) {
                        // Move to a directly addressed destination.
                        const u16 value = move_source();
                        u16 address = static_cast<u16>(cif_address(instruction));
                        if (nrof_indirect(instruction)) address = read(address);
                        write(address, value);
                    } else if (branch_taken_[branch_bits(instruction)]
                                            [flag_index(flags, exact)] != 0) {
                        pc_ = cif_address(instruction);
                    }
                    break;
            }
            continue;
        }

        // Arithmetic.
        requests_ = decoded_[instruction].requests;
        barrel_shift_ = decoded_[instruction].shift;
        gather_arithmetic_operands(aif_numops(instruction));

        const u32 alu = aif_alu(instruction);
        // ACSBU: two of the ALU operations take the carry as their second
        // input rather than an operand, and that changes what the multiplier
        // path feeds the first one.
        const bool acsbu = (alu == 3) || (alu == 5);

        u32 a = 0;
        switch (aif_muxa(instruction)) {
            case 3:
                if (!aif_m2sel(instruction)) {
                    a = acsbu ? (flags.carry() ? (static_cast<u32>(static_cast<s16>(mult1_)) << 16) : 0u)
                              : static_cast<u32>(static_cast<s32>(static_cast<s16>(mult1_)) *
                                                 ((static_cast<s32>(accumulator) >> 15) & ~1));
                } else {
                    a = static_cast<u32>(static_cast<s32>(static_cast<s16>(mult1_)) *
                                         static_cast<s32>(static_cast<s16>(mult2_)) * 2);
                }
                break;
            case 1: a = static_cast<u32>(alu1_) << 16; break;
            case 0: a = accumulator; break;
            default: a = static_cast<u32>(alu2_) << 16; break;
        }

        u32 b = 0;
        if (acsbu) {
            b = flags.carry() ? (1u << 16) : 0u;
        } else {
            switch (aif_muxb(instruction)) {
                case 0: b = accumulator; break;
                case 1: b = static_cast<u32>(alu1_) << 16; break;
                case 2: b = static_cast<u32>(alu2_) << 16; break;
                default:
                    if (!aif_m2sel(instruction)) {
                        b = static_cast<u32>(static_cast<s32>(static_cast<s16>(mult1_)) *
                                             (static_cast<s32>(accumulator) >> 15)) & ~1u;
                    } else {
                        b = static_cast<u32>(static_cast<s32>(static_cast<s16>(mult1_)) *
                                             static_cast<s32>(static_cast<s16>(mult2_)) * 2);
                    }
                    break;
            }
        }

        const AluResult alu_result = evaluate_alu(alu, a, b);
        u32 y = alu_result.value;
        flags.set_carry(alu_result.carry);
        flags.set_overflow(alu_result.overflow);
        flags.set_zero((y & 0xffff0000u) == 0);
        flags.set_negative((y >> 31) != 0);
        exact = (y & 0x0000f000u) == 0;

        y = apply_barrel_shift(static_cast<u8>(barrel_shift_), y, flags);

        accumulator = y;
        if (writeback_ != 0) {
            write(writeback_, static_cast<u16>(static_cast<s32>(y) >> 16));
        }
    }

    if (generate_interrupt_) {
        generate_interrupt_ = false;
        if (host_ != nullptr) {
            host_->dsp_audio_interrupt();
        }
    }

    counter_ = static_cast<s16>(counter_ - kSystemTicks);
    if (counter_ <= 0) {
        counter_ = static_cast<s16>(counter_ + reload_);
    }

    return (static_cast<u32>(data_[0x3ff]) << 16) | data_[0x3fe];
}

void Dsp::write_program(u16 address, u16 value) {
    program_[address & 0x3ff] = value;
}

void Dsp::write_data(u16 address, u16 value) {
    // The window from 0x70 to 0x7C is a value handed straight to the DSPP,
    // which reads it once and then goes back to its DMA channel.
    if (address >= 0x70 && address <= 0x7c) {
        cpu_supplied_[address - 0x70] = true;
        data_[address & 0x7f] = value;
        return;
    }
    if ((address & 0x80) == 0) {
        data_[address & 0x7f] = value;
    }
}

u16 Dsp::read_data(u16 address) const {
    switch (address) {
        case 0x3eb: return audio_out_status_;
        case 0x3ec: return semaphore_status_;
        case 0x3ed: return semaphore_data_;
        case 0x3ee: return interrupt_word_;
        case 0x3ef: return static_cast<u16>(reload_);
        default: break;
    }
    return data_[address & 0x3ff];
}

void Dsp::write_semaphore(u32 value) {
    semaphore_data_ = static_cast<u16>(value & 0xffffu);
    semaphore_status_ = 0x08;   // the CPU wrote last
}

u32 Dsp::read_semaphore() const {
    return (static_cast<u32>(semaphore_status_) << 16) | semaphore_data_;
}

}  // namespace retro3do
