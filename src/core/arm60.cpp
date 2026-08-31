#include <cstdio>
#include <cstdlib>
#include "arm60.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include "bus.h"

namespace retro3do {

// What a memory cycle costs.
//
// The ARM's own timings are quoted in these three: a sequential access, a
// non-sequential one, and an internal cycle with no bus activity at all. On
// this machine a non-sequential access is four times a sequential one, and
// that ratio is most of what decides how fast the emulated CPU runs.
//
// Charging one cycle for everything makes the processor about twice as fast as
// the real one - which does not look like a bug, because everything still
// works. It looks like a game whose animation is a little quick and whose
// timing never quite matches a reference machine's.
namespace {
constexpr u32 kSeq = 1;      // S
constexpr u32 kNonSeq = 4;   // N
constexpr u32 kInternal = 1; // I
}  // namespace
namespace {

// Banked-register slots. User and System share the user bank.
constexpr unsigned kBankUser = 0;
constexpr unsigned kBankFiq  = 1;
constexpr unsigned kBankIrq  = 2;
constexpr unsigned kBankSvc  = 3;
constexpr unsigned kBankAbt  = 4;
constexpr unsigned kBankUnd  = 5;

unsigned bank_index(Mode mode) {
    switch (mode) {
        case Mode::Fiq:        return kBankFiq;
        case Mode::Irq:        return kBankIrq;
        case Mode::Supervisor: return kBankSvc;
        case Mode::Abort:      return kBankAbt;
        case Mode::Undefined:  return kBankUnd;
        case Mode::User:
        case Mode::System:
        default:               return kBankUser;
    }
}

bool mode_has_spsr(Mode mode) {
    return mode != Mode::User && mode != Mode::System;
}

// Data-processing opcodes, bits 24..21.
enum : unsigned {
    kAnd = 0x0, kEor = 0x1, kSub = 0x2, kRsb = 0x3,
    kAdd = 0x4, kAdc = 0x5, kSbc = 0x6, kRsc = 0x7,
    kTst = 0x8, kTeq = 0x9, kCmp = 0xa, kCmn = 0xb,
    kOrr = 0xc, kMov = 0xd, kBic = 0xe, kMvn = 0xf,
};

bool opcode_writes_result(unsigned opcode) {
    return opcode < kTst || opcode > kCmn;
}

}  // namespace

// ---------------------------------------------------------------------------
// Decode cache
// ---------------------------------------------------------------------------
//
// One page of decoded instructions per 4 KB of code memory, allocated on first
// use. A page holds 1024 entries because every ARM instruction is four bytes.
// `valid` distinguishes "not yet decoded" from "decoded to Undefined", which is
// a real instruction the CPU must be able to execute repeatedly.
struct Arm60::DecodeCache {
    static constexpr u32 kPageBytes   = 4096;
    static constexpr u32 kPageEntries = kPageBytes / 4;

    // Validity is a generation stamp rather than a flag.
    //
    // Clearing a flag array meant every invalidation memset four kilobytes, and
    // the machine invalidates constantly - the ROM's own memory test writes
    // across the whole of DRAM. Bumping a counter instead makes invalidating a
    // page O(1), which turns the cost of a memory test from proportional to
    // memory size into nothing at all.
    struct Page {
        Decoded entries[kPageEntries];
        u32     stamp[kPageEntries];
        u32     generation;
    };

    std::unordered_map<u32, std::vector<Page>::size_type> page_index;
    std::vector<Page> pages;

    Page& page_for(u32 address) {
        const u32 key = address / kPageBytes;
        auto it = page_index.find(key);
        if (it != page_index.end()) {
            return pages[it->second];
        }
        pages.emplace_back();
        Page& page = pages.back();
        std::memset(page.stamp, 0, sizeof(page.stamp));
        page.generation = 1;
        page_index.emplace(key, pages.size() - 1);
        return page;
    }

    void clear() {
        page_index.clear();
        pages.clear();
    }

