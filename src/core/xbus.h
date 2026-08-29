// XBUS — the Opera Expansion Bus, and the devices on it.
//
// Implemented from The 3DO Company's own patent, WO 94/16382 "Expansion Bus",
// which documents the protocol in full. See docs/CLEANROOM.md; a granted patent
// is public by construction and on this project's permitted-sources list.
//
// The model, in the patent's own terms
// ------------------------------------
// The bus is FIFOs, not registers. Each device holds:
//
//   Command FIFO       the system writes command bytes into it
//   Status Return FIFO the device returns status through it; EVERY command
//                      returns at least one Status Byte when it completes
//   Data Return FIFO   bulk data back to the system, in "chunks"
//   Data Write FIFO    bulk data to the device (writable devices only)
//
// Interrupts say that a return FIFO has something in it; the Status Return
// interrupt is what tells the system a command has finished.
//
// The two "valid" bits in the Poll Register are ACTIVE LOW. `StatValid-` is
// HIGH when the Status Return FIFO has nothing left, not when it has something.
// That is the opposite of the obvious reading and the easiest thing here to
// implement backwards.
#pragma once

#include <deque>
#include <vector>
#include <string>

#include "types.h"

namespace retro3do {

// Poll Register bits, from the patent's table.
enum : u32 {
    kPollReserved0      = 1u << 0,  // reads 0
    kPollInterruptOff   = 1u << 1,  // Interrupt Disable-: disabled when LOW
    kPollMediaAccess    = 1u << 2,  // media may have been touched; write 1 clears
    kPollReset          = 1u << 3,  // Reset-
    kPollStatusEmpty    = 1u << 4,  // StatValid-: HIGH when status FIFO is empty
    kPollNoChunk        = 1u << 5,  // ChunkValid-: HIGH when no complete chunk
};

// Drive status byte, from The 3DO Company's own SDK header <cdrom.h>. The bus
// patent only defines ERROR; the remaining bits belong to the device, and this
// is the device's own published definition of them. Note that ERROR falls at
// 0x10 in both, which is a useful consistency check on the whole reading.
enum : u32 {
    kStatusReady        = 0x01,
    kStatusDoubleSpeed  = 0x02,
    kStatusError        = 0x10,
    kStatusSpinUp       = 0x20,
    kStatusDiscIn       = 0x40,
    kStatusDoor         = 0x80,
};

// The bus supports sixteen devices, addressed 0..15. A device takes its address
// at power-up by counting strobes, so an address reflects position on the chain
// rather than anything fixed in the device.
constexpr u32 kXbusMaxDevices = 16;

// One device on the bus. Only as much as the boot ROM exercises: accept command
// bytes, and answer with a status byte when the command completes.
class XbusDevice {
public:
    virtual ~XbusDevice() = default;

    // A command byte has been written into the Command FIFO.
    virtual void write_command(u8 byte) = 0;

    // Take the next byte from the Status Return FIFO. Only called when the
    // status FIFO is not empty.
    virtual u8 read_status() = 0;
    virtual bool status_empty() const = 0;

    virtual u8 read_data() { return 0; }
    virtual bool has_chunk() const { return false; }

    // True once if the device has just queued something the host should be
    // interrupted about. Consumed by the caller.
    virtual bool take_interrupt_request() { return false; }

    // Let time pass. A drive does not answer instantly, and here that is not a
    // cosmetic detail: the driver checks that the reply FIFO is EMPTY once it
    // has read the bytes it expected, and treats anything still waiting as an
    // error. A completion queued the instant the acknowledgement is drained
    // therefore fails every command.
    virtual void tick(u32 cycles) { (void)cycles; }

    virtual void reset() = 0;
    virtual const char* name() const = 0;
};

// The 3DO's CD-ROM drive, which is built into the machine rather than plugged
// in. That matters: the drive is always present even with no disc in it, so
// "no disc" must still answer commands. A device that never replies is a broken
// machine, not an empty drive.
class CdRomDevice : public XbusDevice {
public:
    void write_command(u8 byte) override;
    u8 read_status() override;
    bool status_empty() const override { return status_.empty(); }
    void tick(u32 cycles) override;
    void reset() override;
    u8 drive_status() const;
    const char* name() const override { return "CD-ROM"; }

    // Whether a disc is in the drive. With none, commands still complete; they
    // report no media.
    // Inserting a disc is an EVENT, not just a state. The BIOS finishes its
    // start-up with the expansion-bus interrupt armed and then idles; what it
    // is waiting for is to be told the media changed. Setting the flag quietly
    // leaves it idling next to a disc it has no reason to look at.
    void set_disc_present(bool present) {
        if (present == disc_present_) {
            return;
        }
        disc_present_ = present;
        media_changed_ = true;
        interrupt_request_ = true;
    }

    // Media-access is read-clear.
    bool take_media_changed() {
        const bool changed = media_changed_;
        media_changed_ = false;
        return changed;
    }
    bool disc_present() const { return disc_present_; }

    bool take_interrupt_request() override {
        const bool pending = interrupt_request_;
        interrupt_request_ = false;
        return pending;
    }

    u64 commands_received() const { return commands_; }
    u8 last_command() const { return last_command_; }

    // How many bytes make one command. See write_command().
    static constexpr size_t kCommandBytes = 7;

    // How many bytes the drive returns. Observed directly: after the boot ROM
    // finishes writing a command it reads RD_STAT exactly ten times, from a
    // single instruction, without consulting the poll register between reads -
    // so the reply length is fixed and the driver already knows it.
    // How many bytes the drive returns. Measured rather than guessed: over-
    // supply the FIFO and the driver stops at exactly the count it expects.
    static constexpr size_t kReplyBytes = 12;

private:
    std::deque<u8> status_;
    std::deque<u8> pending_;   // bytes of the command still being assembled

    // A command has been acknowledged but has not yet reported completion.
    // Work in flight, and how long until the drive reports it. The exact figure
    // is not critical - it only has to be long enough that the driver finishes
    // reading the acknowledgement first.
    static constexpr u32 kCompletionDelay = 20000;
    u32  completion_delay_ = 0;
    bool completion_pending_ = false;
    bool interrupt_request_  = false;
    bool disc_present_ = false;
    bool media_changed_ = false;
    u64 commands_ = 0;
    u8 last_command_ = 0;
    std::vector<u8> last_command_bytes_;
};

}  // namespace retro3do
