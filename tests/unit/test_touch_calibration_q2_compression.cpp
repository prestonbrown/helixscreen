// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC
//
// Qidi Q2 touch-calibration trap (prestonbrown/helixscreen#943).
//
// The Q2's digitizer over-advertises its ABS range, so the coarse scale applied
// before LVGL sees the event over-divides: every physical touch is reported at
// s * (physical position) for some s < 1, anchored at the TOP-LEFT of the panel.
// Nothing outside [0, s*W] x [0, s*H] can be addressed at all.
//
// Four independent defects combined to trap such a user inside the Touch
// Calibration overlay with no way out and no way to commit a calibration. Each
// test below pins one of them:
//
//   1. VERIFY re-enabled the pre-session (broken) matrix instead of the freshly
//      captured one, so bottom-anchored Accept/Retry stayed unaddressable.
//   2. No escape affordance was deliverable at every point of the capture flow
//      under the production activate ordering (state IDLE at on_activate).
//   3. The VERIFY timeout restarted capture forever with no retry bound.
//   4. The VERIFY touch counters never incremented, so the 3s fast-revert
//      safety net could not fire.
//
// The compression is modelled directly rather than through an indev: what
// matters is which screen rectangles a physically-possible touch can reach.

#include "ui_component_header_bar.h"
#include "ui_touch_calibration_overlay.h"

#include "../test_fixtures.h"
#include "../test_helpers/touch_calibration_overlay_test_access.h"
#include "../test_helpers/touch_calibration_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "misc/lv_event_private.h" // lv_event_dsc_t::filter — no public accessor
#include "touch_calibration.h"
#include "touch_calibration_panel.h"
#include "touch_calibration_session.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using Catch::Approx;

namespace {

// Micro-tier geometry that reproduces the Q2 panel.
constexpr int32_t MICRO_W = 480;
constexpr int32_t MICRO_H = 272;

// The reported/physical ratio the reporter's panel exhibits. Any s < 1 anchored
// at the origin reproduces the trap; 0.6 is the suspected value.
constexpr double COMPRESSION = 0.6;

/// Records what the session installs on the live input device, modelling the
/// backend: disable_affine() leaves the stored matrix intact, enable_affine()
/// re-activates whatever is stored.
struct FakeSink : helix::ICalibrationSink {
    helix::TouchCalibration stored{};
    bool affine_enabled = true;
    std::vector<std::string> ops;

    helix::TouchCalibration current_calibration() const override {
        return stored;
    }
    bool apply_calibration(const helix::TouchCalibration& cal) override {
        if (!cal.valid) {
            ops.emplace_back("apply:rejected");
            return false;
        }
        stored = cal;
        affine_enabled = true;
        ops.emplace_back("apply");
        return true;
    }
    void disable_affine() override {
        affine_enabled = false;
        ops.emplace_back("disable");
    }
    void enable_affine() override {
        affine_enabled = true;
        ops.emplace_back("enable");
    }
    void clear_calibration() override {
        stored = helix::TouchCalibration{};
        ops.emplace_back("clear");
    }
};

/// The matrix a Q2 ships with when nobody has calibrated it: identity. It does
/// nothing to undo the digitizer's compression, which is exactly why the user
/// opens the calibration screen in the first place.
helix::TouchCalibration identity_cal() {
    helix::TouchCalibration c{};
    c.a = 1.0f;
    c.b = 0.0f;
    c.c = 0.0f;
    c.d = 0.0f;
    c.e = 1.0f;
    c.f = 0.0f;
    c.valid = true;
    return c;
}

/// Can the digitizer emit this pre-affine coordinate at all? Under a top-left
/// anchored compression by `s`, a finger anywhere on the panel reports somewhere
/// in [0, s*(W-1)] x [0, s*(H-1)] and nowhere else.
bool reportable(const helix::Point& raw, double s) {
    return raw.x >= 0 && raw.y >= 0 && raw.x <= static_cast<int>((MICRO_W - 1) * s) &&
           raw.y <= static_cast<int>((MICRO_H - 1) * s);
}

/// Is the widget rect centred at (x,y) addressable through `cal`? Invert the
/// active matrix to recover the pre-affine coordinate a finger would have to
/// produce, then ask whether the digitizer can produce it.
bool screen_point_addressable(const helix::TouchCalibration& cal, int32_t x, int32_t y, double s) {
    helix::Point raw{};
    if (!helix::invert_transform_point(cal, {static_cast<int>(x), static_cast<int>(y)}, raw)) {
        return false;
    }
    return reportable(raw, s);
}

/// Does `obj` carry an event callback registered for exactly `code`? This is how
/// the XML <event_cb trigger="..."> wiring is pinned: LVGL exposes no filter
/// accessor, and the registered trampolines dispatch to the process-wide overlay
/// singleton rather than to a test-owned instance.
bool has_event_for(lv_obj_t* obj, lv_event_code_t code) {
    const uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc != nullptr && static_cast<lv_event_code_t>(dsc->filter) == code) {
            return true;
        }
    }
    return false;
}

