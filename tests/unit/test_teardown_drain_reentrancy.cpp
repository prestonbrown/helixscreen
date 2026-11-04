// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_teardown_drain_reentrancy.cpp
 * @brief Teardown that runs no queued UI work under its own stack frame
 *
 * A drain runs every queued callback synchronously inside its caller, so a
 * teardown that drains can be freed, or have its container rebuilt, by work it
 * never asked for. The edit-mode chrome and the AMS widget delete handlers
 * release objects that no queued callback can reach (the chrome is
 * async-deleted and owned by edit mode alone; every AMS observer callback is
 * guarded by its ObserverGuard and a registry lookup), so they must not drain.
 *
 * Each case queues a canary that destroys what the teardown is working on,
 * runs the teardown, and checks the canary waited for the next tick.
 */

#include "ui_ams_lane_bar.h"
#include "ui_ams_lane_spool.h"
#include "ui_ams_slot.h"
#include "ui_clog_bar.h"
#include "ui_clog_meter.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/discovery_of_objects.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/thermistor_test_access.h"
#include "ams_state.h"
#include "clog_detection_config_modal.h"
#include "clog_meter_model.h"
#include "config.h"
#include "core/update_queue.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "src/ui/panel_widgets/clog_detection_widget.h"
#include "src/ui/panel_widgets/fan_stack_widget.h"
#include "src/ui/panel_widgets/fan_widget.h"
#include "src/ui/panel_widgets/thermistor_widget.h"
#include "temperature_sensor_manager.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class ChromeWidget : public PanelWidget {
  public:
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return "humidity";
    }
    bool has_edit_configure() const override {
        return true;
    }
};

void queue_canary(bool& ran, helix::SharedCallback<void()> destroy) {
    helix::ui::UpdateQueue::instance().queue("test::drain_canary", [&ran, destroy]() {
        ran = true;
        destroy();
    });
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "edit-mode chrome teardown runs no queued callback",
                 "[grid_edit][drain_reentrancy]") {
    const int gutter = GridLayout::gutter_px();
    REQUIRE(gutter > 0);
    constexpr int content_w = 715;
    constexpr int content_h = 475;
    const auto dims = GridLayout::get_dimensions(UiBreakpoint::Medium, content_w, content_h);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_size(container, content_w, content_h);
    auto col_dsc = GridLayout::make_col_dsc(dims.cols);
    auto row_dsc = GridLayout::make_row_dsc(dims.rows);
    lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
    lv_obj_set_style_pad_column(container, gutter, 0);
    lv_obj_set_style_pad_row(container, gutter, 0);

    ChromeWidget pw;
    lv_obj_t* widget = lv_obj_create(container);
    lv_obj_set_name(widget, "humidity");
    lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, 0, GridLayout::TRACKS_PER_CELL,
                         LV_GRID_ALIGN_STRETCH, 0, GridLayout::TRACKS_PER_CELL);
    lv_obj_set_user_data(widget, &pw);
    lv_obj_update_layout(container);

    // A second page, so the lattice carries the delete-page button.
    const std::string panel_id = "test_teardown_drain_reentrancy";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "second"},
                          {"widgets",
                           {{{"id", "humidity"},
                             {"enabled", true},
                             {"col", 0},
                             {"row", 0},
                             {"colspan", GridLayout::TRACKS_PER_CELL},
                             {"rowspan", GridLayout::TRACKS_PER_CELL}}}}}}}});
    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    auto& config = mgr.get_widget_config(panel_id);

    // The canary frees the edit session the teardown runs in.
    auto em = std::make_unique<GridEditMode>();
    em->enter(container, &config, 1);
    em->select_widget(widget);
    REQUIRE(GridEditModeTestAccess::remove_button(*em) != nullptr);
    REQUIRE(GridEditModeTestAccess::delete_page_button(*em) != nullptr);
    bool ran = false;

    SECTION("selection chrome") {
        queue_canary(ran, [&em]() { em.reset(); });
        GridEditModeTestAccess::destroy_selection_chrome(*em);
        REQUIRE_FALSE(ran);
        CHECK(GridEditModeTestAccess::remove_button(*em) == nullptr);
    }

    SECTION("snap preview") {
        GridEditModeTestAccess::update_snap_preview(*em, 0, 0, GridLayout::TRACKS_PER_CELL,
                                                    GridLayout::TRACKS_PER_CELL, true);
        REQUIRE(GridEditModeTestAccess::snap_preview(*em) != nullptr);
        queue_canary(ran, [&em]() { em.reset(); });
        GridEditModeTestAccess::destroy_snap_preview(*em);
        REQUIRE_FALSE(ran);
        CHECK(GridEditModeTestAccess::snap_preview(*em) == nullptr);
    }

    SECTION("lattice delete-page button") {
        lv_obj_t* old_btn = GridEditModeTestAccess::delete_page_button(*em);
        // A new content size is a new lattice, which replaces the button.
        lv_obj_set_size(container, content_w - 40, content_h - 40);
        lv_obj_update_layout(container);
        queue_canary(ran, [&em]() { em.reset(); });
        GridEditModeTestAccess::rebuild_lattice(*em);
        REQUIRE_FALSE(ran);
        CHECK(GridEditModeTestAccess::delete_page_button(*em) != old_btn);
    }

    helix::ui::UpdateQueue::instance().drain();
    CHECK(ran);
    CHECK(em == nullptr);
    process_lvgl(20);
}

