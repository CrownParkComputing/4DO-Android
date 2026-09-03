// Metadata and access to Retro-3DO's original built-in demonstration ROM.
#pragma once

#include <cstddef>

#include "types.h"

namespace retro3do {

struct DemoRom {
    const u8* data;
    size_t size;
    const char* name;
    // Hash of the bytes after the normal loader has zero-padded them to the
    // full one-megabyte 3DO ROM window.
    const char* padded_sha256;
};

const DemoRom& builtin_demo_rom();

}  // namespace retro3do
