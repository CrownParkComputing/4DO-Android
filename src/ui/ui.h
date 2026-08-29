// The Dear ImGui layer: launcher, settings, and the in-session overlay.
//
// The UI never touches the console directly for anything with a deadline. It
// reads state and posts intent; the app loop acts on it. That keeps a slow
// frame in the UI from ever showing up as an audio dropout.
#pragma once

#include <string>

#include "core/types.h"
#include "ui/file_browser.h"

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
    std::string bios_name;
    bool test_pattern = false;
    bool disc_chosen = false;
    std::string disc_path;
    std::string disc_name;
    bool eject = false;
    bool toggle_touch_controls = false;
    bool toggle_layout_edit = false;
    bool reset_touch_layout = false;
    bool region_changed = false;
    bool set_region_pal = false;
};

class Ui {
public:
    Ui();
    ~Ui();

    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    bool init(SDL_Window* window, SDL_Renderer* renderer);

    // Show what was remembered from a previous run. Without this the fields
    // come back empty after a restart even though a BIOS and disc are loaded,
    // which reads as "it forgot" when in fact it did not.
    void set_remembered_bios(const std::string& name, const std::string& target);
    void set_remembered_disc(const std::string& name, const std::string& target);
    void shutdown();

    // Feed SDL events in before the app consumes them, so ImGui can claim
    // keyboard and pointer input when a panel has focus.
    void process_event(const SDL_Event& event);
    bool wants_mouse() const;
    bool wants_keyboard() const;

    // Build this frame's UI. Returns what the user asked for.
    // display_fps is how smooth the app feels; emulated_fps is whether the
    // machine is actually keeping up. They are different numbers and both are
    // shown, because a machine running at half speed behind a perfectly smooth
    // window is precisely the failure the previous core made invisible.
    UiIntent build(Console& console, bool emulating, double display_fps,
                   double emulated_fps, double frame_ms, u64 underruns,
                   bool touch_visible, bool touch_editing);

    // Draw the built UI over whatever the app has already presented.
    void render(SDL_Renderer* renderer);

private:
    void draw_launcher(Console& console, bool touch_visible,
                       bool touch_editing, UiIntent& intent);
    void draw_overlay(Console& console, double display_fps,
                      double emulated_fps, double frame_ms, u64 underruns,
                      bool touch_visible, bool touch_editing, UiIntent& intent);

    FileBrowser browser_;
    // Which field the browser is filling in. Without this, a picked file has
    // nowhere to go.
    enum class Browsing { None, Bios, Disc };
    Browsing browsing_ = Browsing::None;

    // Everything drawn is multiplied by this. ImGui's default font is 13 pixels
    // tall, which on a 1080p handheld is unreadable, and fixed pixel button
    // widths clip their own labels. Scale is derived from the window rather
    // than from SDL's display scale, which reports 1.0 on many Android devices.
    float scale_ = 1.0f;

    bool initialised_ = false;
    bool show_launcher_ = true;
    // What the text fields show, and what they actually refer to.
    //
    // These are NOT the same thing and conflating them was a real bug: after
    // browsing, the field shows a readable filename while the thing that must
    // be opened is a document URI. Sending the display text to the opener gives
    // "No content provider: <filename>", because a bare name has no scheme.
    //
    // So the picked target is kept separately, and cleared the moment the user
    // edits the field by hand - at which point what they typed IS the target.
    char bios_path_buffer_[512] = {};
    char disc_path_buffer_[512] = {};
    std::string bios_target_;
    std::string disc_target_;
    std::string status_;
};

}  // namespace retro3do
