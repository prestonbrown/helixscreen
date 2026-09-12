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
