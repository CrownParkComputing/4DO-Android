// The audio ring buffer.
//
// This is the one piece of the core with a hard realtime deadline on both ends:
// the emulator thread pushes samples as it runs, and the platform's audio
// callback pulls them on a schedule nothing can delay. So it is lock-free and
// single-producer, single-consumer, and it never allocates.
//
// Underrun policy is deliberate. When the consumer finds the ring empty it
// takes silence and says so, rather than blocking or repeating the last sample.
// Blocking would stall the audio thread — which on Android and iOS means the
// whole device's audio pipeline, not just this app — and repeating produces a
// buzz that sounds like a broken emulator rather than a dropped frame.
#pragma once

#include <atomic>

#include "types.h"

namespace retro3do {

// One stereo sample pair. The 3DO's DAC runs at 44.1 kHz.
struct StereoSample {
    s16 left = 0;
    s16 right = 0;
};

constexpr u32 kAudioSampleRate = 44100;

class AudioRing {
public:
    // Capacity is a power of two so the wrap is a mask rather than a modulo.
    // 32768 pairs is about 740 ms, which is far more than any sane buffer and
    // cheap enough not to matter.
    static constexpr u32 kCapacity = 32768;
    static constexpr u32 kMask = kCapacity - 1;

    void reset();

    // Producer side. Returns the number actually written. A full ring refuses
    // the excess rather than blocking, because the emulator thread must never
    // wait on the audio thread.
    u32 push(const StereoSample* samples, u32 count);

    // Consumer side. Always fills `count` samples: any shortfall is silence.
    // Returns how many were real, so the caller can tell an underrun from a
    // healthy pull.
    u32 pull(StereoSample* out, u32 count);

    u32 available() const;
    bool empty() const { return available() == 0; }

    // How many times the consumer has come up short. Not decoration — a rising
    // underrun count is the first symptom of the emulator falling behind, and
    // it appears before anything is visible on screen.
    u64 underruns() const { return underruns_.load(std::memory_order_relaxed); }

    // Samples the ring had no room for. Note what this does and does not mean:
    // a producer that retries loses nothing even though this rises, so it is a
    // measure of back-pressure, not of lost audio. The emulator never retries —
    // it cannot afford to wait — so for the emulator the two coincide.
    u64 refused() const { return refused_.load(std::memory_order_relaxed); }

private:
    StereoSample buffer_[kCapacity] = {};
    std::atomic<u32> write_{0};
    std::atomic<u32> read_{0};
    std::atomic<u64> underruns_{0};
    std::atomic<u64> refused_{0};
};

}  // namespace retro3do
