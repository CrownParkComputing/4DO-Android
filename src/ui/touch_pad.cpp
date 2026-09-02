#include "touch_pad.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>

#include "core/pad_layout.h"
#include "core/settings.h"
#include "platform/platform.h"

namespace retro3do {
namespace {

const char* key_for(PadButton button) {
    switch (button) {
        case PadButton::Up:         return "up";
        case PadButton::Down:       return "down";
        case PadButton::Left:       return "left";
        case PadButton::Right:      return "right";
        case PadButton::A:          return "a";
        case PadButton::B:          return "b";
        case PadButton::C:          return "c";
        case PadButton::Play:       return "play";
        case PadButton::Stop:       return "stop";
        case PadButton::LeftShift:  return "lshift";
        case PadButton::RightShift: return "rshift";
        default:                    return "unknown";
    }
}

// The d-pad arrows are drawn rather than lettered, so they read at a glance.
void draw_arrow(SDL_Renderer* renderer, PadButton button, float cx, float cy,
                float r) {
    const float a = r * 0.45f;
    SDL_FPoint p[4];
    switch (button) {
        case PadButton::Up:
            p[0] = {cx, cy - a}; p[1] = {cx - a, cy + a * 0.6f};
            p[2] = {cx + a, cy + a * 0.6f}; p[3] = p[0];
            break;
        case PadButton::Down:
            p[0] = {cx, cy + a}; p[1] = {cx - a, cy - a * 0.6f};
            p[2] = {cx + a, cy - a * 0.6f}; p[3] = p[0];
            break;
        case PadButton::Left:
            p[0] = {cx - a, cy}; p[1] = {cx + a * 0.6f, cy - a};
            p[2] = {cx + a * 0.6f, cy + a}; p[3] = p[0];
            break;
        case PadButton::Right:
            p[0] = {cx + a, cy}; p[1] = {cx - a * 0.6f, cy - a};
            p[2] = {cx - a * 0.6f, cy + a}; p[3] = p[0];
            break;
        default:
            return;
    }
    SDL_RenderLines(renderer, p, 4);
}

void draw_circle(SDL_Renderer* renderer, float cx, float cy, float r) {
    constexpr int kSegments = 28;
    SDL_FPoint points[kSegments + 1];
    for (int i = 0; i <= kSegments; ++i) {
        const float angle =
            static_cast<float>(i) / kSegments * 6.2831853f;
        points[i] = {cx + std::cos(angle) * r, cy + std::sin(angle) * r};
    }
    SDL_RenderLines(renderer, points, kSegments + 1);
}

}  // namespace

TouchPad::TouchPad() : controls_(default_touch_layout(1280, 720)) {
    // Default on where there is no alternative, off where there is.
#if RETRO3DO_MOBILE
    visible_ = true;
#else
    visible_ = false;
#endif
}

void TouchPad::set_physical_gamepad_present(bool present) {
    gamepad_present_ = present;
    if (present) {
        // Editing controls which have just disappeared is both confusing and
        // dangerous: an old drag would otherwise be resumed if the controller
        // were unplugged later.
        editing_ = false;
        active_.clear();
    }
}

void TouchPad::reset_layout(int window_width, int window_height) {
    controls_ = default_touch_layout(window_width, window_height);
}

void TouchPad::load_layout(const Settings& settings) {
    for (TouchControl& control : controls_) {
        const std::string prefix = std::string("touch.") + key_for(control.button);
        const std::string x = settings.get(prefix + ".x");
        const std::string y = settings.get(prefix + ".y");
        const std::string r = settings.get(prefix + ".r");

        // Only accept a stored value that is inside the screen. A layout file
        // from a different build, or a half-written one, must not put a control
        // somewhere it can never be pressed and cannot be dragged back.
        if (!x.empty() && !y.empty()) {
            const float px = std::strtof(x.c_str(), nullptr);
            const float py = std::strtof(y.c_str(), nullptr);
            if (px >= 0.0f && px <= 1.0f && py >= 0.0f && py <= 1.0f) {
                control.x = px;
                control.y = py;
            }
        }
        if (!r.empty()) {
            const float pr = std::strtof(r.c_str(), nullptr);
            if (pr > 0.01f && pr < 0.3f) {
                control.radius = pr;
            }
        }
    }
    visible_ = settings.get_bool(settings_key::kTouchControls, visible_);
}

void TouchPad::save_layout(Settings& settings) const {
    for (const TouchControl& control : controls_) {
        const std::string prefix = std::string("touch.") + key_for(control.button);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.4f", control.x);
        settings.set(prefix + ".x", buffer);
        std::snprintf(buffer, sizeof(buffer), "%.4f", control.y);
        settings.set(prefix + ".y", buffer);
        std::snprintf(buffer, sizeof(buffer), "%.4f", control.radius);
        settings.set(prefix + ".r", buffer);
    }
    settings.set_bool(settings_key::kTouchControls, visible_);
}

float TouchPad::radius_pixels(const TouchControl& control, int w, int h) const {
    // Sized against the smaller dimension so a control is the same physical
    // size whichever way the device is held.
    const float smaller = static_cast<float>(w < h ? w : h);
    return control.radius * smaller;
}

int TouchPad::control_at(float px, float py, int w, int h) const {
    // Last drawn is topmost, so search backwards and the control a user sees on
    // top is the one they get.
    for (int i = static_cast<int>(controls_.size()) - 1; i >= 0; --i) {
        const TouchControl& control = controls_[static_cast<size_t>(i)];
        const float cx = control.x * static_cast<float>(w);
        const float cy = control.y * static_cast<float>(h);
        const float r = radius_pixels(control, w, h);

        const float dx = px - cx;
        const float dy = py - cy;
        // A generous hit area: fingers are imprecise, and a button that needs
        // to be hit exactly feels broken rather than demanding.
        const float reach = r * 1.15f;
        if (dx * dx + dy * dy <= reach * reach) {
            return i;
        }
    }
    return -1;
}

bool TouchPad::handle_event(const SDL_Event& event, int w, int h, PadState& pads) {
    if (!visible() || w <= 0 || h <= 0) {
        return false;
    }

    // SDL reports touches in normalised coordinates and the mouse in pixels, so
    // they are converted to a common frame before anything else.
    float px = 0.0f;
    float py = 0.0f;
    s64 finger = 0;
    bool down = false;
    bool up = false;
    bool motion = false;

    switch (event.type) {
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_MOTION:
            px = event.tfinger.x * static_cast<float>(w);
            py = event.tfinger.y * static_cast<float>(h);
            finger = static_cast<s64>(event.tfinger.fingerID);
            down = event.type == SDL_EVENT_FINGER_DOWN;
            up = event.type == SDL_EVENT_FINGER_UP;
            motion = event.type == SDL_EVENT_FINGER_MOTION;
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            // A mouse is treated as one more finger, with an id no real touch
            // uses. It makes the controls testable on a desktop.
            px = event.button.x;
            py = event.button.y;
            finger = -1;
            down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            up = event.type == SDL_EVENT_MOUSE_BUTTON_UP;
            break;

        case SDL_EVENT_MOUSE_MOTION:
            px = event.motion.x;
            py = event.motion.y;
            finger = -1;
            motion = true;
            break;

        default:
            return false;
    }

    if (down) {
        const int index = control_at(px, py, w, h);
        if (index < 0) {
            return false;
        }
        active_.push_back(ActiveTouch{finger, index});
        if (!editing_) {
            pads.press(0, controls_[static_cast<size_t>(index)].button, true);
        }
        return true;
    }

    if (motion) {
        if (!editing_) {
            return false;
        }
        for (const ActiveTouch& touch : active_) {
            if (touch.finger_id != finger || touch.control_index < 0) continue;
            TouchControl& control = controls_[static_cast<size_t>(touch.control_index)];
            // Clamped so a control can never be dragged off the screen and
            // become impossible to reach again.
            control.x = std::fmin(1.0f, std::fmax(0.0f, px / static_cast<float>(w)));
            control.y = std::fmin(1.0f, std::fmax(0.0f, py / static_cast<float>(h)));
            return true;
        }
        return false;
    }

    if (up) {
        for (size_t i = 0; i < active_.size(); ++i) {
            if (active_[i].finger_id != finger) continue;
            const int index = active_[i].control_index;
            if (index >= 0 && !editing_) {
                pads.press(0, controls_[static_cast<size_t>(index)].button, false);
            }
            active_.erase(active_.begin() + static_cast<long>(i));
            return index >= 0;
        }
    }

    return false;
}

void TouchPad::draw(SDL_Renderer* renderer, int w, int h) const {
    if (!visible() || w <= 0 || h <= 0) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (size_t i = 0; i < controls_.size(); ++i) {
        const TouchControl& control = controls_[i];
        const float cx = control.x * static_cast<float>(w);
        const float cy = control.y * static_cast<float>(h);
        const float r = radius_pixels(control, w, h);

        bool held = false;
        for (const ActiveTouch& touch : active_) {
            if (touch.control_index == static_cast<int>(i)) {
                held = true;
                break;
            }
        }

        // Faint by default: the controls have to be visible enough to aim at
        // and quiet enough not to fight the picture. They brighten when held,
        // which is the only feedback a glass button can give.
        if (editing_) {
            SDL_SetRenderDrawColor(renderer, 255, 200, 90, 200);
        } else if (held) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 190);
        } else {
            SDL_SetRenderDrawColor(renderer, 235, 235, 245, 90);
        }

        draw_circle(renderer, cx, cy, r);
        draw_circle(renderer, cx, cy, r * 0.94f);

        if (!control.label.empty()) {
            SDL_SetRenderScale(renderer, r * 0.09f, r * 0.09f);
            SDL_RenderDebugText(renderer, (cx - r * 0.16f) / (r * 0.09f),
                                (cy - r * 0.28f) / (r * 0.09f),
                                control.label.c_str());
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        } else {
            draw_arrow(renderer, control.button, cx, cy, r);
        }
    }
}

}  // namespace retro3do
