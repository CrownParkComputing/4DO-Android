// The Dear ImGui layer: launcher, settings, and the in-session overlay.
//
// The UI never touches the console directly for anything with a deadline. It
// reads state and posts intent; the app loop acts on it. That keeps a slow
// frame in the UI from ever showing up as an audio dropout.
#pragma once

#include <string>

struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;

namespace retro3do {

class Console;

// What the UI is asking the app to do next. Polled and cleared once per frame.
struct UiIntent {
    bool quit = false;
    bool reset = false;
    bool toggle_pause = false;
    bool bios_chosen = false;
    std::string bios_path;
    bool test_pattern = false;
};

class Ui {
public:
    Ui();
    ~Ui();

    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    bool init(SDL_Window* window, SDL_Renderer* renderer);
    void shutdown();

    // Feed SDL events in before the app consumes them, so ImGui can claim
    // keyboard and pointer input when a panel has focus.
    void process_event(const SDL_Event& event);
    bool wants_mouse() const;
    bool wants_keyboard() const;

    // Build this frame's UI. Returns what the user asked for.
    UiIntent build(Console& console, bool emulating, double fps);

    // Draw the built UI over whatever the app has already presented.
    void render(SDL_Renderer* renderer);

private:
    void draw_launcher(Console& console, UiIntent& intent);
    void draw_overlay(Console& console, double fps, UiIntent& intent);

    bool initialised_ = false;
    bool show_launcher_ = true;
    char bios_path_buffer_[512] = {};
    std::string status_;
};

}  // namespace retro3do
