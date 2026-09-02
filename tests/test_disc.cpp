// Disc image tests.
//
// These build synthetic images on disk and read them back. The sector-stride
// cases matter most: reading a raw image as though it were cooked *almost*
// works — the first sector is right and everything after it is off by 304
// bytes — which presents as a corrupt disc rather than a misread one.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/disc.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

const char* scratch(const char* name) {
    static std::string path;
    path = std::string("/tmp/retro3do-test-") + name;
    return path.c_str();
}

void write_file(const char* path, const std::vector<u8>& bytes) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

void write_text(const char* path, const std::string& text) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

const u8 kSync[12] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                      0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

// A cooked image: user data only, one byte pattern per sector.
std::vector<u8> make_cooked(u32 sectors) {
    std::vector<u8> image(static_cast<size_t>(sectors) * 2048);
    for (u32 s = 0; s < sectors; ++s) {
        std::memset(&image[s * 2048], static_cast<int>(s + 1), 2048);
    }
    return image;
}

// A raw image with the given mode byte, same payload pattern.
std::vector<u8> make_raw(u32 sectors, u8 mode) {
    const u32 user_offset = (mode == 2) ? 24u : 16u;
    std::vector<u8> image(static_cast<size_t>(sectors) * 2352, 0);
    for (u32 s = 0; s < sectors; ++s) {
        u8* sector = &image[s * 2352];
        std::memcpy(sector, kSync, sizeof(kSync));
        sector[15] = mode;
        std::memset(sector + user_offset, static_cast<int>(s + 1), 2048);
    }
    return image;
}

}  // namespace

// ---------------------------------------------------------------------------
// Layout arithmetic
// ---------------------------------------------------------------------------

