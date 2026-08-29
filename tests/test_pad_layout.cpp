// Default on-screen control layout.
//
// Both assertions here are regressions for bugs that were invisible in the code
// and obvious the moment the layout was rendered: a d-pad stretched into a wide
// diamond, and clusters placed so near the edge that their outermost controls
// hung off the screen and could never be pressed.
#include <cmath>

#include "core/pad_layout.h"
#include "test_harness.h"

using namespace retro3do;

namespace {

const TouchControl* find(const std::vector<TouchControl>& layout, PadButton button) {
    for (const TouchControl& control : layout) {
        if (control.button == button) return &control;
    }
    return nullptr;
}

// Distance in pixels between two controls on a screen of this size.
float pixel_distance(const TouchControl& a, const TouchControl& b, int w, int h) {
    const float dx = (a.x - b.x) * static_cast<float>(w);
    const float dy = (a.y - b.y) * static_cast<float>(h);
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

TEST(every_control_is_fully_on_screen) {
    // A control whose edge falls outside the screen cannot be pressed, and in
    // layout mode cannot be dragged back either.
    const int sizes[][2] = {
        {1280, 720},   // a typical phone in landscape
        {2400, 1080},  // a tall modern phone
        {2048, 1536},  // an iPad, much squarer
        {800, 480},    // a small handheld
        {720, 1280},   // held in portrait
    };

    for (const auto& size : sizes) {
        const int w = size[0];
        const int h = size[1];
        const float smaller = static_cast<float>(w < h ? w : h);

        for (const TouchControl& control : default_touch_layout(w, h)) {
            const float r = control.radius * smaller;
            const float cx = control.x * static_cast<float>(w);
            const float cy = control.y * static_cast<float>(h);

            CHECK(cx - r >= 0.0f);
            CHECK(cy - r >= 0.0f);
            CHECK(cx + r <= static_cast<float>(w));
            CHECK(cy + r <= static_cast<float>(h));
        }
    }
}

TEST(the_dpad_is_a_square_cross_not_a_stretched_diamond) {
    // x is a fraction of width and y a fraction of height, so laying a cross
    // out with equal fractional offsets makes it far wider than it is tall on
    // a widescreen phone. The four arms must be the same distance in PIXELS.
    const int w = 2400;
    const int h = 1080;
    const std::vector<TouchControl> layout = default_touch_layout(w, h);

    const TouchControl* up = find(layout, PadButton::Up);
    const TouchControl* down = find(layout, PadButton::Down);
    const TouchControl* left = find(layout, PadButton::Left);
    const TouchControl* right = find(layout, PadButton::Right);
    CHECK(up != nullptr && down != nullptr && left != nullptr && right != nullptr);

    const float vertical = pixel_distance(*up, *down, w, h);
    const float horizontal = pixel_distance(*left, *right, w, h);

    // Within a couple of percent; the conversion is floating point.
    CHECK(std::fabs(vertical - horizontal) < vertical * 0.02f);
}

TEST(the_dpad_stays_square_on_a_squarer_screen_too) {
    const int w = 2048;
    const int h = 1536;
    const std::vector<TouchControl> layout = default_touch_layout(w, h);

    const float vertical = pixel_distance(*find(layout, PadButton::Up),
                                          *find(layout, PadButton::Down), w, h);
    const float horizontal = pixel_distance(*find(layout, PadButton::Left),
                                            *find(layout, PadButton::Right), w, h);
    CHECK(std::fabs(vertical - horizontal) < vertical * 0.02f);
}

TEST(movement_is_on_the_left_and_actions_on_the_right) {
    const std::vector<TouchControl> layout = default_touch_layout(1280, 720);
    CHECK(find(layout, PadButton::Left)->x < 0.4f);
    CHECK(find(layout, PadButton::Up)->x < 0.4f);
    CHECK(find(layout, PadButton::A)->x > 0.6f);
    CHECK(find(layout, PadButton::B)->x > 0.6f);
    CHECK(find(layout, PadButton::C)->x > 0.6f);
}

TEST(the_shoulder_buttons_are_up_out_of_the_way) {
    // They sit where a thumb does not rest, so they are not pressed by accident
    // while holding the device.
    const std::vector<TouchControl> layout = default_touch_layout(1280, 720);
    CHECK(find(layout, PadButton::LeftShift)->y < 0.3f);
    CHECK(find(layout, PadButton::RightShift)->y < 0.3f);
}

TEST(every_button_the_machine_has_is_present_exactly_once) {
    const std::vector<TouchControl> layout = default_touch_layout(1280, 720);
    const PadButton expected[] = {
        PadButton::Up, PadButton::Down, PadButton::Left, PadButton::Right,
        PadButton::A, PadButton::B, PadButton::C,
        PadButton::Play, PadButton::Stop,
        PadButton::LeftShift, PadButton::RightShift,
    };
    for (PadButton button : expected) {
        int count = 0;
        for (const TouchControl& control : layout) {
            if (control.button == button) ++count;
        }
        CHECK_EQ(count, 1);
    }
    CHECK_EQ(layout.size(), sizeof(expected) / sizeof(expected[0]));
}

TEST(controls_do_not_overlap_each_other) {
    // Overlapping controls make one of them unpressable, since a touch resolves
    // to exactly one.
    const int w = 1280;
    const int h = 720;
    const float smaller = static_cast<float>(h);
    const std::vector<TouchControl> layout = default_touch_layout(w, h);

    for (size_t i = 0; i < layout.size(); ++i) {
        for (size_t j = i + 1; j < layout.size(); ++j) {
            const float distance = pixel_distance(layout[i], layout[j], w, h);
            const float radii =
                (layout[i].radius + layout[j].radius) * smaller;
            CHECK(distance >= radii);
        }
    }
}
