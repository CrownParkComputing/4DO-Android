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
    status_.push_back(drive_status());
    interrupt_request_ = true;
}

u8 CdRomDevice::drive_status() const {
    u8 status = kStatusReady;
    if (disc_present_) {
        status |= kStatusDiscIn | kStatusSpinUp;
    }
    return status;
}

void CdRomDevice::reset() {
    status_.clear();
    pending_.clear();
    completion_pending_ = false;
    completion_delay_ = 0;
    media_changed_ = false;
    interrupt_request_ = false;
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

    // Every command returns at least one Status Byte when it completes, and
    // that byte describes the DRIVE, not the command - the layout is the SDK's
    // own. An earlier version echoed the command byte back, which is a
    // The acknowledgement opens by ECHOING the command byte. That is how the
    // driver matches a reply to the request it belongs to, and it is not a
    // guess: sweeping every possible value of this byte against the real ROM
    // changes the machine's behaviour for exactly one of the 256, and that one
    // is the opcode it had just sent.
    status_.push_back(last_command_);

    // The driver reads a fixed-length reply. Returning fewer bytes than it
    // reads does not fail cleanly: it reads zeroes off the end of an empty
    // FIFO, takes them for part of the answer, and abandons the conversation
    // after a single command.
    while (status_.size() < kReplyBytes) {
        status_.push_back(0x00);
    }

    // A command completes in TWO phases. What has just been queued is only the
    // acknowledgement; the drive reports completion separately, afterwards.
    //
    // This is visible in the driver and not really inferable without it: having
    // drained the acknowledgement it goes straight into a loop selecting the
    // device and testing the poll register for status-ready, and waits there
    // indefinitely. Queue only the acknowledgement and the machine hangs in
    // that loop having been told, as far as it can tell, nothing at all.
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