lv_obj_t* hit_at(lv_obj_t* screen, int32_t x, int32_t y) {
    lv_point_t p{x, y};
    return lv_indev_search_obj(screen, &p);
}

std::string name_of(lv_obj_t* obj) {
    if (obj == nullptr)
        return "<null>";
    const char* n = lv_obj_get_name(obj);
    return n != nullptr ? n : "<unnamed>";
}

bool is_within(lv_obj_t* node, lv_obj_t* ancestor) {
    for (lv_obj_t* o = node; o != nullptr; o = lv_obj_get_parent(o)) {
        if (o == ancestor)
            return true;
    }
    return false;
}

/// Builds a 480x272 display with the real header metrics and a fully wired
/// TouchCalibrationOverlay whose calibration handoffs land in `sink`.
class Q2CompressionFixture : public XMLTestFixture {
  public:
    Q2CompressionFixture() {
        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("overlay_panel"));
        REQUIRE(register_component("touch_calibration_overlay"));

        // The XML event trampolines dispatch to the process-wide overlay
        // singleton, so anything asserting real event WIRING has to observe that
        // instance. Initialise its subjects first (idempotent) so its handlers
        // never write through uninitialised lv_subject_t storage; the local
        // instance below then re-registers the same names into the XML scope and
        // is what the bindings in this fixture's widget tree resolve against.
        helix::ui::TouchCalibrationOverlay& wired = helix::ui::get_touch_calibration_overlay();
        wired.init_subjects();
        wired.register_callbacks();

        prev_display_ = lv_display_get_default();
        disp_ = lv_display_create(MICRO_W, MICRO_H);
        REQUIRE(disp_ != nullptr);
        lv_display_set_default(disp_);
        screen_ = lv_obj_create(nullptr);
        lv_screen_load(screen_);

        overlay_ = std::make_unique<helix::ui::TouchCalibrationOverlay>();
        overlay_->init_subjects();
        overlay_->register_callbacks();
        overlay_->set_calibration_sink(&sink_);

