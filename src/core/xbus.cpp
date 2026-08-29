#include <cstdlib>
#include "xbus.h"

#include "disc.h"

namespace retro3do {

namespace {

// A CD address is minutes:seconds:frames, 75 frames a second, and the first two
// seconds are the lead-in - hence the 150.
constexpr u32 kLeadInFrames = 150;

u32 msf_to_lba(u32 msf) {
    const u32 m = (msf >> 16) & 0xffu;
    const u32 s = (msf >> 8) & 0xffu;
    const u32 f = msf & 0xffu;
    const u32 frames = (m * 60u + s) * 75u + f;
    return frames < kLeadInFrames ? 0u : frames - kLeadInFrames;
}

u32 lba_to_msf(u32 lba) {
    const u32 frames = lba + kLeadInFrames;
    return ((frames / (60u * 75u)) << 16) |
           (((frames / 75u) % 60u) << 8) |
           (frames % 75u);
}

}  // namespace


void CdRomDevice::tick(u32 cycles) {
    // Fetch the next sector when the drive would have reached it.
    if (streaming_ && data_pos_ >= data_.size()) {
        if (sector_delay_ > cycles) {
            sector_delay_ -= cycles;
        } else {
            sector_delay_ = kSectorDelay;
            if (fill_next_sector_now()) {
                interrupt_request_ = true;
            }
        }
    }

    if (!completion_pending_) {
        return;
    }
    if (completion_delay_ > cycles) {
        completion_delay_ -= cycles;
        return;
    }
    completion_delay_ = 0;
    completion_pending_ = false;

    // The reply becomes readable now, not when the command was written. The
    // drive does not answer instantly, and the delay is load bearing: the
    // driver reads the bytes it expects and then checks the FIFO is EMPTY,
    // treating anything still waiting as an error. Publishing the reply and a
    // separate completion byte therefore fails every command.
    status_.assign(pending_reply_.begin(), pending_reply_.end());
    pending_reply_.clear();
    interrupt_request_ = true;
}

// Build the reply for one command.
//
// Every command answers with its own opcode first - that is how the driver
// matches a reply to the request it made - and ends with the drive status.
// Reply lengths differ per command and the driver knows them, so returning the
// wrong number of bytes is not a soft failure: it reads zeroes off the end of an
// empty FIFO, takes them for part of the answer, and gives up.
void CdRomDevice::build_reply(u8 command) {
    pending_reply_.clear();
    switch (command) {
        case kCmdVersion:
            // Drive identification. This is what the boot ROM asks for while
            // enumerating the bus, and answering it with zeroes is indoor
            // weather for "no drive fitted" - the machine then boots to its
            // logo and idles forever, never asking about a disc.
            pending_reply_.push_back(kCmdVersion);
            pending_reply_.push_back(0x00);   // manufacturer id
            pending_reply_.push_back(0x10);   //   "
            pending_reply_.push_back(0x00);   // manufacturer number
            pending_reply_.push_back(0x01);   //   "
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(0x00);   // revision
            pending_reply_.push_back(0x00);   //   "
            pending_reply_.push_back(0x00);   // flags
            pending_reply_.push_back(0x00);   //   "
            pending_reply_.push_back(drive_status());
            break;

        case kCmdDataPathCheck:
            // A loopback the driver uses to prove the bus works.
            pending_reply_.push_back(kCmdDataPathCheck);
            pending_reply_.push_back(0xaa);
            pending_reply_.push_back(0x55);
            pending_reply_.push_back(drive_status());
            break;

        case kCmdReadStatus:
            pending_reply_.push_back(kCmdReadStatus);
            pending_reply_.push_back(drive_status());
            break;

        case kCmdReadError:
            // Ten bytes: the opcode, eight bytes of error detail, and a final
            // byte reporting whether there is media. Per MAME's cr560b, which
            // notes it is unsure whether that last byte should instead be the
            // status - so this is a place to revisit if the boot turns on it.
            pending_reply_.push_back(kCmdReadError);
            for (int i = 0; i < 8; ++i) pending_reply_.push_back(0x00);
            pending_reply_.push_back(disc_present_ ? 1 : 0);
            break;

        case kCmdSetMode:
            // Sub-command 0 carries the sector size the host wants back.
            if (last_command_bytes_.size() >= 5 && last_command_bytes_[1] == 0x00) {
                const u32 size = (u32(last_command_bytes_[3]) << 8) |
                                  u32(last_command_bytes_[4]);
                if (size != 0) sector_size_ = size;
            }
            pending_reply_.push_back(kCmdSetMode);
            pending_reply_.push_back(drive_status());
            break;

        case kCmdReadCapacity: {
            pending_reply_.push_back(kCmdReadCapacity);
            const u32 lead_out = lba_to_msf(disc_sectors());
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(u8(lead_out >> 16));
            pending_reply_.push_back(u8(lead_out >> 8));
            pending_reply_.push_back(u8(lead_out));
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(u8(drive_status() | kStatusReady));
            break;
        }

        case kCmdReadDiscInfo: {
            // First track, last track and where the lead-out starts. This is
            // how the machine learns a disc's shape before reading any of it.
            pending_reply_.push_back(kCmdReadDiscInfo);
            if (disc_ == nullptr || !disc_present_) {
                pending_reply_.push_back(u8((drive_status() & ~kStatusReady) |
                                            kStatusError));
                break;
            }
            motor_on_ = true;
            const u32 lead_out = lba_to_msf(disc_sectors());
            pending_reply_.push_back(0x00);              // disc type: CD-ROM
            pending_reply_.push_back(0x01);              // first track
            pending_reply_.push_back(u8(last_track()));
            pending_reply_.push_back(u8(lead_out >> 16));
            pending_reply_.push_back(u8(lead_out >> 8));
            pending_reply_.push_back(u8(lead_out));
            pending_reply_.push_back(drive_status());
            break;
        }

        case kCmdReadToc: {
            // Byte 2 names the track; zero asks about the lead-out.
            const u8 track = last_command_bytes_.size() >= 3
                                 ? last_command_bytes_[2] : 0u;
            pending_reply_.push_back(kCmdReadToc);
            if (disc_ == nullptr || !disc_present_ || track > last_track()) {
                pending_reply_.push_back(u8((drive_status() & ~kStatusReady) |
                                            kStatusError));
                break;
            }
            motor_on_ = true;
            u32 start = 0;
            u8  adr   = 0x14;    // data track, TOC entry
            if (track == 0) {
                start = disc_sectors();
            } else {
                const auto& list = disc_->tracks();
                const size_t index = track - 1;
                if (index < list.size()) {
                    start = list[index].start_lba;
                    adr = list[index].is_audio ? 0x10 : 0x14;
                }
            }
            const u32 msf = lba_to_msf(start);
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(adr);
            pending_reply_.push_back(track == 0 ? 0x01 : track);
            pending_reply_.push_back(track == 0 ? u8(last_track()) : 0x00);
            pending_reply_.push_back(u8(msf >> 16));
            pending_reply_.push_back(u8(msf >> 8));
            pending_reply_.push_back(u8(msf));
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(u8(drive_status() | kStatusReady));
            break;
        }

        case kCmdRead: {
            // Byte 4 selects addressing: 0 means the start is MSF rather than a
            // logical block number.
            u32 start = 0;
            u32 count = 0;
            if (last_command_bytes_.size() >= 7) {
                start = (u32(last_command_bytes_[1]) << 16) |
                        (u32(last_command_bytes_[2]) << 8) |
                         u32(last_command_bytes_[3]);
                if ((last_command_bytes_[4] & 0x01) == 0) start = msf_to_lba(start);
                count = (u32(last_command_bytes_[5]) << 8) |
                         u32(last_command_bytes_[6]);
            }
            start_transfer(start, count);
            motor_on_ = true;
            pending_reply_.push_back(kCmdRead);
            pending_reply_.push_back(drive_status());
            break;
        }

        case kCmdMotorOn:
            motor_on_ = true;
            pending_reply_.push_back(kCmdMotorOn);
            pending_reply_.push_back(drive_status());
            break;

        case kCmdMotorOff:
            motor_on_ = false;
            streaming_ = false;
            pending_reply_.push_back(kCmdMotorOff);
            pending_reply_.push_back(drive_status());
            break;

        default:
            // Acknowledge anything else: echo, then status. A drive that
            // answers nothing looks broken rather than idle.
            pending_reply_.push_back(command);
            pending_reply_.push_back(drive_status());
            break;
    }
}

u32 CdRomDevice::last_track() const {
    if (disc_ == nullptr || disc_->tracks().empty()) {
        return 1;
    }
    return static_cast<u32>(disc_->tracks().size());
}

u32 CdRomDevice::disc_sectors() const {
    return disc_ != nullptr ? disc_->sector_count() : 0u;
}

void CdRomDevice::start_transfer(u32 lba, u32 sectors) {
    transfer_lba_ = lba;
    transfer_sectors_ = sectors;
    streaming_ = true;
    data_.clear();
    data_pos_ = 0;
    sector_delay_ = kSectorDelay;
    fill_next_sector_now();
}

// Pull one sector into the data FIFO. The drive streams a transfer sector by
// sector rather than materialising all of it, which matters: a read can ask for
// far more than would be reasonable to hold at once.
bool CdRomDevice::fill_next_sector_now() {
    // The count in a READ is a floor, not a limit. The drive goes on delivering
    // consecutive sectors for as long as the host keeps draining them, and the
    // host decides how much it actually wants by how much it DMAs.
    //
    // This is not a guess: a real drive asked for ONE block at LBA 0 answers
    // with LBA 0, then 1, then 2, then 3, and keeps going. Stopping at the
    // requested count means the machine reads the disc's volume label and
    // nothing else - it never gets the directory the label points at, so it
    // never finds anything to launch.
    if (disc_ == nullptr || !streaming_) {
        return false;
    }
    if (transfer_lba_ >= disc_->sector_count()) {
        streaming_ = false;
        return false;
    }
    u8 sector[kSectorUserBytes];
    if (!disc_->read_sector(transfer_lba_, sector)) {
        streaming_ = false;
        return false;
    }
    data_.assign(sector, sector + kSectorUserBytes);
    data_pos_ = 0;
    ++transfer_lba_;
    if (transfer_sectors_ > 0) {
        --transfer_sectors_;
    }
    return true;
}

bool CdRomDevice::has_chunk() const {
    return data_pos_ < data_.size() || streaming_;
}

u8 CdRomDevice::read_data() {
    if (data_pos_ >= data_.size()) {
        return 0;   // the next sector arrives when the drive reaches it
    }
    return data_pos_ < data_.size() ? data_[data_pos_++] : 0;
}

u8 CdRomDevice::drive_status() const {
    // Only what is actually true. A drive out of reset has not been told to
    // spin up, so reporting the motor running - and "ready" with it - describes
    // a drive in a state the machine never asked for.
    // The door is shut. This drive is built into the machine rather than
    // plugged into it, so there is no state in which it is not - and the host
    // does look: reporting it changes which code the boot takes.
    // Exactly the bits this BIOS's own driver defines - tray, disc, spin,
    // error, double speed, ready. There is deliberately no "success" bit: MAME
    // lists 0x08 as STATUS_SUCCESS but marks it unconfirmed, and the driver
    // that actually boots this disc has no such bit at all. Setting one the
    // host does not expect is not harmless.
    // Only what is true. Per MAME's cr560b a drive out of reset reports media
    // if a disc is in it and nothing else; the motor is reported once it has
    // been told to spin up.
    u8 status = kStatusDoor;
    if (disc_present_) status |= kStatusDiscIn;
    if (motor_on_)     status |= kStatusSpinUp | kStatusReady;
    return status;
}

void CdRomDevice::reset() {
    status_.clear();
    pending_.clear();
    completion_pending_ = false;
    completion_delay_ = 0;
    media_changed_ = false;
    interrupt_request_ = false;
    motor_on_ = false;
    ready_ = false;
    last_error_ = 0;
    streaming_ = false;
    pending_reply_.clear();
    transfer_lba_ = 0;
    transfer_sectors_ = 0;
    sector_size_ = kSectorUserBytes;
    data_.clear();
    data_pos_ = 0;

    commands_ = 0;
    last_command_ = 0;
}

#include <cstdio>
namespace { bool trace() { return getenv("CDTRACE") != nullptr; } }

void CdRomDevice::write_command(u8 byte) {
    // Commands are multiple bytes and arrive one byte at a time, so the device
    // has to know where one ends. The boot ROM settles the length: it writes
    // 0x83 followed by exactly six more bytes before it starts polling for a
    // reply, and every later command it sends is the same width.
    //
    // Answering per byte instead - which is what this did first - makes a single
    // seven-byte command look like seven completed commands, so the reply FIFO
    // runs six bytes ahead of the conversation and every subsequent exchange
    // reads the previous command's answer.
    pending_.push_back(byte);
    if (pending_.size() < kCommandBytes) {
        return;
    }

    last_command_ = pending_.front();
    last_command_bytes_.assign(pending_.begin(), pending_.end());
    pending_.clear();
    ++commands_;
    if (trace()) {
        fprintf(stderr, "[cd] command");
        for (u8 b : last_command_bytes_) fprintf(stderr, " %02X", b);
        fprintf(stderr, "\n");
    }

    build_reply(last_command_);

    completion_pending_ = true;
    completion_delay_ = kCompletionDelay;
}

u8 CdRomDevice::read_status() {
    if (status_.empty()) {
        return 0;
    }
    const u8 byte = status_.front();
    status_.pop_front();
    if (trace()) fprintf(stderr, "  R %02X\n", byte);

    return byte;
}

}  // namespace retro3do
