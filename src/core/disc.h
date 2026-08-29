// Disc images.
//
// This layer knows nothing about the 3DO. It turns a file on disk into "give me
// logical block N as 2048 bytes of user data", plus a track list, and it is the
// part that has to understand the several ways a CD can be stored in a file.
//
// The sector-stride problem
// -------------------------
// A CD sector on the disc is 2352 bytes: sync, header, then the user data, then
// error correction. Only some of that is the payload, and *where* the payload
// starts depends on the mode. A dump can be stored either as full 2352-byte raw
// sectors or as 2048-byte cooked ones, and the two are indistinguishable from
// the file extension alone.
//
// Reading a raw image as though it were cooked is the classic failure here: the
// first 2048 bytes of a raw sector do contain real data, so it *almost* works,
// and then everything past the first sector is off by 304 bytes and looks like
// a corrupt disc rather than a misread one.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "types.h"

namespace retro3do {

// A CD sector's user area is always this, whatever the container.
constexpr u32 kSectorUserBytes = 2048;

// The layouts a sector can have in a file.
enum class SectorLayout {
    Cooked2048,   // user data only
    Raw2352Mode1, // sync(12) + header(4) + user(2048) + ecc(288)
    Raw2352Mode2, // sync(12) + header(4) + subheader(8) + user(2048) + edc(280)
    Raw2336Mode2, // subheader(8) + user(2048) + edc(280)
};

u32 bytes_per_sector(SectorLayout layout);
u32 user_offset_in_sector(SectorLayout layout);

struct Track {
    u32  number = 0;
    bool is_audio = false;
    u32  start_lba = 0;
    u32  length_sectors = 0;
};

class Disc {
public:
    Disc();
    ~Disc();

    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;

    // Open an image. Accepts .iso, .bin, .img and .cue; the layout of a raw
    // image is detected rather than assumed. Returns false and sets
    // last_error().
    bool open(const std::string& path);

    // Open an already-opened file descriptor, taking ownership of it.
    //
    // This exists for Android's Storage Access Framework, which hands out
    // content URIs rather than paths: there is no filename to fopen, only a
    // descriptor the system opened on the app's behalf. `display_name` is used
    // for messages and to recognise the extension, since the descriptor carries
    // neither.
    //
    // A cue sheet cannot be opened this way — it names a sibling file that a
    // descriptor gives no way to reach. Callers holding a cue must resolve the
    // sibling themselves and pass the image.
    bool open_fd(int fd, const std::string& display_name);

    void close();

    bool is_open() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }
    const std::string& last_error() const { return last_error_; }

    SectorLayout layout() const { return layout_; }
    u32 sector_count() const { return sector_count_; }

    const std::vector<Track>& tracks() const { return tracks_; }

    // Read one logical block's 2048 bytes of user data. Returns false if the
    // block is past the end of the image.
    bool read_sector(u32 lba, u8* out);

    // Read raw bytes for a sector, whatever the container holds. Used for audio
    // tracks, where there is no user-data subset to extract.
    bool read_raw_sector(u32 lba, u8* out, u32* out_size);

private:
    bool open_cue(const std::string& path);
    bool open_image(const std::string& path, SectorLayout layout);
    bool detect_layout(SectorLayout* out);

    std::FILE* file_ = nullptr;
    std::string path_;
    std::string last_error_;

    SectorLayout layout_ = SectorLayout::Cooked2048;
    u32 sector_count_ = 0;
    std::vector<Track> tracks_;
};

}  // namespace retro3do
