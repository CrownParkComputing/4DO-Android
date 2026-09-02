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
    if (transfer_sectors_ > 0 && data_pos_ >= data_.size()) {
        if (sector_delay_ > cycles) {
            sector_delay_ -= cycles;
        } else {
            sector_delay_ = kSectorDelay;
            if (fill_next_sector_now()) {
                interrupt_request_ = true;
            }
        }
    }

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
            // The opcode, the last error repeated eight times, and the drive
            // status. The final byte is the STATUS, not a media-present flag:
            // the driver compares it against the status it got from the
            // command that failed, and a bare 1 never matches.
            pending_reply_.push_back(kCmdReadError);
            for (int i = 0; i < 8; ++i) pending_reply_.push_back(last_error_);
            pending_reply_.push_back(drive_status());
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
            // The lead-out is the first LBA after the final sector. Converting
            // that LBA to MSF adds the standard 150-frame lead-in exactly once.
            // Adding another 150 here reports a fictitious two seconds after
            // the physical lead-out; DIPIR rejects discs whose authored size
            // reaches that boundary (Road Rash is one of them).
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
            // lba_to_msf() supplies the CD lead-in; the sector count already
            // names the physical lead-out and must not be biased a second time.
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
            // Byte 2 names the track. The reply is one entry out of the table
            // of contents, and the table is indexed by track NUMBER - entry
            // zero is not the lead-out, it is simply absent, and reads back as
            // zeroes.
            const u8 track = last_command_bytes_.size() >= 3
                                 ? last_command_bytes_[2] : 0u;
            pending_reply_.push_back(kCmdReadToc);
            if (disc_ == nullptr || !disc_present_) {
                pending_reply_.push_back(u8((drive_status() & ~kStatusReady) |
                                            kStatusError));
                break;
            }
            motor_on_ = true;

            u8 control = 0;
            u8 number  = 0;
            u32 msf    = 0;
            const auto& list = disc_->tracks();
            const size_t index = track >= 1 ? size_t(track - 1) : list.size();
            if (index < list.size()) {
                // The control nibble says what kind of track this is. A data
                // track is 0x04; audio is zero. The 0x10 "address" nibble
                // belongs to a subchannel Q reply, not to this one, and adding
                // it here makes every track look like the wrong kind.
                control = list[index].is_audio ? 0x00 : 0x04;
                number  = track;
                msf     = lba_to_msf(list[index].start_lba);
            }

            pending_reply_.push_back(0x00);
            pending_reply_.push_back(control);
            pending_reply_.push_back(number);
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(u8(msf >> 16));
            pending_reply_.push_back(u8(msf >> 8));
            pending_reply_.push_back(u8(msf));
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(u8(drive_status() | kStatusReady));
            break;
        }

        case kCmdReadSessionInfo: {
            // Where the session ends, in MSF. Eight bytes; answering with a
            // bare acknowledgement makes the driver read two bytes where it
            // expects eight, and it treats the short reply as a failure.
            pending_reply_.push_back(kCmdReadSessionInfo);
            if (disc_ == nullptr || !disc_present_) {
                pending_reply_.push_back(u8((drive_status() & ~kStatusReady) |
                                            kStatusError));
                break;
            }
            const u32 session = lba_to_msf(disc_sectors());
            pending_reply_.push_back(0x00);
            pending_reply_.push_back(u8(session >> 16));
            pending_reply_.push_back(u8(session >> 8));
            pending_reply_.push_back(u8(session));
            pending_reply_.push_back(0x00);
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
            // Spinning up closes the tray as well as starting the motor. This
            // is the other half of the eject handshake: the driver ejects, sees
            // an empty drive, and spins it back up to load it again.
            motor_on_ = true;
            tray_open_ = false;
            media_changed_ = false;
            pending_reply_.push_back(kCmdMotorOn);
            pending_reply_.push_back(drive_status());
            break;

        case kCmdEject:
            tray_open_ = true;
            motor_on_ = false;
            media_changed_ = false;
            last_error_ = 0;
            pending_reply_.push_back(kCmdEject);
            pending_reply_.push_back(drive_status());
            break;

        case kCmdInject:
            // Reports the tray's state without changing it. Closing it is the
            // spin-up's job.
            pending_reply_.push_back(kCmdInject);
            pending_reply_.push_back(drive_status());
            break;

        case kCmdMotorOff:
            motor_on_ = false;
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
    data_.clear();
    data_pos_ = 0;
    sector_delay_ = kSectorDelay;
    fill_next_sector_now();
}

