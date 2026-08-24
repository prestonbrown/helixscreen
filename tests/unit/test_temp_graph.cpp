// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/temp_graph_internal.h"
#include "../../include/theme_manager.h"
#include "../../include/ui_temp_graph.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"
#include "misc/lv_timer_private.h"

#include "../catch_amalgamated.hpp"

// Test fixture for temperature graph tests
class TempGraphTestFixture {
  public:
    TempGraphTestFixture() {
        // Initialize LVGL for testing (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Create a display for testing (headless)
        lv_display_t* disp = lv_display_create(800, 480);
        alignas(64) static lv_color_t buf1[800 * 10];
        lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

        // Create a screen object to use as parent
        screen = lv_obj_create(NULL);
    }

    ~TempGraphTestFixture() {
        // Cleanup is handled by LVGL
    }

    lv_obj_t* screen;
};

// ============================================================================
// Core API Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture, "Create and destroy graph", "[ui][core]") {
    SECTION("Create graph with valid parent") {
        ui_temp_graph_t* graph = ui_temp_graph_create(screen);

        REQUIRE(graph != nullptr);
        REQUIRE(ui_temp_graph_get_chart(graph) != nullptr);
        REQUIRE(graph->series_count == 0);
        REQUIRE(graph->next_series_id == 0);
        REQUIRE(graph->point_count == UI_TEMP_GRAPH_DEFAULT_POINTS);
        REQUIRE(graph->min_temp == UI_TEMP_GRAPH_DEFAULT_MIN_TEMP);
        REQUIRE(graph->max_temp == UI_TEMP_GRAPH_DEFAULT_MAX_TEMP);

        ui_temp_graph_destroy(graph);
    }

    SECTION("Create graph with NULL parent returns NULL") {
        ui_temp_graph_t* graph = ui_temp_graph_create(nullptr);
        REQUIRE(graph == nullptr);
    }

    SECTION("Destroy NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_destroy(nullptr));
    }

    SECTION("Get chart from NULL graph returns NULL") {
        lv_obj_t* chart = ui_temp_graph_get_chart(nullptr);
        REQUIRE(chart == nullptr);
    }
}

// L081 regression: ui_temp_graph_destroy must NOT call sync lv_obj_del on the chart
// (sync widget deletion inside an UpdateQueue::process_pending batch corrupts LVGL's
// global event list — see prestonbrown/helixscreen#867 cluster). Async deletion
// escapes the batch via lv_obj_delete_async; the chart stays alive until the next
// LVGL async drain. Sync delete would make the parent's child count 0 immediately;
// async delete leaves it at 1 until the lv_async one-shot timer fires.
TEST_CASE_METHOD(TempGraphTestFixture, "destroy defers chart deletion (L081)",
                 "[ui][core][crash][L081]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);
    REQUIRE(lv_obj_get_child_count(screen) == 1);

    ui_temp_graph_destroy(graph);
    REQUIRE(lv_obj_get_child_count(screen) == 1);

    // Drain LVGL's lv_async_call queue without spinning lv_timer_handler (which
    // wants tick input the test fixture doesn't drive). Mirrors the helper in
    // test_panel_widget_manager.cpp.
    for (int safety = 0; safety < 50; ++safety) {
        bool fired = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && t->timer_cb) {
                t->timer_cb(t);
                fired = true;
                break;
            }
            t = next;
        }
        if (!fired)
            break;
    }
    REQUIRE(lv_obj_get_child_count(screen) == 0);
}

// ============================================================================
// Series Management Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture, "Add series", "[ui][series]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Add single series returns valid ID") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        REQUIRE(id >= 0);
        REQUIRE(graph->series_count == 1);
        REQUIRE(graph->next_series_id == 1);
        REQUIRE(graph->series_meta[0].id == 0);
        REQUIRE(graph->series_meta[0].chart_series != nullptr);
        REQUIRE(graph->series_meta[0].visible == true);
        REQUIRE(graph->series_meta[0].show_target == false);
        REQUIRE(strcmp(graph->series_meta[0].name, "Nozzle") == 0);
    }

    SECTION("Add multiple series with unique IDs") {
        int id1 = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        int id2 = ui_temp_graph_add_series(graph, "Bed", lv_color_hex(0x2196F3));
        int id3 = ui_temp_graph_add_series(graph, "Chamber", lv_color_hex(0x4CAF50));

        REQUIRE(id1 >= 0);
        REQUIRE(id2 >= 0);
        REQUIRE(id3 >= 0);
        REQUIRE(id1 != id2);
        REQUIRE(id2 != id3);
        REQUIRE(id1 != id3);
        REQUIRE(graph->series_count == 3);
        REQUIRE(graph->next_series_id == 3);
    }

    SECTION("Add series with NULL name fails") {
        int id = ui_temp_graph_add_series(graph, nullptr, lv_color_hex(0xFF5722));
        REQUIRE(id == -1);
        REQUIRE(graph->series_count == 0);
    }

    SECTION("Add series to NULL graph fails") {
        int id = ui_temp_graph_add_series(nullptr, "Nozzle", lv_color_hex(0xFF5722));
        REQUIRE(id == -1);
    }

    SECTION("Add up to max series") {
        int ids[UI_TEMP_GRAPH_MAX_SERIES];

        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Series%d", i);
            ids[i] = ui_temp_graph_add_series(graph, name, lv_color_hex(0xFF5722 + i));
            REQUIRE(ids[i] >= 0);
        }

        REQUIRE(graph->series_count == UI_TEMP_GRAPH_MAX_SERIES);

        // Verify all IDs are unique
        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            for (int j = i + 1; j < UI_TEMP_GRAPH_MAX_SERIES; j++) {
                REQUIRE(ids[i] != ids[j]);
            }
        }
    }

    SECTION("Exceeding max series fails") {
        // Add max series
        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Series%d", i);
            ui_temp_graph_add_series(graph, name, lv_color_hex(0xFF5722));
        }

        // Try to add one more
        int id = ui_temp_graph_add_series(graph, "Overflow", lv_color_hex(0xFF5722));
        REQUIRE(id == -1);
        REQUIRE(graph->series_count == UI_TEMP_GRAPH_MAX_SERIES);
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Remove series", "[ui][series]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Remove existing series") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        REQUIRE(id >= 0);
        REQUIRE(graph->series_count == 1);

        ui_temp_graph_remove_series(graph, id);
        REQUIRE(graph->series_count == 0);
    }

    SECTION("Remove series from middle") {
        int id1 = ui_temp_graph_add_series(graph, "Series1", lv_color_hex(0xFF5722));
        int id2 = ui_temp_graph_add_series(graph, "Series2", lv_color_hex(0x2196F3));
        int id3 = ui_temp_graph_add_series(graph, "Series3", lv_color_hex(0x4CAF50));

        REQUIRE(graph->series_count == 3);

        ui_temp_graph_remove_series(graph, id2);
        REQUIRE(graph->series_count == 2);

        // Verify we can still use remaining series
        ui_temp_graph_update_series(graph, id1, 100.0f);
        ui_temp_graph_update_series(graph, id3, 200.0f);
    }

    SECTION("Remove invalid series ID does nothing") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        REQUIRE(id >= 0);
        REQUIRE(graph->series_count == 1);

        ui_temp_graph_remove_series(graph, 999);
        REQUIRE(graph->series_count == 1);
    }

    SECTION("Remove from NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_remove_series(nullptr, 0));
    }

    SECTION("Remove already removed series is safe") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        ui_temp_graph_remove_series(graph, id);
        ui_temp_graph_remove_series(graph, id); // Remove again
        REQUIRE(graph->series_count == 0);
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Show/hide series", "[ui][series]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Hide visible series") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        REQUIRE(graph->series_meta[0].visible == true);

        ui_temp_graph_show_series(graph, id, false);
        REQUIRE(graph->series_meta[0].visible == false);
    }

    SECTION("Show hidden series") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        ui_temp_graph_show_series(graph, id, false);
        REQUIRE(graph->series_meta[0].visible == false);

        ui_temp_graph_show_series(graph, id, true);
        REQUIRE(graph->series_meta[0].visible == true);
    }

    SECTION("Show/hide invalid series ID does nothing") {
        REQUIRE_NOTHROW(ui_temp_graph_show_series(graph, 999, false));
    }

    SECTION("Show/hide on NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_show_series(nullptr, 0, false));
    }

    ui_temp_graph_destroy(graph);
}

