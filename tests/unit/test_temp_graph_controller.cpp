// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../../include/temp_graph_controller.h"
#include "../../include/ui_temp_graph.h"
#include "../test_helpers/temp_graph_controller_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "lvgl/lvgl.h"
#include "moonraker_types.h"
#include "printer_state.h"
#include "temperature_history_manager.h"
#include "temperature_sensor_manager.h"

#include <chrono>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

class TempGraphControllerFixture {
  public:
    TempGraphControllerFixture() {
        lv_init_safe();

        lv_display_t* disp = lv_display_create(800, 480);
        alignas(64) static lv_color_t buf1[800 * 10];
        lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

        screen = lv_obj_create(NULL);

        // Initialize PrinterState subjects (needed by controller's setup_observers)
        get_printer_state().init_subjects(false);

        // PrinterState is process-global and rediscovery keeps a surviving
        // tool's subjects, readings included. Start every case from a freshly
        // discovered single tool reading 0 rather than a previous case's values.
        get_printer_state().temperature_state().init_extruders({});
        get_printer_state().temperature_state().init_extruders({"extruder"});
    }

    ~TempGraphControllerFixture() {}

    lv_obj_t* screen;
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller creates graph with minimal config",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series.clear(); // No series — avoids printer state lookups for sensors

    auto controller = std::make_unique<TempGraphController>(screen, cfg);

    REQUIRE(controller->is_valid());
    REQUIRE(controller->graph() != nullptr);
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller with series specs returns valid IDs",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
        {"heater_bed", lv_color_hex(0x88C0D0), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);

    REQUIRE(controller->is_valid());

    // series_id_for should return valid (>= 0) IDs for added series
    int extruder_id = controller->series_id_for("extruder");
    int bed_id = controller->series_id_for("heater_bed");
    REQUIRE(extruder_id >= 0);
    REQUIRE(bed_id >= 0);
    REQUIRE(extruder_id != bed_id);

    // Nonexistent series returns -1
    REQUIRE(controller->series_id_for("nonexistent_sensor") == -1);
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller pause and resume do not crash",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    REQUIRE_NOTHROW(controller->pause());
    REQUIRE_NOTHROW(controller->resume());
    REQUIRE_NOTHROW(controller->pause());
    REQUIRE_NOTHROW(controller->resume());
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller set_features applies feature flags",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    // Set a reduced feature set (lines are always forced on)
    uint32_t features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_Y_AXIS;
    controller->set_features(features);

