// CHD recognition.
//
// The point of these is that a CHD must be REFUSED with a reason rather than
// misread. Without recognition a .chd falls through to the raw-image path,
// finds no sync pattern, is taken for a cooked 2048-byte image, and reports a
// plausible sector count computed from compressed data - so every read returns
// compressed bytes as though they were disc contents. That presents as a
// corrupt disc rather than an unsupported file, which is much worse than a
// clear refusal.
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/chd.h"
#include "core/disc.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

const char* scratch(const char* name) {
    static std::string path;
    path = std::string("/tmp/retro3do-chd-") + name;
    return path.c_str();
}

void put_be32(std::vector<u8>& v, size_t at, u32 value) {
    v[at + 0] = static_cast<u8>(value >> 24);
    v[at + 1] = static_cast<u8>(value >> 16);
    v[at + 2] = static_cast<u8>(value >> 8);
    v[at + 3] = static_cast<u8>(value);
}

void put_be64(std::vector<u8>& v, size_t at, u64 value) {
    put_be32(v, at, static_cast<u32>(value >> 32));
    put_be32(v, at + 4, static_cast<u32>(value));
}

// A v5 header, which is what any modern dump is.
std::vector<u8> make_chd_v5(u64 logical, u32 tag0, u32 tag1, bool with_parent) {
    std::vector<u8> v(4096, 0);
    std::memcpy(v.data(), "MComprHD", 8);
    put_be32(v, 8, 124);   // header length
    put_be32(v, 12, 5);    // version
    put_be32(v, 16, tag0);
    put_be32(v, 20, tag1);
    put_be64(v, 32, logical);
    put_be32(v, 56, 19584);  // hunk bytes
    put_be32(v, 60, 2448);   // unit bytes
    if (with_parent) {
        v[104] = 0xAB;  // a non-zero parent SHA1
    }
    return v;
}

void write_file(const char* path, const std::vector<u8>& bytes) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

constexpr u32 kTagCdZlib = 0x63647a6cu;
constexpr u32 kTagCdFlac = 0x6364666cu;
constexpr u32 kTagZstd   = 0x7a737464u;

}  // namespace

TEST(the_magic_identifies_a_chd) {
    const u8 good[8] = {'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D'};
    const u8 bad[8] = {'N', 'o', 't', 'a', 'c', 'h', 'd', '!'};
    CHECK(looks_like_chd(good, sizeof(good)));
    CHECK(!looks_like_chd(bad, sizeof(bad)));
    // Too short to tell must not read past the buffer.
    CHECK(!looks_like_chd(good, 4));
    CHECK(!looks_like_chd(nullptr, 8));
}

TEST(a_v5_header_yields_its_size_and_codecs) {
    const char* path = scratch("v5.chd");
    write_file(path, make_chd_v5(650u * 1024 * 1024, kTagCdZlib, kTagCdFlac, false));

    std::FILE* f = std::fopen(path, "rb");
    const ChdInfo info = probe_chd(f);
    std::fclose(f);

    CHECK(info.is_chd);
    CHECK_EQ(info.version, 5u);
    CHECK_EQ(info.logical_bytes, 650u * 1024 * 1024);
    CHECK_EQ(info.hunk_bytes, 19584u);
    CHECK(info.codecs[0] == ChdCodec::CdZlib);
    CHECK(info.codecs[1] == ChdCodec::CdFlac);
    CHECK(info.codecs[2] == ChdCodec::None);
    CHECK(!info.has_parent);
}

TEST(a_parent_chd_is_noticed) {
    // A delta against a parent cannot be read on its own however good the
    // decompressor is, so it is worth telling the user apart from a plain one.
    const char* path = scratch("parent.chd");
    write_file(path, make_chd_v5(100u * 1024 * 1024, kTagZstd, 0, true));

    std::FILE* f = std::fopen(path, "rb");
    const ChdInfo info = probe_chd(f);
    std::fclose(f);

    CHECK(info.is_chd);
    CHECK(info.has_parent);
    CHECK(info.describe().find("parent") != std::string::npos);
}

