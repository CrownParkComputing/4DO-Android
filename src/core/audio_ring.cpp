#include "audio_ring.h"

namespace retro3do {

void AudioRing::reset() {
    write_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    refused_.store(0, std::memory_order_relaxed);
}

u32 AudioRing::available() const {
    const u32 write = write_.load(std::memory_order_acquire);
    const u32 read = read_.load(std::memory_order_acquire);
    return write - read;  // unsigned wraparound is intended and correct here
}

u32 AudioRing::push(const StereoSample* samples, u32 count) {
    if (samples == nullptr || count == 0) {
        return 0;
    }

    const u32 write = write_.load(std::memory_order_relaxed);
    const u32 read = read_.load(std::memory_order_acquire);
    const u32 space = kCapacity - (write - read);

    const u32 writable = count < space ? count : space;
    for (u32 i = 0; i < writable; ++i) {
        buffer_[(write + i) & kMask] = samples[i];
    }

    // Release so the consumer sees the samples before it sees the new index.
    write_.store(write + writable, std::memory_order_release);

    if (writable < count) {
        refused_.fetch_add(count - writable, std::memory_order_relaxed);
    }
    return writable;
}

u32 AudioRing::pull(StereoSample* out, u32 count) {
    if (out == nullptr || count == 0) {
        return 0;
    }

    const u32 read = read_.load(std::memory_order_relaxed);
    const u32 write = write_.load(std::memory_order_acquire);
    const u32 ready = write - read;

    const u32 readable = count < ready ? count : ready;
    for (u32 i = 0; i < readable; ++i) {
        out[i] = buffer_[(read + i) & kMask];
    }
    read_.store(read + readable, std::memory_order_release);

    // Silence for whatever was missing, rather than a repeat of the last
    // sample: a dropout should sound like a gap, not like a broken instrument.
    for (u32 i = readable; i < count; ++i) {
        out[i] = StereoSample{};
    }
    if (readable < count) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }

    return readable;
}

}  // namespace retro3do
