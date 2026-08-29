#include "disc.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace retro3do {
namespace {

// Every raw CD sector starts with this. Finding it is how a raw image is told
// from a cooked one, rather than trusting the file extension.
constexpr u8 kSyncPattern[12] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

std::string lowercase_extension(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = path.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

std::string directory_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string trimmed(const std::string& text) {
    size_t first = 0;
    while (first < text.size() &&
           std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    size_t last = text.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return text.substr(first, last - first);
}

long file_size(std::FILE* file) {
    const long here = std::ftell(file);
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, here, SEEK_SET);
    return size;
}

}  // namespace

u32 bytes_per_sector(SectorLayout layout) {
    switch (layout) {
        case SectorLayout::Cooked2048:   return 2048;
        case SectorLayout::Raw2352Mode1: return 2352;
        case SectorLayout::Raw2352Mode2: return 2352;
        case SectorLayout::Raw2336Mode2: return 2336;
    }
    return 2048;
}

u32 user_offset_in_sector(SectorLayout layout) {
    switch (layout) {
        case SectorLayout::Cooked2048:   return 0;
        // sync(12) + header(4)
        case SectorLayout::Raw2352Mode1: return 16;
        // sync(12) + header(4) + subheader(8). Missing this eight-byte
        // subheader is what makes a Mode 2 disc read as almost-right garbage.
        case SectorLayout::Raw2352Mode2: return 24;
        case SectorLayout::Raw2336Mode2: return 8;
    }
    return 0;
}

Disc::Disc() = default;

Disc::~Disc() {
    close();
}

void Disc::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    path_.clear();
    tracks_.clear();
    sector_count_ = 0;
}

bool Disc::open(const std::string& path) {
    close();
    last_error_.clear();

    const std::string ext = lowercase_extension(path);
    if (ext == ".cue") {
        return open_cue(path);
    }

    // For a bare image the layout is detected, not assumed: .bin is usually raw
    // and .iso usually cooked, but both conventions are broken often enough
    // that trusting them produces the off-by-304-bytes failure this class
    // exists to avoid.
    if (!open_image(path, SectorLayout::Cooked2048)) {
        return false;
    }

    SectorLayout detected = SectorLayout::Cooked2048;
    if (!detect_layout(&detected)) {
        // detect_layout only fails if the file cannot be read at all.
        close();
        return false;
    }

    layout_ = detected;
    const long size = file_size(file_);
    sector_count_ = static_cast<u32>(size / bytes_per_sector(layout_));

    if (sector_count_ == 0) {
        last_error_ = "Disc image is too small to contain a single sector: " + path;
        close();
        return false;
    }

    // A bare image is one data track covering the whole disc.
    Track track;
    track.number = 1;
    track.is_audio = false;
    track.start_lba = 0;
    track.length_sectors = sector_count_;
    tracks_.push_back(track);

    return true;
}

bool Disc::open_fd(int fd, const std::string& display_name) {
    close();
    last_error_.clear();

    if (fd < 0) {
        last_error_ = "Invalid file descriptor for: " + display_name;
        return false;
    }

    const std::string ext = lowercase_extension(display_name);
    if (ext == ".cue") {
        // A cue names a separate image file, and a descriptor gives no way to
        // find it. Saying so is much better than opening the cue as though it
        // were an image and reporting a corrupt disc.
        last_error_ =
            "A cue sheet cannot be opened by descriptor; open the image it "
            "names instead: " + display_name;
        ::close(fd);
        return false;
    }

    // fdopen adopts the descriptor: closing the FILE* closes it, so ownership
    // passes here and the caller must not close it again.
    file_ = ::fdopen(fd, "rb");
    if (file_ == nullptr) {
        last_error_ = "Could not read the descriptor for: " + display_name;
        ::close(fd);
        return false;
    }
    path_ = display_name;

    SectorLayout detected = SectorLayout::Cooked2048;
    if (!detect_layout(&detected)) {
        close();
        return false;
    }
    layout_ = detected;

    const long size = file_size(file_);
    sector_count_ = static_cast<u32>(size / bytes_per_sector(layout_));
    if (sector_count_ == 0) {
        last_error_ = "Disc image is too small to contain a sector: " + display_name;
        close();
        return false;
    }

    Track track;
    track.number = 1;
    track.is_audio = false;
    track.start_lba = 0;
    track.length_sectors = sector_count_;
    tracks_.push_back(track);
    return true;
}

bool Disc::open_image(const std::string& path, SectorLayout layout) {
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        last_error_ = "Could not open disc image: " + path;
        return false;
    }
    path_ = path;
    layout_ = layout;
    return true;
}

bool Disc::detect_layout(SectorLayout* out) {
    if (file_ == nullptr) {
        return false;
    }

    u8 header[24] = {};
    std::fseek(file_, 0, SEEK_SET);
    const size_t read = std::fread(header, 1, sizeof(header), file_);
    if (read < sizeof(header)) {
        // Too small to be raw, so it can only be cooked.
        *out = SectorLayout::Cooked2048;
        return true;
    }

    if (std::memcmp(header, kSyncPattern, sizeof(kSyncPattern)) != 0) {
        // No sync pattern: the file holds user data directly.
        *out = SectorLayout::Cooked2048;
        return true;
    }

    // Raw. Byte 15 of the sector is the mode, which decides whether there is an
    // eight-byte subheader before the user data.
    const u8 mode = header[15];
    *out = (mode == 2) ? SectorLayout::Raw2352Mode2 : SectorLayout::Raw2352Mode1;
    return true;
}

