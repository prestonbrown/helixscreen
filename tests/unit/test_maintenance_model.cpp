// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_maintenance_model.cpp
 * @brief The live maintenance meters: print_duration accrues into them, an interval of 0
 * hides a meter, a reset and an interval change save, a saved record loads back, the
 * example plugin's store migrates once, and passing an interval raises one notice.
 *
 * The rules themselves are maintenance_meters.h's ([maintenance][meters]); these pin the
 * model's wiring to PrinterState, SettingsManager, the config file and the XML.
 *
 * Run with: ./build/bin/helix-tests "[maintenance][model]"
 */

#include "ui_maintenance_overlay.h"
#include "ui_notification_history.h"
#include "ui_panel_notification_history.h"
#include "ui_toast_manager.h"

#include "../helix_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/gcode_recording_api.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "lvgl.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/maintenance_model.h"
#include "system_settings_manager.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

constexpr int kFilter = static_cast<int>(maintenance::MeterId::Filter);
constexpr int kNozzle = static_cast<int>(maintenance::MeterId::Nozzle);
constexpr int kRails = static_cast<int>(maintenance::MeterId::XyAxis);
constexpr int kBelt = static_cast<int>(maintenance::MeterId::BeltTension);

/// Brings the model up fresh against this test's sandboxed config: the model and its
/// observers belong to whichever PrinterState subjects existed when it last initialized.
struct MaintenanceModelFixture : public LVGLUITestFixture {
    MaintenanceModelFixture() {
        // A model another test left up saves into this config on its way down.
        StaticSubjectRegistry::instance().deinit_one("MaintenanceModelSubjects");
        erase_record();
        identify_printer();
        restart_model();
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity, const std::string& msg, uint32_t) { toasts.push_back(msg); });
    }
    ~MaintenanceModelFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        // The teardown saves into this test's config, which the next fixture discards.
        StaticSubjectRegistry::instance().deinit_one("MaintenanceModelSubjects");
        drain();
    }

    /// Discovery has run on an enclosed printer with a board fan.
    void identify_printer() {
        PrinterDiscovery hw;
        hw.parse_objects(json::array({"extruder", "controller_fan board"}));
        state().set_hardware(hw);
        state().capabilities_state().set_has_chamber_filter_fan(true);
        drain();
    }

    /// A new session: whatever the config holds now is what loads.
    void restart_model() {
        StaticSubjectRegistry::instance().deinit_one("MaintenanceModelSubjects");
        maintenance_model::init_subjects();
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void set_duration(int seconds) {
        lv_subject_set_int(state().print_state().get_print_duration_subject(), seconds);
        drain();
    }

    /// The first reading the model sees after it starts: a value that differs from the
    /// subject's current one, so the observer fires and the tracker seeds on it.
    void seed() {
        base_ = lv_subject_get_int(state().print_state().get_print_duration_subject()) + 7;
        set_duration(base_);
    }

    /// The current job has printed @p seconds since seed().
    void printed(int seconds) {
        set_duration(base_ + seconds);
    }

    /// Any change of the connection state, whatever it was.
    void toggle_connection() {
        lv_subject_t* c = state().network_state().get_printer_connection_state_subject();
        lv_subject_set_int(c, lv_subject_get_int(c) == 0 ? 2 : 0);
        drain();
    }

    int base_ = 0;

    /// As though this printer never had the meters.
    static void erase_record() {
        json& data = ConfigTestAccess::data(*Config::get_instance());
        const json::json_pointer printer(prefix().substr(0, prefix().size() - 1));
        if (data.contains(printer) && data[printer].is_object())
            data[printer].erase("maintenance");
    }

    static std::string prefix() {
        return Config::get_instance()->df();
    }

    static const maintenance::Meter& meter(int i) {
        return maintenance_model::state().meters[static_cast<size_t>(i)];
    }

    static int subject_int(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
    static std::string subject_text(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }

    std::vector<std::string> toasts;
};

/// Whether an object draws: it and every ancestor is unhidden.
bool shown(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (lv_obj_is_hidden(o))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: a fresh printer starts at the defaults and saves them",
                 "[maintenance][model]") {
    CHECK(maintenance_model::state().total_h == 0.0);
    CHECK(meter(kFilter).interval_h == 200);
    CHECK(meter(kNozzle).interval_h == 500);
    CHECK(meter(kRails).interval_h == 300);
    CHECK(SettingsManager::instance().get_maintenance_record(prefix()).has_value());
    CHECK(subject_int("maintenance_any_on") == 1);
    CHECK(subject_text("maintenance_m4_left") == "200 h");
    CHECK(subject_text("maintenance_total_text") == "Total print time: 0 h");
    // The seeded meters reach the record with their intervals.
    const auto saved = SettingsManager::instance().get_maintenance_record(prefix());
    CHECK(saved->meters[kFilter].seeded);
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: seeding waits for the printer's capabilities, then saves",
                 "[maintenance][model]") {
    state().set_hardware(PrinterDiscovery{});
    drain();
    maintenance::State s = maintenance::default_state();
    REQUIRE(SettingsManager::instance().set_maintenance_record(prefix(), s));
    // Nothing has reported yet: the hardware generation is what says discovery has run.
    lv_subject_t* gen = state().capabilities_state().subject(Capability::HardwareGeneration);
    lv_subject_set_int(gen, 0);
    restart_model();
    CHECK_FALSE(meter(kNozzle).seeded);
    CHECK(meter(kNozzle).interval_h == 0);
    CHECK_FALSE(
        SettingsManager::instance().get_maintenance_record(prefix())->meters[kNozzle].seeded);

    identify_printer();
    CHECK(meter(kNozzle).interval_h == 500);
    CHECK(meter(static_cast<int>(maintenance::MeterId::ElectronicsFans)).interval_h == 500);
    CHECK(meter(kBelt).interval_h == 0);
    CHECK(SettingsManager::instance().get_maintenance_record(prefix())->meters[kNozzle].seeded);
    CHECK(subject_int("maintenance_belt_available") == 0);
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: print_duration accrues only as it rises",
                 "[maintenance][model]") {
    // The first reading seeds: those seconds were printed before the screen watched.
    seed();
    CHECK(maintenance_model::state().total_h == 0.0);

    printed(3600);
    CHECK(maintenance_model::state().total_h == Catch::Approx(1.0));
    CHECK(meter(kFilter).used_h == Catch::Approx(1.0));
    CHECK(meter(kRails).used_h == Catch::Approx(1.0));
    CHECK(subject_text("maintenance_m4_line") == "1 h of 200 h");

    SECTION("an unchanged reading, as while paused or idle, adds nothing") {
        printed(3600);
        CHECK(maintenance_model::state().total_h == Catch::Approx(1.0));
    }
    SECTION("a new job counts from its first reading") {
        set_duration(0);
        set_duration(1);
        set_duration(1 + 1800);
        CHECK(maintenance_model::state().total_h == Catch::Approx(1.5));
    }
    SECTION("a local zero before Klipper's running value is not counted again") {
        set_duration(0);
        printed(3600);
        CHECK(maintenance_model::state().total_h == Catch::Approx(1.0));
        printed(3600 + 360);
        CHECK(maintenance_model::state().total_h == Catch::Approx(1.1));
    }
    SECTION("a connection change re-seeds instead of counting the gap") {
        toggle_connection();
        set_duration(50000);
        CHECK(maintenance_model::state().total_h == Catch::Approx(1.0));
        set_duration(50000 + 360);
        CHECK(maintenance_model::state().total_h == Catch::Approx(1.1));
    }
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: accrual reaches the record before a crash could lose much",
                 "[maintenance][model]") {
    seed();
    printed(300);
    const auto early = SettingsManager::instance().get_maintenance_record(prefix());
    REQUIRE(early.has_value());
    CHECK(early->total_h == 0.0);

    printed(360);
    const auto saved = SettingsManager::instance().get_maintenance_record(prefix());
    REQUIRE(saved.has_value());
    CHECK(saved->total_h == Catch::Approx(0.1));

    SECTION("a print ending saves what is left") {
        lv_subject_set_int(state().print_state().get_print_active_subject(), 1);
        drain();
        printed(400);
        lv_subject_set_int(state().print_state().get_print_active_subject(), 0);
        drain();
        const auto ended = SettingsManager::instance().get_maintenance_record(prefix());
        REQUIRE(ended.has_value());
        CHECK(ended->total_h == Catch::Approx(400.0 / 3600.0));
    }
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: an interval of 0 hides the meter in the overlay and tile",
                 "[maintenance][model]") {
    lv_obj_t* overlay =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "maintenance_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_t* tile =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_maintenance", nullptr));
    REQUIRE(tile != nullptr);
    lv_obj_t* card = lv_obj_find_by_name(overlay, "mt_card_2");
    REQUIRE(card != nullptr);
    CHECK(shown(card));
    CHECK(subject_text("maintenance_r2_short") == "Nozzle");

    maintenance_model::set_interval_choice(kNozzle, 0);
    CHECK(meter(kNozzle).interval_h == 0);
    CHECK(subject_int("maintenance_m2_on") == 0);
    CHECK_FALSE(shown(card));
    CHECK(subject_text("maintenance_r2_short") != "Nozzle");
    // The intervals screen still lists it, so the meter can be turned back on.
    lv_obj_t* intervals = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "maintenance_intervals_overlay", nullptr));
    REQUIRE(intervals != nullptr);
    lv_obj_t* interval_row = lv_obj_find_by_name(intervals, "mt_interval_2");
    REQUIRE(interval_row != nullptr);
    CHECK(shown(interval_row));
    lv_obj_delete(intervals);

    const auto saved = SettingsManager::instance().get_maintenance_record(prefix());
    REQUIRE(saved.has_value());
    CHECK(saved->meters[kNozzle].interval_h == 0);

    seed();
    printed(3600);
    CHECK(meter(kNozzle).used_h == 0.0);
    CHECK(meter(kFilter).used_h == Catch::Approx(1.0));

    SECTION("every meter off: the tile says so") {
        for (int i = 0; i < maintenance::kMeterCount; ++i)
            maintenance_model::set_interval_choice(i, 0);
        CHECK(subject_int("maintenance_any_on") == 0);
        CHECK(subject_text("maintenance_worst_left") == "--");
        CHECK(subject_text("maintenance_worst_name") == "Off");
    }
    lv_obj_delete(tile);
    lv_obj_delete(overlay);
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: the overlay's dropdown sets the interval it shows",
                 "[maintenance][model]") {
    CHECK(subject_int("maintenance_m0_interval") == maintenance::interval_choice_index(300));
    maintenance_model::set_interval_choice(kRails, maintenance::interval_choice_index(1000));
    CHECK(meter(kRails).interval_h == 1000);
    CHECK(subject_int("maintenance_m0_interval") == maintenance::interval_choice_index(1000));
    CHECK(subject_text("maintenance_m0_left") == "1000 h");

    SECTION("an index outside the choices changes nothing") {
        maintenance_model::set_interval_choice(kRails, -1);
        maintenance_model::set_interval_choice(kRails, 99);
        maintenance_model::set_interval_choice(7, 0);
        CHECK(meter(kRails).interval_h == 1000);
    }
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: reset zeroes the meter, stamps the total and saves",
                 "[maintenance][model]") {
    seed();
    printed(7 * 3600);
    REQUIRE(meter(kFilter).used_h == Catch::Approx(7.0));

    maintenance_model::reset_meter(kFilter);
    CHECK(meter(kFilter).used_h == 0.0);
    REQUIRE(meter(kFilter).reset_at_h.has_value());
    CHECK(*meter(kFilter).reset_at_h == Catch::Approx(7.0));
    CHECK(meter(kNozzle).used_h == Catch::Approx(7.0));
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "Air filter meter reset");

    const auto saved = SettingsManager::instance().get_maintenance_record(prefix());
    REQUIRE(saved.has_value());
    CHECK(saved->meters[kFilter].used_h == 0.0);
    REQUIRE(saved->meters[kFilter].reset_at_h.has_value());
    CHECK(*saved->meters[kFilter].reset_at_h == Catch::Approx(7.0));
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: the saved record is what the next session loads",
                 "[maintenance][model]") {
    maintenance::State s = maintenance::default_state();
    maintenance::set_interval(s, kNozzle, 750);
    maintenance::accrue(s, 123.5);
    maintenance::reset_meter(s, kRails);
    REQUIRE(SettingsManager::instance().set_maintenance_record(prefix(), s));

    restart_model();
    CHECK(maintenance_model::state().total_h == Catch::Approx(123.5));
    CHECK(meter(kNozzle).interval_h == 750);
    CHECK(meter(kNozzle).used_h == Catch::Approx(123.5));
    CHECK(meter(kRails).used_h == 0.0);
    CHECK(subject_text("maintenance_total_text") == "Total print time: 124 h");
    CHECK(subject_text("maintenance_m2_left") == "627 h");
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: a crossing mid-print waits for the print to end",
                 "[maintenance][model]") {
    maintenance_model::set_interval_choice(kFilter, maintenance::interval_choice_index(25));
    lv_subject_set_int(state().print_state().get_print_active_subject(), 1);
    drain();
    seed();
    printed(24 * 3600);
    CHECK(subject_int("maintenance_m4_status") == static_cast<int>(ValueStatus::Warn));
    CHECK(toasts.empty());

    printed(25 * 3600);
    CHECK(subject_int("maintenance_m4_status") == static_cast<int>(ValueStatus::Danger));
    CHECK(subject_int("maintenance_worst_status") == static_cast<int>(ValueStatus::Danger));
    CHECK(subject_int("maintenance_worst_pct") == 100);
    CHECK(subject_text("maintenance_worst_name") == "Filter");
    CHECK(toasts.empty());
    CHECK_FALSE(meter(kFilter).notified);

    lv_subject_set_int(state().print_state().get_print_active_subject(), 0);
    drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0] == "Air filter is due");
    // The latch reaches the record with the notice, so a restart does not repeat it.
    const auto saved = SettingsManager::instance().get_maintenance_record(prefix());
    REQUIRE(saved.has_value());
    CHECK(saved->meters[kFilter].notified);

    bool found = false;
    for (const auto& e : NotificationHistory::instance().get_all())
        found = found || std::string(e.message) == "Air filter is due";
    CHECK(found);
}

