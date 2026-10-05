// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_nav_manager.h"
#include "ui_panel_home.h"

#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "display_settings_manager.h"
#include "panel_widget_registry.h"
#include "plugin_canvas.h"
#include "plugin_host.h"
#include "plugin_xml_policy.h"

#include <cmath>
#include <fstream>
#include <iterator>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

/// App subjects (the heater fields the plugin watches) plus a seeded
/// NavigationManager, so one fixture covers the subject cases and the overlay case.
class TempSparkFx : public XMLTestFixture {
  public:
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    lv_obj_t* tile = nullptr;

    TempSparkFx() {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
    }
    ~TempSparkFx() override {
        if (tile)
            lv_obj_delete(tile);
        drain();
        process_lvgl(100); // deferred overlay deletes
        DisplaySettingsManager::instance().set_animations_enabled(true);
    }

    /// The home tile at its 2x1 size, laid out and drained: the spark canvas
    /// reports its content size and the plugin redraws for it.
    lv_obj_t* make_tile() {
        tile =
            static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "temp-spark__tile", nullptr));
        REQUIRE(tile);
        lv_obj_set_size(tile, 400, 200);
        lv_obj_update_layout(tile);
        drain();
        return tile;
    }
};

/// The enabled block plus a 1 s sample interval, so a timer tick is one wait_ms away.
json spark_block() {
    json block = enabled("temp-spark", {});
    block["settings"]["temp-spark"] = {
        {"heater", "extruder"}, {"interval_s", 1}, {"show_target", true}};
    return block;
}

/// A store payload with one heater whose temperatures count from `from`.
json store_series(const std::string& heater, double from) {
    std::vector<double> temps;
    for (int i = 0; i < 30; ++i)
        temps.push_back(from + i);
    return json{{heater, {{"temperatures", temps}}}};
}

/// The committed spark polyline's points, oldest first; empty when the spark
/// canvas holds no polyline. The list also carries the fill rects and the dot.
std::vector<lv_point_precise_t> spark_points() {
    const DisplayList* list = canvas_committed("temp-spark__spark");
    if (!list)
        return {};
    for (const CanvasPrim& p : list->prims)
        if (p.op == CanvasOp::Polyline)
            return {list->points.begin() + p.first, list->points.begin() + p.first + p.count};
    return {};
}

/// How many prims of one op a committed canvas holds.
size_t count_op(const char* canvas, CanvasOp op) {
    const DisplayList* list = canvas_committed(canvas);
    size_t n = 0;
    if (list)
        for (const CanvasPrim& p : list->prims)
            if (p.op == op)
                ++n;
    return n;
}

/// The first Line stroked in `token` at height y (both ends equal: every line
/// the plugin draws is horizontal); null when none matches.
const CanvasPrim* hline_at(const char* canvas, const char* token, double y) {
    const DisplayList* list = canvas_committed(canvas);
    if (!list)
        return nullptr;
    for (const CanvasPrim& p : list->prims)
        if (p.op == CanvasOp::Line && p.color < list->tokens.size() &&
            list->tokens[p.color] == token && std::fabs(p.b - y) < 0.5 && std::fabs(p.d - y) < 0.5)
            return &p;
    return nullptr;
}

/// Whether any Line in `token` exists at all.
bool has_line_in(const char* canvas, const char* token) {
    const DisplayList* list = canvas_committed(canvas);
    if (!list)
        return false;
    for (const CanvasPrim& p : list->prims)
        if (p.op == CanvasOp::Line && p.color < list->tokens.size() &&
            list->tokens[p.color] == token)
            return true;
    return false;
}

std::string text_subject(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}

void set_deci(const char* subject, int deci) {
    lv_subject_set_int(lv_xml_get_subject(nullptr, subject), deci);
}

/// Answers the call requests since `next`, in order, with `value`.
void answer_calls(HostRig& rig, size_t& next, const json& value) {
    while (next < rig.fake.requests.size()) {
        auto& r = rig.fake.requests[next++];
        if (r.kind == "call")
            r.reply(RpcResult{true, value, {}});
    }
    drain();
}

} // namespace

