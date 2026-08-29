// The application shell: window, event pump, frame pacing, and the handoff of
// emulated frames to the screen.
//
// Everything platform-specific lives behind SDL, so this same file is what runs
// on Linux, Windows, Android and iOS. There is no per-platform front end to
// keep in sync, which was the point of choosing SDL over a UI toolkit that
// would have needed a bridge on each side.
#pragma once

#include <memory>
#include <string>

#include "core/types.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_AudioStream;
struct SDL_Gamepad;

namespace retro3do {

class Console;
class Ui;
class Settings;
class TouchPad;
class FrameMailbox;
class EmulatorThread;

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Bring up SDL, the window and the renderer. Returns false and sets
    // last_error() if anything the app cannot run without is missing.
    // Files named on the command line, which override anything remembered from
    // a previous run. Desktop-only in practice - Android and iOS have no
    // argv worth reading - but it is what makes the emulator scriptable, and
    // scriptable is what makes it testable outside a GUI.
    void set_launch_files(std::string bios, std::string disc);

    bool init();

    // Run until the user quits. Returns the process exit code.
    int run();

    const std::string& last_error() const { return last_error_; }

private:
    // One turn of the loop: events, emulate, draw. Split out so that platforms
    // which insist on driving the loop themselves can call it directly.
    void tick();

    void handle_events();
    bool start_audio();
    void stop_audio();
    void feed_audio();
    void apply_keyboard(int scancode, bool down);
    void open_gamepad(u32 which);
    void load_settings();
    void save_settings();
    bool open_bios(const std::string& path, const std::string& name);
    bool open_disc(const std::string& path, const std::string& name);
    void present();
    void ensure_frame_texture(int width, int height);

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  frame_texture_ = nullptr;
    int texture_width_  = 0;
    int texture_height_ = 0;

    std::unique_ptr<Console> console_;
    std::unique_ptr<Ui> ui_;
    std::unique_ptr<FrameMailbox> mailbox_;
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<TouchPad> touch_;
    std::unique_ptr<EmulatorThread> emulator_;

    SDL_AudioStream* audio_stream_ = nullptr;
    SDL_Gamepad* gamepad_ = nullptr;

    std::string launch_bios_;
    std::string launch_disc_;
    bool start_on_launch_ = false;
    bool running_ = false;
    bool emulating_ = false;

    std::string last_error_;
};

}  // namespace retro3do