TEST_CASE_METHOD(
    MaintenanceModelFixture,
    "maintenance model: several due meters share one toast and keep a history row each",
    "[maintenance][model]") {
    const size_t before = NotificationHistory::instance().get_all().size();
    maintenance_model::set_interval_choice(kFilter, maintenance::interval_choice_index(25));
    maintenance_model::set_interval_choice(kNozzle, maintenance::interval_choice_index(25));
    maintenance_model::set_interval_choice(kRails, maintenance::interval_choice_index(25));
    lv_subject_set_int(state().print_state().get_print_active_subject(), 1);
    drain();
    seed();
    printed(26 * 3600);
    lv_subject_set_int(state().print_state().get_print_active_subject(), 0);
    drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0] == "3 maintenance tasks are due");
    CHECK(NotificationHistory::instance().get_all().size() == before + 3);
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: an idle load announces a due meter without a print ending",
                 "[maintenance][model]") {
    maintenance::State s = maintenance_model::state();
    s.meters[kNozzle].used_h = 600.0;
    REQUIRE(SettingsManager::instance().set_maintenance_record(prefix(), s));
    restart_model();
    CHECK(toasts.empty());

    SECTION("idle: announced once") {
        maintenance_model::check_idle_notices();
        drain();
        REQUIRE(toasts.size() == 1);
        CHECK(toasts[0] == "Nozzle is due");
        toasts.clear();
        restart_model();
        maintenance_model::check_idle_notices();
        drain();
        CHECK(toasts.empty());
    }
    SECTION("printing: held for the end of the print") {
        lv_subject_set_int(state().print_state().get_print_active_subject(), 1);
        drain();
        maintenance_model::check_idle_notices();
        drain();
        CHECK(toasts.empty());
        lv_subject_set_int(state().print_state().get_print_active_subject(), 0);
        drain();
        CHECK(toasts.size() == 1);
    }
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: a history action opens that meter's task page",
                 "[maintenance][model]") {
    auto open = helix::ui::notification_action_handler("maintenance:filter");
    REQUIRE(open);
    open();
    drain();
    CHECK(subject_text("maintenance_task_name") == "Air filter");
    CHECK(maintenance_model::task_meter() == kFilter);
    CHECK(subject_text("maintenance_task_what").find("saturated") != std::string::npos);
    CHECK(subject_text("maintenance_task_steps").rfind("1. ", 0) == 0);

    SECTION("an unknown key leaves it alone") {
        helix::ui::notification_action_handler("maintenance:nope")();
        CHECK(maintenance_model::task_meter() == kFilter);
    }
}

