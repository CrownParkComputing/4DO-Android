// The emulator thread.
//
// Emulation runs here and nowhere else. It publishes finished frames into a
// mailbox and pushes audio into a ring; the display thread takes whatever is
// newest whenever it happens to look. Neither ever waits for the other.
//
// The pacing policy, and what it is a reaction to
// -----------------------------------------------
// The previous 3DO core ran emulation and GPU presentation on the same thread
// and then stacked three independent frame-droppers on top: an audio-starvation
// check, an adaptive skip ladder, and a renderer that silently bailed if it
// could not take a lock. None of the three knew about the others, so a machine
// only slightly behind could drop far more frames than any one of them
// intended. Dropped frames with correct audio is exactly what people describe
// as "runs slow" — the mitigation was the symptom.
//
// So there is exactly ONE policy here, and it is stated in one place:
//
//   * Emulation is paced to the field rate against a monotonic deadline.
//   * If a frame overruns, the deadline is allowed to slip rather than the
//     emulator trying to catch up by running faster — catching up produces a
//     burst that starves audio and then overruns again.
//   * If we fall more than a few frames behind, the deadline is reset instead
//     of accumulating debt that can never be paid off.
//   * The thread always sleeps to its deadline. It never spins. A handheld that
//     free-runs at 100% of a core gets hot, and a hot handheld is slower, which
//     is a loop the old core could enter and not leave.
//
// Presentation is not a dropper at all any more: a display that misses a frame
// simply sees the next one, and emulation never notices.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/types.h"

namespace retro3do {

class Console;
class FrameMailbox;

struct EmulatorStats {
    u64 frames_emulated = 0;
    u64 frames_behind = 0;      // deadline missed
    u64 deadline_resets = 0;    // fell so far behind the clock was restarted
    double emulated_fps = 0.0;
    double frame_ms = 0.0;      // time to emulate one frame, smoothed
};

class EmulatorThread {
public:
    EmulatorThread(Console& console, FrameMailbox& mailbox);
    ~EmulatorThread();

    EmulatorThread(const EmulatorThread&) = delete;
    EmulatorThread& operator=(const EmulatorThread&) = delete;

    void start();
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Pause emulation without tearing the thread down, so state survives and
    // resuming is instant. The thread idles rather than spinning.
    void set_paused(bool paused);
    bool paused() const { return paused_.load(std::memory_order_acquire); }

    // Ask for a reset at the next frame boundary. Doing it here rather than
    // from the caller's thread avoids reaching into console state while the
    // emulator is midway through a frame.
    void request_reset();

    // Take a copy of the NVRAM, if the machine has written it since the last
    // time anyone asked. Returns false when there is nothing new, which is the
    // usual answer - titles touch it when the user saves and at no other time.
    //
    // It works this way, rather than the caller reading the bus directly,
    // because emulation owns that memory and is usually midway through a frame
    // when the display thread comes asking. The copy is taken at a frame
    // boundary, so what comes out is always a whole consistent image.
    bool take_nvram(std::vector<u8>& out);

    EmulatorStats stats() const;

private:
    void run();

    Console& console_;
    FrameMailbox& mailbox_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> reset_requested_{false};
    mutable std::mutex nvram_lock_;
    std::vector<u8> nvram_snapshot_;
    bool nvram_pending_ = false;

    std::atomic<u64> frames_emulated_{0};
    std::atomic<u64> frames_behind_{0};
    std::atomic<u64> deadline_resets_{0};
    std::atomic<double> emulated_fps_{0.0};
    std::atomic<double> frame_ms_{0.0};
};

}  // namespace retro3do
