// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_touch_calibration_rotation.cpp
 * @brief The evdev range fit must not run on a rotated display
 *
 * The 3-point solve maps raw -> LOGICAL screen (the wizard's targets are
 * logical, post-rotation), but the solved range executes at the evdev stage
 * whose output LVGL rotates again — on a rotated panel the rotation folds
 * into the stored (min,max,swap) and every later tap lands through a double
 * transform, with the evdev clamp flattening the logical-shaped output
 * against the physical extents (prestonbrown/helixscreen#1394). Rotated
 * panels keep the affine-only shape, the pre-range-stage path, which composes
 * correctly with lv_display_rotate_point().
 */

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "display_backend.h"
#include "touch_calibration.h"
#include "touch_calibration_panel.h"
#include "touch_calibration_wrapper.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Drive the panel through a full capture with pre-scale digitizer readings
/// that are AFFINE-CONSISTENT with the panel's own crosshair targets: the
/// range fit solves device_raw vs screen_points_, and screen_points_ are the
/// TARGETS (compute_target_position), not the tap coordinates. A true raw
/// axis of 2.5 units per pixel on both axes, no swap, reproduces the targets
/// exactly, so the implied ABS range (0..2000 both axes) is plausible and the
/// fit computes wherever the gate allows it.
void capture_three_points_with_raw(TouchCalibrationPanel& panel) {
    // compute_target_position() on 800x480: ratios (0.15,0.20) (0.50,0.78)
    // (0.85,0.20) -> (120,96) (400,374) (680,96). Hardcoded so the derivation
    // is visible; if the ratios change, this test's raws must move with them.
    const Point targets[3] = {{120, 96}, {400, 374}, {680, 96}};
    Point raws[3];
    for (int i = 0; i < 3; i++) {
        raws[i] = Point{targets[i].x * 5 / 2, targets[i].y * 5 / 2};
    }

    for (int i = 0; i < 3; i++) {
        panel.capture_point(raws[i], &raws[i]);
    }
}

/// Put the display's rotation back at end of scope. LVGLTestFixture shares one
/// display across the whole binary, so a rotation left behind changes what
/// every later test measures.
class ScopedRotation {
  public:
    explicit ScopedRotation(lv_display_rotation_t rot)
        : disp_(lv_display_get_default()), prev_(lv_display_get_rotation(disp_)) {
        lv_display_set_rotation(disp_, rot);
    }
    ~ScopedRotation() {
        lv_display_set_rotation(disp_, prev_);
    }

    ScopedRotation(const ScopedRotation&) = delete;
    ScopedRotation& operator=(const ScopedRotation&) = delete;

  private:
    lv_display_t* disp_;
    lv_display_rotation_t prev_;
};

/// The shape of a record written before the rotation key existed: a valid affine
/// and no basis to place it in.
void write_unstamped_affine(Config& cfg) {
    cfg.set<bool>("/input/calibration/valid", true);
    cfg.set<double>("/input/calibration/a", 1.7);
    cfg.set<double>("/input/calibration/b", 0.0);
    cfg.set<double>("/input/calibration/c", -3.0);
    cfg.set<double>("/input/calibration/d", 0.0);
    cfg.set<double>("/input/calibration/e", 1.6);
    cfg.set<double>("/input/calibration/f", -2.0);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "range fit stays off when the display is rotated",
                 "[touch-calibration][rotation][1394]") {
    TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);

    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);
    panel.start();
    capture_three_points_with_raw(panel);

    REQUIRE(panel.get_state() == TouchCalibrationPanel::State::VERIFY);
    // The whole point of the gate: no evdev range on a rotated panel.
    CHECK_FALSE(panel.get_range_fit().valid);
    // The affine half is unaffected — rotated panels still calibrate.
    REQUIRE(panel.get_calibration() != nullptr);
    CHECK(panel.get_calibration()->valid);
    // Targets and taps are both logical, post-rotation coordinates, so the matrix
    // only means anything against the rotation it was solved at. A record that does
    // not name it is unplaceable on the next boot, and the loader's only honest
    // answer for one is to throw it away.
    CHECK(panel.get_calibration()->capture_rotation == 90);
}