TEST_CASE_METHOD(
    MaintenanceModelFixture,
    "maintenance model: a reading that changes nothing shown does not bump the version",
    "[maintenance][model]") {
    seed();
    printed(2 * 3600);
    const int version = subject_int("maintenance_version");
    printed(2 * 3600 + 1);
    CHECK(maintenance_model::state().total_h > 2.0);
    CHECK(subject_int("maintenance_version") == version);
    printed(3 * 3600);
    CHECK(subject_int("maintenance_version") > version);
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: a failed save waits a full interval before the next try",
                 "[maintenance][model]") {
    Config& cfg = *Config::get_instance();
    seed();
    ConfigTestAccess::read_only_mode(cfg) = true;
    printed(7 * 60); // past the save point: the attempt fails
    ConfigTestAccess::read_only_mode(cfg) = false;
    // An attempt writes the record into memory before the disk refuses it, so the
    // in-memory record shows which readings tried.
    const auto tried = SettingsManager::instance().get_maintenance_record(prefix());
    REQUIRE(tried.has_value());
    CHECK(tried->total_h == Catch::Approx(7.0 / 60.0));

    printed(8 * 60);
    CHECK(SettingsManager::instance().get_maintenance_record(prefix())->total_h ==
          Catch::Approx(7.0 / 60.0));
    printed(13 * 60);
    CHECK(SettingsManager::instance().get_maintenance_record(prefix())->total_h ==
          Catch::Approx(13.0 / 60.0));
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: the interval dropdowns offer exactly kIntervalChoices",
                 "[maintenance][model]") {
    maintenance_model::set_interval_choice(kNozzle, maintenance::interval_choice_index(750));
    maintenance_model::set_days_choice(kRails, maintenance::day_choice_index(90));
    lv_obj_t* overlay = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "maintenance_intervals_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    MaintenanceIntervalsOverlay::apply_options(overlay);
    for (int i = 0; i < maintenance::kMeterCount; ++i) {
        INFO("meter " << i);
        lv_obj_t* hours = lv_obj_find_by_name(overlay, ("mt_hours_" + std::to_string(i)).c_str());
        REQUIRE(hours != nullptr);
        std::vector<std::string> options;
        std::stringstream lines(lv_dropdown_get_options(hours));
        for (std::string line; std::getline(lines, line);)
            options.push_back(line);
        REQUIRE(options.size() == maintenance::kIntervalChoices.size());
        CHECK(options.front() == "Off");
        for (size_t k = 1; k < options.size(); ++k)
            CHECK(options[k] == std::to_string(maintenance::kIntervalChoices[k]) + " h");
        CHECK(static_cast<int>(lv_dropdown_get_selected(hours)) ==
              maintenance::interval_choice_index(meter(i).interval_h));
        lv_obj_t* days = lv_obj_find_by_name(overlay, ("mt_days_" + std::to_string(i)).c_str());
        REQUIRE(days != nullptr);
        CHECK(lv_dropdown_get_option_count(days) == maintenance::kDayChoices.size());
    }
    CHECK(lv_dropdown_get_selected(lv_obj_find_by_name(overlay, "mt_days_0")) ==
          static_cast<uint32_t>(maintenance::day_choice_index(90)));
    lv_obj_delete(overlay);
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: seven meters fit the overview at 800x480 without scrolling",
                 "[maintenance][model]") {
    maintenance_model::set_interval_choice(kBelt, maintenance::interval_choice_index(300));
    lv_subject_t* breakpoint = lv_xml_get_subject(nullptr, "ui_breakpoint");
    REQUIRE(breakpoint != nullptr);
    const int previous = lv_subject_get_int(breakpoint);
    lv_subject_set_int(breakpoint, 3);
    lv_obj_t* overlay =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "maintenance_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_set_size(overlay, 708, 480);
    lv_obj_t* content = lv_obj_find_by_name(overlay, "overlay_content");
    REQUIRE(content != nullptr);
    lv_obj_update_layout(overlay);
    CHECK(lv_obj_get_width(lv_obj_find_by_name(overlay, "mt_cell_0")) < 400);
    CHECK(lv_obj_get_scroll_bottom(content) <= 0);
    lv_obj_delete(overlay);
    lv_subject_set_int(breakpoint, previous);
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: the belt row shows only where the model has the meter",
                 "[maintenance][model]") {
    lv_obj_t* overlay = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "maintenance_intervals_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_t* belt = lv_obj_find_by_name(overlay, "mt_interval_6");
    REQUIRE(belt != nullptr);
    CHECK(lv_obj_is_hidden(belt));
    lv_subject_t* available = lv_xml_get_subject(nullptr, "maintenance_belt_available");
    REQUIRE(available != nullptr);
    lv_subject_set_int(available, 1);
    CHECK_FALSE(lv_obj_is_hidden(belt));
    lv_obj_delete(overlay);
}

