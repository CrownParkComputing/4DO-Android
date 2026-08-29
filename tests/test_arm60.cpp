// ARM60 instruction tests.
//
// Each test assembles a short program as raw words, loads it where the reset
// vector points, and single-steps it. Encodings are written out longhand so
// that a failure points at a specific bit field rather than at an assembler.
#include "core/arm60.h"
#include "core/bus.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

// A machine with a program in ROM, ready to step from the reset vector.
struct Machine {
    Bus bus;
    Arm60 cpu{bus};

    explicit Machine(std::initializer_list<u32> program) {
        std::vector<u8> rom(program.size() * 4);
        size_t offset = 0;
        for (u32 word : program) {
            rom[offset++] = static_cast<u8>(word >> 24);
            rom[offset++] = static_cast<u8>(word >> 16);
            rom[offset++] = static_cast<u8>(word >> 8);
            rom[offset++] = static_cast<u8>(word);
        }
        bus.load_bios(rom.data(), rom.size());
        cpu.reset();
    }

    void step(int count = 1) {
        for (int i = 0; i < count; ++i) cpu.step();
    }
};

constexpr u32 kAlways = 0xEu << 28;

}  // namespace

// ---------------------------------------------------------------------------
// Data processing
// ---------------------------------------------------------------------------

TEST(mov_immediate_loads_a_register) {
    // MOV r0, #0x42
    Machine m{kAlways | (0x3Au << 20) | (0u << 12) | 0x42u};
    m.step();
    CHECK_EQ(m.cpu.reg(0), 0x42u);
}

TEST(mov_immediate_rotates_the_operand) {
    // MOV r1, #0xFF000000  -- 0xFF rotated right by 8
    Machine m{kAlways | (0x3Au << 20) | (1u << 12) | (4u << 8) | 0xFFu};
    m.step();
    CHECK_EQ(m.cpu.reg(1), 0xFF000000u);
}

TEST(add_computes_and_sets_flags) {
    Machine m{
        kAlways | (0x3Au << 20) | (0u << 12) | 0xFFu,          // MOV r0, #0xFF
        kAlways | (0x29u << 20) | (0u << 16) | (1u << 12) | 1u,  // ADDS r1, r0, #1
    };
    m.step(2);
    CHECK_EQ(m.cpu.reg(1), 0x100u);
    CHECK((m.cpu.cpsr() & kFlagZ) == 0);
    CHECK((m.cpu.cpsr() & kFlagN) == 0);
}

TEST(subs_to_zero_sets_the_zero_flag) {
    Machine m{
        kAlways | (0x3Au << 20) | (0u << 12) | 7u,             // MOV r0, #7
        kAlways | (0x25u << 20) | (0u << 16) | (1u << 12) | 7u,  // SUBS r1, r0, #7
    };
    m.step(2);
    CHECK_EQ(m.cpu.reg(1), 0u);
    CHECK((m.cpu.cpsr() & kFlagZ) != 0);
    // A subtraction that does not borrow sets carry, ARM's convention.
    CHECK((m.cpu.cpsr() & kFlagC) != 0);
}

TEST(subs_that_borrows_clears_carry_and_sets_negative) {
    Machine m{
        kAlways | (0x3Au << 20) | (0u << 12) | 1u,             // MOV r0, #1
        kAlways | (0x25u << 20) | (0u << 16) | (1u << 12) | 2u,  // SUBS r1, r0, #2
    };
    m.step(2);
    CHECK_EQ(m.cpu.reg(1), 0xFFFFFFFFu);
    CHECK((m.cpu.cpsr() & kFlagN) != 0);
    CHECK((m.cpu.cpsr() & kFlagC) == 0);
}

TEST(overflow_is_signed_not_unsigned) {
    // 0x7FFFFFFF + 1 overflows as a signed value but does not carry. Getting
    // these two confused is the classic ARM flag bug, so they are asserted
    // together.
    Machine m{kAlways | (0x29u << 20) | (0u << 16) | (1u << 12) | 1u};  // ADDS r1,r0,#1
    m.cpu.set_reg(0, 0x7FFFFFFFu);
    m.step();
    CHECK_EQ(m.cpu.reg(1), 0x80000000u);
    CHECK((m.cpu.cpsr() & kFlagV) != 0);
    CHECK((m.cpu.cpsr() & kFlagC) == 0);
    CHECK((m.cpu.cpsr() & kFlagN) != 0);
}