    uint32_t active = ui_temp_graph_get_features(controller->graph());
    // LINES is always forced on
    REQUIRE((active & TEMP_GRAPH_FEATURE_LINES) != 0);
    REQUIRE((active & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
    // Features we did NOT set should be off
    REQUIRE((active & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
    REQUIRE((active & TEMP_GRAPH_FEATURE_GRADIENTS) == 0);
}

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Controller rebuild keeps graph valid and series intact",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
        {"heater_bed", lv_color_hex(0x88C0D0), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());
    REQUIRE(controller->series_id_for("extruder") >= 0);

    // Rebuild should recreate graph and series without crash
    REQUIRE_NOTHROW(controller->rebuild());

    REQUIRE(controller->is_valid());
    REQUIRE(controller->graph() != nullptr);
    REQUIRE(controller->series_id_for("extruder") >= 0);
    REQUIRE(controller->series_id_for("heater_bed") >= 0);
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller with custom scale params does not crash",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.scale_params.step = 25.0f;
    cfg.scale_params.floor = 100.0f;
    cfg.scale_params.ceiling = 400.0f;
    cfg.scale_params.expand_threshold = 0.85f;
    cfg.scale_params.shrink_threshold = 0.55f;

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());
    REQUIRE(controller->graph() != nullptr);
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller destruction with series is safe",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
        {"heater_bed", lv_color_hex(0x88C0D0), true},
    };

    // Create with active series and observers, then immediately destroy
    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    // Destruction tears down observers, drains queue, destroys graph
    REQUIRE_NOTHROW(controller.reset());
    REQUIRE(controller == nullptr);
}

// Reproducer for: chamber series klipper_name is "heater_generic chamber" (full Klipper
// object name), not "chamber". setup_observers() must resolve it to chamber temp/target
// subjects — an exact match on "chamber" would silently skip, leaving the graph empty.
TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Chamber series with heater_generic prefix resolves to chamber subjects",
                 "[controller][temp_graph_controller][chamber]") {
    auto& ps = get_printer_state();

    ps.temperature_state().set_chamber_heater_name("heater_generic chamber");
    // Set chamber temp/target subjects to known values
    lv_subject_set_int(ps.temperature_state().get_chamber_temp_subject(), 423);   // 42.3°C
    lv_subject_set_int(ps.temperature_state().get_chamber_target_subject(), 500); // 50.0°C

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"heater_generic chamber", lv_color_hex(0xA3BE8C), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    // Verify series ID was assigned
    int chamber_id = controller->series_id_for("heater_generic chamber");
    REQUIRE(chamber_id >= 0);

    // Pump the observer callbacks through UpdateQueue
    auto& queue = helix::ui::UpdateQueue::instance();
    queue.drain();
    lv_timer_handler_safe();

    // The chart should have received data — verify the series has points
    auto* graph = controller->graph();
    REQUIRE(graph != nullptr);
    lv_obj_t* chart = ui_temp_graph_get_chart(graph);
    REQUIRE(chart != nullptr);
    uint32_t point_cnt = lv_chart_get_point_count(chart);
    REQUIRE(point_cnt > 0);

    // Verify the actual data point was set.
    // The chart stores values as deci-degrees (temp * 10).
    // 42.3°C → 423 in chart storage.
    lv_chart_series_t* ser = lv_chart_get_series_next(chart, nullptr);
    REQUIRE(ser != nullptr);
    int32_t* y_points = lv_chart_get_series_y_array(chart, ser);
    REQUIRE(y_points != nullptr);
    // On first value, the series is backfilled with the initial temp → 423 (deci-degrees)
    REQUIRE(y_points[0] == 423);
    controller.reset();
    ps.temperature_state().set_chamber_heater_name("");
}

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Chamber series with temperature_fan prefix resolves to chamber subjects",
                 "[controller][temp_graph_controller][chamber]") {
    auto& ps = get_printer_state();

    ps.temperature_state().set_chamber_heater_name("temperature_fan chamber");
    // Set chamber temp/target to known values
    lv_subject_set_int(ps.temperature_state().get_chamber_temp_subject(), 385);   // 38.5°C
    lv_subject_set_int(ps.temperature_state().get_chamber_target_subject(), 450); // 45.0°C

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"temperature_fan chamber", lv_color_hex(0xA3BE8C), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    int chamber_id = controller->series_id_for("temperature_fan chamber");
    REQUIRE(chamber_id >= 0);

    // Pump observer callbacks
    auto& queue = helix::ui::UpdateQueue::instance();
    queue.drain();
    lv_timer_handler_safe();

    auto* graph = controller->graph();
    REQUIRE(graph != nullptr);
    lv_obj_t* chart = ui_temp_graph_get_chart(graph);
    REQUIRE(chart != nullptr);

    lv_chart_series_t* ser = lv_chart_get_series_next(chart, nullptr);
    REQUIRE(ser != nullptr);
    int32_t* y_points = lv_chart_get_series_y_array(chart, ser);
    REQUIRE(y_points != nullptr);
    // 38.5°C → 385 in chart storage (deci-degrees)
    REQUIRE(y_points[0] == 385);
    controller.reset();
    ps.temperature_state().set_chamber_heater_name("");
}