#if HELIX_HAS_PLUGINS
TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: the example plugin's meters carry over once",
                 "[maintenance][model][migration]") {
    const auto dir = std::filesystem::path(test::config_sandbox_dir()) / "plugin-data";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "maintenance-meter.json");
        f << R"({"state": {"total_h": 88.0, "meters": {
                 "filter": {"used": 80.0, "reset_at": 8.0, "notified": false},
                 "nozzle": {"used": 88.0},
                 "rails": {"used": 30.0}}}})";
    }
    Config::get_instance()->set<json>("/plugins/settings/maintenance-meter",
                                      json{{"rails_interval_h", 0}});
    // No record for this printer yet: the session that finds none takes the plugin's.
    erase_record();

    restart_model();
    CHECK(maintenance_model::state().total_h == Catch::Approx(88.0));
    CHECK(meter(kFilter).used_h == Catch::Approx(80.0));
    REQUIRE(meter(kFilter).reset_at_h.has_value());
    CHECK(meter(kNozzle).used_h == Catch::Approx(88.0));
    CHECK(meter(kRails).interval_h == 0);
    REQUIRE(SettingsManager::instance().get_maintenance_record(prefix()).has_value());

    // Once a record exists the plugin's store is not read again.
    {
        std::ofstream f(dir / "maintenance-meter.json");
        f << R"({"state": {"total_h": 999.0}})";
    }
    restart_model();
    CHECK(maintenance_model::state().total_h == Catch::Approx(88.0));
    std::filesystem::remove_all(dir);
}
#endif

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: names re-translate after the settings subjects are rebuilt",
                 "[maintenance][model][i18n]") {
    auto& settings = SystemSettingsManager::instance();
    settings.deinit_subjects();
    settings.init_subjects();
    // The widget layer initializes the model again each time it builds its subjects.
    maintenance_model::init_subjects();

    settings.set_language("ru");
    drain();
    const std::string want = lv_tr("Nozzle");
    REQUIRE(want != "Nozzle");
    const std::string name = "maintenance_m" + std::to_string(kNozzle) + "_name";
    CHECK(subject_text(name.c_str()) == want);

    settings.set_language("en");
    drain();
    CHECK(subject_text(name.c_str()) == "Nozzle");
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: a firmware filter counter drives the filter meter",
                 "[maintenance][model][chamber_air]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"purifier", "fm175xx_reader", "tool", "extruder"}));
    state().set_hardware(hw);
    // Until the printer reports its counter, print time still moves the filter.
    seed();
    printed(3600);
    CHECK(meter(kFilter).used_h == Catch::Approx(1.0));
    state().update_from_status(json{
        {"purifier", {{"power_detected", true}, {"mode", 3}, {"inner_fan_work_time", 7200.0}}}});
    drain();
    CHECK(meter(kFilter).used_h == Catch::Approx(2.0));

    // Print time no longer moves the filter; the other meters still count it.
    printed(7200);
    CHECK(meter(kFilter).used_h == Catch::Approx(2.0));
    CHECK(meter(kNozzle).used_h == Catch::Approx(2.0));

    state().update_from_status(json{{"purifier", {{"inner_fan_work_time", 10800.0}}}});
    drain();
    CHECK(meter(kFilter).used_h == Catch::Approx(3.0));
    state().set_hardware(PrinterDiscovery{});
}

