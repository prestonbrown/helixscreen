// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Which way a lane's spool turns and how fast is decided from plain inputs; a
// spool canvas shows that rotation by stepping its vents through phases.

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "spool_spin.h"
#include "spool_vents.h"

#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsAction;
using namespace helix::ui;

namespace {

SpoolSpinInput printing_lane(int lane) {
    SpoolSpinInput in;
    in.lane = lane;
    in.current_slot = lane;
    in.printing = true;
    in.lane_active_loaded = true;
    in.animations_enabled = true;
    return in;
}

SpoolSpinInput moving(int lane, int current, int target, AmsAction action) {
    SpoolSpinInput in;
    in.lane = lane;
    in.current_slot = current;
    in.target_slot = target;
    in.action = action;
    in.animations_enabled = true;
    return in;
}

constexpr SpoolSpin kStill{};
constexpr SpoolSpin kLoad{1, kSpoolSpinLoadStepMs};
constexpr SpoolSpin kUnload{-1, kSpoolSpinLoadStepMs};

} // namespace

TEST_CASE("spool spin: a printing lane turns slowly, paced by flow when it is known",
          "[spool_spin]") {
    auto in = printing_lane(2);
    CHECK(decide_spool_spin(in) == SpoolSpin{1, kSpoolSpinPrintStepMs});

    in.extruder_velocity_centimm = 100; // 1 mm/s
    CHECK(decide_spool_spin(in) == SpoolSpin{1, kSpoolSpinPrintSlowStepMs});
    in.extruder_velocity_centimm = 300;
    CHECK(decide_spool_spin(in) == SpoolSpin{1, kSpoolSpinPrintStepMs});
    in.extruder_velocity_centimm = 800;
    CHECK(decide_spool_spin(in) == SpoolSpin{1, kSpoolSpinPrintFastStepMs});
    // A retract reads negative: the spool keeps its slow pace rather than twitching.
    in.extruder_velocity_centimm = -2500;
    CHECK(decide_spool_spin(in) == SpoolSpin{1, kSpoolSpinPrintStepMs});
}

TEST_CASE("spool spin: only the lane feeding a running print turns", "[spool_spin]") {
    auto other = printing_lane(2);
    other.lane_active_loaded = false;
    CHECK(decide_spool_spin(other) == kStill);

    auto paused = printing_lane(2);
    paused.printing = false;
    CHECK(decide_spool_spin(paused) == kStill);

    // Mid-print toolchange work stops the extruder for the print's own lane.
    auto heating = printing_lane(2);
    heating.action = AmsAction::HEATING;
    CHECK(decide_spool_spin(heating) == kStill);
}

TEST_CASE("spool spin: a load spins the moving lane fast and forward", "[spool_spin]") {
    CHECK(decide_spool_spin(moving(3, 1, 3, AmsAction::LOADING)) == kLoad);
    CHECK(decide_spool_spin(moving(1, 1, 3, AmsAction::LOADING)) == kStill);
    // A load that names no target is the current lane's.
    CHECK(decide_spool_spin(moving(1, 1, -1, AmsAction::LOADING)) == kLoad);
    CHECK(decide_spool_spin(moving(2, 1, -1, AmsAction::LOADING)) == kStill);
}

TEST_CASE("spool spin: an unload spins the current lane fast and backward", "[spool_spin]") {
    CHECK(decide_spool_spin(moving(1, 1, 3, AmsAction::UNLOADING)) == kUnload);
    CHECK(decide_spool_spin(moving(3, 1, 3, AmsAction::UNLOADING)) == kStill);
}

TEST_CASE("spool spin: a load during a print wins over the print's slow turn", "[spool_spin]") {
    auto in = moving(3, 1, 3, AmsAction::LOADING);
    in.printing = true;
    in.lane_active_loaded = true;
    CHECK(decide_spool_spin(in) == kLoad);
}

TEST_CASE("spool spin: every other action leaves the lane still", "[spool_spin]") {
    for (AmsAction action : {AmsAction::IDLE, AmsAction::SELECTING, AmsAction::HEATING,
                             AmsAction::CUTTING, AmsAction::PURGING, AmsAction::ERROR}) {
        CHECK(decide_spool_spin(moving(1, 1, 1, action)) == kStill);
    }
}