// Only the chamber's own objects plot chamber data. Any other heater_generic or
// temperature_fan is a separate object (a filament dryer, an exhaust fan) and
// must plot its own reading and target under its own label.
TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Non-chamber heater_generic and temperature_fan bind to their own subjects",
                 "[controller][temp_graph_controller][heater_generic]") {
    auto& ps = get_printer_state();
    ps.temperature_state().set_chamber_heater_name("heater_generic chamber");
    lv_subject_set_int(ps.temperature_state().get_chamber_temp_subject(), 423);
    lv_subject_set_int(ps.temperature_state().get_chamber_target_subject(), 500);

    auto& tsm = sensors::TemperatureSensorManager::instance();
    tsm.init_subjects();
    tsm.set_sync_mode(true);
    tsm.discover({"heater_generic filament_dryer", "temperature_fan exhaust"});
    tsm.update_from_status(
        {{"heater_generic filament_dryer", {{"temperature", 48.5}, {"target", 55.0}}},
         {"temperature_fan exhaust", {{"temperature", 31.0}, {"target", 40.0}}}});

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"heater_generic filament_dryer", lv_color_hex(0xA3BE8C), true},
        {"temperature_fan exhaust", lv_color_hex(0x88C0D0), true},
        {"chamber", lv_color_hex(0xBF616A), true},
    };
    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());
    helix::ui::UpdateQueue::instance().drain();
    lv_timer_handler_safe();

    auto* graph = controller->graph();
    lv_obj_t* chart = ui_temp_graph_get_chart(graph);
    auto first_point = [&](const char* name) {
        int id = controller->series_id_for(name);
        REQUIRE(id >= 0);
        return lv_chart_get_series_y_array(chart, graph->series_meta[id].chart_series)[0];
    };
    auto target_of = [&](const char* name) {
        return graph->series_meta[controller->series_id_for(name)].target_temp;
    };

    CHECK(first_point("heater_generic filament_dryer") == 485);
    CHECK(target_of("heater_generic filament_dryer") == Catch::Approx(55.0f));
    CHECK(first_point("temperature_fan exhaust") == 310);
    CHECK(target_of("temperature_fan exhaust") == Catch::Approx(40.0f));
    CHECK(first_point("chamber") == 423);
    CHECK(target_of("chamber") == Catch::Approx(50.0f));

    controller.reset();
    tsm.discover({});
    tsm.set_sync_mode(false);
    ps.temperature_state().set_chamber_heater_name("");
}

// ============================================================================
// Regression test for #1124 — persistent graph misses post-connect history
// ============================================================================
//
// A graph built before the WebSocket connects (persistent panels created at
// app startup: filament mini graph, home dashboard widget) runs its
// construction-time backfill against an empty history manager. #944's
// seed_from_store populates history only after discovery, and the connection
// observer deliberately skips the initial connect — so without the seed-time
// broadcast the graph stays empty until slowly repainted by live samples.
// refresh_all_from_history() (called right after seed_from_store) must pull the
// now-available history into every live controller.

