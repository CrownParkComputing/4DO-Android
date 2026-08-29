// Entry point.
//
// SDL provides main on every platform it supports, including the Android
// activity and the iOS UIApplication, so this file is the same everywhere and
// there is no per-platform launcher to maintain.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "platform/app.h"

int main(int argc, char* argv[]) {

    // Set before SDL_Init, or it is read too late to matter. Immersive mode
    // keeps the navigation and status bars out of the picture; without it they
    // overlay the app and take touches intended for the controls beneath them.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
    SDL_SetHint("SDL_ANDROID_BLOCK_ON_PAUSE", "1");

    retro3do::App app;

    // retro3do [bios] [disc] - both optional, and either overrides whatever the
    // previous run remembered. Without this the emulator can only be pointed at
    // a file through the GUI, which makes it awkward to test and impossible to
    // script.
    app.set_launch_files(argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "");

    if (!app.init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", app.last_error().c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Retro-3DO",
                                 app.last_error().c_str(), nullptr);
        return 1;
    }
    return app.run();
}
