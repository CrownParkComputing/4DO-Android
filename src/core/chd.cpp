#include "chd.h"

#include <cstring>

namespace retro3do {
namespace {

// Every CHD starts with this, whatever its version.
constexpr char kMagic[8] = {'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D'};

// CHD stores its header big-endian, like the machine it describes happens to
// be. Nothing to do with the 3DO — it is just the format's convention.
u32 read_be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

u64 read_be64(const u8* p) {
    return (static_cast<u64>(read_be32(p)) << 32) | read_be32(p + 4);
}

// Codecs are four-character tags. A zero tag means that slot is unused, which
// is how an uncompressed CHD is expressed.
ChdCodec codec_from_tag(u32 tag) {
    switch (tag) {
        case 0:                    return ChdCodec::None;
        case 0x7a6c6962u:  /*zlib*/ return ChdCodec::Zlib;
        case 0x7a737464u:  /*zstd*/ return ChdCodec::Zstd;
        case 0x6c7a6d61u:  /*lzma*/ return ChdCodec::Lzma;
        case 0x68756666u:  /*huff*/ return ChdCodec::Huffman;
        case 0x666c6163u:  /*flac*/ return ChdCodec::Flac;
        case 0x63647a6cu:  /*cdzl*/ return ChdCodec::CdZlib;
        case 0x63646c7au:  /*cdlz*/ return ChdCodec::CdLzma;
        case 0x6364666cu:  /*cdfl*/ return ChdCodec::CdFlac;
        case 0x63647a73u:  /*cdzs*/ return ChdCodec::CdZstd;
        case 0x61766875u:  /*avhu*/ return ChdCodec::AvHuff;
        default:                    return ChdCodec::Unknown;
    }
}

bool all_zero(const u8* p, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

}  // namespace

const char* name_of(ChdCodec codec) {
    switch (codec) {
        case ChdCodec::None:    return "none";
        case ChdCodec::Zlib:    return "zlib";
        case ChdCodec::Zstd:    return "zstd";
        case ChdCodec::Lzma:    return "lzma";
        case ChdCodec::Huffman: return "huffman";
        case ChdCodec::Flac:    return "flac";
        case ChdCodec::CdZlib:  return "cd-zlib";
        case ChdCodec::CdLzma:  return "cd-lzma";
        case ChdCodec::CdFlac:  return "cd-flac";
        case ChdCodec::CdZstd:  return "cd-zstd";
        case ChdCodec::AvHuff:  return "av-huffman";
        case ChdCodec::Unknown:
        default:                return "unrecognised";
    }
}

std::string ChdInfo::describe() const {
    if (!is_chd) {
        return "not a CHD";
    }

    std::string text = "CHD v" + std::to_string(version);

    if (logical_bytes > 0) {
        // Megabytes rather than bytes: the number is for a person deciding
        // whether this is the disc they meant, not for arithmetic.
        const u64 megabytes = logical_bytes / (1024u * 1024u);
        text += ", " + std::to_string(megabytes) + " MB uncompressed";
    }

    // Only the codecs actually in use are worth naming.
    std::string codec_list;
    for (const ChdCodec codec : codecs) {
        if (codec == ChdCodec::None) continue;
        if (!codec_list.empty()) codec_list += " + ";
        codec_list += name_of(codec);
    }
    if (!codec_list.empty()) {
        text += ", " + codec_list;
    } else {
        text += ", uncompressed";
    }

    if (has_parent) {
        text += " (delta against a parent CHD)";
    }
    return text;
}

bool looks_like_chd(const u8* header, size_t length) {
    return header != nullptr && length >= sizeof(kMagic) &&
           std::memcmp(header, kMagic, sizeof(kMagic)) == 0;
}

ChdInfo probe_chd(std::FILE* file) {
    ChdInfo info;
    if (file == nullptr) {
        return info;
    }

    const long restore = std::ftell(file);

    // The largest header this understands is v5's 124 bytes.
    u8 header[128] = {};
    std::fseek(file, 0, SEEK_SET);
    const size_t read = std::fread(header, 1, sizeof(header), file);
    std::fseek(file, restore, SEEK_SET);

    if (read < 16 || !looks_like_chd(header, read)) {
        return info;
    }

    info.is_chd = true;
    const u32 header_length = read_be32(header + 8);
    info.version = read_be32(header + 12);

    // Versions differ in where everything after the magic lives, so each is
    // read on its own terms rather than through a guessed common layout.
    if (info.version == 5 && read >= 124 && header_length >= 124) {
        for (int i = 0; i < 4; ++i) {
            info.codecs[i] = codec_from_tag(read_be32(header + 16 + i * 4));
        }
        info.logical_bytes = read_be64(header + 32);
        info.hunk_bytes = read_be32(header + 56);
        info.unit_bytes = read_be32(header + 60);
        // A parent is named by a non-zero parent SHA1.
        info.has_parent = !all_zero(header + 104, 20);
        return info;
    }

    if (info.version == 4 && read >= 108 && header_length >= 108) {
        // v4 names a single compressor by number rather than by tag.
        const u32 compression = read_be32(header + 20);
        switch (compression) {
            case 0: info.codecs[0] = ChdCodec::None; break;
            case 1: info.codecs[0] = ChdCodec::Zlib; break;
            case 2: info.codecs[0] = ChdCodec::Zlib; break;  // "zlib+"
            case 3: info.codecs[0] = ChdCodec::AvHuff; break;
            default: info.codecs[0] = ChdCodec::Unknown; break;
        }
        info.logical_bytes = read_be64(header + 28);
        info.hunk_bytes = read_be32(header + 76);
        info.has_parent = !all_zero(header + 64, 20);
        return info;
    }

    // A CHD of a version this does not parse. It is still definitely a CHD, and
    // saying so is more useful than pretending the file is unrecognisable.
    return info;
}

}  // namespace retro3do
