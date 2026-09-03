#include "demo_rom.h"

#include "demo_rom_data.h"

namespace retro3do {

static_assert(kDemoRomBytes_len > 32,
              "The demo ROM must contain its ARM exception vectors");
static_assert(kDemoRomBytes_len <= 1024u * 1024u,
              "The demo ROM must fit the 3DO ROM window");

const DemoRom& builtin_demo_rom() {
    static const DemoRom rom{
        kDemoRomBytes,
        kDemoRomBytes_len,
        "Retro-3DO Original Hardware Demo",
        "4d76c53f120b6f0465ae434108773747265ad91e6ce7b9050dfb19a16e3cab49",
    };
    return rom;
}

}  // namespace retro3do