TEST_CASE_METHOD(
    MaintenanceModelFixture,
    "maintenance model: resetting a firmware filter counter clears it on the printer first",
    "[maintenance][model][chamber_air]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::SNAPMAKER_U1);
    helix::test::GcodeRecordingApi api(client, state());
    helix::IMoonrakerAPI* previous = get_moonraker_api();
    set_moonraker_api(&api);
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"purifier", "fm175xx_reader", "tool", "extruder"}));
    state().set_hardware(hw);
    state().update_from_status(
        json{{"purifier", {{"power_detected", true}, {"inner_fan_work_time", 7200.0}}}});
    drain();
    REQUIRE(meter(kFilter).used_h == Catch::Approx(2.0));

    SECTION("the printer refuses: the meter keeps its hours") {
        api.fail.insert("SET_PURIFIER FAN=inner WORK=0");
        maintenance_model::reset_meter(kFilter);
        drain();
        CHECK(api.sent == std::vector<std::string>{"SET_PURIFIER FAN=inner WORK=0"});
        CHECK(meter(kFilter).used_h == Catch::Approx(2.0));
    }
    SECTION("the printer takes it: the meter starts over") {
        maintenance_model::reset_meter(kFilter);
        drain();
        CHECK(api.sent == std::vector<std::string>{"SET_PURIFIER FAN=inner WORK=0"});
        CHECK(meter(kFilter).used_h == Catch::Approx(0.0));
    }
    set_moonraker_api(previous);
    state().set_hardware(PrinterDiscovery{});
}
