// ARM60 CPU — the 3DO's main processor.
//
// The ARM60 is an ARM6-family core running the ARMv3 instruction set at
// 12.5 MHz: 32-bit data, 32-bit addressing, no Thumb, no long multiply, no
// coprocessor 15 to speak of. Everything implemented here comes from ARM's own
// publicly documented architecture; see docs/CLEANROOM.md.
//
// Design note — why this is not a plain switch interpreter
// -------------------------------------------------------
// The obvious shape for an ARM interpreter is "fetch a word, switch on the
// top nibble, do the thing", which re-derives the same facts about the same
// instruction every single time it executes. Game code is overwhelmingly loops,
// so that work is almost entirely repeated.
//
// Instead each instruction is decoded ONCE into a `Decoded` record — the handler
// to call plus its already-extracted operands — and cached, keyed by address.
// Executing is then a load and an indirect call, with no bit-twiddling in the
// hot path. The cache is invalidated wholesale when code memory is written,
// which for this machine is rare and easy to detect.
//
// This is the piece of the old core's design that made it slow on phones, and
// it is much easier to build in from the start than to retrofit.
#pragma once

#include "types.h"

namespace retro3do {

class Bus;

// ---------------------------------------------------------------------------
// Processor modes. The low five bits of the CPSR.
// ---------------------------------------------------------------------------
enum class Mode : u32 {
    User       = 0x10,
    Fiq        = 0x11,
    Irq        = 0x12,
    Supervisor = 0x13,
    Abort      = 0x17,
    Undefined  = 0x1b,
    System     = 0x1f,
};

// CPSR condition and control bits.
enum : u32 {
    kFlagN = 1u << 31,  // negative
    kFlagZ = 1u << 30,  // zero
    kFlagC = 1u << 29,  // carry
    kFlagV = 1u << 28,  // overflow
    kFlagI = 1u << 7,   // IRQ disable
    kFlagF = 1u << 6,   // FIQ disable
    kFlagT = 1u << 5,   // Thumb — never set on ARM60, but the bit exists
    kModeMask = 0x1fu,
};

// Exception vector addresses. ARM's, not the 3DO's.
enum : u32 {
    kVectorReset         = 0x00000000,
    kVectorUndefined     = 0x00000004,
    kVectorSwi           = 0x00000008,
    kVectorPrefetchAbort = 0x0000000c,
    kVectorDataAbort     = 0x00000010,
    kVectorAddressExc    = 0x00000014,  // ARMv3 26-bit address exception
    kVectorIrq           = 0x00000018,
    kVectorFiq           = 0x0000001c,
};

// ---------------------------------------------------------------------------
// Decoded instruction cache
// ---------------------------------------------------------------------------

// The instruction classes we dispatch on. Kept small and dense so the jump
// table stays in cache.
enum class Op : u8 {
    Undefined = 0,
    DataProcessing,
    Multiply,
    SingleDataTransfer,
    BlockDataTransfer,
    Branch,
    SoftwareInterrupt,
    Swap,
    MoveStatusToRegister,   // MRS
    MoveRegisterToStatus,   // MSR
};

// One decoded instruction. Deliberately 12 bytes and trivially copyable: this
// array is the single largest thing the CPU touches, so its density matters
// more than its readability.
struct Decoded {
    u32 raw;        // the original word, for operand bits handlers still need
    u8  op;         // Op
    u8  cond;       // condition field, bits 31..28
    u16 flags;      // per-op predecoded booleans (see the handlers)
};

// ---------------------------------------------------------------------------
// The CPU
// ---------------------------------------------------------------------------
class Arm60 {
public:
    explicit Arm60(Bus& bus);
    ~Arm60();

    Arm60(const Arm60&) = delete;
    Arm60& operator=(const Arm60&) = delete;

    // Return the CPU to its power-on state and enter the reset vector.
    void reset();

    // Run until at least `cycles` have elapsed. Returns the number actually
    // run, which may overshoot by up to one instruction. Running in blocks
    // rather than single-stepping is what keeps the dispatch overhead amortised.
    u32 run(u32 cycles);

    // Execute exactly one instruction. Returns its cycle count. Used by tests
    // and the debugger; the emulator proper calls run().
    u32 step();

    // The interrupt inputs, in both flavours.
    //
    // `set_irq` holds the line, which is the textbook model: while it is high
    // and unmasked the CPU keeps taking the exception, so a handler that does
    // not acknowledge its source is re-entered forever.
    //
    // `signal_irq` latches a single edge instead, cleared when the CPU takes
    // it. CLIO uses this one. That is not a simplification but what the boot
    // ROM requires: its vertical-blank handler reads a software flag, returns,
    // and never touches a CLIO register to acknowledge anything. Held level
    // would livelock the machine on its own startup interrupt.
    void set_irq(bool asserted) { irq_line_ = asserted; }
    void signal_irq() { irq_latched_ = true; }
    void set_fiq(bool asserted) { fiq_line_ = asserted; }

