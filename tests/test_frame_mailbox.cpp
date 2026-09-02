// Frame mailbox tests.
//
// The property that matters is that a consumer never sees a half-written frame,
// however badly the two sides are paced against each other. Tearing here would
// show up as an occasional flickering band, which is easy to blame on a display
// driver and hard to trace back.
#include <atomic>
#include <chrono>
#include <thread>

#include "core/frame_mailbox.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

// Fill a frame with one repeated value, so any tear is a frame with two
// different values in it.
void fill(u32* pixels, int count, u32 value) {
    for (int i = 0; i < count; ++i) pixels[i] = value;
}

bool uniform(const u32* pixels, int count, u32* out_value) {
    const u32 first = pixels[0];
    for (int i = 1; i < count; ++i) {
        if (pixels[i] != first) return false;
    }
    *out_value = first;
    return true;
}

}  // namespace

TEST(a_published_frame_comes_back_to_the_consumer) {
    FrameMailbox mailbox;
    mailbox.resize(4, 4);

    fill(mailbox.writable(), 16, 0xAAAAAAAAu);
    mailbox.publish();

    const u32* frame = mailbox.acquire();
    CHECK(frame != nullptr);
    CHECK_EQ(frame[0], 0xAAAAAAAAu);
    CHECK_EQ(frame[15], 0xAAAAAAAAu);
}

TEST(acquiring_twice_reports_that_nothing_is_new) {
    // Returning the same frame again would make the caller re-upload a texture
    // that has not changed, every single display frame.
    FrameMailbox mailbox;
    mailbox.resize(4, 4);

    fill(mailbox.writable(), 16, 1);
    mailbox.publish();

    CHECK(mailbox.acquire() != nullptr);
    CHECK(mailbox.acquire() == nullptr);
    CHECK(mailbox.acquire() == nullptr);
}

TEST(the_most_recent_frame_is_still_readable_when_nothing_is_new) {
    // A window resize has to draw something even though no new frame arrived.
    FrameMailbox mailbox;
    mailbox.resize(4, 4);

    fill(mailbox.writable(), 16, 0x1234u);
    mailbox.publish();
    CHECK(mailbox.acquire() != nullptr);

    CHECK(mailbox.acquire() == nullptr);
    CHECK_EQ(mailbox.current()[0], 0x1234u);
}

TEST(a_slow_consumer_skips_to_the_newest_frame_rather_than_a_backlog) {
    // A stale frame is worthless: if the display is behind, the right answer is
    // the newest frame, not the oldest unread one. A queue would do the wrong
    // thing here and add latency that never recovers.
    FrameMailbox mailbox;
    mailbox.resize(4, 4);

    for (u32 value = 1; value <= 5; ++value) {
        fill(mailbox.writable(), 16, value);
        mailbox.publish();
    }

    const u32* frame = mailbox.acquire();
    CHECK(frame != nullptr);
    CHECK_EQ(frame[0], 5u);
}

TEST(the_producer_never_writes_the_slot_the_consumer_is_reading) {
    // This is the actual safety property of the triple buffer. If it did not
    // hold, the frame the consumer is drawing could change underneath it.
    FrameMailbox mailbox;
    mailbox.resize(4, 4);

    fill(mailbox.writable(), 16, 100);
    mailbox.publish();
    const u32* held = mailbox.acquire();
    CHECK(held != nullptr);
    CHECK_EQ(held[0], 100u);

    // Produce two more frames while still holding the first.
    fill(mailbox.writable(), 16, 200);
    mailbox.publish();
    fill(mailbox.writable(), 16, 300);
    mailbox.publish();

    // The frame being held is untouched.
    CHECK_EQ(held[0], 100u);
    CHECK_EQ(held[15], 100u);
}

TEST(resizing_reshapes_every_slot) {
    FrameMailbox mailbox;
    mailbox.resize(320, 240);
    CHECK_EQ(mailbox.width(), 320);
    CHECK_EQ(mailbox.height(), 240);

    mailbox.resize(320, 288);
    CHECK_EQ(mailbox.height(), 288);

    // Every slot must be the new size, or a publish after a region change
    // writes past the end of a stale one.
    fill(mailbox.writable(), 320 * 288, 7);
    mailbox.publish();
    fill(mailbox.writable(), 320 * 288, 8);
    mailbox.publish();
    fill(mailbox.writable(), 320 * 288, 9);
    mailbox.publish();
    CHECK_EQ(mailbox.acquire()[320 * 288 - 1], 9u);
}

TEST(a_frame_is_never_seen_half_written) {
    // Producer and consumer at deliberately mismatched rates. Every frame the
    // consumer sees must be uniform: a torn frame would contain two values.
    FrameMailbox mailbox;
    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    constexpr int kPixels = kWidth * kHeight;
    mailbox.resize(kWidth, kHeight);

    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> seen{0};

    std::thread producer([&] {
        u32 value = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            fill(mailbox.writable(), kPixels, value++);
            mailbox.publish();
        }
    });

    // Wait for frames rather than for iterations. A fixed spin count can
    // finish before the producer thread is ever scheduled, which leaves the
    // test passing vacuously on a slow machine and failing outright on a fast
    // one - the deadline is a safety net, not the exit condition.
    constexpr int kWantFrames = 200;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (seen.load() < kWantFrames &&
           std::chrono::steady_clock::now() < deadline) {
        const u32* frame = mailbox.acquire();
        if (frame == nullptr) {
            std::this_thread::yield();
            continue;
        }
        u32 value = 0;
        if (!uniform(frame, kPixels, &value)) {
            torn.fetch_add(1);
        }
        seen.fetch_add(1);
    }

    stop.store(true, std::memory_order_relaxed);
    producer.join();

    CHECK_EQ(torn.load(), 0);
    // The test is only meaningful if frames actually got through.
    CHECK_EQ(seen.load(), kWantFrames);
}

TEST(frames_only_ever_go_forwards) {
    // The consumer may skip frames but must never be handed an older one than
    // it has already seen — that would look like the picture jumping backwards.
    FrameMailbox mailbox;
    constexpr int kPixels = 16 * 16;
    mailbox.resize(16, 16);

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        u32 value = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            fill(mailbox.writable(), kPixels, value++);
            mailbox.publish();
        }
    });

    u32 previous = 0;
    bool went_backwards = false;
    for (int i = 0; i < 20000; ++i) {
        const u32* frame = mailbox.acquire();
        if (frame == nullptr) continue;
        const u32 value = frame[0];
        if (value < previous) {
            went_backwards = true;
            break;
        }
        previous = value;
    }

    stop.store(true, std::memory_order_relaxed);
    producer.join();
    CHECK(!went_backwards);
}