    void invalidate(u32 address, u32 length) {
        const u32 first = address / kPageBytes;
        const u32 last  = (address + length - 1) / kPageBytes;
        for (u32 key = first; key <= last; ++key) {
            auto it = page_index.find(key);
            if (it == page_index.end()) {
                continue;
            }
            Page& page = pages[it->second];
            if (++page.generation == 0) {
                // Wrapped, so old stamps could look current again.
                std::memset(page.stamp, 0, sizeof(page.stamp));
                page.generation = 1;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
Arm60::Arm60(Bus& bus) : bus_(bus), cache_(new DecodeCache()) {
    reset();
}

Arm60::~Arm60() {
    delete cache_;
}

void Arm60::reset() {
    std::memset(regs_, 0, sizeof(regs_));
    std::memset(banked_r13_, 0, sizeof(banked_r13_));
    std::memset(banked_r14_, 0, sizeof(banked_r14_));
    std::memset(banked_spsr_, 0, sizeof(banked_spsr_));
    std::memset(banked_fiq_r8_r12_, 0, sizeof(banked_fiq_r8_r12_));
    std::memset(banked_user_r8_r12_, 0, sizeof(banked_user_r8_r12_));

    // Reset enters supervisor mode with both interrupt lines masked.
    cpsr_ = static_cast<u32>(Mode::Supervisor) | kFlagI | kFlagF;

    // The 3DO's reset vector is the base of ROM rather than address zero: the
    // BIOS is mapped there and the machine begins executing it directly.
    regs_[15] = kRomBase;

    irq_line_ = false;
    irq_latched_ = false;
    fiq_line_ = false;
    total_cycles_ = 0;

    cache_->clear();
}

void Arm60::invalidate_decode_cache() {
    cache_->clear();
}

void Arm60::invalidate_decode_cache(u32 address, u32 length) {
    if (length == 0) return;
    ++invalidations_;
    cache_->invalidate(address, length);
}

void Arm60::set_reg(unsigned index, u32 value) {
    regs_[index & 15u] = value;
}

u32 Arm60::spsr() const {
    const Mode m = mode();
    return mode_has_spsr(m) ? banked_spsr_[bank_index(m)] : cpsr_;
}

// ---------------------------------------------------------------------------
// Mode switching and banking
// ---------------------------------------------------------------------------
void Arm60::switch_mode(Mode new_mode) {
    const Mode old_mode = mode();
    if (old_mode == new_mode) {
        return;
    }

    const unsigned old_bank = bank_index(old_mode);
    const unsigned new_bank = bank_index(new_mode);

    // R8..R12 are banked only for FIQ, so they only move when FIQ is entered
    // or left.
    const bool was_fiq = old_mode == Mode::Fiq;
    const bool now_fiq = new_mode == Mode::Fiq;
    if (was_fiq != now_fiq) {
        if (was_fiq) {
            for (unsigned i = 0; i < 5; ++i) {
                banked_fiq_r8_r12_[i] = regs_[8 + i];
                regs_[8 + i] = banked_user_r8_r12_[i];
            }
        } else {
            for (unsigned i = 0; i < 5; ++i) {
                banked_user_r8_r12_[i] = regs_[8 + i];
                regs_[8 + i] = banked_fiq_r8_r12_[i];
            }
        }
    }

    if (old_bank != new_bank) {
        banked_r13_[old_bank] = regs_[13];
        banked_r14_[old_bank] = regs_[14];
        regs_[13] = banked_r13_[new_bank];
        regs_[14] = banked_r14_[new_bank];
    }

    cpsr_ = (cpsr_ & ~kModeMask) | static_cast<u32>(new_mode);
}

void Arm60::set_cpsr(u32 value) {
    const Mode new_mode = static_cast<Mode>(value & kModeMask);
    switch_mode(new_mode);
    // switch_mode has already written the mode bits; take everything else.
    cpsr_ = (value & ~kModeMask) | (cpsr_ & kModeMask);
}

void Arm60::enter_exception(u32 vector, Mode new_mode, u32 return_address,
                            bool disable_fiq) {
    const u32 saved_cpsr = cpsr_;
    switch_mode(new_mode);
    banked_spsr_[bank_index(new_mode)] = saved_cpsr;

    regs_[14] = return_address;
    cpsr_ |= kFlagI;
    if (disable_fiq) {
        cpsr_ |= kFlagF;
    }
    write_pc(vector);
}

void Arm60::check_interrupts() {
    if (fiq_line_ && (cpsr_ & kFlagF) == 0) {
        // R14_fiq holds the address of the instruction after the one that was
        // interrupted, plus the pipeline offset.
        ++fiqs_taken_;
        enter_exception(kVectorFiq, Mode::Fiq, regs_[15] + 4, true);
        return;
    }
    if ((irq_line_ || irq_latched_) && (cpsr_ & kFlagI) == 0) {
        // A latched edge is consumed; a held line is not.
        irq_latched_ = false;
        ++irqs_taken_;
        enter_exception(kVectorIrq, Mode::Irq, regs_[15] + 4, false);
    }
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------
Decoded Arm60::decode(u32 instruction) {
    Decoded d{};
    d.raw   = instruction;
    d.cond  = static_cast<u8>(instruction >> 28);
    d.flags = 0;

    // Order matters: the more specific encodings share bit patterns with the
    // data-processing space and have to be tested first.
    if ((instruction & 0x0fc000f0u) == 0x00000090u) {
        d.op = static_cast<u8>(Op::Multiply);
    } else if ((instruction & 0x0fb00ff0u) == 0x01000090u) {
        d.op = static_cast<u8>(Op::Swap);
    } else if ((instruction & 0x0fbf0fffu) == 0x010f0000u) {
        d.op = static_cast<u8>(Op::MoveStatusToRegister);   // MRS
    } else if ((instruction & 0x0db0f000u) == 0x0120f000u) {
        d.op = static_cast<u8>(Op::MoveRegisterToStatus);   // MSR
    } else if ((instruction & 0x0f000000u) == 0x0f000000u) {
        d.op = static_cast<u8>(Op::SoftwareInterrupt);
    } else if ((instruction & 0x0e000000u) == 0x0a000000u) {
        d.op = static_cast<u8>(Op::Branch);
    } else if ((instruction & 0x0e000000u) == 0x08000000u) {
        d.op = static_cast<u8>(Op::BlockDataTransfer);
    } else if ((instruction & 0x0c000000u) == 0x04000000u) {
        // Single data transfer. The undefined-instruction hole sits inside
        // this space: register-offset form with bit 4 set.
        if ((instruction & 0x0e000010u) == 0x06000010u) {
            d.op = static_cast<u8>(Op::Undefined);
        } else {
            d.op = static_cast<u8>(Op::SingleDataTransfer);
        }
    } else if ((instruction & 0x0c000000u) == 0x00000000u) {
        d.op = static_cast<u8>(Op::DataProcessing);
    } else {
        // Coprocessor space. The ARM60 in a 3DO has nothing attached, so these
        // take the undefined-instruction trap.
        d.op = static_cast<u8>(Op::Undefined);
    }

    return d;
}

const Decoded& Arm60::decoded_at(u32 address) {
    DecodeCache::Page& page = cache_->page_for(address);
    const u32 slot = (address % DecodeCache::kPageBytes) / 4;

    if (page.stamp[slot] != page.generation) {
        ++decodes_;
        page.entries[slot] = decode(bus_.fetch32(address));
        page.stamp[slot] = page.generation;
    }
    return page.entries[slot];
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------
bool Arm60::condition_passes(u8 cond) const {
    const bool n = (cpsr_ & kFlagN) != 0;
    const bool z = (cpsr_ & kFlagZ) != 0;
    const bool c = (cpsr_ & kFlagC) != 0;
    const bool v = (cpsr_ & kFlagV) != 0;

    switch (cond) {
        case 0x0: return z;                 // EQ
        case 0x1: return !z;                // NE
        case 0x2: return c;                 // CS/HS
        case 0x3: return !c;                // CC/LO
        case 0x4: return n;                 // MI
        case 0x5: return !n;                // PL
        case 0x6: return v;                 // VS
        case 0x7: return !v;                // VC
        case 0x8: return c && !z;           // HI
        case 0x9: return !c || z;           // LS
        case 0xa: return n == v;            // GE
        case 0xb: return n != v;            // LT
        case 0xc: return !z && (n == v);    // GT
        case 0xd: return z || (n != v);     // LE
        case 0xe: return true;              // AL
        default:  return false;             // NV — never, on ARMv3
    }
}

// ---------------------------------------------------------------------------
// Barrel shifter
// ---------------------------------------------------------------------------
u32 Arm60::shift_operand(u32 value, unsigned type, unsigned amount,
                         bool amount_from_register, bool& carry_out) const {
    if (amount_from_register && amount == 0) {
        // A register-specified shift of zero leaves both value and carry alone,
        // for every shift type.
        return value;
    }

    switch (type) {
        case 0:  // LSL
            if (amount == 0) {
                return value;  // immediate LSL #0 is "no shift"
            }
            if (amount < 32) {
                carry_out = ((value >> (32 - amount)) & 1u) != 0;
                return value << amount;
            }
            carry_out = (amount == 32) && ((value & 1u) != 0);
            return 0;

        case 1:  // LSR
            // An immediate LSR #0 encodes LSR #32.
            if (amount == 0 || amount == 32) {
                carry_out = (value & 0x80000000u) != 0;
                return 0;
            }
            if (amount < 32) {
                carry_out = ((value >> (amount - 1)) & 1u) != 0;
                return value >> amount;
            }
            carry_out = false;
            return 0;

        case 2: {  // ASR
            const u32 sign = (value & 0x80000000u) ? 0xffffffffu : 0u;
            // An immediate ASR #0 encodes ASR #32.
            if (amount == 0 || amount >= 32) {
                carry_out = sign != 0;
                return sign;
            }
            carry_out = ((static_cast<s32>(value) >> (amount - 1)) & 1) != 0;
            return static_cast<u32>(static_cast<s32>(value) >> amount);
        }

        case 3:  // ROR / RRX
        default:
            if (amount == 0) {
                // Immediate ROR #0 encodes RRX: a 33-bit rotate through carry.
                const bool old_carry = (cpsr_ & kFlagC) != 0;
                carry_out = (value & 1u) != 0;
                return (value >> 1) | (old_carry ? 0x80000000u : 0u);
            }
            {
                const unsigned rot = amount & 31u;
                if (rot == 0) {
                    // ROR by a multiple of 32 leaves the value, carry from bit 31.
                    carry_out = (value & 0x80000000u) != 0;
                    return value;
                }
                carry_out = ((value >> (rot - 1)) & 1u) != 0;
                return ror32(value, rot);
            }
    }
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
u32 Arm60::execute(const Decoded& d) {
    switch (static_cast<Op>(d.op)) {
        case Op::DataProcessing:        return exec_data_processing(d);
        case Op::Multiply:              return exec_multiply(d);
        case Op::SingleDataTransfer:    return exec_single_data_transfer(d);
        case Op::BlockDataTransfer:     return exec_block_data_transfer(d);
        case Op::Branch:                return exec_branch(d);
        case Op::SoftwareInterrupt:     return exec_swi(d);
        case Op::Swap:                  return exec_swap(d);
        case Op::MoveStatusToRegister:  return exec_mrs(d);
        case Op::MoveRegisterToStatus:  return exec_msr(d);
        case Op::Undefined:
        default:                        return exec_undefined(d);
    }
}

#if RETRO3DO_TRACING
namespace {
// A raw stream of executed addresses, off unless PCTRACE names a file. Diffing
// this against a machine known to work is how you find where two otherwise
// identical builds stopped agreeing, and it is worth keeping around: it is what
// located the reset-cause and timer-rate bugs.
//
// Resolved once at static-init rather than through a function-local static, so
// the cost in the instruction loop is one predictable branch on a null pointer
// rather than a guard-variable check per instruction.
std::FILE* const g_pc_trace = [] {
    const char* path = std::getenv("PCTRACE");
    return path != nullptr ? std::fopen(path, "wb") : nullptr;
}();

// A stall shows up long after boot, and the whole trace from reset is far too
// large to keep. PCTRACESKIP discards that many instructions first.
const u64 g_pc_trace_skip = [] {
    const char* skip = std::getenv("PCTRACESKIP");
    return skip != nullptr ? std::strtoull(skip, nullptr, 10) : 0ull;
}();
u64 g_pc_trace_seen = 0;
}  // namespace
#endif

u32 Arm60::step() {
    const u32 address = regs_[15];
#if RETRO3DO_TRACING
    if (g_pc_trace != nullptr) {
        if (g_pc_trace_seen++ >= g_pc_trace_skip) {
            std::fwrite(&address, 4, 1, g_pc_trace);
        }
    }
#endif
    if (exec_map_ != nullptr && address < 0x00100000u) {
        const u32 word = address >> 2;
        exec_map_[word >> 3] |= static_cast<u8>(1u << (word & 7u));
    }
    const Decoded& d = decoded_at(address);

    // The ARM pipeline makes PC read as the instruction address plus eight.
    regs_[15] = address + 8;
    pc_written_ = false;

    u32 cycles = kSeq;
    if (condition_passes(d.cond)) {
        cycles = execute(d);
    }

    if (!pc_written_) {
        regs_[15] = address + 4;
    }

    total_cycles_ += cycles;
    check_interrupts();
    return cycles;
}

u32 Arm60::run(u32 cycles) {
    u32 spent = 0;
    while (spent < cycles) {
        spent += step();
    }
    return spent;
}

// --- data processing -------------------------------------------------------
u32 Arm60::exec_data_processing(const Decoded& d) {
    const u32 instruction = d.raw;
    const bool immediate  = (instruction & (1u << 25)) != 0;
    const unsigned opcode = (instruction >> 21) & 0xfu;
    const bool set_flags  = (instruction & (1u << 20)) != 0;
    const unsigned rn     = (instruction >> 16) & 0xfu;
    const unsigned rd     = (instruction >> 12) & 0xfu;

    bool carry = (cpsr_ & kFlagC) != 0;
    u32 operand2;
    u32 cycles = 1;

    if (immediate) {
        const u32 value  = instruction & 0xffu;
        const unsigned rot = ((instruction >> 8) & 0xfu) * 2u;
        operand2 = ror32(value, rot);
        if (rot != 0) {
            carry = (operand2 & 0x80000000u) != 0;
        }
    } else {
        const unsigned rm = instruction & 0xfu;
        const unsigned shift_type = (instruction >> 5) & 0x3u;
        const bool by_register = (instruction & (1u << 4)) != 0;

        unsigned amount;
        u32 rm_value = regs_[rm];
        if (by_register) {
            const unsigned rs = (instruction >> 8) & 0xfu;
            amount = regs_[rs] & 0xffu;
            // A register-specified shift costs an extra cycle, and PC reads as
            // twelve ahead rather than eight because of the extra pipeline slot.
            if (rm == 15) rm_value += 4;
            cycles += kInternal;   // the shift amount comes from a register
        } else {
            amount = (instruction >> 7) & 0x1fu;
        }
        operand2 = shift_operand(rm_value, shift_type, amount, by_register, carry);
    }

    u32 rn_value = regs_[rn];
    if (rn == 15 && !immediate && (instruction & (1u << 4)) != 0) {
        rn_value += 4;
    }

    u32 result = 0;
    bool result_carry = carry;
    bool overflow = (cpsr_ & kFlagV) != 0;

    auto add_with_carry = [&](u32 a, u32 b, u32 carry_in) {
        const u64 wide = static_cast<u64>(a) + static_cast<u64>(b) + carry_in;
        const u32 sum = static_cast<u32>(wide);
        result_carry = (wide >> 32) != 0;
        overflow = (~(a ^ b) & (a ^ sum) & 0x80000000u) != 0;
        return sum;
    };

    switch (opcode) {
        case kAnd: result = rn_value & operand2; break;
        case kEor: result = rn_value ^ operand2; break;
        case kSub: result = add_with_carry(rn_value, ~operand2, 1); break;
        case kRsb: result = add_with_carry(operand2, ~rn_value, 1); break;
        case kAdd: result = add_with_carry(rn_value, operand2, 0); break;
        case kAdc: result = add_with_carry(rn_value, operand2,
                                           (cpsr_ & kFlagC) ? 1 : 0); break;
        case kSbc: result = add_with_carry(rn_value, ~operand2,
                                           (cpsr_ & kFlagC) ? 1 : 0); break;
        case kRsc: result = add_with_carry(operand2, ~rn_value,
                                           (cpsr_ & kFlagC) ? 1 : 0); break;
        case kTst: result = rn_value & operand2; break;
        case kTeq: result = rn_value ^ operand2; break;
        case kCmp: result = add_with_carry(rn_value, ~operand2, 1); break;
        case kCmn: result = add_with_carry(rn_value, operand2, 0); break;
        case kOrr: result = rn_value | operand2; break;
        case kMov: result = operand2; break;
        case kBic: result = rn_value & ~operand2; break;
        case kMvn: result = ~operand2; break;
        default: break;
    }

    if (set_flags) {
        if (rd == 15) {
            // S with PC as destination means "return from exception": restore
            // CPSR from the current mode's SPSR.
            if (mode_has_spsr(mode())) {
                set_cpsr(banked_spsr_[bank_index(mode())]);
            }
        } else {
            u32 flags = cpsr_ & ~(kFlagN | kFlagZ | kFlagC | kFlagV);
            if (result & 0x80000000u) flags |= kFlagN;
            if (result == 0)          flags |= kFlagZ;

            const bool logical = opcode == kAnd || opcode == kEor ||
                                 opcode == kTst || opcode == kTeq ||
                                 opcode == kOrr || opcode == kMov ||
                                 opcode == kBic || opcode == kMvn;
            if (logical) {
                if (carry) flags |= kFlagC;
                flags |= (cpsr_ & kFlagV);
            } else {
                if (result_carry) flags |= kFlagC;
                if (overflow)     flags |= kFlagV;
            }
            cpsr_ = flags;
        }
    }

    if (opcode_writes_result(opcode)) {
        if (rd == 15) {
            write_pc(result);
            cycles += kSeq + kNonSeq;   // pipeline refill
        } else {
            regs_[rd] = result;
        }
    }

    return cycles;
}

// --- multiply --------------------------------------------------------------
u32 Arm60::exec_multiply(const Decoded& d) {
    const u32 instruction = d.raw;
    const bool accumulate = (instruction & (1u << 21)) != 0;
    const bool set_flags  = (instruction & (1u << 20)) != 0;
    const unsigned rd = (instruction >> 16) & 0xfu;
    const unsigned rn = (instruction >> 12) & 0xfu;
    const unsigned rs = (instruction >> 8) & 0xfu;
    const unsigned rm = instruction & 0xfu;

    u32 result = regs_[rm] * regs_[rs];
    if (accumulate) {
        result += regs_[rn];
    }
    if (rd == 15) {
        write_pc(result);
    } else {
        regs_[rd] = result;
    }

    if (set_flags) {
        u32 flags = cpsr_ & ~(kFlagN | kFlagZ);
        if (result & 0x80000000u) flags |= kFlagN;
        if (result == 0)          flags |= kFlagZ;
        // The carry flag is architecturally unpredictable after a multiply;
        // leaving it untouched is the behaviour software can actually rely on.
        cpsr_ = flags;
    }

    // Cost depends on how many significant bytes the multiplier holds: the
    // Booth's-algorithm early-out that real silicon performs.
    const u32 operand = regs_[rs];
    // 1S plus an internal cycle for each byte of the multiplier that still has
    // significant bits in it - the hardware stops early on small operands.
    u32 internal = 4;
    if      ((operand & 0xffffff00u) == 0 || (operand & 0xffffff00u) == 0xffffff00u) internal = 1;
    else if ((operand & 0xffff0000u) == 0 || (operand & 0xffff0000u) == 0xffff0000u) internal = 2;
    else if ((operand & 0xff000000u) == 0 || (operand & 0xff000000u) == 0xff000000u) internal = 3;
    u32 cycles = kSeq + internal * kInternal;
    if (accumulate) cycles += kInternal;
    return cycles;
}

// --- single data transfer --------------------------------------------------
u32 Arm60::exec_single_data_transfer(const Decoded& d) {
    const u32 instruction = d.raw;
    const bool register_offset = (instruction & (1u << 25)) != 0;
    const bool pre_index       = (instruction & (1u << 24)) != 0;
    const bool add             = (instruction & (1u << 23)) != 0;
    const bool byte_access     = (instruction & (1u << 22)) != 0;
    const bool write_back      = (instruction & (1u << 21)) != 0;
    const bool load            = (instruction & (1u << 20)) != 0;
    const unsigned rn = (instruction >> 16) & 0xfu;
    const unsigned rd = (instruction >> 12) & 0xfu;

    u32 offset;
    if (register_offset) {
        const unsigned rm = instruction & 0xfu;
        const unsigned shift_type = (instruction >> 5) & 0x3u;
        const unsigned amount = (instruction >> 7) & 0x1fu;
        bool ignored_carry = (cpsr_ & kFlagC) != 0;
        offset = shift_operand(regs_[rm], shift_type, amount, false, ignored_carry);
    } else {
        offset = instruction & 0xfffu;
    }

    u32 base = regs_[rn];
    u32 address = base;
    if (pre_index) {
        address = add ? base + offset : base - offset;
    }

    u32 cycles = kSeq + kNonSeq + kInternal;

    if (load) {
        u32 value;
        if (byte_access) {
            value = bus_.read8(address);
        } else {
            // An unaligned word load rotates the addressed word so the wanted
            // byte lands in the low bits, rather than faulting.
            value = ror32(bus_.read32(address), (address & 3u) * 8u);
        }

        // Write-back happens before the loaded value lands, which matters when
        // base and destination are the same register.
        if (!pre_index) {
            const u32 post = add ? base + offset : base - offset;
            regs_[rn] = post;
        } else if (write_back) {
            regs_[rn] = address;
        }

        if (rd == 15) {
            write_pc(value);
            cycles += kSeq + kNonSeq;
        } else {
            regs_[rd] = value;
        }
    } else {
        // Storing PC writes the value twelve ahead of the instruction.
        u32 value = regs_[rd];
        if (rd == 15) value += 4;

        if (byte_access) {
            bus_.write8(address, static_cast<u8>(value));
        } else {
            bus_.write32(address, value);
        }

        if (!pre_index) {
            regs_[rn] = add ? base + offset : base - offset;
        } else if (write_back) {
            regs_[rn] = address;
        }
        cycles = 2 * kNonSeq;
    }

    return cycles;
}

// --- block data transfer ---------------------------------------------------
u32 Arm60::exec_block_data_transfer(const Decoded& d) {
    const u32 instruction = d.raw;
    const bool pre_index  = (instruction & (1u << 24)) != 0;
    const bool add        = (instruction & (1u << 23)) != 0;
    const bool use_user_bank = (instruction & (1u << 22)) != 0;
    const bool write_back = (instruction & (1u << 21)) != 0;
    const bool load       = (instruction & (1u << 20)) != 0;
    const unsigned rn = (instruction >> 16) & 0xfu;
    const u32 list = instruction & 0xffffu;

    // An empty register list is architecturally unpredictable; treat it as a
    // no-op rather than transferring sixteen registers by accident.
    if (list == 0) {
        return 2;
    }

    unsigned count = 0;
    for (unsigned i = 0; i < 16; ++i) {
        if (list & (1u << i)) ++count;
    }

    const u32 base = regs_[rn];
    const u32 total = count * 4u;

    // Transfers always run from the lowest address upwards, whichever way the
    // addressing mode points; work out where the block starts.
    u32 address;
    if (add) {
        address = pre_index ? base + 4 : base;
    } else {
        address = pre_index ? base - total : base - total + 4;
    }
    const u32 final_base = add ? base + total : base - total;

    // The S bit with PC absent means "use the user-mode bank"; save the real
    // mode and pretend to be in user mode for the duration.
    const bool pc_in_list = (list & (1u << 15)) != 0;
    const Mode saved_mode = mode();
    const bool bank_swap = use_user_bank && !(load && pc_in_list);
    if (bank_swap && saved_mode != Mode::User && saved_mode != Mode::System) {
        switch_mode(Mode::User);
    }

    if (load) {
        // Write-back is applied before the loads so that a base register also
        // present in the list ends up holding the loaded value, not the address.
        if (write_back) {
            regs_[rn] = final_base;
        }
        for (unsigned i = 0; i < 16; ++i) {
            if ((list & (1u << i)) == 0) continue;
            const u32 loaded = bus_.read32(address);
            if (i == 15) {
                write_pc(loaded);
            } else {
                regs_[i] = loaded;
            }
            address += 4;
        }
        if (pc_in_list && use_user_bank && mode_has_spsr(saved_mode)) {
            // LDM with PC and the S bit restores CPSR — the usual way out of an
            // exception handler.
            set_cpsr(banked_spsr_[bank_index(saved_mode)]);
        }
    } else {
        for (unsigned i = 0; i < 16; ++i) {
            if ((list & (1u << i)) == 0) continue;
            u32 value = regs_[i];
            if (i == 15) value += 4;
            bus_.write32(address, value);
            address += 4;
            // Write-back takes effect after the first register is stored, so a
            // base in the list stores its original value only if it is lowest.
            if (write_back && i == rn) {
                regs_[rn] = final_base;
            }
        }
        if (write_back) {
            regs_[rn] = final_base;
        }
    }

    if (bank_swap && saved_mode != Mode::User && saved_mode != Mode::System) {
        switch_mode(saved_mode);
    }

    u32 cycles = load ? (count * kSeq + kNonSeq + kInternal)
                      : ((count > 0 ? count - 1 : 0) * kSeq + 2 * kNonSeq);
    if (load && pc_in_list) cycles += kSeq + kNonSeq;
    return cycles;
}

// --- branch ----------------------------------------------------------------
u32 Arm60::exec_branch(const Decoded& d) {
    const u32 instruction = d.raw;
    const bool link = (instruction & (1u << 24)) != 0;

    // 24-bit signed word offset, sign-extended and scaled.
    s32 offset = static_cast<s32>(instruction << 8) >> 6;

    if (link) {
        // regs_[15] currently reads as the instruction address plus eight, so
        // the return address is four back from it.
        regs_[14] = regs_[15] - 4;
    }
    write_pc(static_cast<u32>(static_cast<s32>(regs_[15]) + offset));
    return 2 * kSeq + kNonSeq;
}

// --- SWI, SWP, MRS, MSR, undefined ----------------------------------------
u32 Arm60::exec_swi(const Decoded& d) {
    (void)d;
    enter_exception(kVectorSwi, Mode::Supervisor, regs_[15] - 4, false);
    return 3;
}

u32 Arm60::exec_swap(const Decoded& d) {
    const u32 instruction = d.raw;
    const bool byte_access = (instruction & (1u << 22)) != 0;
    const unsigned rn = (instruction >> 16) & 0xfu;
    const unsigned rd = (instruction >> 12) & 0xfu;
    const unsigned rm = instruction & 0xfu;

    const u32 address = regs_[rn];
    if (byte_access) {
        const u32 old = bus_.read8(address);
        bus_.write8(address, static_cast<u8>(regs_[rm]));
        if (rd == 15) write_pc(old); else regs_[rd] = old;
    } else {
        const u32 old = ror32(bus_.read32(address), (address & 3u) * 8u);
        bus_.write32(address, regs_[rm]);
        if (rd == 15) write_pc(old); else regs_[rd] = old;
    }
    return 4;
}

u32 Arm60::exec_mrs(const Decoded& d) {
    const u32 instruction = d.raw;
    const bool from_spsr = (instruction & (1u << 22)) != 0;
    const unsigned rd = (instruction >> 12) & 0xfu;
    const u32 status = from_spsr ? spsr() : cpsr_;
    if (rd == 15) write_pc(status); else regs_[rd] = status;
    return 1;
}

u32 Arm60::exec_msr(const Decoded& d) {
    const u32 instruction = d.raw;
    const bool to_spsr   = (instruction & (1u << 22)) != 0;
    const bool immediate = (instruction & (1u << 25)) != 0;
    // Bit 16 distinguishes "whole register" from "flags only".
    const bool flags_only = (instruction & (1u << 16)) == 0;

    u32 value;
    if (immediate) {
        const u32 raw_value = instruction & 0xffu;
        const unsigned rot = ((instruction >> 8) & 0xfu) * 2u;
        value = ror32(raw_value, rot);
    } else {
        value = regs_[instruction & 0xfu];
    }

    const Mode current = mode();
    const bool privileged = current != Mode::User;

    if (to_spsr) {
        if (mode_has_spsr(current)) {
            u32& target = banked_spsr_[bank_index(current)];
            target = flags_only ? ((target & 0x0fffffffu) | (value & 0xf0000000u))
                                : value;
        }
    } else if (flags_only || !privileged) {
        // User mode may only change the condition flags, whatever it asks for.
        cpsr_ = (cpsr_ & 0x0fffffffu) | (value & 0xf0000000u);
    } else {
        set_cpsr(value);
    }
    return 1;
}

u32 Arm60::exec_undefined(const Decoded& d) {
    (void)d;
    enter_exception(kVectorUndefined, Mode::Undefined, regs_[15] - 4, false);
    return 3;
}

}  // namespace retro3do