TEST(each_layout_knows_its_stride_and_payload_offset) {
    CHECK_EQ(bytes_per_sector(SectorLayout::Cooked2048), 2048u);
    CHECK_EQ(bytes_per_sector(SectorLayout::Raw2352Mode1), 2352u);
    CHECK_EQ(bytes_per_sector(SectorLayout::Raw2352Mode2), 2352u);

    CHECK_EQ(user_offset_in_sector(SectorLayout::Cooked2048), 0u);
    CHECK_EQ(user_offset_in_sector(SectorLayout::Raw2352Mode1), 16u);
    // Mode 2 has an eight-byte subheader after the header. Missing it is what
    // makes a Mode 2 disc read as almost-right garbage.
    CHECK_EQ(user_offset_in_sector(SectorLayout::Raw2352Mode2), 24u);
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

TEST(a_cooked_image_is_detected_and_read) {
    const char* path = scratch("cooked.iso");
    write_file(path, make_cooked(4));

    Disc disc;
    CHECK(disc.open(path));
    CHECK(disc.layout() == SectorLayout::Cooked2048);
    CHECK_EQ(disc.sector_count(), 4u);

    u8 sector[2048];
    CHECK(disc.read_sector(2, sector));
    CHECK_EQ(sector[0], 3u);
    CHECK_EQ(sector[2047], 3u);
}

TEST(a_raw_mode1_image_is_detected_by_its_sync_pattern_not_its_name) {
    // Deliberately named .iso while holding raw sectors. Trusting the extension
    // here is exactly the bug this detection exists to prevent.
    const char* path = scratch("raw-mode1.iso");
    write_file(path, make_raw(4, 1));

    Disc disc;
    CHECK(disc.open(path));
    CHECK(disc.layout() == SectorLayout::Raw2352Mode1);
    CHECK_EQ(disc.sector_count(), 4u);

    u8 sector[2048];
    CHECK(disc.read_sector(3, sector));
    CHECK_EQ(sector[0], 4u);
}

TEST(a_raw_mode2_image_skips_the_subheader) {
    const char* path = scratch("raw-mode2.bin");
    write_file(path, make_raw(4, 2));

    Disc disc;
    CHECK(disc.open(path));
    CHECK(disc.layout() == SectorLayout::Raw2352Mode2);

    u8 sector[2048];
    CHECK(disc.read_sector(1, sector));
    CHECK_EQ(sector[0], 2u);
    CHECK_EQ(sector[2047], 2u);
}

TEST(every_sector_of_a_raw_image_reads_correctly_not_just_the_first) {
    // The off-by-304 failure only shows from the second sector on, so the whole
    // image is walked rather than spot-checked.
    const char* path = scratch("raw-walk.bin");
    write_file(path, make_raw(16, 1));

    Disc disc;
    CHECK(disc.open(path));

    u8 sector[2048];
    for (u32 s = 0; s < 16; ++s) {
        CHECK(disc.read_sector(s, sector));
        CHECK_EQ(sector[0], static_cast<u8>(s + 1));
        CHECK_EQ(sector[1024], static_cast<u8>(s + 1));
    }
}

TEST(reading_past_the_end_fails_rather_than_returning_rubbish) {
    const char* path = scratch("short.iso");
    write_file(path, make_cooked(2));

    Disc disc;
    CHECK(disc.open(path));
    u8 sector[2048];
    CHECK(disc.read_sector(1, sector));
    CHECK(!disc.read_sector(2, sector));
    CHECK(!disc.read_sector(1000, sector));
}

TEST(a_missing_file_reports_why) {
    Disc disc;
    CHECK(!disc.open("/definitely/not/here.iso"));
    CHECK(!disc.last_error().empty());
    CHECK(!disc.is_open());
}

TEST(a_bare_image_presents_as_one_data_track) {
    const char* path = scratch("single-track.iso");
    write_file(path, make_cooked(8));

    Disc disc;
    CHECK(disc.open(path));
    CHECK_EQ(disc.tracks().size(), size_t{1});
    CHECK_EQ(disc.tracks()[0].number, 1u);
    CHECK(!disc.tracks()[0].is_audio);
    CHECK_EQ(disc.tracks()[0].length_sectors, 8u);
}

// ---------------------------------------------------------------------------
// Cue sheets
// ---------------------------------------------------------------------------

TEST(a_cue_sheet_gives_track_numbers_types_and_starts) {
    write_file(scratch("multi.bin"), make_raw(1000, 1));
    write_text(scratch("multi.cue"),
               "FILE \"retro3do-test-multi.bin\" BINARY\n"
               "  TRACK 01 MODE1/2352\n"
               "    INDEX 01 00:00:00\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 01 00:04:00\n"
               "  TRACK 03 AUDIO\n"
               "    INDEX 01 00:08:00\n");

    Disc disc;
    CHECK(disc.open(scratch("multi.cue")));
    CHECK_EQ(disc.tracks().size(), size_t{3});

    CHECK_EQ(disc.tracks()[0].number, 1u);
    CHECK(!disc.tracks()[0].is_audio);
    CHECK_EQ(disc.tracks()[0].start_lba, 0u);

    // 4 seconds at 75 frames a second.
    CHECK_EQ(disc.tracks()[1].start_lba, 300u);
    CHECK(disc.tracks()[1].is_audio);

    CHECK_EQ(disc.tracks()[2].start_lba, 600u);
}

TEST(track_lengths_come_from_the_next_tracks_start) {
    write_file(scratch("len.bin"), make_raw(1000, 1));
    write_text(scratch("len.cue"),
               "FILE \"retro3do-test-len.bin\" BINARY\n"
               "  TRACK 01 MODE1/2352\n"
               "    INDEX 01 00:00:00\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 01 00:04:00\n");

    Disc disc;
    CHECK(disc.open(scratch("len.cue")));
    CHECK_EQ(disc.tracks()[0].length_sectors, 300u);
    // The last track runs to the end of the image.
    CHECK_EQ(disc.tracks()[1].length_sectors, 700u);
}

TEST(a_cue_naming_a_wave_file_is_refused_rather_than_playing_silence) {
    write_file(scratch("wav.bin"), make_raw(10, 1));
    write_text(scratch("wav.cue"),
               "FILE \"retro3do-test-wav.bin\" BINARY\n"
               "  TRACK 01 MODE1/2352\n"
               "    INDEX 01 00:00:00\n"
               "FILE \"music.wav\" WAVE\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 01 00:00:00\n");

    Disc disc;
    CHECK(!disc.open(scratch("wav.cue")));
    CHECK(!disc.last_error().empty());
}

TEST(a_cue_with_no_file_line_reports_why) {
    write_text(scratch("empty.cue"), "TRACK 01 MODE1/2352\n  INDEX 01 00:00:00\n");
    Disc disc;
    CHECK(!disc.open(scratch("empty.cue")));
    CHECK(!disc.last_error().empty());
}

TEST(data_read_through_a_cue_still_lands_on_the_right_sector) {
    write_file(scratch("cuedata.bin"), make_raw(64, 1));
    write_text(scratch("cuedata.cue"),
               "FILE \"retro3do-test-cuedata.bin\" BINARY\n"
               "  TRACK 01 MODE1/2352\n"
               "    INDEX 01 00:00:00\n");

    Disc disc;
    CHECK(disc.open(scratch("cuedata.cue")));
    u8 sector[2048];
    CHECK(disc.read_sector(10, sector));
    CHECK_EQ(sector[0], 11u);
}

// ---------------------------------------------------------------------------
// Raw access
// ---------------------------------------------------------------------------

TEST(raw_reads_hand_back_the_whole_sector) {
    const char* path = scratch("rawread.bin");
    write_file(path, make_raw(4, 1));

    Disc disc;
    CHECK(disc.open(path));

    u8 sector[2352];
    u32 size = 0;
    CHECK(disc.read_raw_sector(1, sector, &size));
    CHECK_EQ(size, 2352u);
    // The sync pattern is part of what comes back, unlike a cooked read.
    CHECK_EQ(sector[0], 0x00u);
    CHECK_EQ(sector[1], 0xffu);
}

// ---------------------------------------------------------------------------
// Opening by descriptor
// ---------------------------------------------------------------------------
//
// Android's scoped storage hands out content URIs, not paths: there is no
// filename to open, only a descriptor the system opened for us. These cover the
// same ground as the path tests, because the two routes must agree.

#include <fcntl.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

int open_read_fd(const char* path) {
    return ::open(path, O_RDONLY);
}

}  // namespace

