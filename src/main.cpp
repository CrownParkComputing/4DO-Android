// Entry point.
//
// SDL provides main on every platform it supports, including the Android
// activity and the iOS UIApplication, so this file is the same everywhere and
// there is no per-platform launcher to maintain.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "platform/app.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // Set before SDL_Init, or it is read too late to matter. Immersive mode
    // keeps the navigation and status bars out of the picture; without it they
    // overlay the app and take touches intended for the controls beneath them.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
    SDL_SetHint("SDL_ANDROID_BLOCK_ON_PAUSE", "1");

    retro3do::App app;
    if (!app.init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", app.last_error().c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Retro-3DO",
                                 app.last_error().c_str(), nullptr);
        return 1;
    }
    return app.run();
}