// ============================================================================
// Data Update Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture, "Update series data (push mode)", "[ui][data]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Update single series with single value") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        REQUIRE_NOTHROW(ui_temp_graph_update_series(graph, id, 210.5f));
        REQUIRE(graph->series_count == 1);
    }

    SECTION("Update series multiple times") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        for (int i = 0; i < 10; i++) {
            ui_temp_graph_update_series(graph, id, 200.0f + i);
        }
        // Series still intact after multiple updates
        REQUIRE(graph->series_count == 1);
        REQUIRE(graph->series_meta[0].chart_series != nullptr);
    }

    SECTION("Update invalid series ID is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_update_series(graph, 999, 100.0f));
        REQUIRE(graph->series_count == 0);
    }

    SECTION("Update NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_update_series(nullptr, 0, 100.0f));
    }

    SECTION("Update with boundary values") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        REQUIRE_NOTHROW(ui_temp_graph_update_series(graph, id, 0.0f));
        REQUIRE_NOTHROW(ui_temp_graph_update_series(graph, id, 300.0f));
        REQUIRE_NOTHROW(ui_temp_graph_update_series(graph, id, -50.0f));
        REQUIRE_NOTHROW(ui_temp_graph_update_series(graph, id, 500.0f));
        REQUIRE(graph->series_count == 1);
        REQUIRE(graph->series_meta[0].chart_series != nullptr);
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Set series data (array mode)", "[ui][data]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Set data with valid array") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        float temps[] = {20.0f, 50.0f, 100.0f, 150.0f, 200.0f, 210.5f};
        REQUIRE_NOTHROW(ui_temp_graph_set_series_data(graph, id, temps, 6));
        REQUIRE(graph->series_count == 1);
        REQUIRE(graph->series_meta[0].chart_series != nullptr);
    }

    SECTION("Set data with array larger than point count") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        // Create array larger than default point count
        float* temps = new float[UI_TEMP_GRAPH_DEFAULT_POINTS + 100];
        for (int i = 0; i < UI_TEMP_GRAPH_DEFAULT_POINTS + 100; i++) {
            temps[i] = 20.0f + i * 0.5f;
        }

        REQUIRE_NOTHROW(
            ui_temp_graph_set_series_data(graph, id, temps, UI_TEMP_GRAPH_DEFAULT_POINTS + 100));
        // Series still intact after truncation
        REQUIRE(graph->series_count == 1);
        REQUIRE(graph->series_meta[0].chart_series != nullptr);
        delete[] temps;
    }

    SECTION("Set data with NULL array fails gracefully") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        REQUIRE_NOTHROW(ui_temp_graph_set_series_data(graph, id, nullptr, 10));
        // Series still exists despite invalid data
        REQUIRE(graph->series_count == 1);
    }

    SECTION("Set data with zero count fails gracefully") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        float temps[] = {100.0f};
        REQUIRE_NOTHROW(ui_temp_graph_set_series_data(graph, id, temps, 0));
        REQUIRE(graph->series_count == 1);
    }

    SECTION("Set data with negative count fails gracefully") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        float temps[] = {100.0f};
        REQUIRE_NOTHROW(ui_temp_graph_set_series_data(graph, id, temps, -5));
        REQUIRE(graph->series_count == 1);
    }

    SECTION("Set data on NULL graph is safe") {
        float temps[] = {100.0f};
        REQUIRE_NOTHROW(ui_temp_graph_set_series_data(nullptr, 0, temps, 1));
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Clear graph data", "[ui][data]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Clear all series data") {
        int id1 = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        int id2 = ui_temp_graph_add_series(graph, "Bed", lv_color_hex(0x2196F3));

        // Add some data
        ui_temp_graph_update_series(graph, id1, 210.0f);
        ui_temp_graph_update_series(graph, id2, 60.0f);

        ui_temp_graph_clear(graph);

        // Series should still exist, just data cleared
        REQUIRE(graph->series_count == 2);
    }

    SECTION("Clear NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_clear(nullptr));
    }

    SECTION("Clear empty graph is safe") {
        ui_temp_graph_clear(graph);
        REQUIRE(graph->series_count == 0);
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Clear individual series data", "[ui][data]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Clear single series") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        ui_temp_graph_update_series(graph, id, 210.0f);

        ui_temp_graph_clear_series(graph, id);

        // Series should still exist
        REQUIRE(graph->series_count == 1);
    }

    SECTION("Clear one series leaves others intact") {
        int id1 = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        int id2 = ui_temp_graph_add_series(graph, "Bed", lv_color_hex(0x2196F3));

        ui_temp_graph_update_series(graph, id1, 210.0f);
        ui_temp_graph_update_series(graph, id2, 60.0f);

        ui_temp_graph_clear_series(graph, id1);

        REQUIRE(graph->series_count == 2);
    }

    SECTION("Clear invalid series ID is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_clear_series(graph, 999));
        REQUIRE(graph->series_count == 0);
    }

    SECTION("Clear on NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_clear_series(nullptr, 0));
    }

    ui_temp_graph_destroy(graph);
}

