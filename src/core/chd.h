// CHD ("Compressed Hunks of Data") recognition.
//
// CHD is the archive format most 3DO libraries are actually stored in, so an
// emulator that cannot at least *recognise* one is confusing to use: without
// this, a .chd falls through to the raw-image path, finds no sync pattern,
// is taken for a cooked 2048-byte image, and reports a plausible-looking sector
// count computed from compressed data. Every read then returns compressed bytes
// as though they were disc contents. That is far worse than refusing it,
// because it looks like a corrupt disc rather than an unsupported file.
//
// This header parses enough to identify a CHD and describe it. It does not
// decompress: reading the data needs a decompressor per codec, which is a
// separate job. What it buys is an honest answer in the interface, and the
// information needed to say *why* a given file will not play yet.
//
// The layout below is the published CHD format. Nothing here is specific to the
// 3DO, and none of it comes from another emulator's source.
#pragma once

#include <cstdio>
#include <string>

#include "types.h"

namespace retro3do {

// The codecs a CHD may be compressed with. Only the names are needed here; what
// matters to the user is whether we can decompress it, not how it works.
enum class ChdCodec {
    None,
    Zlib,
    Zstd,
    Lzma,
    Huffman,
    Flac,
    CdZlib,
    CdLzma,
    CdFlac,
    CdZstd,
    AvHuff,
    Unknown,
};

const char* name_of(ChdCodec codec);

struct ChdInfo {
    bool is_chd = false;
    u32 version = 0;

    // Size of the disc image once decompressed, and the granularity it is
    // compressed in.
    u64 logical_bytes = 0;
    u32 hunk_bytes = 0;
    u32 unit_bytes = 0;

    // Up to four codecs may be used across a single file.
    ChdCodec codecs[4] = {ChdCodec::None, ChdCodec::None, ChdCodec::None,
                          ChdCodec::None};

    // True when the file names a parent CHD — a delta against another file,
    // which cannot be read on its own however good the decompressor is.
    bool has_parent = false;

    // Sectors the disc would have, if the geometry can be worked out.
    u32 sector_count() const {
        return logical_bytes == 0 ? 0
                                  : static_cast<u32>(logical_bytes / 2048u);
    }

    // A short human-readable summary for the interface.
    std::string describe() const;
};

// Look at a file and decide whether it is a CHD. Does not take ownership and
// restores the read position, so it is safe to call before deciding what to do
// with the file.
ChdInfo probe_chd(std::FILE* file);

// True if the first bytes are CHD's magic. Cheap enough to call on anything.
bool looks_like_chd(const u8* header, size_t length);

}  // namespace retro3do