namespace {

lv_obj_t* make_ams_widget(lv_obj_t* parent, const char* component, int slot_index) {
    const std::string idx = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", idx.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, component, attrs));
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "AMS widget delete runs no queued callback",
                 "[ams][drain_reentrancy]") {
    ui_ams_lane_spool_register();
    ui_ams_lane_bar_register();
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);

    const char* component = nullptr;
    SECTION("ams_slot") {
        component = "ams_slot";
    }
    SECTION("ams_lane_spool") {
        component = "ams_lane_spool";
    }
    SECTION("ams_lane_bar") {
        component = "ams_lane_bar";
    }
    CAPTURE(component);

    lv_obj_t* doomed = make_ams_widget(test_screen(), component, 0);
    REQUIRE(doomed != nullptr);
    process_lvgl(20);

    // The canary grows the registry the delete handler is erasing from, enough
    // to rehash it, as a queued panel rebuild that creates lanes would.
    std::vector<lv_obj_t*> created;
    bool ran = false;
    queue_canary(ran, [&created, component]() {
        for (int i = 0; i < 32; ++i) {
            created.push_back(make_ams_widget(lv_screen_active(), component, i % 4));
        }
    });

    lv_obj_delete(doomed);
    CHECK_FALSE(ran);

    helix::ui::UpdateQueue::instance().drain();
    CHECK(ran);
    CHECK(created.size() == 32);
    for (lv_obj_t* obj : created) {
        REQUIRE(obj != nullptr);
        lv_obj_delete(obj);
    }
    process_lvgl(20);
}