TEST_CASE_METHOD(TempSparkFx, "temp-spark loads with a registered 2x1 widget",
                 "[plugin][example]") {
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.info("temp-spark"));
    CHECK(rig.info("temp-spark")->status == PluginStatus::Loaded);

    const helix::PanelWidgetDef* def = helix::find_widget_def("temp-spark__tile");
    REQUIRE(def);
    CHECK(def->category == helix::WidgetCategory::Plugins);
    CHECK(def->colspan == 4); // two cells, in tracks
    CHECK(def->rowspan == 2);
    rig.host->disable("temp-spark");
    CHECK(helix::find_widget_def("temp-spark__tile") == nullptr);

    // Loading already ran the policy; one explicit pass per UI file keeps that
    // contract named.
    for (const char* file : {"temp-spark__tile", "temp-spark__detail"}) {
        const std::string path = std::string("examples/plugins/temp-spark/ui/") + file + ".xml";
        CAPTURE(path);
        std::ifstream in(path);
        REQUIRE(in);
        std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(check_plugin_xml("temp-spark", {"temp-spark__tile", "temp-spark__detail"}, xml) ==
              std::string());
    }
}

TEST_CASE_METHOD(TempSparkFx, "temp-spark backfills the window from the store",
                 "[plugin][example]") {
    set_deci("extruder_target", 2000);
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    make_tile(); // the spark canvas knows its size before the window arrives
    REQUIRE(rig.fake.requests.size() == 1);
    CHECK(rig.fake.requests[0].a == "server.temperature_store");
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    const auto [w, h] = canvas_size("temp-spark__spark");
    REQUIRE(w > 1);
    REQUIRE(h > 1);
    const std::vector<lv_point_precise_t> pts = spark_points();
    REQUIRE(pts.size() == 30);
    // The tile's range is the window's own: 25..54 is span 29 centered on
    // 39.5 and padded 20%, 22.1 to 56.9. The 200 target is not folded in; the
    // tile draws no target line, so folding it would flatten the data.
    const double lo = 39.5 - 29 * 0.6, hi = 39.5 + 29 * 0.6;
    // A full window spans the whole width: 25 degrees at x 0, 54 at xmax,
    // which keeps the 2px dot clear of the canvas's right edge.
    CHECK(pts[0].x == Approx(0.0).margin(0.01));
    CHECK(pts[0].y == Approx((h - 1) * (1 - (25.0 - lo) / (hi - lo))).margin(0.01));
    CHECK(pts[29].x == Approx(w - 4).margin(0.01));
    CHECK(pts[29].y == Approx((h - 1) * (1 - (54.0 - lo) / (hi - lo))).margin(0.01));
    // The area fill rides on the polyline itself, so no Rect columns; the one
    // dot marks the newest sample.
    CHECK(count_op("temp-spark__spark", CanvasOp::Rect) == 0);
    CHECK(count_op("temp-spark__spark", CanvasOp::Circle) == 1);
    const DisplayList* spark = canvas_committed("temp-spark__spark");
    REQUIRE(spark);
    for (const CanvasPrim& p : spark->prims)
        if (p.op == CanvasOp::Polyline) {
            CHECK(spark->tokens[p.border] == "primary"); // the fill token
            CHECK(p.fill_opa == 38);                     // FILL_OPA 15 percent, 0-255 scale
            CHECK(p.a == h - 1);                         // baseline at the plot's floor
        }
    CHECK(text_subject("temp-spark__value") == "54");
    CHECK(text_subject("temp-spark__target_beside") == "/ 200");
    CHECK(text_subject("temp-spark__min_text") == "25°");
    CHECK(text_subject("temp-spark__max_text") == "54°");
    CHECK(text_subject("temp-spark__avg_text") == "40°"); // mean of 25..54
    CHECK(text_subject("temp-spark__target_text") == "200°");
    CHECK(text_subject("temp-spark__window_text") == "last 30 s");
    // A whole-minute window reads as an integer count, never "1.0 min".
    REQUIRE(rig.host->set_setting("temp-spark", "interval_s", 2));
    process_lvgl(2000);
    CHECK(text_subject("temp-spark__window_text") == "last 1 min");
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "temp-spark__show_target")) == 1);
}