namespace {
// RAII: install a real history manager for the duration of a test and restore
// the nullptr default on scope exit, even if a REQUIRE throws.
struct ScopedTestHistoryManager {
    explicit ScopedTestHistoryManager(TemperatureHistoryManager* mgr) {
        set_test_temperature_history_manager(mgr);
    }
    ~ScopedTestHistoryManager() {
        set_test_temperature_history_manager(nullptr);
    }
};

// Count chart points in a series that carry a given deci-degree value.
int count_series_points_eq(ui_temp_graph_t* graph, int32_t deci_value) {
    lv_obj_t* chart = ui_temp_graph_get_chart(graph);
    if (!chart)
        return 0;
    lv_chart_series_t* ser = lv_chart_get_series_next(chart, nullptr);
    if (!ser)
        return 0;
    int32_t* y = lv_chart_get_series_y_array(chart, ser);
    if (!y)
        return 0;
    uint32_t pc = lv_chart_get_point_count(chart);
    int n = 0;
    for (uint32_t i = 0; i < pc; ++i)
        if (y[i] == deci_value)
            ++n;
    return n;
}
} // namespace

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Persistent graph backfills seeded history via refresh_all (#1124)",
                 "[controller][temp_graph_controller][backfill][regression]") {
    auto& ps = get_printer_state();

    // Real history manager (the test harness stubs this to nullptr by default).
    TemperatureHistoryManager mgr(ps);
    ScopedTestHistoryManager installed(&mgr);

    // Build the controller while history is EMPTY — mirrors a persistent panel
    // graph constructed at startup before the socket connects. Its
    // construction-time backfill finds nothing.
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
    };
    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());
    REQUIRE(controller->series_id_for("extruder") >= 0);

    // Drain queued observer callbacks. The extruder subject defaults to 0, and
    // the live observer drops non-positive readings, so the chart stays empty.
    auto& queue = helix::ui::UpdateQueue::instance();
    queue.drain();
    lv_timer_handler_safe();

    // Precondition: no seeded value has been plotted yet.
    REQUIRE(count_series_points_eq(controller->graph(), 2000) == 0);

    // History arrives post-connect (simulates #944 seed_from_store): a steady
    // 200.0°C nozzle. seed_from_store spaces samples ending at now_ms so they
    // land inside the chart's backfill window.
    TemperatureStore store;
    TemperatureStoreSeries series;
    for (int i = 0; i < 40; ++i) {
        series.temperatures.push_back(200.0f);
        series.targets.push_back(210.0f);
        series.powers.push_back(0.5f);
    }
    store["extruder"] = series;
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    mgr.seed_from_store(store, now_ms);

    // The seed-time broadcast must reach the live controller and repopulate it.
    TempGraphController::refresh_all_from_history();
    queue.drain();
    lv_timer_handler_safe();

    // The chart now carries the seeded 200.0°C (2000 deci) history.
    REQUIRE(count_series_points_eq(controller->graph(), 2000) > 0);
}

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "A paused graph takes the seed backfill on resume and not while paused",
                 "[controller][temp_graph_controller][backfill]") {
    auto& ps = get_printer_state();
    TemperatureHistoryManager mgr(ps);
    ScopedTestHistoryManager installed(&mgr);

    TempGraphControllerConfig cfg;
    cfg.series = {{"extruder", lv_color_hex(0xFF4444), true}};
    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());
    auto& queue = helix::ui::UpdateQueue::instance();
    queue.drain();
    lv_timer_handler_safe();
    controller->pause();

    TemperatureStore store;
    TemperatureStoreSeries series;
    for (int i = 0; i < 40; ++i) {
        series.temperatures.push_back(200.0f);
        series.targets.push_back(210.0f);
        series.powers.push_back(0.5f);
    }
    store["extruder"] = series;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    mgr.seed_from_store(store, now_ms);

    TempGraphController::refresh_all_from_history();
    queue.drain();
    lv_timer_handler_safe();
    CHECK(count_series_points_eq(controller->graph(), 2000) == 0);

    controller->resume();
    queue.drain();
    lv_timer_handler_safe();
    CHECK(count_series_points_eq(controller->graph(), 2000) > 0);
}