// ============================================================================
// Target Temperature Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture, "Set series target temperature", "[ui][target]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Set target temperature with visibility") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        ui_temp_graph_set_series_target(graph, id, 210.0f, true);

        REQUIRE(graph->series_meta[0].target_temp == 210.0f);
        REQUIRE(graph->series_meta[0].show_target == true);
    }

    SECTION("Set target temperature without showing") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        ui_temp_graph_set_series_target(graph, id, 210.0f, false);

        REQUIRE(graph->series_meta[0].target_temp == 210.0f);
        REQUIRE(graph->series_meta[0].show_target == false);
    }

    SECTION("Update target temperature") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        ui_temp_graph_set_series_target(graph, id, 200.0f, true);
        REQUIRE(graph->series_meta[0].target_temp == 200.0f);

        ui_temp_graph_set_series_target(graph, id, 220.0f, true);
        REQUIRE(graph->series_meta[0].target_temp == 220.0f);
    }

    SECTION("Set target with boundary values") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        ui_temp_graph_set_series_target(graph, id, 0.0f, true);
        REQUIRE(graph->series_meta[0].target_temp == 0.0f);

        ui_temp_graph_set_series_target(graph, id, 300.0f, true);
        REQUIRE(graph->series_meta[0].target_temp == 300.0f);
    }

    SECTION("Set target on invalid series ID is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_set_series_target(graph, 999, 210.0f, true));
        REQUIRE(graph->series_count == 0);
    }

    SECTION("Set target on NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_set_series_target(nullptr, 0, 210.0f, true));
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Show/hide target temperature", "[ui][target]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Show target temperature") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        ui_temp_graph_set_series_target(graph, id, 210.0f, false);
        REQUIRE(graph->series_meta[0].show_target == false);

        ui_temp_graph_show_target(graph, id, true);
        REQUIRE(graph->series_meta[0].show_target == true);
    }

    SECTION("Hide target temperature") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        ui_temp_graph_set_series_target(graph, id, 210.0f, true);
        REQUIRE(graph->series_meta[0].show_target == true);

        ui_temp_graph_show_target(graph, id, false);
        REQUIRE(graph->series_meta[0].show_target == false);
    }

    SECTION("Show/hide on invalid series ID is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_show_target(graph, 999, true));
        REQUIRE(graph->series_count == 0);
    }

    SECTION("Show/hide on NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_show_target(nullptr, 0, true));
    }

    ui_temp_graph_destroy(graph);
}

