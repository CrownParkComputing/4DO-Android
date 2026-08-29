package com.crownparkcomputing.retro3do;

import org.libsdl.app.SDLActivity;

/**
 * The Android entry point.
 *
 * Everything the emulator does lives in the native library; this class exists so
 * that Android-specific concerns — storage access, lifecycle, intents — have
 * somewhere to go that is not SDL's own activity. It is deliberately thin, and
 * should stay that way: logic that ends up here has to be written again for iOS.
 */
public class Retro3DOActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        // Loaded in order. SDL first, because our library links against it.
        return new String[] {
            "SDL3",
            "main"
        };
    }
}