TEST_CASE_METHOD(TempSparkFx, "a live reading shifts the window on the timer",
                 "[plugin][example]") {
    set_deci("extruder_target", 2000);
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    make_tile();
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    set_deci("extruder_temp", 600);                   // the watch records 60.0 for the next tick
    CHECK(text_subject("temp-spark__value") == "54"); // sampling waits for the timer
    // helix.timer.every is a chain of one-shot LVGL timers, which
    // lv_timer_handler_safe fires deterministically on the virtual clock.
    process_lvgl(1200);

    // Window 26..54, 60: the oldest point rose, the newest sits at xmax.
    // The tile's own range: 26..60 is span 34 centered on 43, padded to
    // 22.6..63.4 (no target folding on the tile).
    const auto [w, h] = canvas_size("temp-spark__spark");
    const std::vector<lv_point_precise_t> pts = spark_points();
    REQUIRE(pts.size() == 30);
    const double lo = 43.0 - 34 * 0.6, hi = 43.0 + 34 * 0.6;
    CHECK(pts[0].x == Approx(0.0).margin(0.01));
    CHECK(pts[0].y == Approx((h - 1) * (1 - (26.0 - lo) / (hi - lo))).margin(0.01));
    CHECK(pts[29].x == Approx(w - 4).margin(0.01));
    CHECK(pts[29].y == Approx((h - 1) * (1 - (60.0 - lo) / (hi - lo))).margin(0.01));
    CHECK(text_subject("temp-spark__value") == "60");
    CHECK(text_subject("temp-spark__min_text") == "26°"); // the 25 left the window
}

TEST_CASE_METHOD(TempSparkFx, "a failed backfill leaves the plugin loaded and empty",
                 "[plugin][example]") {
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    make_tile();
    REQUIRE(rig.fake.requests.size() == 1);
    rig.fake.requests[0].reply(RpcResult{false, {}, "store unavailable"});
    drain();

    CHECK(rig.info("temp-spark")->status == PluginStatus::Loaded);
    const DisplayList* spark = canvas_committed("temp-spark__spark");
    CHECK((spark == nullptr || spark->prims.empty()));
    CHECK(text_subject("temp-spark__value") == "--");

    // Live sampling fills the window: two ticks give a two-point line that
    // already spans the full width, oldest at 0, newest at xmax.
    set_deci("extruder_temp", 300);
    process_lvgl(1200);
    set_deci("extruder_temp", 310);
    process_lvgl(1200);
    const auto [w, h] = canvas_size("temp-spark__spark");
    REQUIRE(w > 1);
    REQUIRE(h > 1);
    const std::vector<lv_point_precise_t> pts = spark_points();
    REQUIRE(pts.size() == 2);
    CHECK(pts[0].x == Approx(0.0).margin(0.01));
    CHECK(pts[1].x == Approx(w - 4).margin(0.01));
    // A 1-degree window (30, 31) still gets the 10-degree minimum span,
    // centered on 30.5: 24.5 to 36.5.
    const double lo = 30.5 - 10 * 0.6, hi = 30.5 + 10 * 0.6;
    CHECK(pts[0].y == Approx((h - 1) * (1 - (30.0 - lo) / (hi - lo))).margin(0.01));
    CHECK(pts[1].y == Approx((h - 1) * (1 - (31.0 - lo) / (hi - lo))).margin(0.01));
}

TEST_CASE_METHOD(TempSparkFx, "switching heaters refetches for the new store key",
                 "[plugin][example]") {
    set_deci("extruder_target", 2000);
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    make_tile();
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    set_deci("bed_target", 600); // 60.0: inside the window span, so it widens nothing
    REQUIRE(rig.host->set_setting("temp-spark", "heater", "heater_bed"));
    REQUIRE(rig.fake.requests.size() == 2);
    answer_calls(rig, next, store_series("heater_bed", 40.0));

    const auto [w, h] = canvas_size("temp-spark__spark");
    const std::vector<lv_point_precise_t> pts = spark_points();
    REQUIRE(pts.size() == 30);
    // Range: 40..69 is span 29 centered on 54.5, padded to 37.1..71.9.
    const double lo = 54.5 - 29 * 0.6, hi = 54.5 + 29 * 0.6;
    CHECK(pts[0].y == Approx((h - 1) * (1 - (40.0 - lo) / (hi - lo))).margin(0.01));
    CHECK(pts[29].x == Approx(w - 4).margin(0.01));
    CHECK(pts[29].y == Approx((h - 1) * (1 - (69.0 - lo) / (hi - lo))).margin(0.01));
    CHECK(text_subject("temp-spark__value") == "69");
    CHECK(text_subject("temp-spark__avg_text") == "55°"); // mean of 40..69
    CHECK(text_subject("temp-spark__target_text") == "60°");
}