TEST_CASE_METHOD(LVGLTestFixture, "range fit solves on an unrotated display",
                 "[touch-calibration][rotation][1394]") {
    TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);

    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_0);
    panel.start();
    capture_three_points_with_raw(panel);

    REQUIRE(panel.get_state() == TouchCalibrationPanel::State::VERIFY);
    // The same capture with raw readings produces a range fit when nothing
    // rotates underneath it.
    CHECK(panel.get_range_fit().valid);
    CHECK(panel.get_calibration()->valid);
    // The unrotated basis is stamped just as explicitly, so a stamp hardcoded to
    // the rotated case's 90 fails here.
    CHECK(panel.get_calibration()->capture_rotation == 0);
}

// ---------------------------------------------------------------------------
// The solver and the backends must answer "is the display rotated?" the same
// way. They did not: the solver asked LVGL while the fbdev/DRM stored-range
// gate asked `/display/rotate`, and the two inputs disagree in both
// directions. display_rotation_degrees() is the one source both now read; the
// cases below pin it against a config key that says something else, and assert
// the solver's observable half in the same test so a gate that drifts back to
// the key cannot pass one and fail the other silently.
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "rotation from CLI/env still gates the range stage",
                 "[touch-calibration][rotation][1394]") {
    // A unit started with HELIX_DISPLAY_ROTATION=270 (or --rotate 270) and no
    // rotate key in settings.json: DisplayManager resolves CLI, then env, and
    // only falls back to the key when both are 0, so the display is rotated
    // while the key reads 0.
    REQUIRE(helix::Config::get_instance()->get<int>("/display/rotate", 0) == 0);
    ScopedRotation rotated(LV_DISPLAY_ROTATION_270);

    // The backends' half: what create_input_pointer() consults before it
    // programs a stored range onto the evdev stage. Reading the key here left
    // #1394 fully live on such a unit.
    CHECK(display_rotation_degrees() == 270);
    CHECK(display_is_rotated());

    // The solver's half, through the panel's own observable output.
    TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);
    panel.start();
    capture_three_points_with_raw(panel);

    REQUIRE(panel.get_state() == TouchCalibrationPanel::State::VERIFY);
    CHECK_FALSE(panel.get_range_fit().valid);
}

TEST_CASE_METHOD(LVGLTestFixture, "a rotation the display never applied leaves the range stage on",
                 "[touch-calibration][rotation][1394]") {
    // The mirror case: a DSI/EGL unit whose DRM->fbdev rotation fallback
    // failed. DisplayManager logs "Continuing without rotation" and never
    // calls lv_display_set_rotation(), so the key stays at 90 over a display
    // that is not rotated. A gate believing the key throws away a legitimate
    // stored range on every boot and the user sees calibration "not stick".
    helix::Config::get_instance()->set("/display/rotate", 90);
    ScopedRotation unrotated(LV_DISPLAY_ROTATION_0);

    CHECK(display_rotation_degrees() == 0);
    CHECK_FALSE(display_is_rotated());

    TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);
    panel.start();
    capture_three_points_with_raw(panel);

    REQUIRE(panel.get_state() == TouchCalibrationPanel::State::VERIFY);
    CHECK(panel.get_range_fit().valid);
}

// ============================================================================
// Affine composition with display rotation
//
// The affine runs in the read callback, BEFORE lv_display_rotate_point(), so it
// receives panel-space coordinates. The wizard solves it against logical-space
// targets. Everything below pins the two together.
// ============================================================================

TEST_CASE("rotate_panel_to_logical matches lv_display_rotate_point",
          "[touch-calibration][rotation]") {
    // Panel 480x800 (K2 shape): hor_res=480, ver_res=800.
    const int pw = 480;
    const int ph = 800;

    SECTION("0 degrees is identity") {
        REQUIRE(rotate_panel_to_logical({100, 200}, 0, pw, ph).x == 100);
        REQUIRE(rotate_panel_to_logical({100, 200}, 0, pw, ph).y == 200);
    }

    SECTION("270: x becomes y, y becomes hor_res - x - 1") {
        const Point out = rotate_panel_to_logical({100, 200}, 270, pw, ph);
        REQUIRE(out.x == 200);
        REQUIRE(out.y == 480 - 100 - 1);
    }

    SECTION("90: x becomes ver_res - y - 1, y becomes x") {
        const Point out = rotate_panel_to_logical({100, 200}, 90, pw, ph);
        REQUIRE(out.x == 800 - 200 - 1);
        REQUIRE(out.y == 100);
    }

    SECTION("180 mirrors both axes") {
        const Point out = rotate_panel_to_logical({100, 200}, 180, pw, ph);
        REQUIRE(out.x == 480 - 100 - 1);
        REQUIRE(out.y == 800 - 200 - 1);
    }
}