// ============================================================================
// Configuration Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture, "Set temperature range", "[ui][config]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Set valid temperature range") {
        ui_temp_graph_set_temp_range(graph, 0.0f, 250.0f);

        REQUIRE(graph->min_temp == 0.0f);
        REQUIRE(graph->max_temp == 250.0f);
    }

    SECTION("Set custom temperature range") {
        ui_temp_graph_set_temp_range(graph, -50.0f, 500.0f);

        REQUIRE(graph->min_temp == -50.0f);
        REQUIRE(graph->max_temp == 500.0f);
    }

    SECTION("Invalid range (min >= max) is rejected") {
        float original_min = graph->min_temp;
        float original_max = graph->max_temp;

        ui_temp_graph_set_temp_range(graph, 100.0f, 50.0f);

        // Should not change
        REQUIRE(graph->min_temp == original_min);
        REQUIRE(graph->max_temp == original_max);
    }

    SECTION("Invalid range (min == max) is rejected") {
        float original_min = graph->min_temp;
        float original_max = graph->max_temp;

        ui_temp_graph_set_temp_range(graph, 100.0f, 100.0f);

        // Should not change
        REQUIRE(graph->min_temp == original_min);
        REQUIRE(graph->max_temp == original_max);
    }

    SECTION("Set range on NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_set_temp_range(nullptr, 0.0f, 250.0f));
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Set point count", "[ui][config]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Set valid point count") {
        ui_temp_graph_set_point_count(graph, 600);
        REQUIRE(graph->point_count == 600);
    }

    SECTION("Set point count to 1") {
        ui_temp_graph_set_point_count(graph, 1);
        REQUIRE(graph->point_count == 1);
    }

    SECTION("Set point count to large value") {
        ui_temp_graph_set_point_count(graph, 10000);
        REQUIRE(graph->point_count == 10000);
    }

    SECTION("Invalid point count (zero) is rejected") {
        int original_count = graph->point_count;

        ui_temp_graph_set_point_count(graph, 0);

        // Should not change
        REQUIRE(graph->point_count == original_count);
    }

    SECTION("Invalid point count (negative) is rejected") {
        int original_count = graph->point_count;

        ui_temp_graph_set_point_count(graph, -100);

        // Should not change
        REQUIRE(graph->point_count == original_count);
    }

    SECTION("Set point count on NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_set_point_count(nullptr, 600));
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Set series gradient", "[ui][config]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Set custom gradient opacities") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        ui_temp_graph_set_series_gradient(graph, id, LV_OPA_80, LV_OPA_20);

        REQUIRE(graph->series_meta[0].gradient_bottom_opa == LV_OPA_80);
        REQUIRE(graph->series_meta[0].gradient_top_opa == LV_OPA_20);
    }

    SECTION("Set gradient to full opacity") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        ui_temp_graph_set_series_gradient(graph, id, LV_OPA_COVER, LV_OPA_COVER);

        REQUIRE(graph->series_meta[0].gradient_bottom_opa == LV_OPA_COVER);
        REQUIRE(graph->series_meta[0].gradient_top_opa == LV_OPA_COVER);
    }

    SECTION("Set gradient to transparent") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        ui_temp_graph_set_series_gradient(graph, id, LV_OPA_TRANSP, LV_OPA_TRANSP);

        REQUIRE(graph->series_meta[0].gradient_bottom_opa == LV_OPA_TRANSP);
        REQUIRE(graph->series_meta[0].gradient_top_opa == LV_OPA_TRANSP);
    }

    SECTION("Set gradient on invalid series ID is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_set_series_gradient(graph, 999, LV_OPA_50, LV_OPA_10));
        REQUIRE(graph->series_count == 0);
    }

    SECTION("Set gradient on NULL graph is safe") {
        REQUIRE_NOTHROW(ui_temp_graph_set_series_gradient(nullptr, 0, LV_OPA_50, LV_OPA_10));
    }

    ui_temp_graph_destroy(graph);
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture, "Complete workflow scenarios", "[ui][integration]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Typical heating profile") {
        // Add nozzle series
        int nozzle_id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        REQUIRE(nozzle_id >= 0);

        // Set target temperature
        ui_temp_graph_set_series_target(graph, nozzle_id, 210.0f, true);

        // Simulate heating from 20°C to 210°C
        for (int temp = 20; temp <= 210; temp += 10) {
            ui_temp_graph_update_series(graph, nozzle_id, (float)temp);
        }

        // Verify state
        REQUIRE(graph->series_count == 1);
        REQUIRE(graph->series_meta[0].target_temp == 210.0f);
        REQUIRE(graph->series_meta[0].show_target == true);
    }

    SECTION("Multi-heater monitoring") {
        // Add multiple heaters
        int nozzle_id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        int bed_id = ui_temp_graph_add_series(graph, "Bed", lv_color_hex(0x2196F3));
        int chamber_id = ui_temp_graph_add_series(graph, "Chamber", lv_color_hex(0x4CAF50));

        REQUIRE(nozzle_id >= 0);
        REQUIRE(bed_id >= 0);
        REQUIRE(chamber_id >= 0);

        // Set targets
        ui_temp_graph_set_series_target(graph, nozzle_id, 210.0f, true);
        ui_temp_graph_set_series_target(graph, bed_id, 60.0f, true);
        ui_temp_graph_set_series_target(graph, chamber_id, 40.0f, false);

        // Update temperatures
        ui_temp_graph_update_series(graph, nozzle_id, 205.3f);
        ui_temp_graph_update_series(graph, bed_id, 58.7f);
        ui_temp_graph_update_series(graph, chamber_id, 35.2f);

        REQUIRE(graph->series_count == 3);
    }

    SECTION("Series removal and re-addition") {
        int id1 = ui_temp_graph_add_series(graph, "Series1", lv_color_hex(0xFF5722));
        int id2 = ui_temp_graph_add_series(graph, "Series2", lv_color_hex(0x2196F3));

        // Remove first series
        ui_temp_graph_remove_series(graph, id1);
        REQUIRE(graph->series_count == 1);

        // Add new series (should reuse slot)
        int id3 = ui_temp_graph_add_series(graph, "Series3", lv_color_hex(0x4CAF50));
        REQUIRE(id3 >= 0);
        REQUIRE(graph->series_count == 2);

        // Verify second series still works
        ui_temp_graph_update_series(graph, id2, 100.0f);
        ui_temp_graph_update_series(graph, id3, 200.0f);
    }

    SECTION("Bulk data update") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));

        // Create historical temperature data
        const int count = 100;
        float temps[count];
        for (int i = 0; i < count; i++) {
            temps[i] = 20.0f + (190.0f / count) * i; // Heat from 20 to 210
        }

        // Set all at once
        ui_temp_graph_set_series_data(graph, id, temps, count);

        REQUIRE(graph->series_count == 1);
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture, "Stress tests", "[ui][stress]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Large data updates") {
        int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
        REQUIRE(id >= 0);

        // Push many data points
        for (int i = 0; i < 1000; i++) {
            ui_temp_graph_update_series(graph, id, 20.0f + (i % 200));
        }

        // No crash = success
        REQUIRE(graph->series_count == 1);
    }

    SECTION("Rapid configuration changes") {
        int id = ui_temp_graph_add_series(graph, "Test", lv_color_hex(0xFF5722));
        REQUIRE(id >= 0);

        // Rapidly change configuration
        for (int i = 0; i < 100; i++) {
            ui_temp_graph_set_series_target(graph, id, 100.0f + i, true);
            ui_temp_graph_show_series(graph, id, i % 2 == 0);
            ui_temp_graph_set_series_gradient(graph, id, LV_OPA_50 + i % 50, LV_OPA_10);
            ui_temp_graph_update_series(graph, id, 50.0f + i);
        }

        REQUIRE(graph->series_count == 1);
    }

    ui_temp_graph_destroy(graph);
}

