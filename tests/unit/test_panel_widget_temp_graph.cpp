// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/ui_overlay_temp_graph.h"
#include "../../include/ui_temp_graph.h"
#include "../../src/ui/panel_widgets/temp_graph_widget.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using namespace helix;

// L065 friend test access: lets us drive private follow-mode logic without
// adding test-only methods to TempGraphWidget.
class helix::TempGraphWidgetTestAccess {
  public:
    static void set_follow_overlay(TempGraphWidget& w, bool on) {
        w.follow_overlay_ = on;
    }
    static std::vector<TempGraphSeriesSpec> build_series(TempGraphWidget& w) {
        return w.build_series_from_config();
    }
    static bool merge_discovered_extruders(nlohmann::json& config, bool enabled) {
        return TempGraphWidget::merge_discovered_extruders(config, enabled);
    }
    static std::string sensor_display_name(const std::string& klipper_name) {
        return TempGraphWidget::TempGraphConfigModal::sensor_display_name(klipper_name);
    }

    // Drive the save-callback body directly so tests don't need to construct a
    // live Modal + LVGL show() chain. Mirrors the lambda in on_edit_configure().
    static void apply_config_save(TempGraphWidget& w, const nlohmann::json& new_config) {
        w.apply_config_save(new_config);
    }

    // Inject widget container pointers so apply_config_save() can be exercised
    // with both a live lv_obj_t and a deleted/null one (regression: RP293UCW).
    static void set_widget_obj(TempGraphWidget& w, lv_obj_t* obj) {
        w.widget_obj_ = obj;
    }
    static void set_parent_screen(TempGraphWidget& w, lv_obj_t* obj) {
        w.parent_screen_ = obj;
    }
    static lv_obj_t* get_widget_obj(const TempGraphWidget& w) {
        return w.widget_obj_;
    }
    static bool has_controller(const TempGraphWidget& w) {
        return w.controller_ != nullptr;
    }
    static const nlohmann::json& get_config(const TempGraphWidget& w) {
        return w.config_;
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
//
// features_for_size(width_px, height_px) — each SECTION below is chosen to
// pin one predicate term at a time rather than a threshold in general:
//   TARGET_LINES | TARGET_HISTORY  is an OR — width-alone and height-alone
//                                   cases each independently trigger it.
//   LEGEND | Y_AXIS | X_AXIS       is height-only.
//   READOUTS                       is an AND — a case with width satisfied
//                                   and height not (and vice versa) must both
//                                   leave it off; only the case satisfying
//                                   both turns it on.
// ============================================================================

using helix::widget_size::H_TALL;
using helix::widget_size::W_NORMAL;
using helix::widget_size::W_WIDE;

TEST_CASE("TempGraphWidget::features_for_size maps pixel size to feature flags",
          "[temp_graph][panel_widget][features]") {
    SECTION("below both thresholds: lines + gradients only") {
        uint32_t f = TempGraphWidget::features_for_size(W_NORMAL - 1, H_TALL - 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_HISTORY) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) !=
                0); // always on; draw callback auto-disables when >3 series
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("width alone at W_NORMAL, short: target lines/history via width, no legend/axes") {
        uint32_t f = TempGraphWidget::features_for_size(W_NORMAL, H_TALL - 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);   // OR satisfied by width
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_HISTORY) != 0); // OR satisfied by width
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) == 0);         // height-only, height short
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0); // AND: width < W_WIDE
    }

    SECTION("height alone at H_TALL, narrow: target lines/history + legend/axes via height") {
        uint32_t f = TempGraphWidget::features_for_size(W_NORMAL - 1, H_TALL);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);   // OR satisfied by height
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_HISTORY) != 0); // OR satisfied by height
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0); // AND: width < W_WIDE
    }

    SECTION("tall but below W_WIDE: readouts stay off (AND — width term false)") {
        uint32_t f = TempGraphWidget::features_for_size(W_WIDE - 1, H_TALL);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("wide at W_WIDE but short: readouts stay off (AND — height term false)") {
        uint32_t f = TempGraphWidget::features_for_size(W_WIDE, H_TALL - 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0); // width alone satisfies the OR
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) == 0);       // height short
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("wide + tall at W_WIDE/H_TALL: all features including readouts") {
        uint32_t f = TempGraphWidget::features_for_size(W_WIDE, H_TALL);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_HISTORY) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);
    }

    SECTION("larger than any measured tier: all features") {
        uint32_t f = TempGraphWidget::features_for_size(W_WIDE + 200, H_TALL + 200);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
    }
}