        root_ = overlay_->create(screen_);
        REQUIRE(root_ != nullptr);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);

        // The runtime overlay_width_transient token is not registered in the
        // unit-test XML scope; pin full-screen geometry deterministically.
        lv_obj_set_align(root_, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(root_, 0, 0);
        lv_obj_set_size(root_, MICRO_W, MICRO_H);

        lv_obj_t* header = lv_obj_find_by_name(root_, "overlay_header");
        REQUIRE(header != nullptr);
        ui_component_header_bar_setup(header, screen_);

        state_ = lv_xml_get_subject(nullptr, "touch_cal_state");
        REQUIRE(state_ != nullptr);

        panel_ = overlay_->get_panel();
        REQUIRE(panel_ != nullptr);
        panel_->set_screen_size(MICRO_W, MICRO_H);
        // Legacy sample-on-press: three on_press() calls per point commit that
        // point without a clock or release edge to model.
        helix::TouchCalibrationPanelTestAccess::set_debounce_enabled(*panel_, false);
    }

    ~Q2CompressionFixture() override {
        overlay_->cleanup();
        overlay_.reset();
        lv_display_set_default(prev_display_);
        lv_display_delete(disp_); // also deletes its screens + reparented widgets
    }

    Q2CompressionFixture(const Q2CompressionFixture&) = delete;
    Q2CompressionFixture& operator=(const Q2CompressionFixture&) = delete;

  protected:
    /// Production ordering: NavigationManager pushes the overlay while the state
    /// subject still reads IDLE, so on_activate() runs with the Cancel chip
    /// hidden. The pre-existing hit-test suite sets POINT_1 first and therefore
    /// never exercised this path.
    void activate_at_idle() {
        lv_subject_set_int(state_, 0);
        lv_obj_update_layout(screen_);
        overlay_->on_activate();
        lv_obj_update_layout(screen_);
    }

    /// Tap all three crosshairs with a finger, reporting compressed coordinates,
    /// and land the panel in VERIFY with a freshly solved matrix.
    void capture_three_points(double s) {
        panel_->start();
        for (int step = 0; step < 3; ++step) {
            const helix::Point target = panel_->get_target_position(step);
            const helix::Point raw{static_cast<int>(target.x * s), static_cast<int>(target.y * s)};
            for (int i = 0; i < helix::TouchCalibrationPanelTestAccess::samples_required(); ++i) {
                panel_->on_press(raw);
            }
        }
        lv_obj_update_layout(screen_);
    }

    /// The overlay instance the XML event trampolines actually reach, parked in
    /// a known-clean state (IDLE, sample-on-press) so a delivered event is
    /// visible as a state change on its panel.
    helix::TouchCalibrationPanel& wired_panel() {
        helix::ui::TouchCalibrationOverlay& wired = helix::ui::get_touch_calibration_overlay();
        helix::TouchCalibrationPanel* p = wired.get_panel();
        REQUIRE(p != nullptr);
        p->reset();
        helix::TouchCalibrationPanelTestAccess::set_debounce_enabled(*p, false);
        return *p;
    }

    lv_area_t coords_of(const char* name) {
        lv_obj_t* o = lv_obj_find_by_name(screen_, name);
        REQUIRE(o != nullptr);
        lv_area_t a{};
        lv_obj_get_coords(o, &a);
        return a;
    }

    FakeSink sink_;
    std::unique_ptr<helix::ui::TouchCalibrationOverlay> overlay_;
    helix::TouchCalibrationPanel* panel_ = nullptr;
    lv_display_t* prev_display_ = nullptr;
    lv_display_t* disp_ = nullptr;
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_subject_t* state_ = nullptr;
};

} // namespace

// ============================================================================
// Defect 1 — VERIFY must run under the matrix it is asking the user to verify
// ============================================================================

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: VERIFY installs the newly captured matrix, making Accept "
                 "reachable",
                 "[touch][calibration][943]") {
    sink_.stored = identity_cal();
    sink_.affine_enabled = true;

    activate_at_idle();
    capture_three_points(COMPRESSION);

    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY);
    const helix::TouchCalibration* fresh = panel_->get_calibration();
    REQUIRE(fresh != nullptr);
    REQUIRE(fresh->valid);

    // The solve must undo the compression: ~1/s on both diagonal terms.
    CHECK(fresh->a == Approx(1.0 / COMPRESSION).epsilon(0.05));
    CHECK(fresh->e == Approx(1.0 / COMPRESSION).epsilon(0.05));

    // Entering VERIFY installs the JUST-COMPUTED matrix, not the pre-session one.
    // Restoring the old matrix here is what made the whole screen unwinnable:
    // the only reason anyone opens it is that the stored matrix is the broken one.
    INFO("sink holds a=" << sink_.stored.a << " e=" << sink_.stored.e << ", fresh a=" << fresh->a);
    CHECK(sink_.affine_enabled);
    CHECK(sink_.stored.a == Approx(fresh->a).margin(1e-4));
    CHECK(sink_.stored.e == Approx(fresh->e).margin(1e-4));

    // The session backup is still armed, so Retry / dismiss / timeout revert.
    // (commit() happens only on Accept.)

    // --- Reachability of the bottom-anchored Accept button --------------------
    lv_subject_set_int(state_, 4); // STATE_VERIFY
    lv_obj_update_layout(screen_);
    const lv_area_t accept = coords_of("btn_accept");
    const int32_t ax = (accept.x1 + accept.x2) / 2;
    const int32_t ay = (accept.y1 + accept.y2) / 2;
    INFO("Accept centre (" << ax << "," << ay << ")");

    // Accept sits below the compressed reachable region, so under the OLD matrix
    // no physical finger can address it — the user can never commit.
    CHECK_FALSE(screen_point_addressable(identity_cal(), ax, ay, COMPRESSION));

    // Under the matrix VERIFY now installs, it is addressable.
    CHECK(screen_point_addressable(sink_.stored, ax, ay, COMPRESSION));

    // Same for Retry, the other way out of VERIFY.
    const lv_area_t retry = coords_of("btn_retry");
    const int32_t rx = (retry.x1 + retry.x2) / 2;
    const int32_t ry = (retry.y1 + retry.y2) / 2;
    CHECK_FALSE(screen_point_addressable(identity_cal(), rx, ry, COMPRESSION));
    CHECK(screen_point_addressable(sink_.stored, rx, ry, COMPRESSION));
}

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: an inverted-Y panel is also fixed by verifying under the new "
                 "matrix",
                 "[touch][calibration][943]") {
    // The regression the old "restore the known-good matrix" guard was added for
    // (#943/#986, AD5M): a resistive panel whose raw Y runs opposite to screen Y.
    // Verifying under the NEW matrix covers it too, because the new matrix is
    // precisely the one that un-inverts Y.
    sink_.stored = identity_cal();

    panel_->start();
    for (int step = 0; step < 3; ++step) {
        const helix::Point t = panel_->get_target_position(step);
        const helix::Point raw{t.x, (MICRO_H - 1) - t.y}; // Y inverted at capture
        for (int i = 0; i < helix::TouchCalibrationPanelTestAccess::samples_required(); ++i) {
            panel_->on_press(raw);
        }
    }
    lv_obj_update_layout(screen_);

    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY);
    const helix::TouchCalibration* fresh = panel_->get_calibration();
    REQUIRE(fresh != nullptr);
    CHECK(fresh->e == Approx(-1.0).epsilon(0.02)); // Y flipped back

    CHECK(sink_.stored.e == Approx(fresh->e).margin(1e-4));

    // Under the new matrix a finger at the bottom of the panel maps to Accept.
    lv_subject_set_int(state_, 4);
    lv_obj_update_layout(screen_);
    const lv_area_t accept = coords_of("btn_accept");
    const int32_t ax = (accept.x1 + accept.x2) / 2;
    const int32_t ay = (accept.y1 + accept.y2) / 2;
    helix::Point raw{};
    REQUIRE(helix::invert_transform_point(sink_.stored, {ax, ay}, raw));
    INFO("Accept centre (" << ax << "," << ay << ") <- raw (" << raw.x << "," << raw.y << ")");
    CHECK(raw.x >= 0);
    CHECK(raw.x < MICRO_W);
    CHECK(raw.y >= 0);
    CHECK(raw.y < MICRO_H);
}