// ============================================================================
// Over-range Point Masking Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture, "Over-range points stored at full precision", "[ui][data]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    // Set a small Y range: 0-150°C
    ui_temp_graph_set_temp_range(graph, 0.0f, 150.0f);

    int id = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF5722));
    REQUIRE(id >= 0);

    // Push values: some in range, some over range
    ui_temp_graph_update_series(graph, id, 50.0f);  // in range
    ui_temp_graph_update_series(graph, id, 100.0f); // in range
    ui_temp_graph_update_series(graph, id, 250.0f); // over range
    ui_temp_graph_update_series(graph, id, 300.0f); // over range
    ui_temp_graph_update_series(graph, id, 80.0f);  // in range

    // Verify data is stored at full precision (not clamped at storage time)
    // so that when Y-axis expands, the full values are available for drawing
    int32_t* y_data = lv_chart_get_y_array(graph->chart, graph->series_meta[0].chart_series);
    uint32_t pc = lv_chart_get_point_count(graph->chart);
    uint32_t sp = lv_chart_get_x_start_point(graph->chart, graph->series_meta[0].chart_series);

    // Read the last 5 values from the circular buffer
    int32_t last_vals[5];
    for (int i = 0; i < 5; i++) {
        last_vals[4 - i] = y_data[(sp + pc - 1 - i) % pc];
    }

    // Values stored as deci-degrees (x10), over-range values preserved
    REQUIRE(last_vals[0] == 500);  // 50.0 x 10
    REQUIRE(last_vals[1] == 1000); // 100.0 x 10
    REQUIRE(last_vals[2] == 2500); // 250.0 x 10 — stored, masked only during draw
    REQUIRE(last_vals[3] == 3000); // 300.0 x 10 — stored, masked only during draw
    REQUIRE(last_vals[4] == 800);  // 80.0 x 10

    // After expanding Y range, previously over-range points become visible again
    ui_temp_graph_set_temp_range(graph, 0.0f, 350.0f);

    // Data unchanged — same values, now all within range
    for (int i = 0; i < 5; i++) {
        last_vals[4 - i] = y_data[(sp + pc - 1 - i) % pc];
    }
    REQUIRE(last_vals[2] == 2500); // Now in range, will draw normally
    REQUIRE(last_vals[3] == 3000); // Now in range, will draw normally

    ui_temp_graph_destroy(graph);
}

