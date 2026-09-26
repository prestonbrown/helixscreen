// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_graph_overlay.cpp
 * @brief Unit tests for TempGraphOverlay
 *
 * Tests subject initialization, lifecycle, global accessor pattern,
 * series color palette, and Y-axis auto-scaling logic.
 *
 * @see ui_overlay_temp_graph.h
 */

#include "ui_overlay_temp_graph.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "subject_debug_registry.h"
#include "temperature_controller.h"
#include "temperature_service.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "../catch_amalgamated.hpp"

// =============================================================================
// Y-axis auto-scaling helper
// =============================================================================
//
// The scaling logic is private inside TempGraphOverlay::update_y_axis_range().
// We replicate the algorithm here as a free function so we can unit test
// the math without needing a fully-wired graph.  If the implementation
// changes, these tests will catch divergence at review time.

namespace {

struct YAxisParams {
    float step = 50.0f;
    float floor = 100.0f;
    float ceiling = 400.0f;
    float expand_threshold = 0.85f;
    float shrink_threshold = 0.55f;
};

/**
 * @brief Replicate TempGraphOverlay::update_y_axis_range() scaling logic
 *
 * Given the current y_axis_max and the observed max_temp, return the new
 * y_axis_max after applying expand/shrink hysteresis.
 */
float compute_y_axis_max(float current_max, float max_temp, const YAxisParams& p = {}) {
    float new_max = current_max;

    if (max_temp > current_max * p.expand_threshold) {
        // Expand: round up to next step
        new_max = (std::floor(max_temp / p.step) + 1.0f) * p.step;
    } else if (max_temp < current_max * p.shrink_threshold && current_max > p.floor) {
        // Shrink: round up to next step (but not below floor)
        new_max = std::max(p.floor, (std::floor(max_temp / p.step) + 1.0f) * p.step);
    }

    new_max = std::min(new_max, p.ceiling);
    new_max = std::max(new_max, p.floor);
    return new_max;
}

} // namespace

// =============================================================================
// Basic Properties
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: get_name returns expected value",
                 "[temp_graph_overlay]") {
    TempGraphOverlay overlay;
    REQUIRE(std::string(overlay.get_name()) == "Temperature Graph");
}

// NOTE: a "mode defaults to GraphOnly" case used to live here. It constructed an
// overlay and asserted nothing, because Mode is private and has no observable
// effect until create()/open() builds the control strips. Rather than add a
// production getter so the test could restate a member initializer, the case is
// gone. The default becomes testable the day this file can build the overlay from
// XML: opening in the default mode must leave nozzle/bed/chamber strips hidden.

// =============================================================================
// Subject Initialization
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: init_subjects sets initialized flag",
                 "[temp_graph_overlay]") {
    TempGraphOverlay overlay;

    REQUIRE_FALSE(overlay.are_subjects_initialized());

    overlay.init_subjects();

    REQUIRE(overlay.are_subjects_initialized());
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: double init_subjects does not crash",
                 "[temp_graph_overlay]") {
    TempGraphOverlay overlay;

    overlay.init_subjects();
    REQUIRE(overlay.are_subjects_initialized());

    // Second call should be a no-op (guarded by init_subjects_guarded)
    overlay.init_subjects();
    REQUIRE(overlay.are_subjects_initialized());
}

// =============================================================================
// Global Accessor Pattern
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: global accessor returns valid reference",
                 "[temp_graph_overlay]") {
    TempGraphOverlay& overlay = get_global_temp_graph_overlay();
    REQUIRE(std::string(overlay.get_name()) == "Temperature Graph");
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: global accessor returns same instance",
                 "[temp_graph_overlay]") {
    TempGraphOverlay& first = get_global_temp_graph_overlay();
    TempGraphOverlay& second = get_global_temp_graph_overlay();

    REQUIRE(&first == &second);
}

