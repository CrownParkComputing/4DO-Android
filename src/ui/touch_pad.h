// On-screen controls.
//
// A phone has no buttons. Without these the emulator is unplayable on the
// platform it is mainly for, however well it runs.
//
// Positions are stored as fractions of the play area, not pixels. A phone, a
// foldable and a tablet have wildly different screens, and a layout in pixels
// that suits one is off the edge of another — and rotating the device would
// move every control. Fractions survive all of it.
#pragma once

#include <string>
#include <vector>

#include "core/pad.h"
#include "core/pad_layout.h"
#include "core/types.h"

struct SDL_Renderer;
union SDL_Event;

namespace retro3do {

class Settings;

class TouchPad {
public:
    TouchPad();

    // Touch controls are on by default only where there is no other way to
    // play. On a desktop they are off unless asked for.
    void set_visible(bool visible) { visible_ = visible; }

    // visible_ is the saved user preference.  The effective state also takes
    // the host hardware into account so plugging in a controller does not
    // overwrite that preference: unplugging it restores the glass controls
    // exactly as the user left them.
    bool enabled() const { return visible_; }
    bool visible() const { return visible_ && !gamepad_present_; }
    bool physical_gamepad_present() const { return gamepad_present_; }

    // A gamepad appearing is a good reason to get the controls out of the way,
    // and its disappearing a good reason to bring them back.
    void set_physical_gamepad_present(bool present);

    void load_layout(const Settings& settings);
    void save_layout(Settings& settings) const;

    // Build the default layout for a given screen shape.
    //
    // Positions are fractions of width and height respectively, which means a
    // cluster laid out with equal fractional offsets comes out STRETCHED: on a
    // 16:9 screen a horizontal offset spans nearly twice the pixels a vertical
    // one does, so a d-pad "cross" is a wide diamond. Offsets are therefore
    // derived from the smaller dimension and converted per axis, which needs
    // the aspect ratio and so cannot be a static table.
    void reset_layout(int window_width, int window_height);

    // Layout mode: dragging moves controls instead of pressing them.
    void set_editing(bool editing) { editing_ = editing; }
    bool editing() const { return editing_; }

    // Feed touches and mouse. Returns true if the event was consumed, so the
    // caller does not also treat it as something else.
    bool handle_event(const SDL_Event& event, int window_width, int window_height,
                      PadState& pads);

    void draw(SDL_Renderer* renderer, int window_width, int window_height) const;

    const std::vector<TouchControl>& controls() const { return controls_; }

private:
    int control_at(float px, float py, int window_width, int window_height) const;
    float radius_pixels(const TouchControl& control, int w, int h) const;

    std::vector<TouchControl> controls_;
    bool visible_ = false;
    bool editing_ = false;
    bool gamepad_present_ = false;

    // Which control each active finger is holding. A phone has several fingers
    // and a player will press two buttons at once; tracking per-finger is what
    // makes that work rather than the last touch winning.
    struct ActiveTouch {
        s64 finger_id = 0;
        int control_index = -1;
    };
    std::vector<ActiveTouch> active_;
};

}  // namespace retro3do