// ============================================================================
// Defect 2 — an escape affordance deliverable everywhere a finger can reach
// ============================================================================

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: hold-anywhere abort is deliverable across the whole reachable "
                 "region",
                 "[touch][calibration][943]") {
    activate_at_idle();

    const int32_t max_x = static_cast<int32_t>((MICRO_W - 1) * COMPRESSION);
    const int32_t max_y = static_cast<int32_t>((MICRO_H - 1) * COMPRESSION);

    lv_obj_t* capture = lv_obj_find_by_name(screen_, "touch_capture_overlay");
    REQUIRE(capture != nullptr);

    // Every point a compressed digitizer can address during capture lands on the
    // full-screen capture surface, so a press-and-hold there is always delivered
    // — unlike any fixed chip, whose rect is either outside the reachable region
    // (bottom-left) or on the ray a mis-mapped crosshair tap travels (top-left).
    for (int32_t st = 1; st <= 3; ++st) {
        lv_subject_set_int(state_, st);
        lv_obj_update_layout(screen_);
        for (int32_t y = 0; y <= max_y; y += 20) {
            for (int32_t x = 0; x <= max_x; x += 20) {
                lv_obj_t* got = hit_at(screen_, x, y);
                INFO("state " << st << " reachable point (" << x << "," << y << ") -> "
                              << name_of(got));
                REQUIRE(got == capture);
            }
        }
    }

    lv_subject_set_int(state_, 1);
    lv_obj_update_layout(screen_);

    // The gesture is bound to the surface, not to a rectangle inside it.
    CHECK(has_event_for(capture, LV_EVENT_LONG_PRESSED_REPEAT));

    helix::TouchCalibrationPanel& wired = wired_panel();
    wired.start(); // POINT_1: a session in progress
    REQUIRE(wired.get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    // A brief hold must NOT abort — calibration is nothing but taps, and a slow
    // finger on a target crosses LVGL's long-press threshold routinely.
    for (int i = 0; i < 3; ++i) {
        lv_obj_send_event(capture, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    }
    CHECK(wired.get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    // A deliberate hold does. LVGL emits LONG_PRESSED once, then
    // LONG_PRESSED_REPEAT on a fixed cadence; the overlay counts the repeats.
    for (int i = 0; i < 20; ++i) {
        lv_obj_send_event(capture, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    }
    INFO("panel state after hold: " << static_cast<int>(wired.get_state()));
    CHECK(wired.get_state() == helix::TouchCalibrationPanel::State::IDLE);
}

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: hold-anywhere abort also works in VERIFY, where the capture "
                 "surface is hidden",
                 "[touch][calibration][943]") {
    activate_at_idle();
    capture_three_points(COMPRESSION);
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY);

    lv_obj_t* capture = lv_obj_find_by_name(screen_, "touch_capture_overlay");
    REQUIRE(capture != nullptr);
    // The capture surface MUST stay hidden in VERIFY or it covers Accept/Retry.
    CHECK(lv_obj_has_flag(capture, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* content = lv_obj_find_by_name(screen_, "calibration_content");
    REQUIRE(content != nullptr);

    CHECK(has_event_for(content, LV_EVENT_LONG_PRESSED_REPEAT));

    helix::TouchCalibrationPanel& wired = wired_panel();
    wired.start();
    REQUIRE(wired.get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    for (int i = 0; i < 20; ++i) {
        lv_obj_send_event(content, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    }
    CHECK(wired.get_state() == helix::TouchCalibrationPanel::State::IDLE);
}

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: the Cancel chip survives the production activate ordering",
                 "[touch][calibration][943]") {
    // raise_control_above_capture() snapshots the chip's on-screen rect at
    // on_activate() time. In production that happens while touch_cal_state is
    // still IDLE and the chip is hidden, so the snapshot must not pin a
    // degenerate rect that makes the chip unclickable once it un-hides.
    activate_at_idle();

    lv_subject_set_int(state_, 1); // STATE_POINT_1 — chip un-hides here
    lv_obj_update_layout(screen_);

    lv_obj_t* chip = lv_obj_find_by_name(screen_, "cancel_chip");
    REQUIRE(chip != nullptr);
    CHECK_FALSE(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));

    lv_area_t a{};
    lv_obj_get_coords(chip, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    INFO("chip rect " << a.x1 << "," << a.y1 << " " << w << "x" << h);
    CHECK(w >= 24);
    CHECK(h >= 16);

    const int32_t cx = (a.x1 + a.x2) / 2;
    const int32_t cy = (a.y1 + a.y2) / 2;
    lv_obj_t* got = hit_at(screen_, cx, cy);
    INFO("chip centre (" << cx << "," << cy << ") -> " << name_of(got));
    CHECK(is_within(got, chip));

    // It stays clear of every crosshair, as before.
    for (int step = 0; step < 3; ++step) {
        const helix::Point t = panel_->get_target_position(step);
        INFO("target " << step << " (" << t.x << "," << t.y << ")");
        const bool inside_chip = (t.x >= a.x1 && t.x <= a.x2 && t.y >= a.y1 && t.y <= a.y2);
        CHECK_FALSE(inside_chip);
    }
}

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: aborting from inside a dispatch defers the teardown",
                 "[touch][calibration][943]") {
    // abort_session() is reached from an LVGL event callback (the press-and-hold)
    // and from two lv_timer callbacks. Destroying or reparenting a widget
    // synchronously from there corrupts LVGL's event list — the crash family
    // behind #776, #190 and #80. The exit therefore has to be deferred, and
    // NavigationManager::go_back() provides that by wrapping its whole body in a
    // queue_update() lambda. This pins that contract: teardown moves `capture`,
    // so if the pop ever became synchronous the object LVGL is dispatching on
    // would be reparented mid-dispatch.
    auto& q = helix::ui::UpdateQueue::instance();

    activate_at_idle();
    lv_subject_set_int(state_, 1);
    lv_obj_update_layout(screen_);

    lv_obj_t* capture = lv_obj_find_by_name(screen_, "touch_capture_overlay");
    REQUIRE(capture != nullptr);
    lv_obj_t* const capture_parent = lv_obj_get_parent(capture);
    REQUIRE(capture_parent == screen_); // lifted to the screen root for capture

    panel_->start();
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    // Normalise: anything activation queued is not what this test is about.
    helix::ui::UpdateQueueTestAccess::discard_pending(q);
    REQUIRE(helix::ui::UpdateQueueTestAccess::queue_empty(q));

    // Hold long enough to abort, exactly as the event callback would.
    for (int i = 0; i < 40; ++i) {
        overlay_->handle_hold_abort();
        if (panel_->get_state() == helix::TouchCalibrationPanel::State::IDLE) {
            break;
        }
    }
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::IDLE);

    // The exit was requested...
    CHECK_FALSE(helix::ui::UpdateQueueTestAccess::queue_empty(q));
    // ...but nothing has been destroyed or moved yet.
    CHECK(lv_obj_is_valid(root_));
    CHECK(lv_obj_is_valid(capture));
    CHECK(lv_obj_get_parent(capture) == capture_parent);

    // Drop the deferred pop rather than running it: it closes over this
    // fixture's stack-owned widgets, and executing it after the test returns is
    // itself the use-after-free.
    helix::ui::UpdateQueueTestAccess::discard_pending(q);
}

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: the hold-to-cancel hint is visible during capture and clear of "
                 "the targets",
                 "[touch][calibration][943]") {
    // A guaranteed escape nobody can discover does not help the user who is
    // currently trapped, so the gesture has to be advertised on screen.
    activate_at_idle();
    lv_subject_set_int(state_, 1);
    lv_obj_update_layout(screen_);

    lv_obj_t* hint = lv_obj_find_by_name(screen_, "hold_hint");
    REQUIRE(hint != nullptr);
    CHECK_FALSE(lv_obj_has_flag(hint, LV_OBJ_FLAG_HIDDEN));

    lv_area_t h{};
    lv_obj_get_coords(hint, &h);
    INFO("hint rect " << h.x1 << "," << h.y1 << " " << lv_area_get_width(&h) << "x"
                      << lv_area_get_height(&h));
    CHECK(lv_area_get_width(&h) > 0);
    CHECK(lv_area_get_height(&h) > 0);
    // On screen, not clipped off the bottom/right edge.
    CHECK(h.x2 < MICRO_W);
    CHECK(h.y2 < MICRO_H);

    // Clear of every crosshair — P1 is bottom-CENTER, which is why the hint sits
    // bottom-right rather than bottom-center.
    for (int step = 0; step < 3; ++step) {
        const helix::Point t = panel_->get_target_position(step);
        INFO("target " << step << " (" << t.x << "," << t.y << ")");
        const bool inside_hint = (t.x >= h.x1 && t.x <= h.x2 && t.y >= h.y1 && t.y <= h.y2);
        CHECK_FALSE(inside_hint);
    }

    // Clear of the Cancel chip, the other escape affordance on that row.
    lv_obj_t* chip = lv_obj_find_by_name(screen_, "cancel_chip");
    REQUIRE(chip != nullptr);
    lv_area_t c{};
    lv_obj_get_coords(chip, &c);
    INFO("chip rect " << c.x1 << "," << c.y1 << " to " << c.x2 << "," << c.y2);
    const bool overlaps_chip = !(h.x2 < c.x1 || h.x1 > c.x2 || h.y2 < c.y1 || h.y1 > c.y2);
    CHECK_FALSE(overlaps_chip);

    // It must not eat touches meant for the capture surface.
    CHECK_FALSE(lv_obj_has_flag(hint, LV_OBJ_FLAG_CLICKABLE));

    // Visible exactly during active capture, like the Cancel chip.
    struct StateVis {
        int32_t st;
        bool hidden;
    };
    for (const auto& sv : {StateVis{0, true}, StateVis{1, false}, StateVis{2, false},
                           StateVis{3, false}, StateVis{4, true}, StateVis{5, true}}) {
        lv_subject_set_int(state_, sv.st);
        lv_obj_update_layout(screen_);
        INFO("hold hint in state " << sv.st << " expected hidden=" << sv.hidden);
        CHECK(lv_obj_has_flag(hint, LV_OBJ_FLAG_HIDDEN) == sv.hidden);
    }
}

// ============================================================================
// Defect 3 — the VERIFY timeout loop must terminate
// ============================================================================

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: VERIFY timeouts are bounded and then abort the session",
                 "[touch][calibration][943]") {
    sink_.stored = identity_cal();
    activate_at_idle();

    capture_three_points(COMPRESSION);
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY);

    // Round 1: the user could not press Accept, so the countdown expired. One
    // free restart is reasonable — maybe they were just slow.
    helix::TouchCalibrationPanelTestAccess::fire_verify_timeout(*panel_);
    CHECK(panel_->get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    capture_three_points(COMPRESSION);
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY);

    // Round 2: the same thing happened again. Restarting a third time is the
    // infinite loop that trapped the reporter — the session must abort instead.
    const size_t ops_before_abort = sink_.ops.size();
    helix::TouchCalibrationPanelTestAccess::fire_verify_timeout(*panel_);
    INFO("state after the bounded number of timeouts: " << static_cast<int>(panel_->get_state()));
    CHECK(panel_->get_state() != helix::TouchCalibrationPanel::State::POINT_1);
    CHECK(panel_->get_state() == helix::TouchCalibrationPanel::State::IDLE);

    // Aborting runs the session teardown, so touch is left usable rather than
    // stuck in the affine-disabled state capture puts it in. (Reverting to the
    // pre-session matrix is the session's own job and is pinned by
    // test_touch_calibration_session.cpp; this fixture never calls show(), so no
    // backup is held here.)
    const std::vector<std::string> abort_ops(
        sink_.ops.begin() + static_cast<long>(ops_before_abort), sink_.ops.end());
    INFO("sink ops during abort: " << abort_ops.size());
    CHECK(std::find(abort_ops.begin(), abort_ops.end(), "enable") != abort_ops.end());
    CHECK(sink_.affine_enabled);
}

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: a user-initiated Retry clears the timeout budget",
                 "[touch][calibration][943]") {
    sink_.stored = identity_cal();
    activate_at_idle();

    capture_three_points(COMPRESSION);
    helix::TouchCalibrationPanelTestAccess::fire_verify_timeout(*panel_);
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    // Someone who is actually driving the screen presses Retry; that is evidence
    // they can reach the buttons, so the budget resets rather than counting down
    // toward an abort they did not ask for.
    capture_three_points(COMPRESSION);
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY);
    overlay_->handle_retry_clicked();
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    capture_three_points(COMPRESSION);
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY);
    helix::TouchCalibrationPanelTestAccess::fire_verify_timeout(*panel_);
    CHECK(panel_->get_state() == helix::TouchCalibrationPanel::State::POINT_1);
}

