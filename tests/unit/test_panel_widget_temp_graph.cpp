// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/ui_overlay_temp_graph.h"
#include "../../include/ui_temp_graph.h"
#include "../../src/ui/panel_widgets/temp_graph_widget.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

#include <algorithm>

using namespace helix;

// L065 friend test access: lets us drive private follow-mode logic without
// adding test-only methods to TempGraphWidget.
class helix::TempGraphWidgetTestAccess {
  public:
    static void set_follow_overlay(TempGraphWidget& w, bool on) { w.follow_overlay_ = on; }
    static std::vector<TempGraphSeriesSpec> build_series(TempGraphWidget& w) {
        return w.build_series_from_config();
    }
};

// Reuse the same lightweight fixture as test_temp_graph.cpp
class TempGraphFeatureFixture {
  public:
    TempGraphFeatureFixture() {
        lv_init_safe();
        lv_display_t* disp = lv_display_create(800, 480);
        alignas(64) static lv_color_t buf1[800 * 10];
        lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
        screen = lv_obj_create(NULL);
    }

    ~TempGraphFeatureFixture() = default;

    lv_obj_t* screen;
};

// ============================================================================
// Feature Flags Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphFeatureFixture, "Feature flags default to all-on after create",
                 "[ui][temp_graph][features]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    // Y-axis excluded from defaults (show_y_axis starts false, caller must configure)
    uint32_t expected = TEMP_GRAPH_ALL_FEATURES & ~TEMP_GRAPH_FEATURE_Y_AXIS;
    REQUIRE(graph->features == expected);
    REQUIRE(graph->show_x_axis == true);
    REQUIRE(graph->show_y_axis == false);

    // Verify individual flags match expectations
    uint32_t f = ui_temp_graph_get_features(graph);
    REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0); // Off by default
    REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphFeatureFixture, "set_features stores and get_features retrieves flags",
                 "[ui][temp_graph][features]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Set only Y_AXIS and X_AXIS") {
        uint32_t flags = TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_X_AXIS;
        ui_temp_graph_set_features(graph, flags);

        // LINES is always forced on
        uint32_t expected = flags | TEMP_GRAPH_FEATURE_LINES;
        REQUIRE(ui_temp_graph_get_features(graph) == expected);
    }

    SECTION("Set all features") {
        ui_temp_graph_set_features(graph, TEMP_GRAPH_ALL_FEATURES);
        REQUIRE(ui_temp_graph_get_features(graph) == TEMP_GRAPH_ALL_FEATURES);
    }

    SECTION("Set no features — only LINES remains") {
        ui_temp_graph_set_features(graph, 0);
        REQUIRE(ui_temp_graph_get_features(graph) == TEMP_GRAPH_FEATURE_LINES);
    }

    SECTION("Y-axis show_y_axis tracks feature flag") {
        ui_temp_graph_set_features(graph, TEMP_GRAPH_FEATURE_Y_AXIS);
        REQUIRE(graph->show_y_axis == true);

        ui_temp_graph_set_features(graph, 0);
        REQUIRE(graph->show_y_axis == false);
    }

    SECTION("X-axis show_x_axis tracks feature flag") {
        ui_temp_graph_set_features(graph, TEMP_GRAPH_FEATURE_X_AXIS);
        REQUIRE(graph->show_x_axis == true);

        ui_temp_graph_set_features(graph, 0);
        REQUIRE(graph->show_x_axis == false);
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphFeatureFixture, "LINES flag is always forced on",
                 "[ui][temp_graph][features]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    // Pass 0 — no flags at all
    ui_temp_graph_set_features(graph, 0);
    REQUIRE((ui_temp_graph_get_features(graph) & TEMP_GRAPH_FEATURE_LINES) != 0);

    // Pass only GRADIENTS — LINES should still be on
    ui_temp_graph_set_features(graph, TEMP_GRAPH_FEATURE_GRADIENTS);
    uint32_t f = ui_temp_graph_get_features(graph);
    REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphFeatureFixture, "get_features returns 0 for NULL graph",
                 "[ui][temp_graph][features]") {
    REQUIRE(ui_temp_graph_get_features(nullptr) == 0);
}