// ============================================================================
// Regression tests for #1117 — deferred-delete / queued-callback races
// ============================================================================
//
// Crash signature: SIGSEGV in TempGraphController::setup_observers() on K1
// after ~6h uptime. Root cause: a queued connection-state observer fires
// rebuild() → setup_observers() on a controller whose detach() has run but
// whose memory is still valid (deferred-delete window in
// ui_overlay_temp_graph.cpp:171). The connection_observer lambda deref'd
// `self->generation_` before checking the lifetime token, hitting freed
// memory if `self` was already gone.
//
// These tests exercise both shapes of the race:
//   1. Synchronous destroy with a queued callback still pending
//   2. The on_activate() "detach + release + async-delete" swap pattern

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Queued rebuild callback safely no-ops after synchronous destroy (#1117)",
                 "[controller][temp_graph_controller][regression][uaf]") {
    auto& ps = get_printer_state();
    auto* conn_subj = ps.network_state().get_printer_connection_state_subject();
    REQUIRE(conn_subj != nullptr);

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
        {"heater_bed", lv_color_hex(0x88C0D0), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    // Reset prev_state captured at construction so the test transitions are
    // treated as real changes (otherwise the observer short-circuits on
    // "state == *prev_state").
    lv_subject_set_int(conn_subj, 0); // Disconnected
    helix::ui::UpdateQueue::instance().drain();
    lv_timer_handler_safe();

    // Queue a reconnect callback but DON'T drain — leaves a rebuild()
    // invocation pending in UpdateQueue targeting the controller.
    lv_subject_set_int(conn_subj, 2); // Reconnected

    // Destroy the controller synchronously (simulates the deferred-delete
    // firing before the queued callback runs). Without the tearing_down_
    // guard, the pending rebuild() would call setup_observers() on freed
    // memory when the queue drains next.
    controller.reset();

    // Drain — must not crash. The pending callback should early-return via
    // either weak_alive.expired() (ctx freed) or tearing_down_ check.
    REQUIRE_NOTHROW(helix::ui::UpdateQueue::instance().drain());
    lv_timer_handler_safe();
}

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Detach + release + deferred-delete race is safe (#1117)",
                 "[controller][temp_graph_controller][regression][uaf]") {
    auto& ps = get_printer_state();
    auto* conn_subj = ps.network_state().get_printer_connection_state_subject();
    REQUIRE(conn_subj != nullptr);

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    // Prime the disconnect state so the next transition is a reconnect.
    lv_subject_set_int(conn_subj, 0);
    helix::ui::UpdateQueue::instance().drain();
    lv_timer_handler_safe();

    // Queue a reconnect.
    lv_subject_set_int(conn_subj, 2);

    // Reproduce the on_activate swap path from ui_overlay_temp_graph.cpp:171:
    // detach observers synchronously, release ownership, defer deletion via
    // lv_async_call. This is the exact window the K1 crash hit.
    controller->detach();
    TempGraphController* raw = controller.release();
    REQUIRE(raw != nullptr);

    // Drain the UpdateQueue — pending reconnect callback must no-op against
    // the detached controller (tearing_down_ flag + lifetime token).
    REQUIRE_NOTHROW(helix::ui::UpdateQueue::instance().drain());

    // Fire the lv_async_call deletion (lv_timer_handler_safe runs one-shot
    // timers). Controller must be safely destroyed without UAF.
    REQUIRE_NOTHROW(lv_timer_handler_safe());

    // raw is now a dangling pointer — don't touch it. unique_ptr is empty.
    REQUIRE(controller == nullptr);
}

// ============================================================================
// Regression: series built before extruder discovery must still go live
// ============================================================================
//
// get_extruder_temp_subject() is a plain map lookup that returns nullptr until
// PrinterTemperatureState::init_extruders() runs. Persistent graphs (the home
// temp_graph widget) are constructed at app startup, before the WebSocket
// connects, so every nozzle series silently got no observer and stayed frozen
// on backfilled history for the whole session. The controller only rebuilt on
// disconnect->reconnect, never on discovery.

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Series resolves extruder subjects discovered after construction",
                 "[controller][temp_graph_controller][multi_tool]") {
    auto& ps = get_printer_state();
    auto& queue = helix::ui::UpdateQueue::instance();

    // PrinterState is a process-global here and other cases discover tools into
    // it, so reset to the pre-discovery state this case is about.
    ps.temperature_state().init_extruders({});
    queue.drain();

    // Given: a graph built BEFORE discovery — "extruder1" does not exist yet
    REQUIRE(ps.temperature_state().get_extruder_temp_subject("extruder1") == nullptr);

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder1", lv_color_hex(0xFF4444), true},
    };
    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());
    REQUIRE(controller->series_id_for("extruder1") >= 0);

    // When: discovery lands and extruder1 reports a temperature
    ps.temperature_state().init_extruders({"extruder", "extruder1"});
    queue.drain();
    lv_timer_handler_safe();

    lv_subject_set_int(ps.temperature_state().get_extruder_temp_subject("extruder1"), 2295);
    queue.drain();
    lv_timer_handler_safe();

    // Then: the live sample reaches the chart instead of being dropped
    REQUIRE(count_series_points_eq(controller->graph(), 2295) > 0);
}

