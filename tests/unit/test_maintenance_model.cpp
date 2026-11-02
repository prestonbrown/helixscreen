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
#include "ui_toast_manager.h"

#include "../helix_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "config.h"
#include "lvgl.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/maintenance_model.h"
#include "static_subject_registry.h"
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
constexpr int kRails = static_cast<int>(maintenance::MeterId::Rails);

/// Brings the model up fresh against this test's sandboxed config: the model and its
/// observers belong to whichever PrinterState subjects existed when it last initialized.
struct MaintenanceModelFixture : public LVGLUITestFixture {
    MaintenanceModelFixture() {
        // A model another test left up saves into this config on its way down.
        StaticSubjectRegistry::instance().deinit_one("MaintenanceModelSubjects");
        erase_record();
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

    /// A new session: whatever the config holds now is what loads.
    void restart_model() {
        StaticSubjectRegistry::instance().deinit_one("MaintenanceModelSubjects");
        maintenance_model::init_subjects();
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
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
    CHECK(subject_text("maintenance_m0_left") == "200 h");
    CHECK(subject_text("maintenance_total_text") == "Total print time: 0 h");
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
    CHECK(subject_text("maintenance_m0_line") == "1 h of 200 h");

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
    lv_obj_t* card = lv_obj_find_by_name(overlay, "mt_card_1");
    lv_obj_t* row = lv_obj_find_by_name(tile, "mt_row_1");
    REQUIRE(card != nullptr);
    REQUIRE(row != nullptr);
    CHECK_FALSE(lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    maintenance_model::set_interval_choice(kNozzle, 0);
    CHECK(meter(kNozzle).interval_h == 0);
    CHECK(subject_int("maintenance_m1_on") == 0);
    CHECK(lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    // The interval row stays, so the meter can be turned back on.
    lv_obj_t* interval_row = lv_obj_find_by_name(overlay, "row_interval_1");
    REQUIRE(interval_row != nullptr);
    CHECK(shown(interval_row));

    const auto saved = SettingsManager::instance().get_maintenance_record(prefix());
    REQUIRE(saved.has_value());
    CHECK(saved->meters[kNozzle].interval_h == 0);

    seed();
    printed(3600);
    CHECK(meter(kNozzle).used_h == 0.0);
    CHECK(meter(kFilter).used_h == Catch::Approx(1.0));

    SECTION("every meter off: the tile says so") {
        maintenance_model::set_interval_choice(kFilter, 0);
        maintenance_model::set_interval_choice(kRails, 0);
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
    CHECK(subject_int("maintenance_m2_interval") == maintenance::interval_choice_index(300));
    maintenance_model::set_interval_choice(kRails, maintenance::interval_choice_index(1000));
    CHECK(meter(kRails).interval_h == 1000);
    CHECK(subject_int("maintenance_m2_interval") == maintenance::interval_choice_index(1000));
    CHECK(subject_text("maintenance_m2_left") == "1000 h");

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
    CHECK(subject_text("maintenance_m0_note") == "100% left · reset at 7 h");
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "HEPA filter meter reset");

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
    CHECK(subject_text("maintenance_m1_left") == "627 h");
}

TEST_CASE_METHOD(MaintenanceModelFixture,
                 "maintenance model: passing an interval raises one notice and turns it Danger",
                 "[maintenance][model]") {
    maintenance_model::set_interval_choice(kFilter, maintenance::interval_choice_index(25));
    seed();
    printed(24 * 3600);
    CHECK(subject_int("maintenance_m0_status") == static_cast<int>(ValueStatus::Warn));
    CHECK(toasts.empty());

    printed(25 * 3600);
    CHECK(subject_int("maintenance_m0_status") == static_cast<int>(ValueStatus::Danger));
    CHECK(subject_int("maintenance_worst_status") == static_cast<int>(ValueStatus::Danger));
    CHECK(subject_int("maintenance_worst_pct") == 100);
    CHECK(subject_text("maintenance_worst_name") == "HEPA");
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0] == "HEPA filter is due: 25 h reached");
    // The latch reaches the record with the notice, so a restart does not repeat it.
    const auto saved = SettingsManager::instance().get_maintenance_record(prefix());
    REQUIRE(saved.has_value());
    CHECK(saved->meters[kFilter].notified);

    printed(30 * 3600);
    CHECK(toasts.size() == 1);
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
    lv_obj_t* overlay =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "maintenance_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    MaintenanceOverlay::apply_interval_options(overlay);
    for (int i = 0; i < maintenance::kMeterCount; ++i) {
        INFO("meter " << i);
        const std::string row_name = "row_interval_" + std::to_string(i);
        lv_obj_t* row = lv_obj_find_by_name(overlay, row_name.c_str());
        REQUIRE(row != nullptr);
        lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
        REQUIRE(dropdown != nullptr);
        std::vector<std::string> options;
        std::stringstream lines(lv_dropdown_get_options(dropdown));
        for (std::string line; std::getline(lines, line);)
            options.push_back(line);
        REQUIRE(options.size() == maintenance::kIntervalChoices.size());
        CHECK(options.front() == "Off");
        for (size_t k = 1; k < options.size(); ++k)
            CHECK(options[k] == std::to_string(maintenance::kIntervalChoices[k]) + " h");
        CHECK(static_cast<int>(lv_dropdown_get_selected(dropdown)) ==
              maintenance::interval_choice_index(meter(i).interval_h));
    }
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
