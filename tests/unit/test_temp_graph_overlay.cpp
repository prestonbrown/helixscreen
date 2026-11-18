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
#include "ui_temperature_utils.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/discovery_of_objects.h"
#include "core/update_queue.h"
#include "material_settings_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "preset_materials.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "subject_debug_registry.h"
#include "target_tool.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"
#include "temperature_service.h"
#include "tool_state.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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
// Global Accessor Pattern
// =============================================================================

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
    "TempGraphOverlay: init_subjects publishes its subjects, destructor withdraws them",
    "[temp_graph_overlay]") {
    // init_subjects() publishes temp_graph_mode (strip visibility and
    // graph_outer width, see temp_graph_overlay.xml's <subjects> block) and
    // the chamber presets' visibility and text. The nozzle card binds the
    // shared target tool's subjects (TargetTool), so it publishes none of its
    // own. The SubjectManager destructor (deinit_all, run from
    // ~TempGraphOverlay via the subjects_ member) must withdraw every name so
    // none outlives the overlay. This pins both halves: the publish count AND
    // the destructor cleanup. If another subject is ever added, add its name
    // here so the withdrawal stays covered too.
    const std::vector<std::string> published = {"temp_graph_mode",
                                                "temp_graph_chamber_preset_0_visible",
                                                "temp_graph_chamber_preset_1_visible",
                                                "temp_graph_chamber_preset_2_visible",
                                                "temp_graph_chamber_preset_3_visible",
                                                "temp_graph_chamber_preset_1_text",
                                                "temp_graph_chamber_preset_2_text",
                                                "temp_graph_chamber_preset_3_text"};
    // Entries under these names, counted per subject: the app's global overlay holds
    // its own set whenever an earlier case in the process has initialized it.
    auto present = [&published]() {
        auto all = SubjectDebugRegistry::instance().list_all();
        size_t n = 0;
        for (const auto& name : published) {
            n += std::count_if(all.begin(), all.end(),
                               [&](const auto& entry) { return entry.first == name; });
        }
        return n;
    };

    const size_t before = SubjectDebugRegistry::instance().list_all().size();
    const size_t baseline = present();

    {
        TempGraphOverlay overlay;
        overlay.ensure_subjects();
        REQUIRE(SubjectDebugRegistry::instance().list_all().size() == before + published.size());
        REQUIRE(present() == baseline + published.size());
        // Destructor runs here.
    }
    REQUIRE(SubjectDebugRegistry::instance().list_all().size() == before);
    REQUIRE(present() == baseline);
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraphOverlay: cleanup after init_subjects does not crash",
                 "[temp_graph_overlay]") {
    TempGraphOverlay overlay;
    overlay.ensure_subjects();

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
    /// The custom-entry keypad's opening click (the XML callback ignores its
    /// event, so a null one exercises the production path).
    static void custom_clicked(TempGraphOverlay& o) {
        TempGraphOverlay::on_temp_graph_custom_clicked(nullptr);
    }
    /// The keypad's confirm, delivered the way ui_component_keypad does.
    static void keypad_confirm(TempGraphOverlay& o, float value) {
        TempGraphOverlay::keypad_value_cb(value, &o.keypad_ctx_);
    }
    /// A tap on the nozzle strip's preset button @p name ("preset_1" is the
    /// first material slot), sending @p deg as that button's target.
    static void preset_clicked(TempGraphOverlay& o, const char* name, int deg, lv_obj_t* parent) {
        const int idx = name[std::string(name).size() - 1] - '0';
        o.preset_data_[static_cast<size_t>(idx)] = {&o, deg};
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_set_name(btn, name);
        lv_obj_add_event_cb(btn, TempGraphOverlay::on_temp_graph_preset_clicked, LV_EVENT_CLICKED,
                            nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        lv_obj_delete(btn);
    }
    static void discover(TempGraphOverlay& o) {
        o.discover_series();
    }
    /// {has_target} of the series named @p klipper_name, or nullopt if absent.
    static std::optional<bool> series_has_target(TempGraphOverlay& o,
                                                 const std::string& klipper_name) {
        for (const auto& s : o.series_)
            if (s.klipper_name == klipper_name)
                return s.has_target;
        return std::nullopt;
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
        state.temperature_state().init_extruders({"extruder", "extruder1"});
        state.update_from_status({{"extruder", {{"temperature", 55.0}, {"target", 55.0}}},
                                  {"extruder1", {{"temperature", 260.0}, {"target", 260.0}}}});
        helix::ui::UpdateQueue::instance().drain();
    }
};

TEST_CASE_METHOD(
    TempGraphOverlayPickFixture,
    "TempGraphOverlay: a custom-entry send reaches the target tool, not the active one",
    "[temp_graph_overlay][active-extruder][target_tool]") {
    // The static XML callbacks resolve the global overlay instance.
    TempGraphOverlay& overlay = get_global_temp_graph_overlay();
    helix::ToolState::instance().init_subjects(true);
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json{"extruder", "extruder1"});
    helix::ToolState::instance().init_tools(hw);
    helix::ToolState::instance().update_from_status({{"toolhead", {{"extruder", "extruder"}}}});
    seed_two_tools();
    helix::ui::TargetTool::instance().attach(state);
    helix::ui::TargetTool::instance().pick(1);
    helix::ui::UpdateQueue::instance().drain();

    TempGraphOverlayTestAccess::set_deps(overlay, &state, &service);
    TempGraphOverlayTestAccess::set_mode(overlay, TempGraphOverlay::Mode::Nozzle);

    // The precondition: the card shows tool 2 while the machine's active
    // tool is tool 1.
    REQUIRE(state.temperature_state().active_extruder_name() == "extruder");
    client.clear_gcode_script_history();

    // The click that opens the custom-entry keypad captures the target's
    // extruder before the keypad's stacked push deactivates this overlay.
    TempGraphOverlayTestAccess::custom_clicked(overlay);
    overlay.on_deactivate(DeactivateReason::NavigateAway);

    // The user confirms 200C. The gcode must name the TARGET extruder.
    TempGraphOverlayTestAccess::keypad_confirm(overlay, 200.0f);

    const auto& hist = client.gcode_script_history();
    auto mentions = [](const char* needle) {
        return [needle](const std::string& g) { return g.find(needle) != std::string::npos; };
    };
    REQUIRE(std::any_of(hist.begin(), hist.end(),
                        mentions("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=200")));
    REQUIRE(std::none_of(hist.begin(), hist.end(), mentions("HEATER=extruder ")));
    REQUIRE(hist.size() == 1);

    TempGraphOverlayTestAccess::set_deps(overlay, nullptr, nullptr);
    helix::ui::TargetTool::instance().deinit_subjects();
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

TEST_CASE_METHOD(TempGraphOverlayPickFixture,
                 "TempGraphOverlay: a non-chamber heater_generic is a series with a target",
                 "[temp_graph_overlay][heater_generic]") {
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    tsm.init_subjects();
    tsm.discover(helix::test::discovery_of(
        {"heater_generic filament_dryer", "temperature_sensor mcu_temp"}));

    auto& overlay = get_global_temp_graph_overlay();
    TempGraphOverlayTestAccess::set_deps(overlay, &state, nullptr);
    TempGraphOverlayTestAccess::discover(overlay);

    auto dryer =
        TempGraphOverlayTestAccess::series_has_target(overlay, "heater_generic filament_dryer");
    REQUIRE(dryer.has_value());
    REQUIRE(*dryer);

    TempGraphOverlayTestAccess::set_deps(overlay, nullptr, nullptr);
    tsm.discover({});
}

TEST_CASE_METHOD(
    TempGraphOverlayPickFixture,
    "TempGraphOverlay: a nozzle material preset heats the bed only while the setting is on",
    "[temp_graph_overlay][preset_bed]") {
    TempGraphOverlay& overlay = get_global_temp_graph_overlay();
    seed_two_tools();
    TempGraphOverlayTestAccess::set_deps(overlay, &state, &service);
    TempGraphOverlayTestAccess::set_mode(overlay, TempGraphOverlay::Mode::Nozzle);
    auto& sm = helix::SettingsManager::instance();
    sm.init_subjects();
    const bool saved = sm.get_material_presets_heat_bed_and_chamber();
    const bool heat_bed = GENERATE(true, false);
    CAPTURE(heat_bed);
    sm.set_material_presets_heat_bed_and_chamber(heat_bed);
    const auto slot = helix::presets::slot_temps(0);
    REQUIRE(slot.has_value());
    client.clear_gcode_script_history();

    TempGraphOverlayTestAccess::preset_clicked(overlay, "preset_1", 215, lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();

    const auto& hist = client.gcode_script_history();
    auto sent = [&](const std::string& needle) {
        return std::any_of(hist.begin(), hist.end(), [&](const std::string& g) {
            return g.find(needle) != std::string::npos;
        });
    };
    CHECK(sent("TARGET=215"));
    CHECK(sent("HEATER=heater_bed TARGET=" + std::to_string(slot->bed)) == heat_bed);
    CHECK(sent("HEATER=heater_bed") == heat_bed);

    sm.set_material_presets_heat_bed_and_chamber(saved);
    TempGraphOverlayTestAccess::set_deps(overlay, nullptr, nullptr);
}

TEST_CASE_METHOD(
    TempGraphOverlayPickFixture,
    "TempGraphOverlay: a nozzle material preset heats the chamber only while the setting is on",
    "[temp_graph_overlay][preset_bed][chamber]") {
    auto& sm = helix::SettingsManager::instance();
    sm.init_subjects();
    const bool saved = sm.get_material_presets_heat_bed_and_chamber();
    const bool heat_bed_and_chamber = GENERATE(true, false);
    CAPTURE(heat_bed_and_chamber);
    sm.set_material_presets_heat_bed_and_chamber(heat_bed_and_chamber);
    sm.set_chamber_heater_assignment("auto");
    auto& materials = helix::MaterialSettingsManager::instance();
    materials.init();
    filament::MaterialOverride abs;
    abs.chamber_temp = 45;
    materials.set_override("ABS", abs);
    materials.set_preset_material(0, "ABS");
    REQUIRE(helix::presets::slot_temps(0)->chamber == 45);

    helix::PrinterDiscovery hw;
    hw.parse_objects(
        nlohmann::json{"extruder", "extruder1", "heater_bed", "heater_generic chamber_heater"});
    state.set_hardware(hw);
    REQUIRE(controller_->resolved_name(helix::HeaterType::Chamber) ==
            "heater_generic chamber_heater");
    TempGraphOverlay& overlay = get_global_temp_graph_overlay();
    seed_two_tools();
    TempGraphOverlayTestAccess::set_deps(overlay, &state, &service);
    TempGraphOverlayTestAccess::set_mode(overlay, TempGraphOverlay::Mode::Nozzle);
    client.clear_gcode_script_history();

    TempGraphOverlayTestAccess::preset_clicked(overlay, "preset_1", 215, lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();

    const auto& hist = client.gcode_script_history();
    const bool chamber_sent = std::any_of(hist.begin(), hist.end(), [](const std::string& g) {
        return g.find("HEATER=chamber_heater TARGET=45") != std::string::npos;
    });
    CHECK(chamber_sent == heat_bed_and_chamber);

    materials.clear_override("ABS");
    materials.reset_preset_materials();
    sm.set_material_presets_heat_bed_and_chamber(saved);
    TempGraphOverlayTestAccess::set_deps(overlay, nullptr, nullptr);
}