TEST(a_descriptor_reads_the_same_data_as_a_path) {
    const char* path = scratch("fd-cooked.iso");
    write_file(path, make_cooked(4));

    Disc by_path;
    CHECK(by_path.open(path));
    u8 expected[2048];
    CHECK(by_path.read_sector(2, expected));

    Disc by_fd;
    CHECK(by_fd.open_fd(open_read_fd(path), "fd-cooked.iso"));
    CHECK_EQ(by_fd.sector_count(), by_path.sector_count());

    u8 actual[2048];
    CHECK(by_fd.read_sector(2, actual));
    CHECK_EQ(actual[0], expected[0]);
    CHECK_EQ(actual[2047], expected[2047]);
}

TEST(layout_detection_works_on_a_descriptor_too) {
    // The descriptor carries no filename, so detection cannot fall back on the
    // extension even in principle — which is the behaviour we wanted anyway.
    const char* path = scratch("fd-raw.bin");
    write_file(path, make_raw(8, 2));

    Disc disc;
    CHECK(disc.open_fd(open_read_fd(path), "fd-raw.bin"));
    CHECK(disc.layout() == SectorLayout::Raw2352Mode2);

    u8 sector[2048];
    CHECK(disc.read_sector(5, sector));
    CHECK_EQ(sector[0], 6u);
}

TEST(a_bad_descriptor_is_reported_not_crashed_on) {
    Disc disc;
    CHECK(!disc.open_fd(-1, "nothing.iso"));
    CHECK(!disc.last_error().empty());
    CHECK(!disc.is_open());
}

TEST(a_cue_by_descriptor_is_refused_with_a_useful_message) {
    // A cue names a sibling image, and a descriptor gives no way to reach it.
    // Refusing clearly beats opening the cue as though it were an image and
    // then reporting a corrupt disc.
    const char* path = scratch("fd-refuse.cue");
    write_text(path, "FILE \"x.bin\" BINARY\n  TRACK 01 MODE1/2352\n");

    Disc disc;
    CHECK(!disc.open_fd(open_read_fd(path), "fd-refuse.cue"));
    CHECK(disc.last_error().find("cue") != std::string::npos);
}

TEST(closing_the_disc_releases_the_descriptor) {
    // fdopen adopts the descriptor, so it must not be closed twice and must not
    // leak. Opening many in a row would exhaust the process limit if it leaked.
    const char* path = scratch("fd-leak.iso");
    write_file(path, make_cooked(2));

    for (int i = 0; i < 512; ++i) {
        Disc disc;
        CHECK(disc.open_fd(open_read_fd(path), "fd-leak.iso"));
        disc.close();
    }
    // Getting here without running out of descriptors is the assertion.
    Disc final_check;
    CHECK(final_check.open_fd(open_read_fd(path), "fd-leak.iso"));
}