TEST_CASE("panel/logical rotation round-trips exactly", "[touch-calibration][rotation]") {
    const int pw = 480;
    const int ph = 800;
    const Point points[] = {{0, 0}, {1, 1}, {100, 200}, {479, 799}, {0, 799}, {479, 0}};

    for (int deg : {0, 90, 180, 270}) {
        for (const Point& p : points) {
            const Point there = rotate_panel_to_logical(p, deg, pw, ph);
            const Point back = rotate_logical_to_panel(there, deg, pw, ph);
            INFO("rotation " << deg << " point (" << p.x << "," << p.y << ")");
            REQUIRE(back.x == p.x);
            REQUIRE(back.y == p.y);
        }
    }
}

TEST_CASE("logical_extent swaps the axes at 90 and 270", "[touch-calibration][rotation]") {
    int w = 0;
    int h = 0;
    logical_extent(0, 480, 800, w, h);
    REQUIRE(w == 480);
    REQUIRE(h == 800);
    logical_extent(90, 480, 800, w, h);
    REQUIRE(w == 800);
    REQUIRE(h == 480);
    logical_extent(180, 480, 800, w, h);
    REQUIRE(w == 480);
    REQUIRE(h == 800);
    logical_extent(270, 480, 800, w, h);
    REQUIRE(w == 800);
    REQUIRE(h == 480);
}

TEST_CASE("a logical affine lands where it was solved, on a rotated display",
          "[touch-calibration][rotation]") {
    // K2 shape: 480x800 panel presented as 800x480 by a 270-degree rotation.
    const int pw = 480;
    const int ph = 800;

    // A calibration solved in LOGICAL space that shifts x by +10.
    TouchCalibration cal;
    cal.valid = true;
    cal.a = 1.0f;
    cal.b = 0.0f;
    cal.c = 10.0f;
    cal.d = 0.0f;
    cal.e = 1.0f;
    cal.f = 0.0f;

    const Point panel_in{100, 200};

    // What the wizard's solve promises: apply the matrix to the LOGICAL point.
    const Point logical_in = rotate_panel_to_logical(panel_in, 270, pw, ph);
    const Point promised = transform_point(cal, logical_in, 800 - 1, 480 - 1);

    // What the pipeline produces: our panel-space placement, then LVGL's rotation.
    const Point panel_out = apply_calibration_in_panel_space(cal, panel_in, 270, pw, ph);
    const Point delivered = rotate_panel_to_logical(panel_out, 270, pw, ph);

    INFO("promised (" << promised.x << "," << promised.y << ") delivered (" << delivered.x << ","
                      << delivered.y << ")");
    REQUIRE(delivered.x == promised.x);
    REQUIRE(delivered.y == promised.y);

    // And it is genuinely different from evaluating the matrix in panel space,
    // which is what makes this worth pinning.
    const Point naive = transform_point(cal, panel_in, pw - 1, ph - 1);
    const Point naive_delivered = rotate_panel_to_logical(naive, 270, pw, ph);
    REQUIRE_FALSE((naive_delivered.x == promised.x && naive_delivered.y == promised.y));
}

TEST_CASE("an unrotated display is unaffected by the placement", "[touch-calibration][rotation]") {
    TouchCalibration cal;
    cal.valid = true;
    cal.a = 1.02f;
    cal.b = 0.01f;
    cal.c = -3.0f;
    cal.d = 0.0f;
    cal.e = 0.99f;
    cal.f = 2.0f;

    const Point p{321, 123};
    const Point placed = apply_calibration_in_panel_space(cal, p, 0, 800, 480);
    const Point direct = transform_point(cal, p, 800 - 1, 480 - 1);
    REQUIRE(placed.x == direct.x);
    REQUIRE(placed.y == direct.y);
}

TEST_CASE("a calibration survives the display being rotated after it was solved",
          "[touch-calibration][rotation]") {
    // Solved while the display was NOT rotated, then a printer definition lands
    // and the preset rotates the display to 270. The stored matrix still has to
    // put a physical touch on the same logical target it was taught.
    const int pw = 480;
    const int ph = 800;

    TouchCalibration cal;
    cal.valid = true;
    cal.a = 1.0f;
    cal.b = 0.0f;
    cal.c = 10.0f;
    cal.d = 0.0f;
    cal.e = 1.0f;
    cal.f = -5.0f;
    cal.capture_rotation = 0;

    const Point panel_in{100, 200};

    // Taught at rotation 0: the logical point WAS the panel point.
    const Point promised = transform_point(cal, panel_in, pw - 1, ph - 1);

    const Point panel_out =
        apply_calibration_in_panel_space(cal, panel_in, cal.capture_rotation, pw, ph);
    const Point delivered = rotate_panel_to_logical(panel_out, 270, pw, ph);

    // The rotation now sits between the matrix and the screen, so the delivered
    // logical point is the taught point carried through that rotation.
    const Point expected = rotate_panel_to_logical(promised, 270, pw, ph);
    INFO("expected (" << expected.x << "," << expected.y << ") delivered (" << delivered.x << ","
                      << delivered.y << ")");
    REQUIRE(delivered.x == expected.x);
    REQUIRE(delivered.y == expected.y);
}