TEST_CASE("spool spin: animations off or a slow board stops everything", "[spool_spin]") {
    auto print = printing_lane(0);
    auto load = moving(0, 0, -1, AmsAction::LOADING);

    print.animations_enabled = load.animations_enabled = false;
    CHECK(decide_spool_spin(print) == kStill);
    CHECK(decide_spool_spin(load) == kStill);

    print.animations_enabled = load.animations_enabled = true;
    print.reduced_effects = load.reduced_effects = true;
    CHECK(decide_spool_spin(print) == kStill);
    CHECK(decide_spool_spin(load) == kStill);
}

TEST_CASE("spool vents: a full phase cycle returns the vents to where they started",
          "[spool_spin]") {
    for (const auto detail : {SpoolVentDetail::Slots, SpoolVentDetail::SlotsAndDots}) {
        const int phases = spool_vent_phase_count(detail);
        REQUIRE(phases > 1);

        SpoolVentFace rest{100.0f, 100.0f, 200.0f, 1.0f, detail};
        SpoolVentFace turned = rest;
        spool_vent_face_set_phase(turned, 1);
        SpoolVentFace wrapped = rest;
        spool_vent_face_set_phase(wrapped, phases);

        int moved = 0;
        for (float y = 20.0f; y < 180.0f; y += 3.1f) {
            for (float x = 20.0f; x < 180.0f; x += 3.1f) {
                const float at_rest = spool_vent_coverage(rest, x, y);
                CHECK(spool_vent_coverage(wrapped, x, y) == Catch::Approx(at_rest).margin(0.02f));
                if (std::abs(spool_vent_coverage(turned, x, y) - at_rest) > 0.5f)
                    ++moved;
            }
        }
        CHECK(moved > 20);
    }
}

TEST_CASE("spool vents: a solid flange has a single phase", "[spool_spin]") {
    CHECK(spool_vent_phase_count(SpoolVentDetail::None) == 1);
}

namespace {

std::vector<uint8_t> buffer_of(lv_obj_t* canvas) {
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    return {buf->data, buf->data + buf->data_size};
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "spool canvas: a spin moves the vents, stopping holds them",
                 "[spool_spin][ui]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 64);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xCC2020));
    lv_obj_update_layout(canvas);
    process_lvgl(10);
    const auto rest = buffer_of(canvas);

    spool_canvas_set_spin(canvas, SpoolSpin{1, 40});
    process_lvgl(200);
    const auto moved = buffer_of(canvas);
    const bool same = moved == rest;
    CHECK_FALSE(same);

    spool_canvas_set_spin(canvas, SpoolSpin{});
    const auto stopped = buffer_of(canvas);
    process_lvgl(200);
    const bool held = buffer_of(canvas) == stopped;
    CHECK(held);

    // A running spin dies with its widget; the next ticks must find nothing to run.
    spool_canvas_set_spin(canvas, SpoolSpin{-1, 40});
    lv_obj_delete(canvas);
    process_lvgl(200);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool canvas: a hidden spool does not step its vents",
                 "[spool_spin][ui]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 64);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0x20CC20));
    lv_obj_update_layout(canvas);
    process_lvgl(10);
    lv_obj_set_hidden(canvas, true);
    const auto rest = buffer_of(canvas);

    spool_canvas_set_spin(canvas, SpoolSpin{1, 40});
    process_lvgl(200);
    const bool unchanged = buffer_of(canvas) == rest;
    CHECK(unchanged);
    lv_obj_delete(canvas);
}

TEST_CASE_METHOD(LVGLTestFixture, "A hidden spool does not keep an animation running",
                 "[spool_spin]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 64);
    REQUIRE(canvas != nullptr);
    lv_obj_set_hidden(canvas, true);
    process_lvgl(20);
    const auto before = lv_anim_count_running();
    spool_canvas_set_spin(canvas, helix::ui::SpoolSpin{1, 40});
    CHECK(lv_anim_count_running() == before);
}