TEST(unsigned_carry_without_signed_overflow) {
    // 0xFFFFFFFF + 1 carries but does not overflow — the mirror of the above.
    Machine m{kAlways | (0x29u << 20) | (0u << 16) | (1u << 12) | 1u};  // ADDS r1,r0,#1
    m.cpu.set_reg(0, 0xFFFFFFFFu);
    m.step();
    CHECK_EQ(m.cpu.reg(1), 0u);
    CHECK((m.cpu.cpsr() & kFlagC) != 0);
    CHECK((m.cpu.cpsr() & kFlagV) == 0);
    CHECK((m.cpu.cpsr() & kFlagZ) != 0);
}

TEST(cmp_does_not_write_its_destination) {
    Machine m{
        kAlways | (0x35u << 20) | (0u << 16) | (1u << 12) | 0u,  // CMP r0, #0
    };
    m.cpu.set_reg(1, 0xABCDu);
    m.step();
    CHECK_EQ(m.cpu.reg(1), 0xABCDu);
    CHECK((m.cpu.cpsr() & kFlagZ) != 0);
}

// ---------------------------------------------------------------------------
// Barrel shifter
// ---------------------------------------------------------------------------

TEST(logical_shift_left_by_register) {
    // MOV r2, r0, LSL r1
    Machine m{kAlways | (0x1Au << 20) | (2u << 12) | (1u << 8) | (0u << 5) |
              (1u << 4) | 0u};
    m.cpu.set_reg(0, 0x1u);
    m.cpu.set_reg(1, 4u);
    m.step();
    CHECK_EQ(m.cpu.reg(2), 0x10u);
}

TEST(arithmetic_shift_right_preserves_sign) {
    // MOV r2, r0, ASR #4
    Machine m{kAlways | (0x1Au << 20) | (2u << 12) | (4u << 7) | (2u << 5) | 0u};
    m.cpu.set_reg(0, 0xFFFFFF00u);
    m.step();
    CHECK_EQ(m.cpu.reg(2), 0xFFFFFFF0u);
}

TEST(immediate_lsr_zero_means_shift_by_thirty_two) {
    // MOV r2, r0, LSR #0  -- encodes LSR #32, so the result is zero
    Machine m{kAlways | (0x1Au << 20) | (2u << 12) | (0u << 7) | (1u << 5) | 0u};
    m.cpu.set_reg(0, 0x80000000u);
    m.step();
    CHECK_EQ(m.cpu.reg(2), 0u);
}

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------

TEST(branch_jumps_relative_to_the_pipeline) {
    // A branch is relative to PC, which reads eight ahead of the branch itself.
    // So an offset of zero already skips the next instruction — the pipeline
    // offset is the thing this test exists to pin down.
    Machine m{
        kAlways | (0xAu << 24) | 0u,                            // B  +0
        kAlways | (0x3Au << 20) | (0u << 12) | 0xAAu,           // MOV r0,#0xAA (skipped)
        kAlways | (0x3Au << 20) | (0u << 12) | 0xBBu,           // MOV r0,#0xBB
    };
    m.step();
    CHECK_EQ(m.cpu.pc(), kRomBase + 8u);
    m.step();
    CHECK_EQ(m.cpu.reg(0), 0xBBu);
}

TEST(branch_with_link_records_the_return_address) {
    Machine m{kAlways | (0xBu << 24) | 4u};  // BL forward
    m.step();
    CHECK_EQ(m.cpu.reg(14), kRomBase + 4u);
}

TEST(a_failed_condition_executes_nothing) {
    // MOVEQ r0, #1 with Z clear must not write r0.
    Machine m{(0x0u << 28) | (0x3Au << 20) | (0u << 12) | 1u};
    m.step();
    CHECK_EQ(m.cpu.reg(0), 0u);
}

