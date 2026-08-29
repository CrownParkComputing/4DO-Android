#include "emulator_thread.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/console.h"
#include "core/frame_mailbox.h"

namespace retro3do {
namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

Nanoseconds field_period(Region region) {
    // 50 or 60 fields a second. Kept in nanoseconds so the deadline does not
    // drift the way repeatedly adding a rounded millisecond would.
    return region == Region::Pal ? Nanoseconds(20000000) : Nanoseconds(16666667);
}

}  // namespace

EmulatorThread::EmulatorThread(Console& console, FrameMailbox& mailbox)
    : console_(console), mailbox_(mailbox) {}

EmulatorThread::~EmulatorThread() {
    stop();
}

void EmulatorThread::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    should_stop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
}

void EmulatorThread::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    should_stop_.store(true, std::memory_order_release);
    // Un-pause so a paused thread notices the stop rather than idling forever.
    paused_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void EmulatorThread::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_release);
}

void EmulatorThread::request_reset() {
    reset_requested_.store(true, std::memory_order_release);
}

EmulatorStats EmulatorThread::stats() const {
    EmulatorStats s;
    s.frames_emulated = frames_emulated_.load(std::memory_order_relaxed);
    s.frames_behind = frames_behind_.load(std::memory_order_relaxed);
    s.deadline_resets = deadline_resets_.load(std::memory_order_relaxed);
    s.emulated_fps = emulated_fps_.load(std::memory_order_relaxed);
    s.frame_ms = frame_ms_.load(std::memory_order_relaxed);
    return s;
}

void EmulatorThread::run() {
    auto deadline = Clock::now();
    double smoothed_frame_ms = 0.0;
    auto fps_window_start = Clock::now();
    u64 fps_window_frames = 0;

    while (!should_stop_.load(std::memory_order_acquire)) {
        if (paused_.load(std::memory_order_acquire)) {
            // Idle, not spin. A paused emulator should cost nothing.
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            deadline = Clock::now();
            continue;
        }

        if (reset_requested_.exchange(false, std::memory_order_acq_rel)) {
            console_.reset();
            deadline = Clock::now();
        }

        const auto started = Clock::now();
        console_.run_frame();

        // Copy the finished field into the mailbox and hand it over. The
        // emulator's involvement with the display ends here — it does not know
        // whether anyone is drawing, and does not care.
        const Frame frame = console_.framebuffer();
        if (frame.pixels != nullptr) {
            if (mailbox_.width() != frame.width || mailbox_.height() != frame.height) {
                mailbox_.resize(frame.width, frame.height);
            }
            std::memcpy(mailbox_.writable(), frame.pixels,
                        static_cast<size_t>(frame.width) * frame.height * sizeof(u32));
            mailbox_.publish();
        }

        const auto finished = Clock::now();
        const double this_frame_ms =
            std::chrono::duration<double, std::milli>(finished - started).count();
        smoothed_frame_ms = smoothed_frame_ms == 0.0
                                ? this_frame_ms
                                : smoothed_frame_ms * 0.9 + this_frame_ms * 0.1;
        frame_ms_.store(smoothed_frame_ms, std::memory_order_relaxed);

        frames_emulated_.fetch_add(1, std::memory_order_relaxed);
        ++fps_window_frames;
        if (finished - fps_window_start >= std::chrono::milliseconds(500)) {
            const double seconds =
                std::chrono::duration<double>(finished - fps_window_start).count();
            emulated_fps_.store(static_cast<double>(fps_window_frames) / seconds,
                                std::memory_order_relaxed);
            fps_window_start = finished;
            fps_window_frames = 0;
        }

        // --- pacing, the only place a frame's timing is decided -------------
        const Nanoseconds period = field_period(console_.region());
        deadline += period;

        const auto now = Clock::now();
        if (now < deadline) {
            std::this_thread::sleep_until(deadline);
        } else {
            frames_behind_.fetch_add(1, std::memory_order_relaxed);

            // Behind. Let the deadline slip rather than trying to catch up:
            // running extra frames back to back produces a burst that starves
            // audio and then overruns again. But do not let debt accumulate
            // without limit either — after a stall (a breakpoint, an app
            // backgrounded, a slow disc read) the clock is restarted rather
            // than the emulator sprinting to make up minutes it will never
            // recover.
            if (now - deadline > period * 4) {
                deadline = now;
                deadline_resets_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

}  // namespace retro3do