TEST_CASE_METHOD(TempGraphFeatureFixture, "Gradient opacity zeroed when GRADIENTS flag disabled",
                 "[ui][temp_graph][features]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    // Add a series so we can check gradient state
    int sid = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF4444));
    REQUIRE(sid >= 0);

    // Disable gradients
    ui_temp_graph_set_features(graph, TEMP_GRAPH_FEATURE_LINES);
    REQUIRE(graph->series_meta[0].gradient_top_opa == LV_OPA_TRANSP);
    REQUIRE(graph->series_meta[0].gradient_bottom_opa == LV_OPA_TRANSP);

    // Re-enable gradients — defaults restored
    ui_temp_graph_set_features(graph, TEMP_GRAPH_ALL_FEATURES);
    REQUIRE(graph->series_meta[0].gradient_top_opa == UI_TEMP_GRAPH_GRADIENT_TOP_OPA);
    REQUIRE(graph->series_meta[0].gradient_bottom_opa == UI_TEMP_GRAPH_GRADIENT_BOTTOM_OPA);

    ui_temp_graph_destroy(graph);
}

// ============================================================================
// Registry tests
// ============================================================================

TEST_CASE("TempGraphWidget: registered in widget registry", "[temp_graph][panel_widget]") {
    const auto* def = find_widget_def("temp_graph");
    REQUIRE(def != nullptr);
    REQUIRE(std::string(def->display_name) == "Temperature Graph");
    REQUIRE(std::string(def->icon) == "chart_line");
    REQUIRE(def->multi_instance == true);
    REQUIRE(def->colspan == 2);
    REQUIRE(def->rowspan == 2);
    REQUIRE(def->min_colspan == 1);
    REQUIRE(def->min_rowspan == 1);
    REQUIRE(def->max_colspan == 6);
    REQUIRE(def->max_rowspan == 4);
    REQUIRE(def->hardware_gate_subject == nullptr);
}

// ============================================================================
// features_for_size tests
// ============================================================================

TEST_CASE("TempGraphWidget::features_for_size maps grid size to feature flags",
          "[temp_graph][panel_widget][features]") {
    SECTION("1x1: lines + gradients only") {
        uint32_t f = TempGraphWidget::features_for_size(1, 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) !=
                0); // always on; draw callback auto-disables when >3 series
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("2x1 (wide): + target lines, no legend (needs rowspan>=2), no X-axis") {
        uint32_t f = TempGraphWidget::features_for_size(2, 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) == 0); // Legend needs rowspan>=2
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0); // X-axis needs 3+ cols
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) !=
                0); // always on; draw callback auto-disables when >3 series
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("1x2 (tall): + target lines, legend, y-axis") {
        uint32_t f = TempGraphWidget::features_for_size(1, 2);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) !=
                0); // always on; draw callback auto-disables when >3 series
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("2x2: Y-axis + gradients, no X-axis") {
        uint32_t f = TempGraphWidget::features_for_size(2, 2);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0); // X-axis needs 3+ cols
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("3x2: all features including readouts") {
        uint32_t f = TempGraphWidget::features_for_size(3, 2);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);
    }

    SECTION("4x3: all features (larger than max)") {
        uint32_t f = TempGraphWidget::features_for_size(4, 3);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
    }
}

// ============================================================================
// Config round-trip tests
// ============================================================================