// ---------------------------------------------------------------------------
// Loads and stores
// ---------------------------------------------------------------------------

TEST(store_then_load_a_word_through_dram) {
    Machine m{
        kAlways | (0x58u << 20) | (1u << 16) | (0u << 12) | 0u,  // STR r0,[r1]
        kAlways | (0x59u << 20) | (1u << 16) | (2u << 12) | 0u,  // LDR r2,[r1]
    };
    m.cpu.set_reg(0, 0xDEADBEEFu);
    m.cpu.set_reg(1, 0x1000u);
    m.step(2);
    CHECK_EQ(m.cpu.reg(2), 0xDEADBEEFu);
    CHECK_EQ(m.bus.read32(0x1000u), 0xDEADBEEFu);
}

TEST(byte_store_touches_only_one_byte) {
    Machine m{
        kAlways | (0x5Cu << 20) | (1u << 16) | (0u << 12) | 0u,  // STRB r0,[r1]
    };
    m.bus.write32(0x2000u, 0x11223344u);
    m.cpu.set_reg(0, 0xFFu);
    m.cpu.set_reg(1, 0x2000u);
    m.step();
    // Big-endian: the byte at address 0x2000 is the most significant one.
    CHECK_EQ(m.bus.read32(0x2000u), 0xFF223344u);
}

TEST(post_indexed_load_writes_the_base_back) {
    // LDR r2, [r1], #4
    Machine m{kAlways | (0x49u << 20) | (1u << 16) | (2u << 12) | 4u};
    m.bus.write32(0x3000u, 0x12345678u);
    m.cpu.set_reg(1, 0x3000u);
    m.step();
    CHECK_EQ(m.cpu.reg(2), 0x12345678u);
    CHECK_EQ(m.cpu.reg(1), 0x3004u);
}

TEST(block_store_and_load_round_trip) {
    // STMIA r4!, {r0-r2} then LDMIA r5!, {r6-r8}
    const u32 stm = kAlways | (0x8Au << 20) | (4u << 16) | 0x0007u;
    const u32 ldm = kAlways | (0x8Bu << 20) | (5u << 16) | 0x01C0u;
    Machine m{stm, ldm};
    m.cpu.set_reg(0, 0x1111u);
    m.cpu.set_reg(1, 0x2222u);
    m.cpu.set_reg(2, 0x3333u);
    m.cpu.set_reg(4, 0x4000u);
    m.cpu.set_reg(5, 0x4000u);
    m.step(2);
    CHECK_EQ(m.cpu.reg(6), 0x1111u);
    CHECK_EQ(m.cpu.reg(7), 0x2222u);
    CHECK_EQ(m.cpu.reg(8), 0x3333u);
    CHECK_EQ(m.cpu.reg(4), 0x400Cu);
    CHECK_EQ(m.cpu.reg(5), 0x400Cu);
}

// ---------------------------------------------------------------------------
// Multiply
// ---------------------------------------------------------------------------

TEST(multiply_produces_the_low_word) {
    // MUL r3, r0, r1   (rd=3, rs=1, rm=0)
    Machine m{kAlways | (0x00u << 20) | (3u << 16) | (1u << 8) | (9u << 4) | 0u};
    m.cpu.set_reg(0, 7u);
    m.cpu.set_reg(1, 6u);
    m.step();
    CHECK_EQ(m.cpu.reg(3), 42u);
}

// ---------------------------------------------------------------------------
// Status registers and exceptions
// ---------------------------------------------------------------------------

TEST(reset_enters_supervisor_mode_with_interrupts_masked) {
    Machine m{kAlways | (0x3Au << 20)};
    CHECK(m.cpu.mode() == Mode::Supervisor);
    CHECK((m.cpu.cpsr() & kFlagI) != 0);
    CHECK((m.cpu.cpsr() & kFlagF) != 0);
    CHECK_EQ(m.cpu.pc(), kRomBase);
}