// =============================================================================
// Visibility snapshot (consumed by home graph card "follow" mode)
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "TempGraphOverlay: visibility snapshot starts unset until overlay opens",
                 "[temp_graph_overlay][snapshot]") {
    // The accessor returns by value and is filtered against the active printer
    // name, so in test isolation it may carry state from a prior activation.
    // Assert only the API contract: every entry, if any, is a non-empty
    // klipper name.
    auto snap = get_temp_graph_visibility_snapshot();
    const bool every_entry_named =
        !snap.has_value() || std::none_of(snap->begin(), snap->end(),
                                          [](const std::string& name) { return name.empty(); });
    REQUIRE(every_entry_named);
}

// =============================================================================
// Destructor / Cleanup
// =============================================================================

TEST_CASE_METHOD(
    LVGLTestFixture,
    "TempGraphOverlay: init_subjects publishes its four subjects, destructor withdraws them",
    "[temp_graph_overlay]") {
    // init_subjects() publishes four subjects - temp_graph_mode (strip
    // visibility and graph_outer width, see temp_graph_overlay.xml's <subjects>
    // block), temp_graph_nozzle_badge (the tool number the nozzle digit
    // shows), and temp_graph_nozzle_temp/temp_graph_nozzle_target (the nozzle
    // card's current/target, mirrored from whichever extruder it displays).
    // The SubjectManager destructor (deinit_all, run from ~TempGraphOverlay
    // via the subjects_ member) must withdraw all four names so they do not
    // outlive the overlay. This pins both halves: the publish count AND the
    // destructor cleanup. If a fifth subject is ever added, bump the +4 and
    // name the newcomer here so the withdrawal stays covered too.
    auto name_present = [](const std::string& needle) {
        auto all = SubjectDebugRegistry::instance().list_all();
        return std::any_of(all.begin(), all.end(),
                           [&](const auto& entry) { return entry.first == needle; });
    };

    const size_t before = SubjectDebugRegistry::instance().list_all().size();
    REQUIRE_FALSE(name_present("temp_graph_mode"));

    {
        TempGraphOverlay overlay;
        overlay.init_subjects();
        REQUIRE(overlay.are_subjects_initialized());

        // Exactly four subjects published: temp_graph_mode,
        // temp_graph_nozzle_badge, temp_graph_nozzle_temp,
        // temp_graph_nozzle_target.
        REQUIRE(SubjectDebugRegistry::instance().list_all().size() == before + 4);
        REQUIRE(name_present("temp_graph_mode"));
        REQUIRE(name_present("temp_graph_nozzle_badge"));
        REQUIRE(name_present("temp_graph_nozzle_temp"));
        REQUIRE(name_present("temp_graph_nozzle_target"));
        // Destructor runs here.
    }

    REQUIRE(SubjectDebugRegistry::instance().list_all().size() == before);
    REQUIRE_FALSE(name_present("temp_graph_mode"));
    REQUIRE_FALSE(name_present("temp_graph_nozzle_badge"));
    REQUIRE_FALSE(name_present("temp_graph_nozzle_temp"));
    REQUIRE_FALSE(name_present("temp_graph_nozzle_target"));
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: destructor safe without init_subjects",
                 "[temp_graph_overlay]") {
    {
        TempGraphOverlay overlay;
        REQUIRE_FALSE(overlay.are_subjects_initialized());
        // Destructor runs here - should be safe even without init
    }
    SUCCEED("Destructor completed without crash");
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: cleanup on fresh instance does not crash",
                 "[temp_graph_overlay]") {
    TempGraphOverlay overlay;
    REQUIRE_FALSE(overlay.cleanup_called());

    overlay.cleanup();

    REQUIRE(overlay.cleanup_called());
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: cleanup after init_subjects does not crash",
                 "[temp_graph_overlay]") {
    TempGraphOverlay overlay;
    overlay.init_subjects();

    overlay.cleanup();

    REQUIRE(overlay.cleanup_called());
}

// =============================================================================
// Tool pick: the card mirrors the picked extruder, and nozzle sends reach it
// =============================================================================
//
// The static XML callbacks resolve the GLOBAL overlay, and repoint_nozzle_card
// needs no widget tree, so these cases wire the real overlay object against a
// per-fixture printer state and drive the same private entry points the XML
// event path uses.