// ============================================================================
// Defect 4 — VERIFY touches must reach the fast-revert counters
// ============================================================================

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: a VERIFY touch increments the counters the fast-revert net "
                 "reads",
                 "[touch][calibration][943]") {
    activate_at_idle();
    capture_three_points(COMPRESSION);
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::VERIFY);

    lv_subject_set_int(state_, 4); // STATE_VERIFY
    lv_obj_update_layout(screen_);

    lv_obj_t* capture = lv_obj_find_by_name(screen_, "touch_capture_overlay");
    REQUIRE(capture != nullptr);
    // The surface that carried the VERIFY press handler is hidden in VERIFY (it
    // has to be, or it covers Accept), which is why the counters never moved.
    REQUIRE(lv_obj_has_flag(capture, LV_OBJ_FLAG_HIDDEN));

    REQUIRE(helix::TouchCalibrationPanelTestAccess::verify_raw_touch_count(*panel_) == 0);

    // --- Wiring: some visible widget must still deliver VERIFY presses --------
    // This is the whole defect. The press handler lived only on the capture
    // surface, which is hidden in VERIFY, so nothing ever called
    // report_verify_touch() and the fast-revert net's counters stayed at zero.
    lv_obj_t* content = lv_obj_find_by_name(screen_, "calibration_content");
    REQUIRE(content != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(content, LV_OBJ_FLAG_HIDDEN));

    INFO("calibration_content event count = " << lv_obj_get_event_count(content));
    CHECK(lv_obj_get_event_count(content) > 0);
    CHECK(has_event_for(content, LV_EVENT_PRESSED));
    CHECK(has_event_for(content, LV_EVENT_RELEASED));
    // ...and it must be clickable, or LVGL never hit-tests it in the first place.
    CHECK(lv_obj_has_flag(content, LV_OBJ_FLAG_CLICKABLE));

    // --- Counting: a delivered VERIFY press arms the fast-revert heuristic ----
    overlay_->handle_screen_touched(nullptr);
    INFO("raw=" << helix::TouchCalibrationPanelTestAccess::verify_raw_touch_count(*panel_)
                << " onscreen="
                << helix::TouchCalibrationPanelTestAccess::verify_onscreen_touch_count(*panel_));
    CHECK(helix::TouchCalibrationPanelTestAccess::verify_raw_touch_count(*panel_) == 1);

    // With no indev the reported point is (0,0) — pinned to the panel corner,
    // which is exactly what a broken matrix produces once calibrated_read_cb()
    // clamps its output. The fast-revert heuristic must therefore be armed.
    CHECK(helix::TouchCalibrationPanelTestAccess::verify_onscreen_touch_count(*panel_) == 0);
    CHECK(helix::TouchCalibrationPanelTestAccess::fast_revert_would_fire(*panel_));
}