namespace {

/// Run @p op with a canary queued ahead of it, and check the canary waited for
/// the next drain instead of running under @p op.
void check_runs_no_queued_callback(const char* what, const helix::SharedCallback<void()>& op) {
    INFO(what);
    bool ran = false;
    queue_canary(ran, [] {});
    op();
    CHECK_FALSE(ran);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(ran);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "home widget carousel bind and detach run no queued callback",
                 "[panel_widget][drain_reentrancy]") {
    SECTION("fan_stack carousel") {
        std::optional<PanelWidgetHarness<FanStackWidget>> h;
        lv_obj_t* speed_label = nullptr;
        check_runs_no_queued_callback("attach binds the carousel", [&] {
            h.emplace(test_screen(), HarnessConfig{{{"display_mode", "carousel"}}}, "fan_stack",
                      state());
            speed_label = h->child("speed_label");
        });
        REQUIRE(speed_label != nullptr);
        // The fans-version observer's first notification, run by that drain,
        // asks for the bind attach already ran: the pages it built stay.
        process_lvgl(30);
        CHECK(h->child("speed_label") == speed_label);
        check_runs_no_queued_callback("detach", [&] { h.reset(); });
    }

    SECTION("thermistor carousel") {
        helix::sensors::TemperatureSensorManager::instance().init_subjects();
        helix::sensors::TemperatureSensorManager::instance().discover(helix::test::discovery_of(
            {"temperature_sensor helix_drain_a", "temperature_sensor helix_drain_b"}));
        // Discovery publishes its count from the queue; let that land before the
        // widget observes it, or it reads as a later change of the sensor set.
        helix::ui::UpdateQueue::instance().drain();
        std::optional<PanelWidgetHarness<ThermistorWidget>> h;
        lv_obj_t* temp_label = nullptr;
        check_runs_no_queued_callback("attach binds the carousel", [&] {
            h.emplace(test_screen(),
                      HarnessConfig{{{"sensors",
                                      std::vector<std::string>{"temperature_sensor helix_drain_a",
                                                               "temperature_sensor helix_drain_b"}},
                                     {"display_mode", "carousel"}}},
                      "thermistor");
            temp_label = ThermistorTestAccess::carousel_temp_label(h->widget(), 0);
        });
        REQUIRE(ThermistorTestAccess::carousel_page_count(h->widget()) == 2);
        REQUIRE(temp_label != nullptr);
        // The sensor-count observer's first notification, run by that drain,
        // asks for the bind attach already ran: the pages it built stay.
        process_lvgl(30);
        CHECK(ThermistorTestAccess::carousel_temp_label(h->widget(), 0) == temp_label);
        check_runs_no_queued_callback("detach", [&] { h.reset(); });
    }

    SECTION("fan") {
        std::optional<PanelWidgetHarness<FanWidget>> h;
        h.emplace(test_screen(), "fan");
        check_runs_no_queued_callback("detach", [&] { h.reset(); });
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "clog meter, bar and widget teardown run no queued callback",
                 "[ams][clog][drain_reentrancy]") {
    AmsState::instance().init_subjects(true);

    SECTION("model") {
        std::optional<ui::ClogMeterModel> model;
        model.emplace([](const ui::ClogMeterSample&) {});
        check_runs_no_queued_callback("model destroyed", [&] { model.reset(); });
    }

    SECTION("bar") {
        auto bar = std::make_unique<ui::UiClogBar>(test_screen());
        check_runs_no_queued_callback("bar destroyed", [&] { bar.reset(); });
    }

    SECTION("meter") {
        auto meter = std::make_unique<ui::UiClogMeter>(test_screen());
        check_runs_no_queued_callback("meter destroyed", [&] { meter.reset(); });
    }

    SECTION("clog_detection widget") {
        std::optional<PanelWidgetHarness<ClogDetectionWidget>> h;
        h.emplace(test_screen());
        check_runs_no_queued_callback("detach", [&] { h.reset(); });
    }
}

// These widgets tear down without draining because a notification already
// queued when one detaches does nothing once it runs.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "home widget teardown turns an already-queued notification into a no-op",
                 "[panel_widget][drain_reentrancy]") {
    SECTION("thermistor carousel") {
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        tsm.init_subjects();
        tsm.discover(helix::test::discovery_of(
            {"temperature_sensor helix_drain_a", "temperature_sensor helix_drain_b"}));
        std::optional<PanelWidgetHarness<ThermistorWidget>> h;
        h.emplace(test_screen(),
                  HarnessConfig{
                      {{"sensors", std::vector<std::string>{"temperature_sensor helix_drain_a",
                                                            "temperature_sensor helix_drain_b"}},
                       {"display_mode", "carousel"}}},
                  "thermistor");
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_t* label = ThermistorTestAccess::carousel_temp_label(h->widget(), 0);
        REQUIRE(label != nullptr);
        lv_subject_t* temp = tsm.get_temp_subject("temperature_sensor helix_drain_a");
        REQUIRE(temp != nullptr);
        // The channel is live: a notification reaches the label on the next drain.
        const std::string initial = lv_label_get_text(label);
        lv_subject_set_int(temp, lv_subject_get_int(temp) + 1234);
        helix::ui::UpdateQueue::instance().drain();
        const std::string before = lv_label_get_text(label);
        REQUIRE(before != initial);

        lv_subject_set_int(temp, lv_subject_get_int(temp) + 1234);
        h->widget().detach_tile();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(std::string(lv_label_get_text(label)) == before);
    }

    SECTION("fan_stack carousel") {
        state().fan_state().init_fans({"fan"});
        std::optional<PanelWidgetHarness<FanStackWidget>> h;
        h.emplace(test_screen(), HarnessConfig{{{"display_mode", "carousel"}}}, "fan_stack",
                  state());
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_t* label = h->child("speed_label");
        REQUIRE(label != nullptr);
        lv_subject_t* speed = state().fan_state().get_fan_speed_subject("fan");
        REQUIRE(speed != nullptr);
        // The channel is live: a notification reaches the label on the next drain.
        const std::string initial = lv_label_get_text(label);
        lv_subject_set_int(speed, 77);
        helix::ui::UpdateQueue::instance().drain();
        const std::string before = lv_label_get_text(label);
        REQUIRE(before != initial);

        lv_subject_set_int(speed, 33);
        h->widget().detach_tile();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(std::string(lv_label_get_text(label)) == before);
    }
}

// The observers skip only the bind attach already ran; a real change rebinds.
TEST_CASE_METHOD(LVGLUITestFixture, "home widget carousels rebind on a later change",
                 "[panel_widget][drain_reentrancy]") {
    SECTION("thermistor: a sensor change") {
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        tsm.init_subjects();
        tsm.discover(helix::test::discovery_of(
            {"temperature_sensor helix_drain_a", "temperature_sensor helix_drain_b"}));
        PanelWidgetHarness<ThermistorWidget> h(
            test_screen(),
            HarnessConfig{
                {{"sensors", std::vector<std::string>{"temperature_sensor helix_drain_a",
                                                      "temperature_sensor helix_drain_b"}},
                 {"display_mode", "carousel"}}},
            "thermistor");
        process_lvgl(30);
        lv_obj_t* label = ThermistorTestAccess::carousel_temp_label(h.widget(), 0);
        REQUIRE(label != nullptr);

        tsm.discover(helix::test::discovery_of({"temperature_sensor helix_drain_a",
                                                "temperature_sensor helix_drain_b",
                                                "temperature_sensor helix_drain_c"}));
        process_lvgl(30);
        CHECK(ThermistorTestAccess::carousel_temp_label(h.widget(), 0) != label);
    }

    SECTION("fan_stack: a fans version bump") {
        PanelWidgetHarness<FanStackWidget> h(
            test_screen(), HarnessConfig{{{"display_mode", "carousel"}}}, "fan_stack", state());
        process_lvgl(30);
        lv_obj_t* label = h.child("speed_label");
        REQUIRE(label != nullptr);

        lv_subject_t* version = state().fan_state().get_fans_version_subject();
        lv_subject_set_int(version, lv_subject_get_int(version) + 1);
        process_lvgl(30);
        CHECK(h.child("speed_label") != label);
    }
}
