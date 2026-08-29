#include "frame_mailbox.h"

namespace retro3do {

FrameMailbox::FrameMailbox() {
    resize(320, 240);
}

void FrameMailbox::resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    width_ = width;
    height_ = height;

    const size_t pixels = static_cast<size_t>(width) * height;
    for (int i = 0; i < kSlots; ++i) {
        slots_[i].assign(pixels, 0xff000000u);
    }

    writing_ = 0;
    reading_ = 2;
    ready_.store(1, std::memory_order_release);
}

u32* FrameMailbox::writable() {
    return slots_[writing_].data();
}

void FrameMailbox::publish() {
    // Hand the slot just written to the consumer and take back whatever it had.
    // A single exchange, so there is no window in which the consumer could see
    // an index that does not yet name a complete frame.
    const u32 previous = ready_.exchange(
        static_cast<u32>(writing_) | kFreshBit, std::memory_order_acq_rel);

    writing_ = static_cast<int>(previous & ~kFreshBit);
    published_.fetch_add(1, std::memory_order_relaxed);
}

const u32* FrameMailbox::acquire() {
    // Nothing new: say so rather than handing back the same frame again, so the
    // caller can skip a texture upload it does not need.
    if ((ready_.load(std::memory_order_acquire) & kFreshBit) == 0) {
        return nullptr;
    }

    const u32 previous = ready_.exchange(static_cast<u32>(reading_),
                                         std::memory_order_acq_rel);
    reading_ = static_cast<int>(previous & ~kFreshBit);
    consumed_.fetch_add(1, std::memory_order_relaxed);
    return slots_[reading_].data();
}

const u32* FrameMailbox::current() const {
    return slots_[reading_].data();
}

}  // namespace retro3do