// ============================================================================
// Defect 5 — capture press markers must land in the space touches track in
// ============================================================================

namespace {

/// The lingering press dot create_touch_marker() drops on the top layer: the
/// 18px circle with a 2px white ring (#1082). `children_before` is the layer's
/// child count captured before the press; the ripple dropped alongside it has
/// no border, so the ring identifies the dot.
lv_obj_t* find_press_dot(uint32_t children_before) {
    lv_obj_t* layer = lv_layer_top();
    for (uint32_t i = children_before; i < lv_obj_get_child_count(layer); ++i) {
        lv_obj_t* c = lv_obj_get_child(layer, i);
        if (c != nullptr && lv_obj_get_style_border_width(c, LV_PART_MAIN) == 2) {
            return c;
        }
    }
    return nullptr;
}

/// Centre of the marker dot: create_touch_marker() positions the 18px dot at
/// (x - 9, y - 9).
lv_point_t dot_center(lv_obj_t* dot) {
    return {lv_obj_get_style_x(dot, LV_PART_MAIN) + 9, lv_obj_get_style_y(dot, LV_PART_MAIN) + 9};
}

} // namespace

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: capture press markers land where the pre-session calibration "
                 "maps the finger",
                 "[touch][calibration][943][1082]") {
    // The 0.99.114 reporter's remaining complaint: touch works after
    // calibration, but "the red dots that show during the calibration were
    // still off". The Q2's Goodix over-reports its ABS range, so raw capture
    // space is compressed ~0.5x relative to the screen (bundle N4ZN3YY2: span
    // ratios 0.57/0.49) and a dot drawn at the raw point lands centimetres
    // from its crosshair — reading as "broken" while calibration works. The
    // dot must instead show where the finger lands under the mapping the
    // user's touches were tracking in when the session opened: the session
    // backup.
    sink_.stored = identity_cal();
    sink_.stored.c = 60.0f; // a pure-offset backup: raw (0,0) -> screen (60, 30)
    sink_.stored.f = 30.0f;
    sink_.affine_enabled = true;

    activate_at_idle();
    helix::ui::TouchCalibrationOverlayTestAccess::begin_session(*overlay_);
    panel_->start();
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    const uint32_t before = lv_obj_get_child_count(lv_layer_top());
    overlay_->handle_screen_touched(nullptr); // no indev in tests: point reads (0,0)
    lv_obj_t* dot = find_press_dot(before);
    REQUIRE(dot != nullptr);

    const lv_point_t centre = dot_center(dot);
    INFO("dot centre (" << centre.x << "," << centre.y << ")");
    // Through the backup mapping of the raw press — NOT at the raw point,
    // which would read (0,0).
    CHECK(centre.x == 60);
    CHECK(centre.y == 30);
}

TEST_CASE_METHOD(Q2CompressionFixture,
                 "Q2 compression: with no pre-session calibration the marker stays at the raw "
                 "point",
                 "[touch][calibration][943][1082]") {
    // First-ever calibration: there is no mapping to draw through, and the
    // pre-calibration state IS broken — the raw point is the honest position.
    sink_.stored = helix::TouchCalibration{}; // uncalibrated device
    sink_.affine_enabled = true;

    activate_at_idle();
    helix::ui::TouchCalibrationOverlayTestAccess::begin_session(*overlay_);
    panel_->start();
    REQUIRE(panel_->get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    const uint32_t before = lv_obj_get_child_count(lv_layer_top());
    overlay_->handle_screen_touched(nullptr);
    lv_obj_t* dot = find_press_dot(before);
    REQUIRE(dot != nullptr);

    const lv_point_t centre = dot_center(dot);
    INFO("dot centre (" << centre.x << "," << centre.y << ")");
    CHECK(centre.x == 0);
    CHECK(centre.y == 0);
}
