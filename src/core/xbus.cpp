#include <cstdlib>
#include "xbus.h"

namespace retro3do {

void CdRomDevice::tick(u32 cycles) {
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

        case kCmdMotorOn:
            motor_on_ = true;
            pending_reply_.push_back(kCmdMotorOn);
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

u8 CdRomDevice::drive_status() const {
    // Only what is actually true. A drive out of reset has not been told to
    // spin up, so reporting the motor running - and "ready" with it - describes
    // a drive in a state the machine never asked for.
    u8 status = 0;
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
    pending_reply_.clear();

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

    return byte;
}

}  // namespace retro3do