// ---------------------------------------------------------------------------
// CUE sheets
// ---------------------------------------------------------------------------
bool Disc::open_cue(const std::string& path) {
    std::FILE* cue = std::fopen(path.c_str(), "rb");
    if (cue == nullptr) {
        last_error_ = "Could not open cue sheet: " + path;
        return false;
    }

    std::string binary_path;
    std::vector<Track> parsed;
    bool binary_is_raw = true;

    char line_buffer[1024];
    Track pending;
    bool have_pending = false;

    while (std::fgets(line_buffer, sizeof(line_buffer), cue) != nullptr) {
        const std::string line = trimmed(line_buffer);
        if (line.empty()) continue;

        if (line.compare(0, 4, "FILE") == 0) {
            const size_t open_quote = line.find('"');
            const size_t close_quote = line.find('"', open_quote + 1);
            if (open_quote != std::string::npos && close_quote != std::string::npos) {
                binary_path = line.substr(open_quote + 1, close_quote - open_quote - 1);
            }
            // A cue can name a WAVE or MP3 file for audio tracks. Only binary
            // images are handled; anything else is reported rather than
            // silently producing silence.
            if (line.find("WAVE") != std::string::npos ||
                line.find("MP3") != std::string::npos) {
                binary_is_raw = false;
            }
        } else if (line.compare(0, 5, "TRACK") == 0) {
            if (have_pending) {
                parsed.push_back(pending);
            }
            pending = Track{};
            have_pending = true;
            pending.number = static_cast<u32>(std::atoi(line.c_str() + 6));
            pending.is_audio = line.find("AUDIO") != std::string::npos;
        } else if (line.compare(0, 5, "INDEX") == 0 && have_pending) {
            // INDEX nn mm:ss:ff — the start position in minutes, seconds and
            // frames, 75 frames to the second.
            const size_t space = line.find(' ', 6);
            if (space != std::string::npos) {
                unsigned m = 0, s = 0, f = 0;
                if (std::sscanf(line.c_str() + space + 1, "%u:%u:%u", &m, &s, &f) == 3) {
                    const u32 lba = (m * 60u + s) * 75u + f;
                    // INDEX 01 is the track proper; INDEX 00 is pre-gap.
                    const int index_number = std::atoi(line.c_str() + 6);
                    if (index_number == 1 || pending.start_lba == 0) {
                        pending.start_lba = lba;
                    }
                }
            }
        }
    }
    if (have_pending) {
        parsed.push_back(pending);
    }
    std::fclose(cue);

    if (binary_path.empty()) {
        last_error_ = "Cue sheet names no image file: " + path;
        return false;
    }
    if (!binary_is_raw) {
        last_error_ =
            "Cue sheet references WAVE or MP3 audio, which is not supported: " + path;
        return false;
    }

    const std::string resolved = directory_of(path) + binary_path;
    if (!open_image(resolved, SectorLayout::Raw2352Mode1)) {
        return false;
    }

    SectorLayout detected = SectorLayout::Raw2352Mode1;
    detect_layout(&detected);
    layout_ = detected;

    const long size = file_size(file_);
    sector_count_ = static_cast<u32>(size / bytes_per_sector(layout_));
    if (sector_count_ == 0) {
        last_error_ = "Image named by the cue sheet is empty: " + resolved;
        close();
        return false;
    }

    // Track lengths come from the next track's start, and the last runs to the
    // end of the image.
    tracks_ = parsed;
    std::sort(tracks_.begin(), tracks_.end(),
              [](const Track& a, const Track& b) { return a.start_lba < b.start_lba; });
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const u32 end = (i + 1 < tracks_.size()) ? tracks_[i + 1].start_lba
                                                 : sector_count_;
        tracks_[i].length_sectors =
            end > tracks_[i].start_lba ? end - tracks_[i].start_lba : 0;
    }

    if (tracks_.empty()) {
        Track track;
        track.number = 1;
        track.start_lba = 0;
        track.length_sectors = sector_count_;
        tracks_.push_back(track);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------
bool Disc::read_sector(u32 lba, u8* out) {
    if (file_ == nullptr || out == nullptr || lba >= sector_count_) {
        return false;
    }

    const u32 stride = bytes_per_sector(layout_);
    const u32 offset = user_offset_in_sector(layout_);

    if (std::fseek(file_, static_cast<long>(lba) * stride + offset, SEEK_SET) != 0) {
        return false;
    }
    return std::fread(out, 1, kSectorUserBytes, file_) == kSectorUserBytes;
}

bool Disc::read_raw_sector(u32 lba, u8* out, u32* out_size) {
    if (file_ == nullptr || out == nullptr || lba >= sector_count_) {
        return false;
    }

    const u32 stride = bytes_per_sector(layout_);
    if (std::fseek(file_, static_cast<long>(lba) * stride, SEEK_SET) != 0) {
        return false;
    }
    if (std::fread(out, 1, stride, file_) != stride) {
        return false;
    }
    if (out_size != nullptr) {
        *out_size = stride;
    }
    return true;
}

}  // namespace retro3do