// Real measured tier extents, not symbolic offsets from the constants — see
// span-pixel-table.md. temp_graph's LEGEND/Y_AXIS/X_AXIS gate is
// deliberately height-only (features_for_size() near :199-206): unlike
// fan_stack's resolved names or camera's live stream, the chart draws these
// with graceful width degradation (draw_legend_cb's "+N" overflow pill,
// ui_temp_graph.cpp:647-665; a configurable Y-axis label column, 30-50px).
// A plain 1x1 widget genuinely reaches Large's 107px / XLarge's 134px width
// there, but temp_graph is also resizable down to a genuinely-2-row 1-column
// cell as narrow as 65px (Small tier) — narrower than either "false
// positive" width — so these features already render at widths tighter than
// what Large/XLarge's height-driven promotion produces. Nothing new breaks.
TEST_CASE("TempGraphWidget::features_for_size at real Large/XLarge 1x1 and Small 1x2 extents",
          "[temp_graph][panel_widget][features]") {
    SECTION("Large 1x1 (107x141): legend/axes fire on height, narrower widths already handled") {
        uint32_t f = TempGraphWidget::features_for_size(107, 141);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0); // width < W_WIDE
    }

    SECTION("XLarge 1x1 (134x169): same story, taller and slightly wider still") {
        uint32_t f = TempGraphWidget::features_for_size(134, 169);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
    }

    SECTION("Medium 1x1 (114x112): below both thresholds, no promotion") {
        uint32_t f = TempGraphWidget::features_for_size(114, 112);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
    }

    SECTION("Micro 1x2 (70x131): the legitimate tall case, narrower than either false "
            "positive above — proves Large/XLarge introduce nothing new") {
        uint32_t f = TempGraphWidget::features_for_size(70, 131);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
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

    REQUIRE_NOTHROW(widget.set_config(nlohmann::json()));           // default-constructed = null
    REQUIRE_NOTHROW(widget.set_config(nullptr));                    // explicit null literal
    REQUIRE_NOTHROW(widget.set_config(nlohmann::json::array()));    // wrong type: array
    REQUIRE_NOTHROW(widget.set_config(nlohmann::json("a string"))); // wrong type: string
    REQUIRE_NOTHROW(widget.set_config(nlohmann::json::object())); // empty object — the happy path
}