// ============================================================================
// Target History Buffer Lifecycle Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture, "ui_temp_graph: target buffer allocated on add_series",
                 "[temp_graph][target_history]") {
    ui_temp_graph_t* g = ui_temp_graph_create(screen);
    REQUIRE(g != nullptr);

    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    REQUIRE(id >= 0);

    bool found = false;
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (g->series_meta[i].chart_series && g->series_meta[i].id == id) {
            REQUIRE(g->series_meta[i].target_deci_buf != nullptr);
            REQUIRE(g->series_meta[i].target_head == 0);
            for (int j = 0; j < g->point_count; j++) {
                REQUIRE(g->series_meta[i].target_deci_buf[j] == 0);
            }
            found = true;
            break;
        }
    }
    REQUIRE(found);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TempGraphTestFixture, "ui_temp_graph: target buffer freed on remove_series",
                 "[temp_graph][target_history]") {
    ui_temp_graph_t* g = ui_temp_graph_create(screen);
    REQUIRE(g != nullptr);

    int id = ui_temp_graph_add_series(g, "Bed", lv_color_hex(0x44FF44));
    REQUIRE(id >= 0);

    ui_temp_graph_remove_series(g, id);

    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        REQUIRE(g->series_meta[i].target_deci_buf == nullptr);
    }

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TempGraphTestFixture, "ui_temp_graph: set_point_count reallocs all target buffers",
                 "[temp_graph][target_history]") {
    ui_temp_graph_t* g = ui_temp_graph_create(screen);
    REQUIRE(g != nullptr);

    int id_a = ui_temp_graph_add_series(g, "A", lv_color_hex(0xFF0000));
    int id_b = ui_temp_graph_add_series(g, "B", lv_color_hex(0x00FF00));
    REQUIRE(id_a >= 0);
    REQUIRE(id_b >= 0);

    // Capture original pointers
    int16_t* orig_a = nullptr;
    int16_t* orig_b = nullptr;
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (g->series_meta[i].chart_series) {
            if (g->series_meta[i].id == id_a)
                orig_a = g->series_meta[i].target_deci_buf;
            if (g->series_meta[i].id == id_b)
                orig_b = g->series_meta[i].target_deci_buf;
        }
    }
    REQUIRE(orig_a != nullptr);
    REQUIRE(orig_b != nullptr);

    // Shrink: default (1200) → 100
    ui_temp_graph_set_point_count(g, 100);
    REQUIRE(g->point_count == 100);

    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (g->series_meta[i].chart_series) {
            REQUIRE(g->series_meta[i].target_deci_buf != nullptr);
            REQUIRE(g->series_meta[i].target_head == 0);

            // The buffer pointer MUST have changed — set_point_count must free
            // the old allocation and create a fresh one. Without this assertion
            // the test trivially passes even on a no-op implementation, since
            // the default 1200-element buffer is also zero-initialized.
            if (g->series_meta[i].id == id_a) {
                REQUIRE(g->series_meta[i].target_deci_buf != orig_a);
            }
            if (g->series_meta[i].id == id_b) {
                REQUIRE(g->series_meta[i].target_deci_buf != orig_b);
            }

            // First `count` entries are accessible and zeroed.
            for (int j = 0; j < 100; j++) {
                REQUIRE(g->series_meta[i].target_deci_buf[j] == 0);
            }
        }
    }

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TempGraphTestFixture,
                 "ui_temp_graph: update_series pushes current target into buffer",
                 "[temp_graph][target_history]") {
    ui_temp_graph_t* g = ui_temp_graph_create(screen);
    REQUIRE(g != nullptr);

    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    REQUIRE(id >= 0);

    auto get_meta = [&]() -> ui_temp_series_meta_t* {
        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            if (g->series_meta[i].chart_series && g->series_meta[i].id == id)
                return &g->series_meta[i];
        }
        return nullptr;
    };

    // Heater off → push three samples → all target slots should be 0.
    ui_temp_graph_update_series(g, id, 25.0f);
    ui_temp_graph_update_series(g, id, 26.0f);
    ui_temp_graph_update_series(g, id, 27.0f);

    auto* m = get_meta();
    REQUIRE(m != nullptr);
    REQUIRE(m->target_head == 3);
    REQUIRE(m->target_deci_buf[0] == 0);
    REQUIRE(m->target_deci_buf[1] == 0);
    REQUIRE(m->target_deci_buf[2] == 0);

    // Set target then push two more — those slots should record 220.0 (deci = 2200)
    m->target_temp = 220.0f;
    m->show_target = true;
    ui_temp_graph_update_series(g, id, 100.0f);
    ui_temp_graph_update_series(g, id, 150.0f);

    REQUIRE(m->target_head == 5);
    REQUIRE(m->target_deci_buf[3] == 2200);
    REQUIRE(m->target_deci_buf[4] == 2200);

    // Turn heater off → next push records 0 (segment break sentinel)
    m->target_temp = 0.0f;
    ui_temp_graph_update_series(g, id, 145.0f);

    REQUIRE(m->target_head == 6);
    REQUIRE(m->target_deci_buf[5] == 0);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TempGraphTestFixture, "ui_temp_graph: target buffer shifts left when full",
                 "[temp_graph][target_history]") {
    ui_temp_graph_t* g = ui_temp_graph_create(screen);
    REQUIRE(g != nullptr);

    // Shrink to a tractable size for the test.
    ui_temp_graph_set_point_count(g, 4);

    int id = ui_temp_graph_add_series(g, "X", lv_color_hex(0x123456));
    REQUIRE(id >= 0);

    auto get_meta = [&]() -> ui_temp_series_meta_t* {
        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            if (g->series_meta[i].chart_series && g->series_meta[i].id == id)
                return &g->series_meta[i];
        }
        return nullptr;
    };

    auto* m = get_meta();
    REQUIRE(m != nullptr);
    m->show_target = true;

    // Push 4 samples with increasing target values.
    m->target_temp = 10.0f;
    ui_temp_graph_update_series(g, id, 1.0f);
    m->target_temp = 20.0f;
    ui_temp_graph_update_series(g, id, 2.0f);
    m->target_temp = 30.0f;
    ui_temp_graph_update_series(g, id, 3.0f);
    m->target_temp = 40.0f;
    ui_temp_graph_update_series(g, id, 4.0f);

    REQUIRE(m->target_head == 4);
    REQUIRE(m->target_deci_buf[0] == 100);
    REQUIRE(m->target_deci_buf[3] == 400);

    // Push 5th → oldest shifts off
    m->target_temp = 50.0f;
    ui_temp_graph_update_series(g, id, 5.0f);

    REQUIRE(m->target_head == 4);          // capped
    REQUIRE(m->target_deci_buf[0] == 200); // was 100, shifted off
    REQUIRE(m->target_deci_buf[1] == 300);
    REQUIRE(m->target_deci_buf[2] == 400);
    REQUIRE(m->target_deci_buf[3] == 500);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TempGraphTestFixture,
                 "ui_temp_graph: set_current_target stages without pushing to buffer",
                 "[temp_graph][target_history]") {
    ui_temp_graph_t* g = ui_temp_graph_create(screen);
    REQUIRE(g != nullptr);

    int id = ui_temp_graph_add_series(g, "N", lv_color_hex(0xFF0000));
    REQUIRE(id >= 0);

    auto get_meta = [&]() -> ui_temp_series_meta_t* {
        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            if (g->series_meta[i].chart_series && g->series_meta[i].id == id)
                return &g->series_meta[i];
        }
        return nullptr;
    };
    auto* m = get_meta();
    REQUIRE(m != nullptr);

    // Set current target three times back-to-back (simulating multiple observer fires
    // between actuals samples). The buffer must remain untouched.
    ui_temp_graph_set_current_target(g, id, 100.0f, true);
    ui_temp_graph_set_current_target(g, id, 150.0f, true);
    ui_temp_graph_set_current_target(g, id, 220.0f, true);

    REQUIRE(m->target_temp == Catch::Approx(220.0f));
    REQUIRE(m->show_target == true);
    REQUIRE(m->target_head == 0); // No buffer pushes.

    // Now one actuals push should capture the latest (220), not any intermediate.
    ui_temp_graph_update_series(g, id, 50.0f);
    REQUIRE(m->target_head == 1);
    REQUIRE(m->target_deci_buf[0] == 2200);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TempGraphTestFixture,
                 "ui_temp_graph: set_series_data_with_targets populates both buffers",
                 "[temp_graph][target_history]") {
    ui_temp_graph_t* g = ui_temp_graph_create(screen);
    REQUIRE(g != nullptr);

    int id = ui_temp_graph_add_series(g, "N", lv_color_hex(0xFF0000));
    REQUIRE(id >= 0);

    auto get_meta = [&]() -> ui_temp_series_meta_t* {
        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            if (g->series_meta[i].chart_series && g->series_meta[i].id == id)
                return &g->series_meta[i];
        }
        return nullptr;
    };

    const float temps[] = {25.0f, 30.0f, 100.0f, 200.0f, 210.0f};
    const float targets[] = {0.0f, 0.0f, 220.0f, 220.0f, 220.0f};
    ui_temp_graph_set_series_data_with_targets(g, id, temps, targets, 5);

    auto* m = get_meta();
    REQUIRE(m != nullptr);
    REQUIRE(m->target_head == 5);
    REQUIRE(m->target_deci_buf[0] == 0);
    REQUIRE(m->target_deci_buf[1] == 0);
    REQUIRE(m->target_deci_buf[2] == 2200);
    REQUIRE(m->target_deci_buf[3] == 2200);
    REQUIRE(m->target_deci_buf[4] == 2200);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TempGraphTestFixture, "ui_temp_graph: clear_series zeroes target buffer",
                 "[temp_graph][target_history]") {
    ui_temp_graph_t* g = ui_temp_graph_create(screen);
    REQUIRE(g != nullptr);

    int id = ui_temp_graph_add_series(g, "X", lv_color_hex(0xFF0000));
    REQUIRE(id >= 0);

    auto get_meta = [&]() -> ui_temp_series_meta_t* {
        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            if (g->series_meta[i].chart_series && g->series_meta[i].id == id)
                return &g->series_meta[i];
        }
        return nullptr;
    };
    auto* m = get_meta();
    m->show_target = true;
    m->target_temp = 200.0f;

    ui_temp_graph_update_series(g, id, 100.0f);
    ui_temp_graph_update_series(g, id, 110.0f);
    REQUIRE(m->target_head == 2);
    REQUIRE(m->target_deci_buf[0] == 2000);

    ui_temp_graph_clear_series(g, id);

    REQUIRE(m->target_head == 0);
    for (int i = 0; i < g->point_count; i++) {
        REQUIRE(m->target_deci_buf[i] == 0);
    }

    ui_temp_graph_destroy(g);
}

