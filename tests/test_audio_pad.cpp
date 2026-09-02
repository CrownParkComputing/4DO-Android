// Audio ring and control pad.
//
// Both are read by one thread and written by another, so the tests here are
// about the contract rather than the arithmetic: what a consumer sees when the
// producer is behind, and whether a pad is ever observed halfway through an
// update.
#include <thread>
#include <vector>

#include "core/audio_ring.h"
#include "core/console.h"
#include "core/pad.h"
#include "core/pbus.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

StereoSample sample_of(int value) {
    StereoSample s;
    s.left = static_cast<s16>(value);
    s.right = static_cast<s16>(-value);
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Audio ring
// ---------------------------------------------------------------------------

TEST(samples_come_back_in_the_order_they_went_in) {
    AudioRing ring;
    ring.reset();

    StereoSample in[8];
    for (int i = 0; i < 8; ++i) in[i] = sample_of(i + 1);
    CHECK_EQ(ring.push(in, 8), 8u);
    CHECK_EQ(ring.available(), 8u);

    StereoSample out[8] = {};
    CHECK_EQ(ring.pull(out, 8), 8u);
    for (int i = 0; i < 8; ++i) {
        CHECK_EQ(out[i].left, static_cast<s16>(i + 1));
        CHECK_EQ(out[i].right, static_cast<s16>(-(i + 1)));
    }
    CHECK(ring.empty());
}

TEST(an_underrun_gives_silence_not_a_repeated_sample) {
    // Repeating the last sample is the other obvious choice and it is wrong: it
    // buzzes, which sounds like a broken emulator rather than a dropped frame.
    AudioRing ring;
    ring.reset();

    StereoSample in[2] = {sample_of(100), sample_of(200)};
    ring.push(in, 2);

    StereoSample out[6];
    for (int i = 0; i < 6; ++i) out[i] = sample_of(999);

    CHECK_EQ(ring.pull(out, 6), 2u);   // only two were real
    CHECK_EQ(out[0].left, static_cast<s16>(100));
    CHECK_EQ(out[1].left, static_cast<s16>(200));
    for (int i = 2; i < 6; ++i) {
        CHECK_EQ(out[i].left, static_cast<s16>(0));
        CHECK_EQ(out[i].right, static_cast<s16>(0));
    }
    CHECK_EQ(ring.underruns(), 1u);
}

TEST(a_full_ring_refuses_rather_than_blocking) {
    // The emulator thread must never wait on the audio thread, so a full ring
    // refuses the excess and counts it.
    AudioRing ring;
    ring.reset();

    std::vector<StereoSample> huge(AudioRing::kCapacity + 100, sample_of(7));
    const u32 written = ring.push(huge.data(), static_cast<u32>(huge.size()));

    CHECK_EQ(written, AudioRing::kCapacity);
    CHECK_EQ(ring.refused(), 100u);
}

TEST(the_ring_keeps_working_across_the_wrap) {
    // The index is unsigned and allowed to wrap; if the masking is wrong this
    // is where it shows, and only after hours of play.
    AudioRing ring;
    ring.reset();

    StereoSample one[1];
    StereoSample out[1];
    for (u32 i = 0; i < AudioRing::kCapacity * 3; ++i) {
        one[0] = sample_of(static_cast<int>(i & 0x7fff));
        CHECK_EQ(ring.push(one, 1), 1u);
        CHECK_EQ(ring.pull(out, 1), 1u);
        CHECK_EQ(out[0].left, static_cast<s16>(i & 0x7fff));
    }
    CHECK_EQ(ring.underruns(), 0u);
}

TEST(a_producer_and_consumer_on_two_threads_lose_nothing) {
    AudioRing ring;
    ring.reset();

    constexpr u32 kTotal = 200000;
    std::vector<StereoSample> received;
    received.reserve(kTotal);

    std::thread producer([&] {
        u32 sent = 0;
        StereoSample chunk[64];
        while (sent < kTotal) {
            const u32 want = (kTotal - sent) < 64 ? (kTotal - sent) : 64;
            for (u32 i = 0; i < want; ++i) {
                chunk[i] = sample_of(static_cast<int>((sent + i) & 0x7fff));
            }
            const u32 wrote = ring.push(chunk, want);
            sent += wrote;
            if (wrote < want) std::this_thread::yield();
        }
    });

    StereoSample chunk[64];
    while (received.size() < kTotal) {
        const u32 got = ring.pull(chunk, 64);
        for (u32 i = 0; i < got && received.size() < kTotal; ++i) {
            received.push_back(chunk[i]);
        }
        if (got == 0) std::this_thread::yield();
    }
    producer.join();

    // Every sample, in order, with none invented or lost.
    CHECK_EQ(received.size(), size_t{kTotal});
    bool ordered = true;
    for (u32 i = 0; i < kTotal; ++i) {
        if (received[i].left != static_cast<s16>(i & 0x7fff)) {
            ordered = false;
            break;
        }
    }
    CHECK(ordered);
    // refused() may well be non-zero here: the producer hit a full ring and
    // retried. That is back-pressure, not lost audio, and the ordering check
    // above is what proves nothing was actually lost.
}

TEST(a_frame_of_emulation_produces_a_frame_of_audio) {
    // A stopped DSP produces zero-valued DAC pairs, but the machine still
    // advances at its real sample cadence and fills the ring. Pacing and
    // underrun behaviour are therefore exercised before a program starts it.
    Console console;
    console.reset();

    console.run_frame();
    // 44100 / 60, give or take the integer division.
    CHECK(console.audio().available() >= 700u);
    CHECK(console.audio().available() <= 750u);
}

TEST(an_arm_boost_does_not_speed_up_audio_or_video_time) {
    Console native;
    native.reset();
    native.bus().write32(0x8000u, 0xeafffffeu);  // branch to itself
    native.cpu().set_reg(15, 0x8000u);
    const u64 native_before = native.cpu().total_cycles();
    native.run_frame();
    const u64 native_cycles = native.cpu().total_cycles() - native_before;
    const u32 native_samples = native.audio().available();

    Console boosted;
    boosted.reset();
    boosted.set_cpu_scale_percent(150);
    boosted.bus().write32(0x8000u, 0xeafffffeu);
    boosted.cpu().set_reg(15, 0x8000u);
    const u64 boosted_before = boosted.cpu().total_cycles();
    boosted.run_frame();
    const u64 boosted_cycles = boosted.cpu().total_cycles() - boosted_before;

    // The ARM gets roughly half again as many cycles, while the independently
    // clocked DSP still emits exactly one native field's worth of samples.
    CHECK(boosted_cycles * 100u >= native_cycles * 145u);
    CHECK(boosted_cycles * 100u <= native_cycles * 155u);
    CHECK_EQ(boosted.audio().available(), native_samples);
}

// ---------------------------------------------------------------------------
// Control pad
// ---------------------------------------------------------------------------

TEST(a_button_press_is_visible_and_release_clears_it) {
    PadState pads;
    pads.reset();

    CHECK(!pads.pressed(0, PadButton::A));
    pads.press(0, PadButton::A, true);
    CHECK(pads.pressed(0, PadButton::A));
    pads.press(0, PadButton::A, false);
    CHECK(!pads.pressed(0, PadButton::A));
}

TEST(buttons_from_different_sources_do_not_clobber_each_other) {
    // Keyboard and gamepad can both be mapped to pad one. A plain store would
    // lose whichever arrived first, which shows up as a diagonal that will not
    // hold.
    PadState pads;
    pads.reset();

    pads.press(0, PadButton::Up, true);
    pads.press(0, PadButton::Left, true);

    CHECK(pads.pressed(0, PadButton::Up));
    CHECK(pads.pressed(0, PadButton::Left));

    pads.press(0, PadButton::Up, false);
    CHECK(!pads.pressed(0, PadButton::Up));
    CHECK(pads.pressed(0, PadButton::Left));
}

TEST(pads_are_independent) {
    PadState pads;
    pads.reset();
    pads.set_connected(1, true);

    pads.press(0, PadButton::A, true);
    pads.press(1, PadButton::B, true);

    CHECK(pads.pressed(0, PadButton::A));
    CHECK(!pads.pressed(0, PadButton::B));
    CHECK(pads.pressed(1, PadButton::B));
    CHECK(!pads.pressed(1, PadButton::A));
}

TEST(one_pad_is_connected_by_default_as_a_machine_ships) {
    PadState pads;
    pads.reset();
    CHECK(pads.connected(0));
    CHECK(!pads.connected(1));
    CHECK_EQ(pads.connected_count(), 1u);

    pads.set_connected(1, true);
    CHECK_EQ(pads.connected_count(), 2u);
}

TEST(disconnecting_a_pad_releases_its_buttons) {
    // Otherwise a pad unplugged mid-press leaves the machine holding a
    // direction forever.
    PadState pads;
    pads.reset();
    pads.set_connected(1, true);
    pads.press(1, PadButton::Right, true);
    CHECK(pads.pressed(1, PadButton::Right));

    pads.set_connected(1, false);
    CHECK(!pads.pressed(1, PadButton::Right));
}

TEST(out_of_range_pads_are_ignored_rather_than_corrupting_memory) {
    PadState pads;
    pads.reset();
    pads.press(kMaxPads, PadButton::A, true);
    pads.press(999, PadButton::A, true);
    CHECK(!pads.pressed(kMaxPads, PadButton::A));
    CHECK_EQ(pads.buttons(999), 0u);
}

// ---------------------------------------------------------------------------
// From the host's buttons to the machine
// ---------------------------------------------------------------------------
//
// Every test above this point checks PadState on its own, and PadState worked
// perfectly. What did not exist was anything reading it: the front end wrote
// presses into PadState while the PBUS transfer read a separate Joypad that
// only the headless harness ever set. So the on-screen controls and every
// controller were silently connected to nothing, and no test noticed, because
// no test followed a button past the object it was stored in.

TEST(a_press_reaches_the_machine_and_not_merely_the_pad_state) {
    Console console;
    console.pads().press(0, PadButton::A, true);
    console.pads().press(0, PadButton::Right, true);

    const Joypad seen = console.joypad();
    CHECK(seen.a);
    CHECK(seen.right);
    CHECK(!seen.b);
    CHECK(!seen.left);

    console.pads().press(0, PadButton::A, false);
    CHECK(!console.joypad().a);
    CHECK(console.joypad().right);
}

TEST(setting_pad_one_directly_lands_in_the_same_place_the_front_end_writes) {
    // The harness has no PadState of its own to drive, so it sets a Joypad.
    // That has to write through rather than into a second copy, or the two
    // paths can disagree about what is held - which is how the first one came
    // to be dead without anybody noticing.
    Console console;
    Joypad pad;
    pad.play = true;
    console.set_joypad(pad);

    CHECK(console.pads().pressed(0, PadButton::Play));
    CHECK(console.joypad().play);
}

TEST(every_attached_pad_is_chained_onto_the_one_stream) {
    // A 3DO needs no multitap: the pads chain, and the machine sees one serial
    // stream with each pad's report in it, in order.
    Pbus bus;
    bus.begin();

    Joypad first;
    first.a = true;
    Joypad second;
    second.b = true;
    bus.add_joypad(first);
    bus.add_joypad(second);

    // Two bytes each, and the second pad's report must not overwrite the
    // first's.
    CHECK(bus.size() >= 4);
    const u8* data = bus.data();
    CHECK_EQ(data[0] & 0x80u, 0x80u);   // a pad announces itself
    CHECK_EQ(data[0] & 0x01u, 0x01u);   // ...and pad one is holding A
    CHECK_EQ(data[2] & 0x80u, 0x80u);
    CHECK_EQ(data[2] & 0x01u, 0x00u);   // pad two is not
    CHECK_EQ(data[3] & 0x80u, 0x80u);   // pad two is holding B
}