// ---------------------------------------------------------------------------
// Provenance: what an ABSENT rotation key means
//
// The placement above is only as good as the rotation handed to it. A record
// written before the key existed carries none, and the value chosen for it
// decides which basis the matrix lands in.
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture,
                 "an unstamped calibration on an unrotated display loads as panel-space",
                 "[touch-calibration][rotation][provenance][1394]") {
    // A record with no rotation key was written when the affine was fed panel-space
    // points directly, which is what a capture rotation of zero describes. With
    // nothing rotating underneath it that reading is exact rather than a guess, so
    // the record stands and the user is never asked to recalibrate.
    Config* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    write_unstamped_affine(*cfg);
    REQUIRE_FALSE(cfg->exists("/input/calibration/rotation"));

    ScopedRotation unrotated(LV_DISPLAY_ROTATION_0);
    REQUIRE(display_rotation_degrees() == 0);

    const TouchCalibration loaded = load_touch_calibration();

    // Proves the load ran rather than bailing early, so the rotation below is
    // the loader's answer and not a default-constructed struct.
    REQUIRE(loaded.valid);
    REQUIRE(loaded.a == Catch::Approx(1.7f));
    CHECK(loaded.capture_rotation == 0);
    // Left completely alone: the next boot finds the same record.
    CHECK(cfg->get<bool>("/input/calibration/valid", false));

    cfg->set<bool>("/input/calibration/valid", false);
}

TEST_CASE_METHOD(LVGLTestFixture, "an unstamped calibration on a rotated display is discarded",
                 "[touch-calibration][rotation][provenance][1394]") {
    // The upgrade case. A rotated unit calibrated before the rotation key existed
    // carries a matrix solved in logical space against a rotation nothing recorded,
    // so reading zero for it lands every tap a quarter turn out - on a screen the
    // user then cannot navigate to Settings to fix. One recalibration is the
    // recoverable outcome, so the record goes.
    //
    // The drop is PERSISTED, not just applied in memory: /input/calibration/valid is
    // what the wizard step and the Settings row each read to decide "already
    // calibrated", so an in-memory-only drop would leave the device reporting itself
    // calibrated and never offer the wizard that fixes it.
    Config* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    write_unstamped_affine(*cfg);
    REQUIRE_FALSE(cfg->exists("/input/calibration/rotation"));

    ScopedRotation rotated(LV_DISPLAY_ROTATION_270);
    REQUIRE(display_rotation_degrees() == 270);

    const TouchCalibration loaded = load_touch_calibration();

    CHECK_FALSE(loaded.valid);
    // The stored key was true going in, so it flipping is positive evidence the
    // invalidation branch ran, rather than a loader that bailed before reading it.
    CHECK_FALSE(cfg->get<bool>("/input/calibration/valid", true));
}

TEST_CASE_METHOD(LVGLTestFixture, "a stored rotation key is honoured over the display's",
                 "[touch-calibration][rotation][provenance][1394]") {
    // The other half: a record that DOES carry provenance keeps it, whatever the
    // display is doing now. Without this the pair above is satisfied by a loader
    // that hardcodes zero and never reads the key at all, and it is what pins the
    // discard above to the MISSING stamp rather than to the rotation alone.
    Config* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    write_unstamped_affine(*cfg);
    cfg->set<int>("/input/calibration/rotation", 90);

    ScopedRotation rotated(LV_DISPLAY_ROTATION_270);
    REQUIRE(display_rotation_degrees() == 270);

    const TouchCalibration loaded = load_touch_calibration();

    REQUIRE(loaded.valid);
    CHECK(loaded.capture_rotation == 90);
    CHECK(cfg->get<bool>("/input/calibration/valid", false));

    cfg->set<bool>("/input/calibration/valid", false);
}
