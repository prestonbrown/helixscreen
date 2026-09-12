// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_rotation_source.cpp
 * @brief The touch pipeline's rotation answer comes from the live backend
 *
 * A backend that does not route rotation through LVGL - a scanout plane that
 * owns the angle - leaves lv_display_get_rotation() describing something other
 * than the picture on screen. display_rotation_degrees() asks the backend that
 * applied the rotation instead, so the gates deciding whether a stored evdev
 * range or an unstamped affine is safe to use keep answering about the panel
 * (prestonbrown/helixscreen#1275, prestonbrown/helixscreen#1394).
 */

#include "../lvgl_test_fixture.h"
#include "display_backend.h"
#include "drm_rotation_strategy.h"

#include "../catch_amalgamated.hpp"

namespace {

/// A backend that reports whatever angle it was told to, independent of LVGL.
/// Registers itself as the live backend on construction, like every real one.
class FakeRotatedBackend : public DisplayBackend {
  public:
    explicit FakeRotatedBackend(int degrees) : degrees_(degrees) {}

    int applied_rotation_degrees(lv_display_t* /*disp*/ = nullptr) const override {
        return degrees_;
    }

    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }
    DisplayBackendType type() const override {
        return DisplayBackendType::FBDEV;
    }
    const char* name() const override {
        return "fake-rotated";
    }
    bool is_available() const override {
        return true;
    }

  private:
    int degrees_;
};

/// Same, minus the override: pins that a backend which says nothing about
/// rotation still answers exactly what LVGL answers.
class FakeSilentBackend : public FakeRotatedBackend {
  public:
    FakeSilentBackend() : FakeRotatedBackend(0) {}

    int applied_rotation_degrees(lv_display_t* disp = nullptr) const override {
        return DisplayBackend::applied_rotation_degrees(disp);
    }
};

/// An angle LVGL is definitely not reporting right now.
int angle_lvgl_does_not_report() {
    return DisplayBackend::lvgl_rotation_degrees() == 90 ? 180 : 90;
}

/// A backend with nothing special to say about rotation: no override at all,
/// which is SDL's situation and the case the base default has to carry.
class FakePlainBackend : public DisplayBackend {
  public:
    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }
    DisplayBackendType type() const override {
        return DisplayBackendType::SDL;
    }
    const char* name() const override {
        return "fake-plain";
    }
    bool is_available() const override {
        return true;
    }
};

/// Put the display's rotation back: the fixture shares one display across the
/// whole binary, so a rotation left behind changes what later tests measure.
class ScopedRotation {
  public:
    ScopedRotation() : disp_(lv_display_get_default()), prev_(lv_display_get_rotation(disp_)) {}
    ~ScopedRotation() {
        lv_display_set_rotation(disp_, prev_);
    }

  private:
    lv_display_t* disp_;
    lv_display_rotation_t prev_;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the live backend owns the applied rotation",
                 "[display][rotation]") {
    const int ambient = display_rotation_degrees();
    const int claimed = angle_lvgl_does_not_report();

    {
        FakeRotatedBackend backend(claimed);
        REQUIRE(display_rotation_degrees() == claimed);
        REQUIRE(display_rotation_degrees() != DisplayBackend::lvgl_rotation_degrees());
        REQUIRE(display_is_rotated());
    }

    // Gone with the backend, not left behind as a cached value.
    REQUIRE(display_rotation_degrees() == ambient);
}

TEST_CASE_METHOD(LVGLTestFixture, "a backend swap hands the answer over", "[display][rotation]") {
    const int ambient = display_rotation_degrees();

    {
        FakeRotatedBackend first(90);
        REQUIRE(display_rotation_degrees() == 90);
    }
    {
        FakeRotatedBackend second(270);
        REQUIRE(display_rotation_degrees() == 270);
    }

    REQUIRE(display_rotation_degrees() == ambient);
}

TEST_CASE_METHOD(LVGLTestFixture, "a backend that does not own rotation answers as LVGL does",
                 "[display][rotation]") {
    FakeSilentBackend backend;
    REQUIRE(display_rotation_degrees() == DisplayBackend::lvgl_rotation_degrees());
    REQUIRE(display_is_rotated() == (DisplayBackend::lvgl_rotation_degrees() != 0));
}

TEST_CASE_METHOD(LVGLTestFixture, "a backend with no rotation override still rotates the display",
                 "[display][rotation]") {
    ScopedRotation restore;
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);

    FakePlainBackend backend;
    backend.set_display_rotation(disp, LV_DISPLAY_ROTATION_90, 800, 480);

    // DisplayManager no longer writes this itself, so a backend that overrides
    // nothing has to inherit a default that does.
    REQUIRE(lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_90);
    REQUIRE(display_rotation_degrees(disp) == 90);
}

TEST_CASE_METHOD(LVGLTestFixture, "rotating through a null display does not crash",
                 "[display][rotation]") {
    FakePlainBackend backend;
    backend.set_display_rotation(nullptr, LV_DISPLAY_ROTATION_180, 800, 480);
    SUCCEED();
}

// The plane transform has to be the one LVGL would have applied, or touch lands
// in a different place depending on which device did the rotating. Asserted
// against LVGL itself at every angle, so the two cannot drift apart silently.
TEST_CASE_METHOD(LVGLTestFixture, "the plane pointer transform matches LVGL's own",
                 "[display][rotation]") {
    ScopedRotation restore;
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);
    const int32_t panel_w = lv_display_get_horizontal_resolution(disp);
    const int32_t panel_h = lv_display_get_vertical_resolution(disp);
    REQUIRE(panel_w > 1);
    REQUIRE(panel_h > 1);

    const lv_display_rotation_t rots[] = {LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90,
                                          LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270};
    const PointerXY samples[] = {{0, 0},
                                 {panel_w - 1, 0},
                                 {0, panel_h - 1},
                                 {panel_w - 1, panel_h - 1},
                                 {panel_w / 3, panel_h / 4}};

    for (lv_display_rotation_t rot : rots) {
        const int degrees = static_cast<int>(rot) * 90;
        lv_display_set_rotation(disp, rot);
        for (PointerXY raw : samples) {
            lv_point_t lvgl_point = {raw.x, raw.y};
            lv_display_rotate_point(disp, &lvgl_point);
            const PointerXY ours = rotate_pointer_for_plane(raw, degrees, panel_w, panel_h);
            INFO("angle " << degrees << " raw (" << raw.x << "," << raw.y << ")");
            REQUIRE(ours.x == lvgl_point.x);
            REQUIRE(ours.y == lvgl_point.y);
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "the plane pointer transform leaves unknown angles alone",
                 "[display][rotation]") {
    const PointerXY raw{37, 91};
    for (int degrees : {-90, 1, 45, 360, 12345}) {
        const PointerXY out = rotate_pointer_for_plane(raw, degrees, 800, 480);
        INFO("angle " << degrees);
        REQUIRE(out.x == raw.x);
        REQUIRE(out.y == raw.y);
    }
}
