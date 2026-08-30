#include "dsp.h"

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

// The four ALU flags live one to a byte, and the branch table is indexed by a
// value squeezed out of them with a multiply. Kept in exactly that form rather
// than as four bools, because the packing is what the table was built around.
struct Flags {
    u32 raw = 0;
    void set_zero(bool v)     { raw = (raw & ~0x000000ffu) | (v ? 1u : 0u); }
    void set_negative(bool v) { raw = (raw & ~0x0000ff00u) | (v ? 0x100u : 0u); }
    void set_carry(bool v)    { raw = (raw & ~0x00ff0000u) | (v ? 0x10000u : 0u); }
    void set_overflow(bool v) { raw = (raw & ~0xff000000u) | (v ? 0x1000000u : 0u); }
    bool zero() const     { return (raw & 0x000000ffu) != 0; }
    bool negative() const  { return (raw & 0x0000ff00u) != 0; }
    bool carry() const     { return (raw & 0x00ff0000u) != 0; }
    bool overflow() const  { return (raw & 0xff000000u) != 0; }
    u32 index() const      { return (raw * 0x10080402u) >> 24; }
};

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

    // Whether each conditional branch is taken, for every flag state. This is
    // combinational logic on the real chip; here it is a table, because
    // evaluating it per branch is the same work done a million times a second.
    for (u32 word = 0xa000; word <= 0xffff; word += 1024) {
        const u32 bits = branch_bits(word);
        const bool flagm0  = field(word, 10, 1) != 0;
        const bool flagm1  = field(word, 11, 1) != 0;
        const bool flagsel = field(word, 12, 1) != 0;
        const bool mode0   = field(word, 13, 1) != 0;
        const bool mode1   = field(word, 14, 1) != 0;

        for (u32 packed = 0; packed < 16; ++packed) {
            Flags flags;
            flags.set_zero((packed & 1) != 0);
            flags.set_negative((packed & 2) != 0);
            flags.set_carry((packed & 4) != 0);
            flags.set_overflow((packed & 8) != 0);

            for (u32 exact = 0; exact < 2; ++exact) {
                const bool md1 = !mode1 && mode0;
                const bool md2 = mode1 && !mode0;
                const bool md3 = mode1 && mode0;

                const bool stat0 = (flagsel && flags.carry()) ||
                                   (!flagsel && flags.negative());
                const bool stat1 = (flagsel && flags.zero()) ||
                                   (!flagsel && flags.overflow());
                const bool nstat0 = stat0 != md2;
                const bool nstat1 = stat1 != md2;

                const bool dcare1 = !flagm1 || nstat0;
                const bool dcare0 = !flagm0 || nstat1;
                const bool no_care = !flagm1 && !flagm0;

                const bool md12s = dcare1 && dcare0 && (mode1 != mode0) && !no_care;

                const bool super0 = md1 && !flagsel && no_care;
                const bool super1 = md1 && flagsel && no_care;
                const bool all_zero = super0 && flags.zero() && exact != 0;
                const bool not_all_zero = super1 && !(flags.zero() && exact != 0);
                const bool sds = all_zero || not_all_zero;

                const bool nv_test =
                    ((((flags.negative() != flags.overflow()) ||
                       (flags.zero() && flagm0)) != flagm1) && !flagsel);
                const bool tmpcs = flags.carry() && !flags.zero();
                const bool cz_test = (tmpcs != flagm0) && flagsel && !flagm1;
                const bool exact_test = ((exact != 0) != flagm0) && flagsel && flagm1;
                const bool md3s = (nv_test || cz_test || exact_test) && md3;

                branch_taken_[bits][exact + flags.index()] =
                    static_cast<u8>(md12s || md3s || sds);
            }
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

u16 Dsp::load_one_operand() {
    const u32 operand = program_[pc_++ & 0x3ff];
    switch (op_type(operand)) {
        case 0: case 1: case 2: case 3: {
            const u16 value = read(register_map_[register_map_index_][r3_r3(operand)] ^
                                   register_base_x4_);
            return r3_r3_di(operand) ? read(value) : value;
        }
        case 4: {
            const u16 value = read(nrof_address(operand));
            return nrof_indirect(operand) ? read(value) : value;
        }
        case 5: {
            const u16 value = read(register_map_[register_map_index_][r2_r1(operand)] ^
                                   register_base_x4_);
            return r2_r1_di(operand) ? read(value) : value;
        }
        default:
            return static_cast<u16>(immediate_value(operand));
    }
}

void Dsp::load_operands(int requested) {
    writeback_ = 0;

    if (requested == 0) {
        if (requests_ == 0) {
            return;
        }
        requested = 4;
    }

    u16 operands[8] = {};
    int count = 0;
    u16 deferred_writeback = 0;

    do {
        const u32 operand = program_[pc_++ & 0x3ff];
        switch (op_type(operand)) {
            case 0: case 1: case 2: case 3:
                operands[count] = read(register_map_[register_map_index_][r3_r3(operand)] ^
                                       register_base_x4_);
                if (r3_r3_di(operand)) operands[count] = read(operands[count]);
                ++count;

                operands[count] = read(register_map_[register_map_index_][r3_r2(operand)] ^
                                       register_base_x4_);
                if (r3_r2_di(operand)) operands[count] = read(operands[count]);
                ++count;

                // Only the first register can be written back to.
                writeback_ = static_cast<u16>(
                    register_map_[register_map_index_][r3_r1(operand)] ^ register_base_x4_);
                operands[count] = read(writeback_);
                if (r3_r1_di(operand)) operands[count] = read(operands[count]);
                ++count;
                break;

            case 4:
                writeback_ = static_cast<u16>(nrof_address(operand));
                operands[count] = read(writeback_);
                if (nrof_indirect(operand)) operands[count] = read(operands[count]);
                ++count;
                if (nrof_wb1(operand)) deferred_writeback = writeback_;
                break;

            case 5:
                if (r2_numregs(operand)) {
                    writeback_ = static_cast<u16>(
                        register_map_[register_map_index_][r2_r2(operand)] ^ register_base_x4_);
                    if (r2_r2_di(operand)) writeback_ = read(writeback_);
                    operands[count++] = read(writeback_);
                    if (r2_wb2(operand)) deferred_writeback = writeback_;
                }
                writeback_ = static_cast<u16>(
                    register_map_[register_map_index_][r2_r1(operand)] ^ register_base_x4_);
                if (r2_r1_di(operand)) writeback_ = read(writeback_);
                operands[count++] = read(writeback_);
                if (r2_wb1(operand)) deferred_writeback = writeback_;
                break;

            default:
                operands[count] = static_cast<u16>(immediate_value(operand));
                writeback_ = operands[count++];
                break;
        }
    } while (count < requested && count < 6);

    // The operand mask lets a program suppress slots it does not want filled
    // this time round.
    requests_ &= static_cast<u8>(operand_mask_);

    int index = 0;
    if (requests_ & kNeedMult1) mult1_ = operands[index++];
    if (requests_ & kNeedMult2) mult2_ = operands[index++];
    if (requests_ & kNeedAlu1)  alu1_  = operands[index++];
    if (requests_ & kNeedAlu2)  alu2_  = operands[index++];
    if (requests_ & kNeedShift) barrel_shift_ = operands[index++];

    // If the instruction consumed fewer operands than were fetched, the extra
    // one carries the write-back; otherwise the deferred one does, even when
    // that is nothing.
    if (count != index) {
        if (deferred_writeback != 0) {
            writeback_ = deferred_writeback;
        }
    } else {
        writeback_ = deferred_writeback;
    }
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
                        const u16 value = load_one_operand();
                        u16 address = static_cast<u16>(
                            register_map_[register_map_index_][r2_r1(instruction)] ^
                            register_base_x4_);
                        if (r2_r1_di(instruction)) address = read(address);
                        write(address, value);
                    } else if (opcode >= 48 && opcode <= 63) {
                        // Move to a directly addressed destination.
                        const u16 value = load_one_operand();
                        u16 address = static_cast<u16>(cif_address(instruction));
                        if (nrof_indirect(instruction)) address = read(address);
                        write(address, value);
                    } else if (branch_taken_[branch_bits(instruction)]
                                            [(exact ? 1u : 0u) + flags.index()] != 0) {
                        pc_ = cif_address(instruction);
                    }
                    break;
            }
            continue;
        }

        // Arithmetic.
        requests_ = decoded_[instruction].requests;
        barrel_shift_ = decoded_[instruction].shift;
        load_operands(static_cast<int>(aif_numops(instruction)));

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

        bool carry = false;
        bool overflow = false;
        u32 y = 0;
        switch (alu) {
            case 0: y = a; break;
            case 1:
                y = 0u - b;
                carry = sub_carry(0, b, y);
                overflow = sub_overflow(0, b, y);
                break;
            case 2: case 3:
                y = a + b;
                carry = add_carry(a, b, y);
                overflow = add_overflow(a, b, y);
                break;
            case 4: case 5:
                y = a - b;
                carry = sub_carry(a, b, y);
                overflow = sub_overflow(a, b, y);
                break;
            case 6:
                y = a + 0x1000u;
                carry = add_carry(a, 0x1000u, y);
                overflow = add_overflow(a, 0x1000u, y);
                break;
            case 7:
                y = a - 0x1000u;
                carry = sub_carry(a, 0x1000u, y);
                overflow = sub_overflow(a, 0x1000u, y);
                break;
            case 8:  y = a; break;
            case 9:  y = ~a; break;
            case 10: y = a & b; break;
            case 11: y = ~(a & b); break;
            case 12: y = a | b; break;
            case 13: y = ~(a | b); break;
            case 14: y = a ^ b; break;
            default: y = ~(a ^ b); break;
        }
        flags.set_carry(carry);
        flags.set_overflow(overflow);
        flags.set_zero((y & 0xffff0000u) == 0);
        flags.set_negative((y >> 31) != 0);
        exact = (y & 0x0000f000u) == 0;

        // The barrel shifter. Cases below sixteen are arithmetic, above are
        // logical, and two of them are neither: 7 and 23 clip a saturated
        // result, 8 and 24 shift one bit out into the carry.
        switch (barrel_shift_) {
            case 1:  case 17: y <<= 1; break;
            case 2:  case 18: y <<= 2; break;
            case 3:  case 19: y <<= 3; break;
            case 4:  case 20: y <<= 4; break;
            case 5:  case 21: y <<= 5; break;
            case 6:  case 22: y <<= 8; break;

            case 9:  y = static_cast<u32>(static_cast<s32>(y) >> 16); break;
            case 10: y = static_cast<u32>(static_cast<s32>(y) >> 8);  break;
            case 11: y = static_cast<u32>(static_cast<s32>(y) >> 5);  break;
            case 12: y = static_cast<u32>(static_cast<s32>(y) >> 4);  break;
            case 13: y = static_cast<u32>(static_cast<s32>(y) >> 3);  break;
            case 14: y = static_cast<u32>(static_cast<s32>(y) >> 2);  break;
            case 15: y = static_cast<u32>(static_cast<s32>(y) >> 1);  break;

            case 7: case 23:
                if (flags.overflow()) {
                    y = flags.negative() ? 0x7ffff000u : 0x80000000u;
                }
                break;

            case 8: case 24: {
                const bool shifted_out = (static_cast<s32>(y) < 0);
                flags.set_carry(shifted_out);
                y = ((y << 1) & 0xfffe0000u) | (shifted_out ? (1u << 16) : 0u) |
                    (y & 0xf000u);
                break;
            }

            case 25: y >>= 16; break;
            case 26: y >>= 8;  break;
            case 27: y >>= 5;  break;
            case 28: y >>= 4;  break;
            case 29: y >>= 3;  break;
            case 30: y >>= 2;  break;
            case 31: y >>= 1;  break;
            default: break;
        }

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
