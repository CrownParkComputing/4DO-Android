#include "xbus.h"

namespace retro3do {

void CdRomDevice::reset() {
    status_.clear();
    pending_.clear();
    commands_ = 0;
    last_command_ = 0;
}

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
    pending_.clear();
    ++commands_;

    // Every command returns at least one Status Byte when it completes, and
    // that byte describes the DRIVE, not the command - the layout is the SDK's
    // own. An earlier version echoed the command byte back, which is a
    // plausible-looking reply that means nothing to the driver.
    //
    // ERROR stays clear: the drive is present and working even with no disc in
    // it. Reporting a broken drive is not the same as reporting an empty one,
    // and the difference decides whether the BIOS offers to load a disc or
    // gives up.
    u8 status = kStatusReady;
    if (disc_present_) {
        status |= kStatusDiscIn | kStatusSpinUp;
    }
    status_.push_back(status);

    // The driver reads a fixed-length reply. Returning fewer bytes than it
    // reads does not fail cleanly: it reads zeroes off the end of an empty
    // FIFO, takes them for part of the answer, and abandons the conversation
    // after a single command.
    while (status_.size() < kReplyBytes) {
        status_.push_back(0x00);
    }
}

u8 CdRomDevice::read_status() {
    if (status_.empty()) {
        return 0;
    }
    const u8 byte = status_.front();
    if (status_.size() > 1) status_.pop_front();
    return byte;
}

}  // namespace retro3do