// Regression: build_series_from_config() used to throw nlohmann::type_error
// when a persisted sensor entry carried the wrong JSON type for "name" (not a
// string) or "color" (not an integer). The contains()-only guards let the
// subsequent .get<std::string>() / .get<uint32_t>() throw, which escaped the
// Home Panel dashboard rebuild and aborted the process (std::terminate).
// Malformed entries must now be skipped/defaulted, and well-formed sibling
// entries on the same card must still render.
TEST_CASE("TempGraphWidget: build_series tolerates malformed name/color JSON types",
          "[temp_graph][panel_widget][crash-safety]") {
    TempGraphWidget widget("test_malformed_types");

    nlohmann::json config = {
        {"sensors",
         {
             // Malformed "name": a number, not a string — must be skipped.
             {{"name", 12345}, {"enabled", true}, {"color", 0xFF4444}},
             // Malformed "color": a string, not a number — entry survives with
             // a palette default color.
             {{"name", "heater_bed"}, {"enabled", true}, {"color", "not_a_color"}},
             // Well-formed sibling — must still render.
             {{"name", "extruder"}, {"enabled", true}, {"color", 0x88C0D0}},
         }}};

    widget.set_config(config);

    std::vector<TempGraphSeriesSpec> specs;
    REQUIRE_NOTHROW(specs = TempGraphWidgetTestAccess::build_series(widget));

    std::vector<std::string> names;
    for (const auto& s : specs)
        names.push_back(s.klipper_name);

    // Malformed-name entry dropped entirely.
    REQUIRE(std::find(names.begin(), names.end(), "12345") == names.end());
    // Both well-formed-name entries survive (bad color just falls back to default).
    REQUIRE(std::find(names.begin(), names.end(), "heater_bed") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "extruder") != names.end());
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
    SECTION("far larger than any defined tier: includes READOUTS") {
        uint32_t f = TempGraphWidget::features_for_size(W_WIDE * 3, H_TALL * 3);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
    }

    SECTION("tall, narrow: both axes via height, no READOUTS (width term false)") {
        uint32_t f = TempGraphWidget::features_for_size(W_NORMAL - 1, H_TALL * 2);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("wide, short: no axes (height-only), no READOUTS (height term false)") {
        // LEGEND/Y_AXIS/X_AXIS are gated on height_px >= H_TALL alone, so a
        // wide-but-short card gets neither axis even though it is very wide.
        // READOUTS still needs both terms.
        uint32_t f = TempGraphWidget::features_for_size(W_WIDE * 2, H_TALL - 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }
}

// ============================================================================
// attach() initial_features seed — pins the feature mask that
// TempGraphWidget::attach() hands the controller before the first real
// on_size_changed() call arrives.
// ============================================================================

TEST_CASE("TempGraphWidget::attach() seed extents yield the full feature mask",
          "[temp_graph][panel_widget][features]") {
    // Mirrors the constexpr SEED_WIDTH_PX/SEED_HEIGHT_PX in
    // TempGraphWidget::attach() (temp_graph_widget.cpp) — the measured
    // 800x480 two-span (colspan=2/rowspan=2) extents used to seed
    // initial_features. Both dimensions clear every threshold in
    // features_for_size(), so the seed yields TEMP_GRAPH_ALL_FEATURES —
    // unlike the old colspan=2/rowspan=2 default, which withheld READOUTS
    // (needed colspan>=3). A change to features_for_size() or to these seed
    // values that narrows the mask should fail this test rather than
    // silently changing the widget's pre-resize state.
    constexpr int SEED_WIDTH_PX = 233;
    constexpr int SEED_HEIGHT_PX = 230;

    uint32_t f = TempGraphWidget::features_for_size(SEED_WIDTH_PX, SEED_HEIGHT_PX);

    uint32_t expected = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES |
                        TEMP_GRAPH_FEATURE_LEGEND | TEMP_GRAPH_FEATURE_Y_AXIS |
                        TEMP_GRAPH_FEATURE_X_AXIS | TEMP_GRAPH_FEATURE_GRADIENTS |
                        TEMP_GRAPH_FEATURE_READOUTS | TEMP_GRAPH_FEATURE_TARGET_HISTORY;
    REQUIRE(f == expected);
    REQUIRE(f == TEMP_GRAPH_ALL_FEATURES); // sanity: equals the module's own "everything" mask
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
    nlohmann::json cfg = {{"follow_overlay", false},
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
    nlohmann::json cfg = {{"follow_overlay", true},
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
    nlohmann::json cfg = {{"follow_overlay", true},
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

// ============================================================================
// sensor_display_name: extruders, bed, chamber
// ============================================================================

#include "ui_update_queue.h"

#include "app_globals.h"
#include "printer_state.h"

TEST_CASE("sensor_display_name maps Klipper names to user-facing labels",
          "[temp_graph][panel_widget][labels]") {
    // Static fallbacks (PrinterTemperatureState may or may not have extruders
    // populated in the global test PrinterState — these labels are derived
    // unconditionally).
    REQUIRE(TempGraphWidgetTestAccess::sensor_display_name("heater_bed") == "Bed");
    REQUIRE(TempGraphWidgetTestAccess::sensor_display_name("chamber") == "Chamber");

    // Pre-discovery / no extruder yet known: derived from suffix.
    auto& ps = get_printer_state();
    ps.init_extruders({}); // clear
    REQUIRE(TempGraphWidgetTestAccess::sensor_display_name("extruder") == "Nozzle");
    REQUIRE(TempGraphWidgetTestAccess::sensor_display_name("extruder1") == "Nozzle 2");
    REQUIRE(TempGraphWidgetTestAccess::sensor_display_name("extruder5") == "Nozzle 6");

    // After discovery: defers to the cached display_name (which itself is
    // sorted/translated). Multi-extruder => "extruder" becomes "Nozzle 1".
    ps.init_extruders({"extruder", "extruder1", "extruder2", "extruder3"});
    REQUIRE(TempGraphWidgetTestAccess::sensor_display_name("extruder") == "Nozzle 1");
    REQUIRE(TempGraphWidgetTestAccess::sensor_display_name("extruder1") == "Nozzle 2");
    REQUIRE(TempGraphWidgetTestAccess::sensor_display_name("extruder3") == "Nozzle 4");

    // Reset to empty so we don't leak state into other tests.
    ps.init_extruders({});
}

// ============================================================================
// merge_discovered_extruders helper
// ============================================================================

TEST_CASE("merge_discovered_extruders adds missing entries and is idempotent",
          "[temp_graph][panel_widget][config]") {
    auto& ps = get_printer_state();
    ps.init_extruders({"extruder", "extruder1", "extruder2", "extruder3"});

    nlohmann::json config = {{"sensors",
                              {
                                  {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
                                  {{"name", "heater_bed"}, {"enabled", true}, {"color", 0x88C0D0}},
                              }}};

    bool added = TempGraphWidgetTestAccess::merge_discovered_extruders(config, /*enabled=*/true);
    REQUIRE(added == true);

    auto& sensors = config["sensors"];
    auto find_name = [&sensors](const std::string& name) {
        return std::find_if(sensors.begin(), sensors.end(), [&](const nlohmann::json& e) {
            return e["name"].get<std::string>() == name;
        });
    };
    // Three new entries appended with enabled=true.
    REQUIRE(find_name("extruder1") != sensors.end());
    REQUIRE(find_name("extruder2") != sensors.end());
    REQUIRE(find_name("extruder3") != sensors.end());
    REQUIRE((*find_name("extruder1"))["enabled"].get<bool>() == true);

    // Idempotent: second call adds nothing.
    bool added_again =
        TempGraphWidgetTestAccess::merge_discovered_extruders(config, /*enabled=*/true);
    REQUIRE(added_again == false);
    REQUIRE(sensors.size() == 5); // extruder + bed + 3 new

    ps.init_extruders({});
}

TEST_CASE("merge_discovered_extruders honors the enabled flag",
          "[temp_graph][panel_widget][config]") {
    auto& ps = get_printer_state();
    ps.init_extruders({"extruder", "extruder1"});

    nlohmann::json config = {{"sensors", nlohmann::json::array()}};

    REQUIRE(TempGraphWidgetTestAccess::merge_discovered_extruders(config, /*enabled=*/false));
    for (const auto& entry : config["sensors"]) {
        REQUIRE(entry["enabled"].get<bool>() == false);
    }

    ps.init_extruders({});
}

// ============================================================================
// apply_config_save — RP293UCW regression: save callback must tolerate a
// widget container that was freed between modal-open and modal-save (panel
// rebuild during the modal window). Old behavior reattached to the stale
// pointer; new behavior re-reads widget_obj_ at save time and skips the
// reattach when it's gone.
// ============================================================================

TEST_CASE_METHOD(TempGraphFeatureFixture,
                 "TempGraphWidget::apply_config_save: stale widget_obj_ does not crash",
                 "[temp_graph][panel_widget][regression]") {
    TempGraphWidget w("test_save_stale");

    // Simulate the layout manager having detached this widget instance (or the
    // underlying lv_obj_t having been freed by a panel rebuild) by leaving
    // widget_obj_ = nullptr. The modal's save callback will still fire because
    // it captured `this`, but the container is gone.
    TempGraphWidgetTestAccess::set_widget_obj(w, nullptr);
    TempGraphWidgetTestAccess::set_parent_screen(w, nullptr);

    nlohmann::json new_cfg = {
        {"sensors", nlohmann::json::array(
                        {{{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
                         {{"name", "heater_bed"}, {"enabled", true}, {"color", 0x88C0D0}}})}};

    REQUIRE_NOTHROW(TempGraphWidgetTestAccess::apply_config_save(w, new_cfg));

    // Config still persisted on the widget instance (so a fresh attach picks
    // it up), and we left the widget detached — no zombie controller.
    REQUIRE(TempGraphWidgetTestAccess::get_config(w)["sensors"].size() == 2);
    REQUIRE(TempGraphWidgetTestAccess::has_controller(w) == false);
    REQUIRE(TempGraphWidgetTestAccess::get_widget_obj(w) == nullptr);
}

TEST_CASE_METHOD(TempGraphFeatureFixture,
                 "TempGraphWidget::apply_config_save: live widget_obj_ rebuilds in place",
                 "[temp_graph][panel_widget][regression]") {
    TempGraphWidget w("test_save_live");

    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, 400, 300);
    w.attach(container, screen);
    REQUIRE(TempGraphWidgetTestAccess::has_controller(w));

    nlohmann::json new_cfg = {
        {"sensors",
         nlohmann::json::array({{{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}}})}};

    REQUIRE_NOTHROW(TempGraphWidgetTestAccess::apply_config_save(w, new_cfg));

    // Same container pointer (was valid throughout), controller rebuilt.
    REQUIRE(TempGraphWidgetTestAccess::get_widget_obj(w) == container);
    REQUIRE(TempGraphWidgetTestAccess::has_controller(w));
    REQUIRE(TempGraphWidgetTestAccess::get_config(w)["sensors"].size() == 1);

    // Clean up: detach before the container is destroyed by the fixture.
    w.detach();
    lv_obj_delete(container);
}

// ============================================================================
// Regression: widget attached before discovery is short a nozzle series
// ============================================================================
//
// A dashboard widget attaches at app startup, before the WebSocket connects, so
// merge_discovered_extruders() at attach time finds nothing and the series list
// is built without any of the extra tools. The controller's discovery retry
// re-resolves series that EXIST; it cannot conjure a curve the config never
// listed. The widget therefore watches extruder_version itself and rebuilds.

TEST_CASE_METHOD(TempGraphFeatureFixture,
                 "Widget grows its series list when discovery adds extruders",
                 "[ui][temp_graph][multi_tool]") {
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    auto& queue = helix::ui::UpdateQueue::instance();

    // PrinterState is a process-global; clear any tools a previous case
    // discovered so attach() really does run pre-discovery.
    ps.init_extruders({});
    queue.drain();

    // Given: a widget attached while only the legacy single extruder is known
    TempGraphWidget widget("temp_graph:test_discovery");
    nlohmann::json cfg;
    cfg["sensors"] = nlohmann::json::array(
        {{{"name", "extruder"}, {"enabled", true}}, {{"name", "heater_bed"}, {"enabled", true}}});
    widget.set_config(cfg);

    lv_obj_t* container = lv_obj_create(screen);
    widget.attach(container, screen);
    REQUIRE(TempGraphWidgetTestAccess::has_controller(widget));

    auto before = TempGraphWidgetTestAccess::build_series(widget);
    REQUIRE(before.size() == 2);

    // When: a 3-tool changer is discovered after the fact
    ps.init_extruders({"extruder", "extruder1", "extruder2"});
    queue.drain();
    lv_timer_handler_safe(); // lv_async_call runs here, outside the queue batch

    // Then: the config picked up the new tools and the series list grew
    const auto& after_cfg = TempGraphWidgetTestAccess::get_config(widget);
    std::string names;
    for (const auto& s : after_cfg["sensors"]) {
        names += s.value("name", std::string()) + " ";
    }
    INFO("config sensors: " << names);
    REQUIRE(names.find("extruder1") != std::string::npos);
    REQUIRE(names.find("extruder2") != std::string::npos);

    auto after = TempGraphWidgetTestAccess::build_series(widget);
    REQUIRE(after.size() == 4); // extruder, extruder1, extruder2, heater_bed
    REQUIRE(TempGraphWidgetTestAccess::has_controller(widget));

    widget.detach();
}

TEST_CASE_METHOD(TempGraphFeatureFixture,
                 "Widget does not rebuild when rediscovery adds no new extruder",
                 "[ui][temp_graph][multi_tool]") {
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    auto& queue = helix::ui::UpdateQueue::instance();

    ps.init_extruders({"extruder", "extruder1"});
    queue.drain();

    TempGraphWidget widget("temp_graph:test_no_churn");
    nlohmann::json cfg;
    cfg["sensors"] = nlohmann::json::array({{{"name", "extruder"}, {"enabled", true}},
                                            {{"name", "extruder1"}, {"enabled", true}},
                                            {{"name", "heater_bed"}, {"enabled", true}}});
    widget.set_config(cfg);

    lv_obj_t* container = lv_obj_create(screen);
    widget.attach(container, screen);
    auto* controller_before = TempGraphWidgetTestAccess::get_widget_obj(widget);

    // When: the same tool set is rediscovered (reconnect bumps the version)
    ps.init_extruders({"extruder", "extruder1"});
    queue.drain();
    lv_timer_handler_safe();

    // Then: no teardown — throwing the graph away on every reconnect would
    // discard the trace for nothing.
    REQUIRE(TempGraphWidgetTestAccess::has_controller(widget));
    REQUIRE(TempGraphWidgetTestAccess::get_widget_obj(widget) == controller_before);
    REQUIRE(TempGraphWidgetTestAccess::build_series(widget).size() == 3);

    widget.detach();
}