TEST_CASE_METHOD(TempSparkFx, "the detail graph draws gridlines and the target",
                 "[plugin][example]") {
    // 210.0: off the 50-degree grid ladder, so the target trace and the
    // gridlines are distinguishable lines.
    set_deci("extruder_target", 2100);
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    rig.host->dispatch_event("temp-spark__open");
    drain();
    lv_obj_t* graph = lv_obj_find_by_name(lv_screen_active(), "temp-spark__graph");
    REQUIRE(graph);
    lv_obj_update_layout(graph);
    drain();

    const auto [w, h] = canvas_size("temp-spark__graph");
    REQUIRE(w > 1);
    REQUIRE(h > 1);
    // Range: window 25..54 widened for the 210 target, span 185 centered on
    // 117.5, padded to 6.5..228.5.
    const double lo = 117.5 - 185 * 0.6, hi = 117.5 + 185 * 0.6;
    CHECK(count_op("temp-spark__graph", CanvasOp::Polyline) == 1);
    CHECK(count_op("temp-spark__graph", CanvasOp::Rect) == 0); // the fill rides on the polyline
    CHECK(count_op("temp-spark__graph", CanvasOp::Circle) == 1);
    // Gridlines every 50 degrees: 50, 100, 150, 200, each with a degree label;
    // the target trace is a second line of dashes at 210 plus a Target label.
    for (double v = 50; v <= 200; v += 50) {
        const double y = (h - 1) * (1 - (v - lo) / (hi - lo));
        CAPTURE(v);
        REQUIRE(hline_at("temp-spark__graph", "border", y));
    }
    const double target_y = (h - 1) * (1 - (210.0 - lo) / (hi - lo));
    REQUIRE(hline_at("temp-spark__graph", "text_muted", target_y));
    CHECK(count_op("temp-spark__graph", CanvasOp::Text) == 5);
    // Degree labels read as whole numbers and the Target label sits on the
    // left, above the dashed line, clear of the newest dot at the right edge.
    const DisplayList* list = canvas_committed("temp-spark__graph");
    REQUIRE(list);
    bool seen_50 = false, seen_200 = false, seen_target_beside = false;
    for (const CanvasPrim& p : list->prims) {
        if (p.op != CanvasOp::Text)
            continue;
        const std::string s = list->text.substr(p.first, p.count);
        seen_50 |= s == "50°";
        seen_200 |= s == "200°";
        seen_target_beside |= s == "Target" && p.a <= 60;
    }
    CHECK(seen_50);
    CHECK(seen_200);
    CHECK(seen_target_beside);

    // show_target off removes the trace and keeps the window.
    REQUIRE(rig.host->set_setting("temp-spark", "show_target", false));
    drain();
    CHECK_FALSE(has_line_in("temp-spark__graph", "text_muted"));
    CHECK(count_op("temp-spark__graph", CanvasOp::Polyline) == 1);
    CHECK(text_subject("temp-spark__target_beside") == "");

    // Back on, the trace returns: its absence above was the setting.
    REQUIRE(rig.host->set_setting("temp-spark", "show_target", true));
    drain();
    CHECK(has_line_in("temp-spark__graph", "text_muted"));
    CHECK(text_subject("temp-spark__target_beside") == "/ 210");

    // A target of 0 is a heater that is off: no trace on the bottom edge.
    set_deci("extruder_target", 0);
    process_lvgl(1200); // the tick re-reads the target and redraws
    CHECK_FALSE(has_line_in("temp-spark__graph", "text_muted"));
    CHECK(count_op("temp-spark__graph", CanvasOp::Polyline) == 1);
}

TEST_CASE_METHOD(TempSparkFx, "the tile event opens the detail overlay until unload",
                 "[plugin][example]") {
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));
    // The detail view extends overlay_panel, whose header is a header_bar; an
    // unregistered dependency silently vanishes from the tree in tests.
    REQUIRE(register_component("header_bar"));

    rig.host->dispatch_event("temp-spark__open");
    drain();
    CHECK(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("temp-spark") == 1);

    // The overlay's title arrives as the creation attr overlay() passes; a
    // header's title_subject retitles only on change, and the heater label
    // never changes after load. The header uppercases what it shows.
    lv_obj_t* title = lv_obj_find_by_name(lv_screen_active(), "header_title");
    REQUIRE(title);
    CHECK(std::string(lv_label_get_text(title)) == "EXTRUDER");

    rig.host->disable("temp-spark");
    drain(); // close_all queues go_back; its body runs on the next queue pass
    process_lvgl(500);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("temp-spark") == 0);
}

#endif // HELIX_HAS_PLUGINS