TEST(probing_does_not_disturb_the_read_position) {
    // probe_chd is called before deciding what to do with a file, so it must
    // leave it exactly as it found it.
    const char* path = scratch("seek.chd");
    write_file(path, make_chd_v5(1024, kTagZstd, 0, false));

    std::FILE* f = std::fopen(path, "rb");
    std::fseek(f, 64, SEEK_SET);
    probe_chd(f);
    CHECK_EQ(std::ftell(f), 64L);
    std::fclose(f);
}

TEST(an_ordinary_image_is_not_mistaken_for_a_chd) {
    const char* path = scratch("plain.iso");
    write_file(path, std::vector<u8>(4096, 0x42));

    std::FILE* f = std::fopen(path, "rb");
    const ChdInfo info = probe_chd(f);
    std::fclose(f);
    CHECK(!info.is_chd);
    CHECK(info.describe() == "not a CHD");
}

TEST(a_synthetic_chd_header_alone_is_not_a_readable_disc) {
    // A CHD is now read rather than refused, but a header with no hunks and no
    // track metadata behind it still is not a disc. It must fail with a reason
    // naming the file, and must NOT report a sector count - falling through to
    // the raw-image path would take compressed bytes for disc contents and look
    // like a corrupt disc rather than an unreadable one.
    const char* path = scratch("refuse.chd");
    write_file(path, make_chd_v5(650u * 1024 * 1024, kTagCdZlib, 0, false));

    Disc disc;
    CHECK(!disc.open(path));
    CHECK(!disc.is_open());
    CHECK_EQ(disc.sector_count(), 0u);

    // And it must still be recognised as a CHD, not as something unknown.
    CHECK(disc.is_chd());
    CHECK(disc.chd().is_chd);
    CHECK(!disc.last_error().empty());
}

TEST(the_description_names_the_version_size_and_compression) {
    const char* path = scratch("describe.chd");
    write_file(path, make_chd_v5(650u * 1024 * 1024, kTagCdZlib, kTagCdFlac, false));

    std::FILE* f = std::fopen(path, "rb");
    const std::string text = probe_chd(f).describe();
    std::fclose(f);

    CHECK(text.find("CHD v5") != std::string::npos);
    CHECK(text.find("650 MB") != std::string::npos);
    CHECK(text.find("cd-zlib") != std::string::npos);
    CHECK(text.find("cd-flac") != std::string::npos);
}

TEST(an_uncompressed_chd_says_so) {
    const char* path = scratch("raw.chd");
    write_file(path, make_chd_v5(1024 * 1024, 0, 0, false));

    std::FILE* f = std::fopen(path, "rb");
    const std::string text = probe_chd(f).describe();
    std::fclose(f);
    CHECK(text.find("uncompressed") != std::string::npos);
}

TEST(a_chd_of_an_unparsed_version_is_still_reported_as_a_chd) {
    // Better to say "a CHD I do not understand" than "an unrecognisable file".
    std::vector<u8> v = make_chd_v5(1024, kTagZstd, 0, false);
    put_be32(v, 12, 99);   // a version from the future

    const char* path = scratch("future.chd");
    write_file(path, v);

    std::FILE* f = std::fopen(path, "rb");
    const ChdInfo info = probe_chd(f);
    std::fclose(f);

    CHECK(info.is_chd);
    CHECK_EQ(info.version, 99u);
    CHECK(info.describe().find("CHD v99") != std::string::npos);
}

TEST(a_truncated_file_that_starts_like_a_chd_does_not_read_past_its_end) {
    std::vector<u8> v(20, 0);
    std::memcpy(v.data(), "MComprHD", 8);
    put_be32(v, 8, 124);
    put_be32(v, 12, 5);

    const char* path = scratch("short.chd");
    write_file(path, v);

    std::FILE* f = std::fopen(path, "rb");
    const ChdInfo info = probe_chd(f);
    std::fclose(f);

    // Identified, but no fields invented from bytes that are not there.
    CHECK(info.is_chd);
    CHECK_EQ(info.logical_bytes, 0u);
}