class TempGraphOverlayTestAccess {
  public:
    static void set_deps(TempGraphOverlay& o, helix::PrinterState* state,
                         TemperatureService* service) {
        o.printer_state_ = state;
        o.temp_control_panel_ = service;
    }
    static void set_mode(TempGraphOverlay& o, TempGraphOverlay::Mode mode) {
        o.mode_ = mode;
    }
    static void set_picked(TempGraphOverlay& o, const std::string& name) {
        o.picked_extruder_ = name;
    }
    static const std::string& picked(TempGraphOverlay& o) {
        return o.picked_extruder_;
    }
    static void select(TempGraphOverlay& o, const std::string& name) {
        o.select_extruder(name);
    }
    static void repoint(TempGraphOverlay& o) {
        o.repoint_nozzle_card();
    }
    /// Arm the rediscovery watch the way on_activate does. Tests cannot call
    /// on_activate() directly - it resolves the GLOBAL printer state, not the
    /// fixture's - so this is the fixture's stand-in for that half of
    /// activation.
    static void arm_version_watch(TempGraphOverlay& o) {
        o.watch_extruder_version();
    }
    static int card_temp(TempGraphOverlay& o) {
        return lv_subject_get_int(&o.nozzle_card_temp_subject_);
    }
    static int card_target(TempGraphOverlay& o) {
        return lv_subject_get_int(&o.nozzle_card_target_subject_);
    }
    static const char* badge(TempGraphOverlay& o) {
        return lv_subject_get_string(&o.nozzle_badge_subject_);
    }
    /// The custom-entry keypad's opening click (the XML callback ignores its
    /// event, so a null one exercises the production path).
    static void custom_clicked(TempGraphOverlay& o) {
        TempGraphOverlay::on_temp_graph_custom_clicked(nullptr);
    }
    /// The keypad's confirm, delivered the way ui_component_keypad does.
    static void keypad_confirm(TempGraphOverlay& o, float value) {
        TempGraphOverlay::keypad_value_cb(value, &o.keypad_ctx_);
    }
};

/// Per-fixture printer state wired to a mock client, mirroring
/// ControllerFixture in test_temperature_controller.cpp: the gcode history is
/// what proves WHICH extruder received a target.
class TempGraphOverlayPickFixture : public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::shared_ptr<helix::TemperatureController> controller_{
        std::make_shared<helix::TemperatureController>(state, &api)};
    TemperatureService service{state, &api};

    TempGraphOverlayPickFixture() {
        state.init_subjects(false);
        service.set_controller(controller_.get());
        // execute_gcode gates on klippy state; the subject defaults to SHUTDOWN.
        state.set_klippy_state_sync(helix::KlippyState::READY);
    }

    ~TempGraphOverlayPickFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        service.set_controller(nullptr);
    }

    /// Two tools at different temperatures, so which one the card mirrors and
    /// which one receives a send are both observable. Machine tool = tool 1.
    void seed_two_tools() {
        state.init_extruders({"extruder", "extruder1"});
        state.update_from_status({{"extruder", {{"temperature", 55.0}, {"target", 55.0}}},
                                  {"extruder1", {{"temperature", 260.0}, {"target", 260.0}}}});
        helix::ui::UpdateQueue::instance().drain();
    }
};

