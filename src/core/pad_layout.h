// Default placement of the on-screen controls.
//
// Pure arithmetic, kept out of the drawing code so it can be tested without a
// window. That separation earned itself immediately: the first version laid
// clusters out with equal fractional offsets in x and y, which stretches a
// d-pad into a wide diamond on any widescreen phone, and then placed cluster
// centres too near the edges so the outermost controls hung off the screen.
// Both are invisible in code and obvious in a picture — or in a test.
#pragma once

#include <string>
#include <vector>

#include "pad.h"

namespace retro3do {

// One on-screen control. Positions are fractions of the play area: a phone, a
// foldable and a tablet have wildly different screens, and a layout in pixels
// that suits one is off the edge of another.
struct TouchControl {
    PadButton button = PadButton::A;
    std::string label;
    float x = 0.5f;        // centre, as a fraction of width
    float y = 0.5f;        // centre, as a fraction of height
    float radius = 0.06f;  // as a fraction of the SMALLER screen dimension
};

// Build the default layout for a screen of this shape.
//
// x is a fraction of width and y a fraction of height, so equal fractional
// offsets are not equal distances. Offsets here are given in units of the
// smaller dimension and converted per axis, which is what keeps a cluster its
// own shape on any screen — and is why this needs the aspect ratio and cannot
// be a static table.
std::vector<TouchControl> default_touch_layout(int width, int height);

}  // namespace retro3do