TEST(mrs_reads_the_status_register) {
    // MRS r0, CPSR
    Machine m{kAlways | 0x010F0000u};
    m.step();
    CHECK_EQ(m.cpu.reg(0), m.cpu.cpsr());
}

TEST(software_interrupt_enters_supervisor_mode_at_the_vector) {
    Machine m{kAlways | (0xFu << 24) | 0x123456u};
    m.step();
    CHECK_EQ(m.cpu.pc(), kVectorSwi);
    CHECK(m.cpu.mode() == Mode::Supervisor);
    CHECK_EQ(m.cpu.reg(14), kRomBase + 4u);
}

TEST(an_unmasked_irq_is_taken_at_an_instruction_boundary) {
    Machine m{
        kAlways | (0x3Au << 20) | (0u << 12) | 1u,  // MOV r0,#1
        kAlways | (0x3Au << 20) | (0u << 12) | 2u,  // MOV r0,#2
    };
    // Clear the I bit so the line can be seen.
    m.cpu.set_cpsr((m.cpu.cpsr() & ~kFlagI));
    m.cpu.set_irq(true);
    m.step();
    CHECK_EQ(m.cpu.reg(0), 1u);
    CHECK_EQ(m.cpu.pc(), kVectorIrq);
    CHECK(m.cpu.mode() == Mode::Irq);
}

TEST(a_masked_irq_is_ignored) {
    Machine m{kAlways | (0x3Au << 20) | (0u << 12) | 1u};
    m.cpu.set_irq(true);  // I is still set from reset
    m.step();
    CHECK_EQ(m.cpu.pc(), kRomBase + 4u);
    CHECK(m.cpu.mode() == Mode::Supervisor);
}

TEST(banked_stack_pointers_survive_a_mode_change) {
    Machine m{kAlways | (0x3Au << 20)};
    m.cpu.set_reg(13, 0xAAAA0000u);          // r13_svc
    m.cpu.set_cpsr((m.cpu.cpsr() & ~kModeMask) | static_cast<u32>(Mode::Irq));
    m.cpu.set_reg(13, 0xBBBB0000u);          // r13_irq
    CHECK_EQ(m.cpu.reg(13), 0xBBBB0000u);
    m.cpu.set_cpsr((m.cpu.cpsr() & ~kModeMask) |
                   static_cast<u32>(Mode::Supervisor));
    CHECK_EQ(m.cpu.reg(13), 0xAAAA0000u);
}

// ---------------------------------------------------------------------------
// The decode cache
// ---------------------------------------------------------------------------

TEST(rewritten_code_is_not_executed_from_a_stale_decode) {
    // The point of the cache is that an instruction is decoded once. This test
    // exists to prove that overwriting code and invalidating still re-decodes,
    // because getting that wrong produces bugs that only appear after a game
    // has been running for a while.
    Bus bus;
    Arm60 cpu(bus);

    // Put MOV r0,#1 in DRAM, run it, then rewrite it to MOV r0,#2.
    const u32 mov_one = kAlways | (0x3Au << 20) | (0u << 12) | 1u;
    const u32 mov_two = kAlways | (0x3Au << 20) | (0u << 12) | 2u;

    bus.write32(0x8000u, mov_one);
    cpu.set_reg(15, 0x8000u);
    cpu.step();
    CHECK_EQ(cpu.reg(0), 1u);

    bus.write32(0x8000u, mov_two);
    cpu.invalidate_decode_cache(0x8000u, 4u);
    cpu.set_reg(15, 0x8000u);
    cpu.step();
    CHECK_EQ(cpu.reg(0), 2u);
}

TEST(the_bus_reports_writes_that_could_land_on_code) {
    Bus bus;
    bus.write32(0x9000u, 0x12345678u);
    CHECK(bus.write_watch().dirty);
    CHECK(bus.write_watch().low <= 0x9000u);
    CHECK(bus.write_watch().high >= 0x9004u);

    bus.write_watch().clear();
    CHECK(!bus.write_watch().dirty);

    // A write to VRAM is not code and must not force an invalidation.
    bus.write32(kVramBase + 0x100u, 0u);
    CHECK(!bus.write_watch().dirty);
}