// =============================================================================
// Target history segmenter (pure logic, no LVGL)
// =============================================================================
//
// The segmenter walks the parallel target buffer and emits [start, end) index
// pairs for each run of positive samples. Tested as a free function so we can
// exercise edge cases (all-zero, all-positive, mid-zeros, single-sample runs)
// without spinning up a real chart.

#include <utility>
#include <vector>

TEST_CASE("segment_target_buf: empty input yields no segments", "[temp_graph][target_history]") {
    auto segs = helix::temp_graph_internal::segment_target_buf(nullptr, 0);
    REQUIRE(segs.empty());

    int16_t buf[1] = {0};
    segs = helix::temp_graph_internal::segment_target_buf(buf, 0);
    REQUIRE(segs.empty());
}

TEST_CASE("segment_target_buf: all zeros yields no segments", "[temp_graph][target_history]") {
    int16_t buf[5] = {0, 0, 0, 0, 0};
    auto segs = helix::temp_graph_internal::segment_target_buf(buf, 5);
    REQUIRE(segs.empty());
}

TEST_CASE("segment_target_buf: single all-positive run yields one segment",
          "[temp_graph][target_history]") {
    int16_t buf[4] = {100, 200, 300, 400};
    auto segs = helix::temp_graph_internal::segment_target_buf(buf, 4);
    REQUIRE(segs.size() == 1);
    REQUIRE(segs[0].first == 0);
    REQUIRE(segs[0].second == 4); // exclusive end
}

TEST_CASE("segment_target_buf: gap in the middle splits into two segments",
          "[temp_graph][target_history]") {
    int16_t buf[8] = {0, 0, 500, 600, 600, 0, 0, 800};
    auto segs = helix::temp_graph_internal::segment_target_buf(buf, 8);
    REQUIRE(segs.size() == 2);
    REQUIRE(segs[0].first == 2);
    REQUIRE(segs[0].second == 5);
    REQUIRE(segs[1].first == 7);
    REQUIRE(segs[1].second == 8);
}

TEST_CASE("segment_target_buf: negative values treated as gaps too",
          "[temp_graph][target_history]") {
    int16_t buf[5] = {100, -50, 200, 0, 300};
    auto segs = helix::temp_graph_internal::segment_target_buf(buf, 5);
    REQUIRE(segs.size() == 3);
    REQUIRE(segs[0] == std::make_pair(0, 1));
    REQUIRE(segs[1] == std::make_pair(2, 3));
    REQUIRE(segs[2] == std::make_pair(4, 5));
}

// ============================================================================
// coalesce_target_runs (#979) — collapse equal-value runs of the target step
// trace into single line segments so a flat setpoint draws ONE dashed line
// instead of one sub-pixel line per buffer sample. Each returned pair (a,b)
// means "draw a line from buffer index a to index b"; the union of these pairs
// is geometrically identical to connecting every consecutive sample.
// ============================================================================

TEST_CASE("coalesce_target_runs: fewer than two points yields nothing",
          "[temp_graph][target_history]") {
    int16_t buf[3] = {250, 250, 250};
    REQUIRE(helix::temp_graph_internal::coalesce_target_runs(nullptr, 0, 5).empty());
    REQUIRE(helix::temp_graph_internal::coalesce_target_runs(buf, 1, 2).empty()); // single point
    REQUIRE(helix::temp_graph_internal::coalesce_target_runs(buf, 2, 2).empty()); // empty range
}

TEST_CASE("coalesce_target_runs: flat run collapses to one segment",
          "[temp_graph][target_history]") {
    int16_t buf[6] = {250, 250, 250, 250, 250, 250};
    auto runs = helix::temp_graph_internal::coalesce_target_runs(buf, 0, 6);
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0] == std::make_pair(0, 5)); // one line spanning the whole flat run
}

TEST_CASE("coalesce_target_runs: strictly varying samples are not merged",
          "[temp_graph][target_history]") {
    // No two consecutive values equal → identical to point-to-point connectors.
    int16_t buf[3] = {100, 200, 300};
    auto runs = helix::temp_graph_internal::coalesce_target_runs(buf, 0, 3);
    REQUIRE(runs.size() == 2);
    REQUIRE(runs[0] == std::make_pair(0, 1));
    REQUIRE(runs[1] == std::make_pair(1, 2));
}

TEST_CASE("coalesce_target_runs: flat-step-flat keeps the connector, merges the flats",
          "[temp_graph][target_history]") {
    // 200,200,200 -> step -> 250,250 : horizontal run, connector, horizontal run.
    int16_t buf[5] = {200, 200, 200, 250, 250};
    auto runs = helix::temp_graph_internal::coalesce_target_runs(buf, 0, 5);
    REQUIRE(runs.size() == 3);
    REQUIRE(runs[0] == std::make_pair(0, 2)); // flat run 200
    REQUIRE(runs[1] == std::make_pair(2, 3)); // connector 200 -> 250
    REQUIRE(runs[2] == std::make_pair(3, 4)); // flat run 250
}

TEST_CASE("coalesce_target_runs: respects a sub-range [first,second)",
          "[temp_graph][target_history]") {
    int16_t buf[6] = {0, 250, 250, 250, 250, 0};
    auto runs = helix::temp_graph_internal::coalesce_target_runs(buf, 1, 5);
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0] == std::make_pair(1, 4)); // flat run within the segment only
}

