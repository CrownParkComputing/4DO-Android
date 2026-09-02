// DSPP instruction tests.
//
// These deliberately run real instruction streams rather than reaching into
// the decoder.  That pins the contract between arithmetic flags, conditional
// control flow, operand words and the memory-mapped DACs.
#include "core/dsp.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

// Transfer one immediate into the ALU, branch on signed-less-than, and leave
// one in the left DAC only when the branch is taken.
u16 run_signed_less_program(u16 immediate) {
    Dsp dsp;
    const u16 program[] = {
        0x2400, immediate,  // ALU transfer, one operand
        0xe006,             // signed less-than -> word 6
        0x9bfe, 0xc000,     // fall-through: left DAC = 0
        0x8408,             // jump to sleep
        0x9bfe, 0xc001,     // taken: left DAC = 1
        0x8380,             // sleep
    };
    for (u16 address = 0; address < sizeof(program) / sizeof(program[0]); ++address) {
        dsp.write_program(address, program[address]);
    }
    dsp.set_running(true);
    return static_cast<u16>(dsp.run());
}

}  // namespace

TEST(dspp_signed_less_branch_takes_for_a_negative_result) {
    // A type-six operand contains a signed 13-bit immediate.  0xDFFF is -1.
    CHECK_EQ(run_signed_less_program(0xdfff), 1);
}

TEST(dspp_signed_less_branch_falls_through_for_a_positive_result) {
    CHECK_EQ(run_signed_less_program(0xc001), 0);
}

TEST(dspp_three_register_packet_supplies_two_inputs_and_a_writeback) {
    Dsp dsp;
    dsp.write_data(3, 3);
    dsp.write_data(2, 4);

    // ADD ALU1,ALU2 with three supplied values. The packed operand orders
    // R3,R2,R4: R3 and R2 feed the requested inputs, while the unused R4 slot
    // selects a DSP-writable register in the upper data bank.
    dsp.write_program(0, 0x6620);
    dsp.write_program(1, 0x0c44);
    dsp.write_program(2, 0x8380);
    dsp.set_running(true);
    dsp.run();

    CHECK_EQ(dsp.read_data(0x104), 7);
}