    // Invalidate the decode cache. Must be called whenever memory that could
    // hold instructions changes underneath us — DMA into DRAM, a new BIOS, a
    // save-state load.
    void invalidate_decode_cache();
    void invalidate_decode_cache(u32 address, u32 length);

    // --- register access -------------------------------------------------
    u32  reg(unsigned index) const { return regs_[index]; }
    void set_reg(unsigned index, u32 value);

    u32  pc() const { return regs_[15]; }
    u32  cpsr() const { return cpsr_; }
    void set_cpsr(u32 value);
    u32  spsr() const;

    Mode mode() const { return static_cast<Mode>(cpsr_ & kModeMask); }

    // Execution map: one bit per word of DRAM, set the first time an address is
    // executed. Null disables it and costs one predictable branch.
    //
    // This is the tool for "what did the machine do, and when did it stop doing
    // it". Sampling the PC at frame boundaries answers neither - a boot sequence
    // is thousands of functions deep and the interesting one runs for a few
    // hundred microseconds, so it is never the address you happen to catch.
    void set_execution_map(u8* bits) { exec_map_ = bits; }

    // How many IRQ exceptions have been taken since reset. A bring-up
    // diagnostic: "is the handler running at all" is otherwise very hard to
    // tell apart from "the handler runs and does nothing useful", and the two
    // have completely different causes.
    u64 irqs_taken() const { return irqs_taken_; }

    // CLIO drives FIQ, so this is the one that says whether the machine is
    // being serviced at all.
    u64 fiqs_taken() const { return fiqs_taken_; }

    // Cycles executed since reset. The scheduler drives everything from this.
    u64 total_cycles() const { return total_cycles_; }

private:
    // --- decode ----------------------------------------------------------
    static Decoded decode(u32 instruction);
    const Decoded& decoded_at(u32 address);

    // --- execution -------------------------------------------------------
    bool condition_passes(u8 cond) const;
    u32  execute(const Decoded& d);

    u32 exec_data_processing(const Decoded& d);
    u32 exec_multiply(const Decoded& d);
    u32 exec_single_data_transfer(const Decoded& d);
    u32 exec_block_data_transfer(const Decoded& d);
    u32 exec_branch(const Decoded& d);
    u32 exec_swi(const Decoded& d);
    u32 exec_swap(const Decoded& d);
    u32 exec_mrs(const Decoded& d);
    u32 exec_msr(const Decoded& d);
    u32 exec_undefined(const Decoded& d);

    // Barrel shifter. Updates carry_out when the shift defines a carry.
    u32 shift_operand(u32 value, unsigned type, unsigned amount,
                      bool amount_from_register, bool& carry_out) const;

    // --- exceptions and banking ------------------------------------------
    void enter_exception(u32 vector, Mode new_mode, u32 return_address,
                         bool disable_fiq);
    void switch_mode(Mode new_mode);
    void check_interrupts();

    // --- state -----------------------------------------------------------
    Bus& bus_;

    u32 regs_[16] = {};
    u32 cpsr_ = 0;

    // Banked registers. ARMv3 banks R13/R14 for every privileged mode, and
    // R8..R12 additionally for FIQ. Index by mode via bank_index().
    u32 banked_r13_[6] = {};
    u32 banked_r14_[6] = {};
    u32 banked_spsr_[6] = {};
    u32 banked_fiq_r8_r12_[5] = {};
    u32 banked_user_r8_r12_[5] = {};

    // Set by any handler that writes PC. Inferring "a branch happened" by
    // comparing PC against the value we installed does not work: a branch whose
    // offset is zero lands exactly where the pipeline already pointed, and would
    // be mistaken for a fall-through.
    void write_pc(u32 value) {
        regs_[15] = value;
        pc_written_ = true;
    }

    bool pc_written_ = false;
    bool irq_line_ = false;
    bool irq_latched_ = false;
    bool fiq_line_ = false;

    u64 total_cycles_ = 0;
    u64 irqs_taken_ = 0;
    u64 fiqs_taken_ = 0;
    u8* exec_map_ = nullptr;

    // Decode cache, one entry per instruction slot of addressable code memory.
    // Allocated lazily per region rather than for the whole 4 GB space.
    struct DecodeCache;
    DecodeCache* cache_ = nullptr;
};

}  // namespace retro3do
