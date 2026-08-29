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

    // Every command returns at least one Status Byte when it completes. The
    // first byte echoes the command, which is how the driver matches a reply to
    // what it asked. ERROR stays clear: the drive is present and working even
    // with no disc in it, and reporting a broken drive is not the same as
    // reporting an empty one.
    status_.push_back(last_command_);
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
