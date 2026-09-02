// Shared fixed-width types for the 3DO core.
#pragma once

#include <cstddef>
#include <cstdint>

namespace retro3do {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Rotate right, the ARM barrel shifter's basic operation. Written so the
// compiler recognises it and emits a single ROR; the naive `(x >> n) | (x << (32
// - n))` is undefined when n == 0, which the ARM immediate decoder does produce.
constexpr u32 ror32(u32 value, unsigned amount) noexcept {
    amount &= 31u;
    return amount == 0 ? value : ((value >> amount) | (value << (32u - amount)));
}

}  // namespace retro3do