// ============================================================================
// decimate_indices (#979) — compress a dense (1 Hz) history window down to the
// chart's point budget by time-bucketing, so a 20-min window fits in ~400
// points instead of 1200. Returns indices to keep (last sample per bucket,
// always including the most recent).
// ============================================================================

#include <cstdint>

// Forward declaration of the function under test (defined in temp_graph_controller.cpp).
namespace helix::temp_graph_internal {
std::vector<int> decimate_indices(const std::vector<int64_t>& timestamps_ms, int max_points);
}

TEST_CASE("decimate_indices: empty / non-positive budget yields nothing",
          "[temp_graph][decimate]") {
    REQUIRE(helix::temp_graph_internal::decimate_indices({}, 400).empty());
    REQUIRE(helix::temp_graph_internal::decimate_indices({1, 2, 3}, 0).empty());
}

TEST_CASE("decimate_indices: fewer samples than budget keeps all", "[temp_graph][decimate]") {
    std::vector<int64_t> ts = {0, 1000, 2000, 3000};
    auto keep = helix::temp_graph_internal::decimate_indices(ts, 400);
    REQUIRE(keep.size() == 4);
    REQUIRE(keep == std::vector<int>({0, 1, 2, 3}));
}

TEST_CASE("decimate_indices: all-equal timestamps keep endpoints only", "[temp_graph][decimate]") {
    std::vector<int64_t> ts(10, 5000); // span 0
    auto keep = helix::temp_graph_internal::decimate_indices(ts, 4);
    REQUIRE(keep.size() == 2);
    REQUIRE(keep.front() == 0);
    REQUIRE(keep.back() == 9);
}

TEST_CASE("decimate_indices: dense window compresses to <= budget, keeps newest",
          "[temp_graph][decimate]") {
    // 1200 samples at 1 Hz (20 min) -> at most 400 points, most recent retained.
    std::vector<int64_t> ts;
    ts.reserve(1200);
    for (int i = 0; i < 1200; i++)
        ts.push_back(static_cast<int64_t>(i) * 1000);
    auto keep = helix::temp_graph_internal::decimate_indices(ts, 400);

    REQUIRE(!keep.empty());
    REQUIRE(keep.size() <= 400);
    REQUIRE(keep.size() >= 390);  // buckets are near-evenly filled
    REQUIRE(keep.back() == 1199); // newest sample always kept
    // strictly increasing
    for (size_t i = 1; i < keep.size(); i++)
        REQUIRE(keep[i] > keep[i - 1]);
    // every index in range
    for (int idx : keep)
        REQUIRE((idx >= 0 && idx < 1200));
}

// ============================================================================
// Gradient cache dirty-tracking (#979: K2 Plus touch freeze)
//
// The gradient cache recomputes the per-column software fill only when the
// underlying data / viewport changed; otherwise it blits a cached buffer.
// These tests pin the "recompute only when necessary" guarantee by asserting
// the dirty flag transitions. They fail if dirty-tracking is removed.
// ============================================================================

TEST_CASE_METHOD(TempGraphTestFixture,
                 "ui_temp_graph: gradient cache marked dirty after data update",
                 "[temp_graph][gradient_cache]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    int s = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF4444));
    REQUIRE(s >= 0);

    // Pushing a sample shifts the curve -> cache must recompute.
    ui_temp_graph_update_series(graph, s, 100.0f);
    REQUIRE(ui_temp_graph_gradient_cache_is_dirty(graph));

    // Simulate a completed render (the real recompute happens in the draw
    // callback, which needs a live draw context we don't have here).
    ui_temp_graph_mark_gradient_cache_clean(graph);
    REQUIRE_FALSE(ui_temp_graph_gradient_cache_is_dirty(graph));

    // A new sample flips it back to dirty.
    ui_temp_graph_update_series(graph, s, 105.0f);
    REQUIRE(ui_temp_graph_gradient_cache_is_dirty(graph));

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture,
                 "ui_temp_graph: gradient cache dirty on temp range change but not on no-op",
                 "[temp_graph][gradient_cache]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);
    int s = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF4444));
    REQUIRE(s >= 0);

    ui_temp_graph_set_temp_range(graph, 0.0f, 300.0f);
    ui_temp_graph_mark_gradient_cache_clean(graph);
    REQUIRE_FALSE(ui_temp_graph_gradient_cache_is_dirty(graph));

    // Changing the range moves the curve mapping -> dirty.
    ui_temp_graph_set_temp_range(graph, 0.0f, 250.0f);
    REQUIRE(ui_temp_graph_gradient_cache_is_dirty(graph));

    // Redundant call with identical values must NOT dirty (so we skip recompute).
    ui_temp_graph_mark_gradient_cache_clean(graph);
    ui_temp_graph_set_temp_range(graph, 0.0f, 250.0f);
    REQUIRE_FALSE(ui_temp_graph_gradient_cache_is_dirty(graph));

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphTestFixture,
                 "ui_temp_graph: gradient cache dirty when gradient feature toggled",
                 "[temp_graph][gradient_cache]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);
    int s = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF4444));
    REQUIRE(s >= 0);

    uint32_t feats = ui_temp_graph_get_features(graph);
    REQUIRE((feats & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);

    // Removing the GRADIENTS bit changes the gradient appearance -> dirty.
    ui_temp_graph_mark_gradient_cache_clean(graph);
    ui_temp_graph_set_features(graph, feats & ~TEMP_GRAPH_FEATURE_GRADIENTS);
    REQUIRE(ui_temp_graph_gradient_cache_is_dirty(graph));

    // Re-adding the bit -> dirty again.
    ui_temp_graph_mark_gradient_cache_clean(graph);
    ui_temp_graph_set_features(graph, feats | TEMP_GRAPH_FEATURE_GRADIENTS);
    REQUIRE(ui_temp_graph_gradient_cache_is_dirty(graph));

    ui_temp_graph_destroy(graph);
}