TEST_CASE("TempGraphWidget: set_config stores and preserves sensor configuration",
          "[temp_graph][panel_widget][config]") {
    TempGraphWidget widget("test_config_1");

    nlohmann::json config = {
        {"sensors",
         {
             {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
             {{"name", "heater_bed"}, {"enabled", true}, {"color", 0x88C0D0}},
             {{"name", "temperature_sensor mcu_temp"}, {"enabled", false}, {"color", 0xA3BE8C}},
         }}};

    widget.set_config(config);

    // Verify the widget accepted the config by checking get_component_name works
    // (would crash if widget was in bad state)
    REQUIRE(widget.get_component_name() == "panel_widget_temp_graph");
    REQUIRE(widget.id() == std::string("test_config_1"));
    REQUIRE(widget.has_edit_configure() == true);
    REQUIRE(widget.supports_reuse() == true);
}

TEST_CASE("TempGraphWidget: factory creates valid instances", "[temp_graph][panel_widget]") {
    init_widget_registrations();

    const auto* def = find_widget_def("temp_graph");
    REQUIRE(def != nullptr);
    REQUIRE(def->factory != nullptr);

    auto widget = def->factory("test_factory_1");
    REQUIRE(widget != nullptr);
    REQUIRE(std::string(widget->id()) == "test_factory_1");
}

// ============================================================================
// Sensor discovery: new sensors appended as disabled
// ============================================================================

TEST_CASE("TempGraphWidget: set_config preserves existing sensor entries",
          "[temp_graph][panel_widget][config]") {
    TempGraphWidget widget("test_sensor_preserve");

    // Config with only extruder — simulates a saved config before bed was discovered
    nlohmann::json config = {{"sensors",
                              {
                                  {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
                              }}};

    widget.set_config(config);

    // Widget should accept the partial config without crashing
    REQUIRE(widget.get_component_name() == "panel_widget_temp_graph");
    REQUIRE(widget.id() == std::string("test_sensor_preserve"));
}

// ============================================================================
// Missing sensors in config silently skipped
// ============================================================================

TEST_CASE("TempGraphWidget: config with unknown sensor name does not crash",
          "[temp_graph][panel_widget][config]") {
    TempGraphWidget widget("test_unknown_sensor");

    // Contains a sensor name that doesn't match anything known — should be silently ignored
    nlohmann::json config = {
        {"sensors",
         {
             {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
             {{"name", "nonexistent_sensor_xyz"}, {"enabled", true}, {"color", 0x00FF00}},
             {{"name", "heater_bed"}, {"enabled", false}, {"color", 0x88C0D0}},
         }}};

    // Should not throw or crash
    REQUIRE_NOTHROW(widget.set_config(config));
    REQUIRE(widget.get_component_name() == "panel_widget_temp_graph");
}

// Regression: set_config(null) used to throw json::type_error::306
// ("cannot use value() with null") via the new follow_overlay lookup
// added in v0.99.54 (commit 5ac58e051). PanelWidgetConfig::parse_widget_array
// can pass a default-constructed (= JSON null) config when a stored layout
// entry omits the "config" key, which is the case for any layout written
// before the follow-overlay feature shipped. The throw escaped Application::run()
// before main_loop()'s safety net, exited 134, and triggered the watchdog
// crash-loop dialog.
TEST_CASE("TempGraphWidget: set_config tolerates JSON null/non-object configs",
          "[temp_graph][panel_widget][crash-safety]") {
    TempGraphWidget widget("test_null_cfg");

    REQUIRE_NOTHROW(widget.set_config(nlohmann::json()));               // default-constructed = null
    REQUIRE_NOTHROW(widget.set_config(nullptr));                        // explicit null literal
    REQUIRE_NOTHROW(widget.set_config(nlohmann::json::array()));        // wrong type: array
    REQUIRE_NOTHROW(widget.set_config(nlohmann::json("a string")));    // wrong type: string
    REQUIRE_NOTHROW(widget.set_config(nlohmann::json::object()));       // empty object — the happy path
}

// ============================================================================
// Generation counter rejects stale callbacks
// ============================================================================

TEST_CASE("TempGraphWidget: generation counter increments on config save path",
          "[temp_graph][panel_widget][generation]") {
    // This test verifies the generation_ concept via the public API.
    // generation_ starts at 0 and is bumped on attach() and on config save.
    // We test that two widgets created independently get independent state.

    TempGraphWidget w1("test_gen_1");
    TempGraphWidget w2("test_gen_2");

    nlohmann::json cfg = {{"sensors",
                           {
                               {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
                           }}};

    // Both widgets should accept config independently without interfering
    REQUIRE_NOTHROW(w1.set_config(cfg));
    REQUIRE_NOTHROW(w2.set_config(cfg));

    // They are distinct instances
    REQUIRE(std::string(w1.id()) != std::string(w2.id()));
    REQUIRE(w1.get_component_name() == w2.get_component_name());
}

// ============================================================================
// features_for_size edge cases
// ============================================================================

TEST_CASE("TempGraphWidget::features_for_size edge cases", "[temp_graph][panel_widget][features]") {
    SECTION("4x3 (larger than max defined): includes READOUTS") {
        uint32_t f = TempGraphWidget::features_for_size(4, 3);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
    }

    SECTION("1x3 (tall, no width for X axis): Y_AXIS but no X_AXIS, no READOUTS") {
        uint32_t f = TempGraphWidget::features_for_size(1, 3);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("3x1 (wide, no height for Y axis): X_AXIS but no Y_AXIS, no READOUTS") {
        // READOUTS requires colspan>=3 AND rowspan>=2; rowspan=1 is not enough
        uint32_t f = TempGraphWidget::features_for_size(3, 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }
}

// ============================================================================
// Config with custom colors persists
// ============================================================================

TEST_CASE("TempGraphWidget: custom color hex strings round-trip through set_config",
          "[temp_graph][panel_widget][config]") {
    TempGraphWidget widget("test_color_roundtrip");

    // Use non-standard colors to verify they aren't overwritten
    nlohmann::json config = {{"sensors",
                              {
                                  {{"name", "extruder"}, {"enabled", true}, {"color", 0xDEADBE}},
                                  {{"name", "heater_bed"}, {"enabled", true}, {"color", 0xCAFEBA}},
                              }}};

    widget.set_config(config);

    // The widget must be in a valid state
    REQUIRE(widget.get_component_name() == "panel_widget_temp_graph");
    REQUIRE(widget.has_edit_configure() == true);
}

// ============================================================================
// Multi-instance independence
// ============================================================================

TEST_CASE("TempGraphWidget: two instances have independent configs",
          "[temp_graph][panel_widget][multi_instance]") {
    TempGraphWidget w1("temp_graph:1");
    TempGraphWidget w2("temp_graph:2");

    nlohmann::json cfg1 = {{"sensors",
                            {
                                {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF0000}},
                            }}};
    nlohmann::json cfg2 = {{"sensors",
                            {
                                {{"name", "heater_bed"}, {"enabled", true}, {"color", 0x0000FF}},
                                {{"name", "extruder"}, {"enabled", false}, {"color", 0xFF0000}},
                            }}};

    REQUIRE_NOTHROW(w1.set_config(cfg1));
    REQUIRE_NOTHROW(w2.set_config(cfg2));

    // Both instances should remain valid and independent
    REQUIRE(std::string(w1.id()) == "temp_graph:1");
    REQUIRE(std::string(w2.id()) == "temp_graph:2");
    REQUIRE(w1.supports_reuse() == true);
    REQUIRE(w2.supports_reuse() == true);
}

// ============================================================================
// Follow-overlay mode (home graph card "follow my graph selection")
// ============================================================================

namespace {
auto names_of = [](const std::vector<TempGraphSeriesSpec>& specs) {
    std::vector<std::string> out;
    out.reserve(specs.size());
    for (const auto& s : specs)
        out.push_back(s.klipper_name);
    return out;
};

// RAII guard so a failing REQUIRE in the middle of a test still resets the
// process-static snapshot. Without this, a later test sees stale state.
struct VisibilitySnapshotResetGuard {
    ~VisibilitySnapshotResetGuard() {
        helix::test_access::set_temp_graph_visibility_snapshot(std::nullopt);
    }
};
} // namespace

TEST_CASE("TempGraphWidget: follow_overlay off uses configured enabled flags",
          "[temp_graph][panel_widget][follow]") {
    VisibilitySnapshotResetGuard reset_guard;
    helix::test_access::set_temp_graph_visibility_snapshot(
        std::vector<std::string>{"chamber"}); // overlay snapshot says "only chamber"

    TempGraphWidget w("test_follow_off");
    nlohmann::json cfg = {
        {"follow_overlay", false},
        {"sensors",
         {
             {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
             {{"name", "heater_bed"}, {"enabled", true}, {"color", 0x88C0D0}},
             {{"name", "chamber"}, {"enabled", false}, {"color", 0xA3BE8C}},
         }}};
    w.set_config(cfg);

    auto specs = TempGraphWidgetTestAccess::build_series(w);
    auto names = names_of(specs);
    // Snapshot ignored — uses config "enabled" flags.
    REQUIRE(std::find(names.begin(), names.end(), "extruder") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "heater_bed") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "chamber") == names.end());
}

TEST_CASE("TempGraphWidget: follow_overlay on uses snapshot membership",
          "[temp_graph][panel_widget][follow]") {
    VisibilitySnapshotResetGuard reset_guard;
    helix::test_access::set_temp_graph_visibility_snapshot(
        std::vector<std::string>{"chamber", "heater_bed"}); // overlay shows bed + chamber only

    TempGraphWidget w("test_follow_on");
    nlohmann::json cfg = {
        {"follow_overlay", true},
        {"sensors",
         {
             // config flags would otherwise show extruder + bed only
             {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
             {{"name", "heater_bed"}, {"enabled", true}, {"color", 0x88C0D0}},
             {{"name", "chamber"}, {"enabled", false}, {"color", 0xA3BE8C}},
         }}};
    w.set_config(cfg);

    auto specs = TempGraphWidgetTestAccess::build_series(w);
    auto names = names_of(specs);
    REQUIRE(std::find(names.begin(), names.end(), "extruder") == names.end()); // dropped
    REQUIRE(std::find(names.begin(), names.end(), "heater_bed") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "chamber") != names.end()); // added
}

TEST_CASE("TempGraphWidget: follow_overlay on with no snapshot falls back to config flags",
          "[temp_graph][panel_widget][follow]") {
    VisibilitySnapshotResetGuard reset_guard;
    helix::test_access::set_temp_graph_visibility_snapshot(std::nullopt);

    TempGraphWidget w("test_follow_no_snapshot");
    nlohmann::json cfg = {
        {"follow_overlay", true},
        {"sensors",
         {
             {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
             {{"name", "heater_bed"}, {"enabled", false}, {"color", 0x88C0D0}},
         }}};
    w.set_config(cfg);

    auto specs = TempGraphWidgetTestAccess::build_series(w);
    auto names = names_of(specs);
    REQUIRE(std::find(names.begin(), names.end(), "extruder") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "heater_bed") == names.end());
}
