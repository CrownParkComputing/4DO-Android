// A frame mailbox: the seam between the thread that emulates and the thread
// that draws.
//
// The emulator produces frames on its own schedule and the display consumes
// them on the display's schedule. Neither should ever wait for the other. This
// is a triple buffer: one slot the producer is filling, one holding the most
// recent finished frame, and one the consumer is reading. Publishing is a
// single atomic exchange, so a producer never blocks and a consumer never sees
// a half-written frame.
//
// Why not a queue: a stale frame is worthless. If the display is behind, the
// right behaviour is to skip to the newest frame, not to work through a backlog
// — which is what a queue would make it do, adding latency that never recovers.
//
// Why not a mutex: the previous core took a lock in its present path and
// dropped the frame if it could not get it, which is a frame-dropper hidden
// inside what looks like a rendering detail. Here a slow consumer costs nothing
// but skipped frames, and the emulator's timing is untouched.
#pragma once

#include <atomic>
#include <vector>

#include "types.h"

namespace retro3do {

class FrameMailbox {
public:
    FrameMailbox();

    // Resize every slot. Safe only when neither side is mid-frame — call it
    // from the producer between frames, as a region change does.
    void resize(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    // --- producer ---------------------------------------------------------
    // The slot to write into. Stays valid until publish().
    u32* writable();

    // Make the slot just written the newest frame. Constant time, wait-free.
    void publish();

    // --- consumer ---------------------------------------------------------
    // The newest published frame, or nullptr if nothing new has arrived since
    // the last call. Returning nullptr rather than the previous frame lets the
    // caller skip re-uploading a texture that has not changed.
    const u32* acquire();

    // The most recent frame whether or not it is new. Used when the consumer
    // must draw something — a window resize, say — and no new frame has come.
    const u32* current() const;

    u64 published() const { return published_.load(std::memory_order_relaxed); }
    u64 consumed() const { return consumed_.load(std::memory_order_relaxed); }

private:
    static constexpr int kSlots = 3;

    std::vector<u32> slots_[kSlots];
    int width_ = 0;
    int height_ = 0;

    int writing_ = 0;
    int reading_ = 2;

    // The slot holding the newest finished frame, with a flag in the high bit
    // saying whether the consumer has taken it yet. Keeping both in one word is
    // what makes the handover a single exchange rather than two operations the
    // consumer could observe between.
    static constexpr u32 kFreshBit = 0x80000000u;
    std::atomic<u32> ready_{1};

    std::atomic<u64> published_{0};
    std::atomic<u64> consumed_{0};
};

}  // namespace retro3do