// Pull one sector into the data FIFO. The drive streams a transfer sector by
// sector rather than materialising all of it, which matters: a read can ask for
// far more than would be reasonable to hold at once.
bool CdRomDevice::fill_next_sector_now() {
    // Deliver exactly the blocks the command asked for, and no more.
    //
    // An earlier version streamed on indefinitely, on the evidence that a
    // reference emulator reads LBA 0, then 1, 2 and 3 after a single one-block
    // READ. That reading was wrong: those sectors come from SEPARATE read
    // commands, one per sector, which the command trace shows plainly. Leaving
    // the drive streaming holds data-ready asserted for ever, which tells the
    // host the previous transfer never finished.
    if (disc_ == nullptr || transfer_sectors_ == 0) {
        return false;
    }
    if (transfer_lba_ >= disc_->sector_count()) {
        transfer_sectors_ = 0;
        return false;
    }
    u8 sector[kSectorUserBytes];
    if (!disc_->read_sector(transfer_lba_, sector)) {
        transfer_sectors_ = 0;
        return false;
    }
    const size_t block = sector_size_ != 0 && sector_size_ <= kSectorUserBytes
                             ? sector_size_ : kSectorUserBytes;
    data_.assign(sector, sector + block);
    data_pos_ = 0;
    ++transfer_lba_;
    --transfer_sectors_;
    return true;
}

bool CdRomDevice::has_chunk() const {
    return data_pos_ < data_.size();
}

u8 CdRomDevice::read_data() {
    if (data_pos_ >= data_.size()) {
        return 0;
    }
    const u8 byte = data_[data_pos_++];

    // Draining the last byte of a block pulls the next one in behind it, if
    // the command asked for more. The drive keeps data-ready asserted across
    // the join, so a multi-block read is one continuous stream to the host
    // rather than a sequence the host has to re-arm.
    //
    // Waiting for a rotational delay here instead leaves the host reading
    // zeroes off an empty FIFO while data-ready still says otherwise, and the
    // transfer never completes.
    if (data_pos_ >= data_.size()) {
        data_.clear();
        data_pos_ = 0;
        fill_next_sector_now();
    }
    return byte;
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
    // A drive with a disc in it reports itself ready, tray shut, disc present
    // and ALREADY SPINNING - 0xE1 - without being told to spin up.
    //
    // Observed, not read: a reference emulator logs that byte as the drive's
    // state before it issues any spin-up command. It matters because the boot
    // branches on it. Told the motor is stopped, the driver goes off to check
    // the data path and spin the drive up; told 0xE1, it skips both and gets on
    // with setting the mode and reading.
    //
    // MAME's cr560b reports media only and leaves the motor stopped, which is
    // where this started and is why the sequence diverged.
    //
    // EJECT is the exception to all of it. The tray really can open, and while
    // it is open the drive reports READY and nothing else - no tray, no disc,
    // no motor. The driver ejects the disc on purpose partway through a mount,
    // reads the status back, and uses it to decide what to do next; told the
    // drive is still fully loaded it concludes the eject silently failed and
    // waits for a drive-ready notification that never comes.
    if (tray_open_) {
        return kStatusReady;
    }
    u8 status = kStatusDoor;
    if (disc_present_) status |= kStatusDiscIn | kStatusSpinUp | kStatusReady;
    else if (motor_on_) status |= kStatusSpinUp | kStatusReady;
    return status;
}

void CdRomDevice::reset() {
    status_.clear();
    pending_.clear();
    media_changed_ = false;
    interrupt_request_ = false;
    motor_on_ = false;
    tray_open_ = false;
    ready_ = false;
    last_error_ = 0;
    pending_reply_.clear();
    transfer_lba_ = 0;
    transfer_sectors_ = 0;
    sector_size_ = kSectorUserBytes;
    data_.clear();
    data_pos_ = 0;
    sector_delay_ = 0;

    commands_ = 0;
    last_command_ = 0;
    last_command_bytes_.clear();
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
    // ABORT is the exception, and it is one byte long. It has to be: it is
    // what the driver sends to interrupt a transfer that is already running,
    // and a drive that waited for six more bytes before acting on it would
    // never abort anything.
    //
    // Getting this wrong is not a lost abort. The six bytes that follow belong
    // to the NEXT command, so they are swallowed as part of this one and every
    // command after it is read one byte out of step - the machine goes on
    // issuing perfectly good commands and the drive goes on answering
    // perfectly good replies, to different questions.
    pending_.push_back(byte);
    if (pending_.size() < kCommandBytes && pending_.front() != kCmdAbort) {
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

    // The reply is readable the moment the last command byte lands. The driver
    // writes seven bytes and then polls for status-ready, and it does not tick
    // the machine on while it polls - it spins. Any delay here is a hang, not
    // a slower drive.
    build_reply(last_command_);
    status_.assign(pending_reply_.begin(), pending_reply_.end());
    pending_reply_.clear();
    interrupt_request_ = true;
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