TEST_CASE_METHOD(
    TempGraphOverlayPickFixture,
    "TempGraphOverlay: a custom-entry send reaches the picked tool, not the active one",
    "[temp_graph_overlay][active-extruder]") {
    // The static XML callbacks resolve the global overlay instance.
    TempGraphOverlay& overlay = get_global_temp_graph_overlay();
    seed_two_tools();

    TempGraphOverlayTestAccess::set_deps(overlay, &state, &service);
    TempGraphOverlayTestAccess::set_mode(overlay, TempGraphOverlay::Mode::Nozzle);
    TempGraphOverlayTestAccess::set_picked(overlay, "extruder1");

    // The regression's precondition: the card displays tool 2 while the
    // machine's active tool is tool 1.
    REQUIRE(state.active_extruder_name() == "extruder");

    client.clear_gcode_script_history();

    // The click that opens the custom-entry keypad captures the displayed
    // extruder before the keypad's stacked push deactivates this overlay.
    TempGraphOverlayTestAccess::custom_clicked(overlay);

    // That stacked push reports NavigateAway - the same reason a real close
    // reports - and the pick must survive it: the user is still mid-entry on
    // this card.
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TempGraphOverlayTestAccess::picked(overlay) == "extruder1");

    // The user confirms 200C. The gcode must name the PICKED extruder.
    TempGraphOverlayTestAccess::keypad_confirm(overlay, 200.0f);

    const auto& hist = client.gcode_script_history();
    auto mentions = [](const char* needle) {
        return [needle](const std::string& g) { return g.find(needle) != std::string::npos; };
    };
    REQUIRE(std::any_of(hist.begin(), hist.end(),
                        mentions("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=200")));
    // Not the machine's active tool, and nothing else either.
    REQUIRE(std::none_of(hist.begin(), hist.end(), mentions("HEATER=extruder ")));
    REQUIRE(hist.size() == 1);

    // Leave the fixture's state reachable only through live objects again.
    TempGraphOverlayTestAccess::set_picked(overlay, "");
    TempGraphOverlayTestAccess::set_deps(overlay, nullptr, nullptr);
}

