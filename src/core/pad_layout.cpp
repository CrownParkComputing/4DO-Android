#include "pad_layout.h"

namespace retro3do {

// A comfortable default layout for a device held in landscape: movement under
// the left thumb, actions under the right, system buttons tucked into the top
// corners where they will not be hit by accident.
//
// Offsets are given in units of the SMALLER screen dimension and converted per
// axis, so a cluster keeps its shape whatever the screen is. Laying out with
// equal fractional offsets instead produces a d-pad stretched into a wide
// diamond on any widescreen phone.
std::vector<TouchControl> default_touch_layout(int width, int height) {
    const float w = static_cast<float>(width > 0 ? width : 1280);
    const float h = static_cast<float>(height > 0 ? height : 720);
    const float smaller = w < h ? w : h;

    // A fraction of the smaller dimension, expressed in each axis.
    const auto ax = [&](float units) { return units * smaller / w; };
    const auto ay = [&](float units) { return units * smaller / h; };

    // Spacing between a cluster's controls, and the control size, both in
    // smaller-dimension units.
    const float spread = 0.105f;
    const float dpad_radius = 0.052f;
    const float face_spread = 0.115f;
    const float face_radius = 0.055f;

    // A cluster's centre must sit at least (spread + radius) from the edge or
    // its outermost control hangs off the screen and cannot be pressed. The
    // margins below are that minimum plus a little breathing room, which is why
    // they are not round numbers.
    const float dpad_margin = spread + dpad_radius + 0.055f;
    const float face_margin = face_spread + face_radius + 0.055f;

    const float dpad_x = ax(dpad_margin);
    const float dpad_y = 1.0f - ay(dpad_margin);
    const float face_x = 1.0f - ax(face_margin);
    const float face_y = 1.0f - ay(face_margin);

    return {
        {PadButton::Up,    "", dpad_x,              dpad_y - ay(spread), dpad_radius},
        {PadButton::Down,  "", dpad_x,              dpad_y + ay(spread), dpad_radius},
        {PadButton::Left,  "", dpad_x - ax(spread), dpad_y,              dpad_radius},
        {PadButton::Right, "", dpad_x + ax(spread), dpad_y,              dpad_radius},

        // A, B and C in a shallow arc, as they sit on the real pad, so the
        // thumb travels naturally between them rather than in a straight line.
        {PadButton::C, "C", face_x - ax(face_spread), face_y + ay(0.040f), face_radius},
        {PadButton::B, "B", face_x,                   face_y - ay(0.025f), face_radius},
        {PadButton::A, "A", face_x + ax(face_spread), face_y + ay(0.040f), face_radius},

        {PadButton::LeftShift,  "L", ax(0.10f),        ay(0.10f), 0.046f},
        {PadButton::RightShift, "R", 1.0f - ax(0.10f), ay(0.10f), 0.046f},

        {PadButton::Play, "P", 0.5f - ax(0.075f), 1.0f - ay(0.075f), 0.040f},
        {PadButton::Stop, "X", 0.5f + ax(0.075f), 1.0f - ay(0.075f), 0.040f},
    };
}

}  // namespace retro3do
