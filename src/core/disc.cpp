#include "disc.h"

extern "C" {
#include <libchdr/chd.h>
#include <libchdr/cdrom.h>
}

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

// A CHD reader: the library handle, the geometry needed to turn a logical block
// into a position inside a hunk, and one hunk of cache.
//
// Caching matters more than it looks. A hunk here is 19,584 bytes - eight CD
// frames - and decompressing one costs an LZMA or FLAC decode. Reading a file
// sector by sector without a cache decodes the same hunk eight times over.
struct Disc::ChdReader {
    chd_file* file = nullptr;
    u32 hunk_bytes = 0;
    u32 unit_bytes = 0;
    u32 frames_per_hunk = 0;

    // Where the first data track's frames begin inside the CHD. Tracks are
    // stored consecutively, each padded up to a multiple of four frames, so
    // this is not always zero.
    u32 data_track_frame = 0;

    std::vector<u8> hunk;
    u32 cached_hunk = 0xffffffffu;
};

void Disc::close() {
    if (chd_reader_ != nullptr) {
        if (chd_reader_->file != nullptr) {
            chd_close(chd_reader_->file);
        }
        delete chd_reader_;
        chd_reader_ = nullptr;
    }
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    path_.clear();
    tracks_.clear();
    sector_count_ = 0;
    chd_ = ChdInfo{};
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

    // A CHD takes a completely different path: compressed hunks rather than a
    // flat run of sectors.
    if (is_chd_file()) {
        return open_chd(path);
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

    if (is_chd_file()) {
        return open_chd(display_name);
    }

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

// Recognise a CHD before anything else looks at the file. The raw-image path
// must never see one: it would take compressed bytes for disc contents and look
// like a corrupt disc rather than an unreadable file.
bool Disc::is_chd_file() {
    chd_ = probe_chd(file_);
    return chd_.is_chd;
}

// Open a CHD and work out where the data track lives inside it.
bool Disc::open_chd(const std::string& display_name) {
    // libchdr takes ownership of the handle on success, and closes it itself.
    std::FILE* handle = file_;
    file_ = nullptr;

    auto* reader = new ChdReader();
    chd_error err = chd_open_file(handle, CHD_OPEN_READ, nullptr, &reader->file);
    if (err != CHDERR_NONE) {
        delete reader;
        if (handle != nullptr) {
            std::fclose(handle);
        }
        last_error_ = display_name + ": " + chd_error_string(err);
        return false;
    }

    const chd_header* header = chd_get_header(reader->file);
    reader->hunk_bytes = header->hunkbytes;
    reader->unit_bytes = header->unitbytes;
    if (reader->unit_bytes == 0 || reader->hunk_bytes == 0 ||
        reader->hunk_bytes % reader->unit_bytes != 0) {
        chd_close(reader->file);
        delete reader;
        last_error_ = display_name + ": unexpected CHD geometry";
        return false;
    }
    reader->frames_per_hunk = reader->hunk_bytes / reader->unit_bytes;
    reader->hunk.resize(reader->hunk_bytes);

    // Walk the track metadata. Tracks sit consecutively in the CHD, each padded
    // up to a multiple of four frames, so a track's start has to be accumulated
    // rather than derived from its number.
    u32 chd_frame = 0;
    u32 lba = 0;
    bool found_data = false;
    for (u32 index = 0; index < 99; ++index) {
        char meta[512] = {};
        u32 length = 0;
        int number = 0, frames = 0, pregap = 0, postgap = 0;
        char type[64] = {}, subtype[64] = {}, pgtype[64] = {}, pgsub[64] = {};

        if (chd_get_metadata(reader->file, CDROM_TRACK_METADATA2_TAG, index,
                             meta, sizeof meta, &length, nullptr,
                             nullptr) == CHDERR_NONE) {
            if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &number, type,
                       subtype, &frames, &pregap, pgtype, pgsub,
                       &postgap) != 8) {
                break;
            }
        } else if (chd_get_metadata(reader->file, CDROM_TRACK_METADATA_TAG,
                                    index, meta, sizeof meta, &length, nullptr,
                                    nullptr) == CHDERR_NONE) {
            if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &number, type,
                       subtype, &frames) != 4) {
                break;
            }
        } else {
            break;
        }

        Track track;
        track.number = static_cast<u32>(number);
        track.is_audio = (std::string(type) == "AUDIO");
        track.start_lba = lba;
        track.length_sectors = static_cast<u32>(frames);
        tracks_.push_back(track);

        if (!found_data && !track.is_audio) {
            reader->data_track_frame = chd_frame;
            sector_count_ = static_cast<u32>(frames);
            found_data = true;
        }

        lba += static_cast<u32>(frames);
        chd_frame += static_cast<u32>(frames);
        // Pad to the track boundary the format requires.
        if (chd_frame % CD_TRACK_PADDING != 0) {
            chd_frame += CD_TRACK_PADDING - (chd_frame % CD_TRACK_PADDING);
        }
    }

    if (!found_data) {
        chd_close(reader->file);
        delete reader;
        last_error_ = display_name + ": no data track";
        return false;
    }

    // libchdr is built to hand back cooked user data, so a frame begins at the
    // payload with no sync or header in front of it.
    layout_ = SectorLayout::Cooked2048;
    chd_reader_ = reader;
    last_error_.clear();
    return true;
}

bool Disc::read_chd_frame(u32 chd_frame, u8* out) {
    ChdReader* r = chd_reader_;
    const u32 hunk = chd_frame / r->frames_per_hunk;
    const u32 within = (chd_frame % r->frames_per_hunk) * r->unit_bytes;

    if (hunk != r->cached_hunk) {
        if (chd_read(r->file, hunk, r->hunk.data()) != CHDERR_NONE) {
            return false;
        }
        r->cached_hunk = hunk;
    }
    std::memcpy(out, r->hunk.data() + within, CD_MAX_SECTOR_DATA);
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
    if (out == nullptr || lba >= sector_count_) {
        return false;
    }

    if (chd_reader_ != nullptr) {
        u8 frame[CD_MAX_SECTOR_DATA];
        if (!read_chd_frame(chd_reader_->data_track_frame + lba, frame)) {
            return false;
        }
        std::memcpy(out, frame, kSectorUserBytes);
        return true;
    }

    if (file_ == nullptr) {
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
    if (out == nullptr || lba >= sector_count_) {
        return false;
    }

    if (chd_reader_ != nullptr) {
        if (!read_chd_frame(chd_reader_->data_track_frame + lba, out)) {
            return false;
        }
        if (out_size != nullptr) {
            *out_size = CD_MAX_SECTOR_DATA;
        }
        return true;
    }

    if (file_ == nullptr) {
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