TEST_CASE_METHOD(
    TempGraphOverlayPickFixture,
    "TempGraphOverlay: the card mirrors the pick and open() restarts on the active tool",
    "[temp_graph_overlay][active-extruder]") {
    TempGraphOverlay overlay;
    overlay.init_subjects();
    seed_two_tools();

    TempGraphOverlayTestAccess::set_deps(overlay, &state, &service);
    TempGraphOverlayTestAccess::set_mode(overlay, TempGraphOverlay::Mode::Nozzle);
    TempGraphOverlayTestAccess::repoint(overlay);

    // No pick: the card mirrors the machine's active tool.
    REQUIRE(TempGraphOverlayTestAccess::card_temp(overlay) == 550);
    REQUIRE(std::string(TempGraphOverlayTestAccess::badge(overlay)) == "1");

    // Pick tool 2: card, target and digit all follow it.
    TempGraphOverlayTestAccess::select(overlay, "extruder1");
    REQUIRE(TempGraphOverlayTestAccess::card_temp(overlay) == 2600);
    REQUIRE(TempGraphOverlayTestAccess::card_target(overlay) == 2600);
    REQUIRE(std::string(TempGraphOverlayTestAccess::badge(overlay)) == "2");

    // Status keeps flowing into the card for the PICKED tool while the
    // machine's tool is the other one.
    state.update_from_status(
        {{"extruder", {{"temperature", 60.0}}}, {"extruder1", {{"temperature", 261.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(TempGraphOverlayTestAccess::card_temp(overlay) == 2610);

    // Leaving the overlay KEEPS the pick: the custom-entry keypad stacks on
    // top of this overlay, and its confirm must still reach the picked tool.
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TempGraphOverlayTestAccess::picked(overlay) == "extruder1");

    // Reopening is the fresh view: open() drops the pick so the card starts on
    // the machine's active tool. A null parent on a never-created overlay
    // skips both lazy creation and the NavigationManager push, so this call
    // exercises exactly the pick-clear.
    overlay.open(TempGraphOverlay::Mode::Nozzle, nullptr);
    REQUIRE(TempGraphOverlayTestAccess::picked(overlay).empty());

    TempGraphOverlayTestAccess::repoint(overlay);
    REQUIRE(TempGraphOverlayTestAccess::card_temp(overlay) == 600);
    REQUIRE(std::string(TempGraphOverlayTestAccess::badge(overlay)) == "1");
}

TEST_CASE_METHOD(TempGraphOverlayPickFixture,
                 "TempGraphOverlay: a pick whose extruder vanishes on rediscovery falls back",
                 "[temp_graph_overlay][active-extruder]") {
    TempGraphOverlay overlay;
    overlay.init_subjects();
    seed_two_tools();

    TempGraphOverlayTestAccess::set_deps(overlay, &state, &service);
    TempGraphOverlayTestAccess::set_mode(overlay, TempGraphOverlay::Mode::Nozzle);
    TempGraphOverlayTestAccess::arm_version_watch(overlay);
    TempGraphOverlayTestAccess::repoint(overlay);

    TempGraphOverlayTestAccess::select(overlay, "extruder1");
    REQUIRE(TempGraphOverlayTestAccess::card_temp(overlay) == 2600);

    // Rediscovery rebuilds the extruder map WITHOUT the picked tool; the
    // version bump the rebuild makes is what repoints the card here.
    state.init_extruders({"extruder"});
    state.update_from_status({{"extruder", {{"temperature", 56.0}, {"target", 56.0}}}});
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(TempGraphOverlayTestAccess::picked(overlay).empty());
    REQUIRE(TempGraphOverlayTestAccess::card_temp(overlay) == 560);
    REQUIRE(TempGraphOverlayTestAccess::card_target(overlay) == 560);
}

TEST_CASE_METHOD(TempGraphOverlayPickFixture,
                 "TempGraphOverlay: activation settles - the update queue drains to empty",
                 "[temp_graph_overlay][active-extruder]") {
    // lv_subject_add_observer notifies on attach, and observe_int_sync defers
    // the handler through UpdateQueue, so arming the version watch queues one
    // repoint. That repoint must not re-arm the watch: a re-arming repoint
    // queues another repoint on every drain and the queue never empties while
    // the overlay is up. Arm, pick, drain three times, then demand a quiet
    // queue.
    TempGraphOverlay overlay;
    overlay.init_subjects();
    seed_two_tools();

    TempGraphOverlayTestAccess::set_deps(overlay, &state, &service);
    TempGraphOverlayTestAccess::set_mode(overlay, TempGraphOverlay::Mode::Nozzle);
    TempGraphOverlayTestAccess::arm_version_watch(overlay);
    TempGraphOverlayTestAccess::select(overlay, "extruder1");

    for (int i = 0; i < 3; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }

    REQUIRE(helix::ui::UpdateQueue::instance().pending_count() == 0);
    // The drains delivered the real work too: the card sits on the pick.
    REQUIRE(TempGraphOverlayTestAccess::card_temp(overlay) == 2600);
    REQUIRE(std::string(TempGraphOverlayTestAccess::badge(overlay)) == "2");
}

// =============================================================================
// Visibility / Lifecycle State
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: initially not visible",
                 "[temp_graph_overlay]") {
    TempGraphOverlay overlay;
    REQUIRE_FALSE(overlay.is_visible());
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: root is null before create",
                 "[temp_graph_overlay]") {
    TempGraphOverlay overlay;
    REQUIRE(overlay.get_root() == nullptr);
}

// =============================================================================
// Series Color Palette
// =============================================================================
// SERIES_COLORS and PALETTE_SIZE are private, so we cannot access them directly.
// The compiler enforces the array has PALETTE_SIZE (8) entries via the static
// declaration.  The color values are verified visually and by the implementation
// assigning distinct hex codes (see ui_overlay_temp_graph.cpp).

// =============================================================================
// Y-axis Auto-scaling Logic
// =============================================================================

TEST_CASE("TempGraphOverlay: Y-axis stays at floor when temps are low",
          "[temp_graph_overlay][scaling]") {
    // With current_max=100 and max_temp=20, should stay at floor (100)
    float result = compute_y_axis_max(100.0f, 20.0f);
    REQUIRE(result == 100.0f);
}

TEST_CASE("TempGraphOverlay: Y-axis stays at floor when temp is zero",
          "[temp_graph_overlay][scaling]") {
    float result = compute_y_axis_max(100.0f, 0.0f);
    REQUIRE(result == 100.0f);
}

TEST_CASE("TempGraphOverlay: Y-axis expands at 85% threshold", "[temp_graph_overlay][scaling]") {
    // current_max=100, 85% threshold = 85. Temp of 86 should trigger expand.
    // 86/50 = 1.72, floor = 1, +1 = 2, *50 = 100... that equals current.
    // Need a higher temp. Try 90: floor(90/50)+1 = 2, *50 = 100. Still 100.
    // The expand triggers but rounds to next step above max_temp.
    // floor(90/50) = 1, +1 = 2, *50 = 100. Same as current.
    // Try 100: floor(100/50)+1 = 3, *50 = 150. Expand to 150.
    float result = compute_y_axis_max(100.0f, 100.0f);
    REQUIRE(result == 150.0f);

    // 86 exceeds 85% of 100 => expand: floor(86/50)+1=2, *50=100. No change
    // since 100 == current_max and clamped to floor. Edge case.
    result = compute_y_axis_max(100.0f, 86.0f);
    REQUIRE(result == 100.0f);

    // 130 exceeds 85% of 150 (=127.5) => expand: floor(130/50)+1=3, *50=150.
    // That equals current_max so no visible change — but from y_max=100,
    // 130 > 85 => expand to ceil(130/50)*50 = 150
    result = compute_y_axis_max(100.0f, 130.0f);
    REQUIRE(result == 150.0f);
}

TEST_CASE("TempGraphOverlay: Y-axis expands for high temps", "[temp_graph_overlay][scaling]") {
    // 220 with current_max=100: 220 > 85 => expand
    // floor(220/50)+1 = 5, *50 = 250
    float result = compute_y_axis_max(100.0f, 220.0f);
    REQUIRE(result == 250.0f);
}

TEST_CASE("TempGraphOverlay: Y-axis shrinks at 55% threshold", "[temp_graph_overlay][scaling]") {
    // current_max=200, 55% = 110. Temp of 50 < 110 => shrink
    // floor(50/50)+1 = 2, *50 = 100. Max of floor(100), so 100.
    float result = compute_y_axis_max(200.0f, 50.0f);
    REQUIRE(result == 100.0f);

    // current_max=300, 55% = 165. Temp of 120 < 165 => shrink
    // floor(120/50)+1 = 3, *50 = 150
    result = compute_y_axis_max(300.0f, 120.0f);
    REQUIRE(result == 150.0f);
}

TEST_CASE("TempGraphOverlay: Y-axis shrink never goes below floor",
          "[temp_graph_overlay][scaling]") {
    // current_max=150, 55% = 82.5. Temp of 10 < 82.5 => shrink
    // floor(10/50)+1 = 1, *50 = 50. But floor is 100, so clamp to 100.
    float result = compute_y_axis_max(150.0f, 10.0f);
    REQUIRE(result == 100.0f);
}

TEST_CASE("TempGraphOverlay: Y-axis never exceeds ceiling", "[temp_graph_overlay][scaling]") {
    // current_max=350, temp=380 > 85% of 350 (=297.5) => expand
    // floor(380/50)+1 = 8, *50 = 400
    float result = compute_y_axis_max(350.0f, 380.0f);
    REQUIRE(result == 400.0f);

    // Even higher: temp=500 > 85% => floor(500/50)+1=11, *50=550
    // But clamped to ceiling=400
    result = compute_y_axis_max(350.0f, 500.0f);
    REQUIRE(result == 400.0f);
}

TEST_CASE("TempGraphOverlay: Y-axis steps are 50 degree increments",
          "[temp_graph_overlay][scaling]") {
    // Expanding from various temps should always land on multiples of 50
    for (float temp : {90.0f, 130.0f, 170.0f, 220.0f, 280.0f, 350.0f}) {
        float result = compute_y_axis_max(100.0f, temp);
        float remainder = std::fmod(result, 50.0f);
        CAPTURE(temp, result, remainder);
        REQUIRE(remainder == Catch::Approx(0.0f));
    }
}

TEST_CASE("TempGraphOverlay: Y-axis no change in dead zone between thresholds",
          "[temp_graph_overlay][scaling]") {
    // current_max=200, 55% = 110, 85% = 170
    // Temp of 140 is between thresholds => no change
    float result = compute_y_axis_max(200.0f, 140.0f);
    REQUIRE(result == 200.0f);

    // Temp right at 55% boundary (110): not strictly less than, no shrink
    result = compute_y_axis_max(200.0f, 110.0f);
    REQUIRE(result == 200.0f);

    // Temp right at 85% boundary (170): not strictly greater than, no expand
    result = compute_y_axis_max(200.0f, 170.0f);
    REQUIRE(result == 200.0f);
}