// Discovery re-runs on every klippy ready with the WebSocket still up. A tool
// that drops out of one discovery and returns in the next gets fresh subjects,
// and a series bound to the old ones must follow.
TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Extruder series rebinds when discovery recreates its subjects",
                 "[controller][temp_graph_controller][klippy_restart]") {
    auto& ps = get_printer_state();
    auto& queue = helix::ui::UpdateQueue::instance();
    auto& temps = ps.temperature_state();

    temps.init_extruders({"extruder"});
    lv_subject_set_int(temps.get_bed_temp_subject(), 0);
    queue.drain();

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
        {"heater_bed", lv_color_hex(0x88C0D0), true},
    };
    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    constexpr int64_t slot = UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC * 1000;
    int64_t now = 1'000'000 * slot;
    TempGraphControllerTestAccess::set_clock(*controller, [&now] { return now; });
    queue.drain();

    // Nozzle is the first series: read its newest sample and ring position.
    auto* g = controller->graph();
    auto* nozzle = g->series_meta[controller->series_id_for("extruder")].chart_series;
    auto newest = [&] {
        const uint32_t pc = lv_chart_get_point_count(g->chart);
        const uint32_t sp = lv_chart_get_x_start_point(g->chart, nozzle);
        return lv_chart_get_y_array(g->chart, nozzle)[(sp + pc - 1) % pc];
    };

    lv_subject_set_int(temps.get_extruder_temp_subject("extruder"), 520);
    queue.drain();
    REQUIRE(newest() == 520);

    // The tool drops out and comes back: fresh subjects under the same name.
    temps.init_extruders({});
    temps.init_extruders({"extruder"});
    queue.drain();
    queue.drain();

    // A bed reading on a new slot must not re-plot the pre-restart nozzle value.
    const uint32_t nozzle_start = lv_chart_get_x_start_point(g->chart, nozzle);
    now += slot;
    lv_subject_set_int(temps.get_bed_temp_subject(), 600);
    queue.drain();
    CHECK(lv_chart_get_x_start_point(g->chart, nozzle) == nozzle_start);

    // The nozzle heats: the graph follows the recreated subject.
    now += slot;
    lv_subject_set_int(temps.get_extruder_temp_subject("extruder"), 1000);
    queue.drain();
    REQUIRE(newest() == 1000);
}

// Bed and chamber series observe PrinterState's static subjects. When those die
// first, lv_subject_deinit() has already freed each observer node, so the
// controller's teardown must skip lv_observer_remove() rather than free it again.
TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Controller destroyed after its bed and chamber subjects die is safe",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"heater_bed", lv_color_hex(0x88C0D0), true},
        {"heater_generic chamber", lv_color_hex(0xA3BE8C), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    get_printer_state().deinit_subjects();
    controller.reset();
    get_printer_state().init_subjects(false);

    REQUIRE(controller == nullptr);
}

TEST_CASE("sample_due puts every series on the same wall-clock slots", "[temp_graph_controller]") {
    constexpr int64_t slot = UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC * 1000;
    constexpr int64_t base = 1'000'000 * slot;

    CHECK(TempGraphController::sample_due(0, base + 5));

    // Late in one slot, then just past its boundary: due, though far less than
    // an interval has elapsed.
    CHECK(TempGraphController::sample_due(base + slot - 100, base + slot + 100));

    // Early in a slot, then late in the same one: not due, though nearly an
    // interval has elapsed.
    CHECK_FALSE(TempGraphController::sample_due(base + 100, base + slot - 100));
}
