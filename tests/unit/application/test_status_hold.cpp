// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Status held while the display sleeps (#2037): which frames are telemetry, how held
// deltas merge, and the order MoonrakerManager applies them in.

#include "../../test_helpers/moonraker_manager_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "application_test_fixture.h"
#include "asleep_status_wiring.h"
#include "config.h"
#include "core/observer_factory.h"
#include "core/update_queue.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_client.h"
#include "moonraker_manager.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "status_coalescer.h"

#include <random>
#include <string>
#include <vector>

#include "../../catch_amalgamated.hpp"

using namespace helix;
using nlohmann::json;

TEST_CASE("status_is_telemetry_only holds only listed attributes", "[status_hold]") {
    CHECK(status_is_telemetry_only(
        {{"extruder", {{"temperature", 210.1}}}, {"heater_bed", {{"power", 0.4}}}}));
    CHECK(status_is_telemetry_only({{"extruder1", {{"temperature", 30.0}}},
                                    {"temperature_sensor chamber", {{"temperature", 31.0}}}}));

    CHECK_FALSE(status_is_telemetry_only(json::object()));
    CHECK_FALSE(status_is_telemetry_only({{"foo_sensor bar", {{"value", 1}}}}));
    CHECK_FALSE(status_is_telemetry_only({{"extruder_stepper e", {{"temperature", 1}}}}));
    CHECK_FALSE(status_is_telemetry_only(
        {{"print_stats", {{"print_duration", 5}, {"state", "printing"}}}}));

    // Every field a wake or a safety decision reads is applied at once.
    const std::vector<json> triggers = {
        {{"print_stats", {{"state", "complete"}}}},
        {{"print_stats", {{"message", "x"}}}},
        {{"print_stats", {{"info", {{"current_layer", 3}}}}}},
        {{"webhooks", {{"state", "shutdown"}}}},
        {{"pause_resume", {{"is_paused", true}}}},
        {{"idle_timeout", {{"state", "Printing"}}}},
        {{"filament_switch_sensor runout", {{"filament_detected", false}}}},
        {{"filament_motion_sensor enc", {{"enabled", false}}}},
        {{"extruder", {{"target", 200.0}}}},
        {{"heater_bed", {{"target", 60.0}}}},
        {{"display_status", {{"message", "M117"}}}},
        {{"exclude_object", {{"current_object", "part"}}}},
    };
    for (const json& t : triggers) {
        INFO(t.dump());
        CHECK_FALSE(status_is_telemetry_only(t));
    }
}

TEST_CASE("the telemetry list changes only by a reviewed edit", "[status_hold]") {
    const std::vector<std::string_view> golden = {
        "extruder.temperature",
        "extruder.power",
        "heater_bed.temperature",
        "heater_bed.power",
        "heater_generic.temperature",
        "heater_generic.power",
        "temperature_sensor.temperature",
        "temperature_fan.temperature",
        "temperature_fan.speed",
        "fan.speed",
        "fan.rpm",
        "heater_fan.speed",
        "heater_fan.rpm",
        "controller_fan.speed",
        "controller_fan.rpm",
        "fan_generic.speed",
        "fan_generic.rpm",
        "motion_report.live_position",
        "motion_report.live_velocity",
        "motion_report.live_extruder_velocity",
        "toolhead.position",
        "toolhead.print_time",
        "toolhead.estimated_print_time",
        "gcode_move.gcode_position",
        "gcode_move.position",
        "gcode_move.speed",
        "print_stats.print_duration",
        "print_stats.total_duration",
        "print_stats.filament_used",
        "virtual_sdcard.progress",
        "virtual_sdcard.file_position",
        "display_status.progress",
        "idle_timeout.printing_time",
        "system_stats.sysload",
        "system_stats.cputime",
        "system_stats.memavail",
    };
    CHECK(helix::telemetry_attributes() == golden);
}

TEST_CASE("a merged delta applied once equals the deltas applied in turn", "[status_hold]") {
    const std::vector<std::pair<std::string, std::string>> attrs = {
        {"extruder", "temperature"},       {"heater_bed", "temperature"},  {"toolhead", "position"},
        {"print_stats", "print_duration"}, {"virtual_sdcard", "progress"}, {"fan", "speed"},
        {"motion_report", "live_velocity"}};
    std::mt19937 rng(2037);
    for (int run = 0; run < 200; ++run) {
        const json base = {{"extruder", {{"temperature", 20.0}, {"target", 0.0}}}};
        json sequential = base;
        StatusCoalescer coalescer;
        const int frames = 1 + static_cast<int>(rng() % 6);
        for (int f = 0; f < frames; ++f) {
            json delta = json::object();
            for (const auto& [obj, attr] : attrs) {
                if (rng() % 3 == 0) {
                    delta[obj][attr] = attr == "position"
                                           ? json::array({rng() % 100, rng() % 100, 1, 0})
                                           : json(static_cast<double>(rng() % 1000));
                }
            }
            if (delta.empty()) {
                delta["fan"]["speed"] = 0.5;
            }
            REQUIRE(status_is_telemetry_only(delta));
            sequential.merge_patch(delta);
            coalescer.hold(delta, 10.0 + f, 1, 0);
        }
        auto held = coalescer.take();
        REQUIRE(held);
        CHECK(held->eventtime == 10.0 + frames - 1);
        json merged = base;
        merged.merge_patch(held->status);
        CHECK(merged == sequential);
        CHECK(coalescer.empty());
    }
}

TEST_CASE("held telemetry is due an interval after the first hold", "[status_hold]") {
    StatusCoalescer c;
    CHECK_FALSE(c.due(5000, 1000));
    c.hold({{"fan", {{"speed", 0.1}}}}, 1.0, 7, 100);
    c.hold({{"fan", {{"speed", 0.2}}}}, 2.0, 7, 900);
    CHECK_FALSE(c.due(1099, 1000));
    CHECK(c.due(1100, 1000));
    CHECK(c.klippy_epoch() == 7);
    auto held = c.take();
    REQUIRE(held);
    CHECK(held->status["fan"]["speed"] == 0.2);
    CHECK(c.empty());
    CHECK_FALSE(c.due(5000, 1000));
}

namespace {

class StatusHoldFixture : public ApplicationTestFixture {
  public:
    StatusHoldFixture() {
        get_printer_state().init_subjects(false);
        REQUIRE(mgr.init(config(), nullptr));
        client = get_moonraker_client();
        REQUIRE(client != nullptr);
        send({{"heater_bed", {{"temperature", 20.0}}}, {"print_stats", {{"state", "standby"}}}});
        mgr.process_notifications();
        bed_observer = lv_subject_add_observer(bed_temp(), record_cb, &bed_values);
        bed_values.clear();
    }

    ~StatusHoldFixture() override {
        lv_observer_delete(bed_observer);
        mgr.shutdown();
        set_moonraker_api(nullptr);
        AmsState::instance().set_moonraker_api(nullptr);
        FilamentSensorManager::instance().set_moonraker_api(nullptr);
        SpoolmanManager::instance().set_api(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    static void record_cb(lv_observer_t* obs, lv_subject_t* subject) {
        static_cast<std::vector<int>*>(lv_observer_get_user_data(obs))
            ->push_back(lv_subject_get_int(subject));
    }

    void send(const json& status, bool cached = false, bool whole = false) {
        client->dispatch_status_update(status, cached, whole);
    }
    void send_bed(double temp) {
        send({{"heater_bed", {{"temperature", temp}}}});
    }
    lv_subject_t* bed_temp() {
        return get_printer_state().temperature_state().get_bed_temp_subject();
    }

    MoonrakerManager mgr;
    helix::IMoonrakerClient* client = nullptr;
    std::vector<int> bed_values;
    lv_observer_t* bed_observer = nullptr;
};

} // namespace

TEST_CASE_METHOD(StatusHoldFixture, "without hold every status frame applies",
                 "[status_hold][moonraker_manager]") {
    for (double t : {21.0, 22.0, 23.0}) {
        send_bed(t);
        mgr.process_notifications();
    }
    CHECK(bed_values == std::vector<int>{210, 220, 230});
}

TEST_CASE_METHOD(StatusHoldFixture, "held telemetry applies once per interval, latest value",
                 "[status_hold][moonraker_manager]") {
    mgr.set_status_hold(true);
    for (double t : {50.0, 51.0, 52.0, 53.0}) {
        if (t > 50.0) {
            lv_tick_inc(300);
        }
        send_bed(t);
        mgr.process_notifications();
    }
    lv_tick_inc(99); // exact steps: process_lvgl() rounds to its poll interval
    mgr.process_notifications();
    CHECK(lv_subject_get_int(bed_temp()) == 200);
    CHECK(bed_values.empty());

    lv_tick_inc(1);
    mgr.process_notifications();
    CHECK(bed_values == std::vector<int>{530});

    SECTION("turning hold off applies what is held") {
        send_bed(54.0);
        mgr.process_notifications();
        mgr.set_status_hold(false);
        CHECK(bed_values == std::vector<int>{530, 540});
    }
}

TEST_CASE_METHOD(StatusHoldFixture,
                 "a state change applies at once, after the telemetry held before it",
                 "[status_hold][moonraker_manager]") {
    auto& print = get_printer_state().print_state();
    int duration_at_edge = -1;
    auto* edge = lv_subject_add_observer(
        print.get_print_state_enum_subject(),
        [](lv_observer_t* obs, lv_subject_t*) {
            *static_cast<int*>(lv_observer_get_user_data(obs)) =
                lv_subject_get_int(get_printer_state().print_state().get_print_duration_subject());
        },
        &duration_at_edge);
    duration_at_edge = -1;

    mgr.set_status_hold(true);
    send({{"print_stats", {{"print_duration", 120.0}}}});
    send({{"print_stats", {{"state", "printing"}}}});
    mgr.process_notifications();

    CHECK(lv_subject_get_int(print.get_print_state_enum_subject()) ==
          static_cast<int>(PrintJobState::PRINTING));
    CHECK(duration_at_edge == 120);
    lv_observer_delete(edge);
}

TEST_CASE_METHOD(StatusHoldFixture,
                 "every non-telemetry notification releases the held delta first",
                 "[status_hold][moonraker_manager]") {
    mgr.set_status_hold(true);
    send_bed(60.0);
    mgr.process_notifications();
    REQUIRE(bed_values.empty());

    std::vector<int> expected = {600, 610};
    SECTION("cached snapshot") {
        send({{"heater_bed", {{"temperature", 61.0}}}}, /*cached=*/true);
    }
    SECTION("whole objects") {
        send({{"heater_bed", {{"temperature", 61.0}}}}, false, /*whole=*/true);
    }
    SECTION("new klippy session") {
        get_printer_state().network_state().reset_klippy_state_freshness();
        send_bed(61.0);
    }
    SECTION("connection state") {
        helix::MoonrakerManagerTestAccess::queue_connection_state(mgr, 2, 3);
        expected = {600};
    }
    mgr.process_notifications();
    CHECK(bed_values == expected);
}

TEST_CASE_METHOD(StatusHoldFixture, "shutdown drops held telemetry unapplied",
                 "[status_hold][moonraker_manager]") {
    mgr.set_status_hold(true);
    send_bed(70.0);
    mgr.process_notifications();
    mgr.shutdown();
    mgr.apply_held_status();
    CHECK(bed_values.empty());
    CHECK(lv_subject_get_int(bed_temp()) == 200);
}

namespace {

class AsleepWakeFixture : public StatusHoldFixture {
  public:
    AsleepWakeFixture() {
        Config* config = Config::get_instance();
        config->set<int>("/display/screensaver_type", 0);
        config->set<int>("/display/dim_sec", 600);
        config->set<int>("/display/sleep_sec", 60);
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();
        dm.set_dim_timeout(600);
        wire_asleep_status(dm, [this] { return &mgr; });
        label = lv_label_create(lv_screen_active());
        lv_display_add_event_cb(lv_display_get_default(), on_refr_start, LV_EVENT_REFR_START, this);
    }
    ~AsleepWakeFixture() override {
        lv_display_remove_event_cb_with_user_data(lv_display_get_default(), on_refr_start, this);
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();
    }

    void sleep_display() {
        lv_display_trigger_activity(nullptr);
        lv_tick_inc(61000);
        dm.check_display_sleep();
        REQUIRE(dm.is_display_sleeping());
    }

    /// What the first refresh pass after the wake starts from.
    static void on_refr_start(lv_event_t* e) {
        auto* self = static_cast<AsleepWakeFixture*>(lv_event_get_user_data(e));
        if (self->refreshes++ == 0) {
            self->text_at_first = lv_label_get_text(self->label);
            self->bed_at_first = lv_subject_get_int(self->bed_temp());
            self->pending_at_first = ui::UpdateQueue::instance().pending_count();
        }
    }

    DisplayManager dm; // no init(): software-overlay sleep on the test display
    lv_obj_t* label = nullptr;
    int refreshes = 0;
    std::string text_at_first;
    int bed_at_first = -1;
    size_t pending_at_first = 99;
};

} // namespace

TEST_CASE_METHOD(AsleepWakeFixture,
                 "the first frame after a wake shows held status and drained observers",
                 "[status_hold][display][wake_frame]") {
    sleep_display();
    send_bed(55.0);
    mgr.process_notifications();
    REQUIRE(lv_subject_get_int(bed_temp()) == 200); // held, not applied

    SubjectLifetime lifetime;
    lv_subject_t* subject = get_printer_state().temperature_state().get_bed_temp_subject(lifetime);
    ObserverGuard guard = ui::observe<int>(
        subject, label, [](lv_obj_t* l, int deci) { lv_label_set_text_fmt(l, "%d", deci); },
        lifetime);
    ui::UpdateQueue::instance().drain();
    ui::queue_update("test::queued_before_wake", [this] { lv_label_set_text(label, "queued"); });
    refreshes = 0;

    REQUIRE(dm.note_wake_press(10, 10));
    dm.check_display_sleep();
    REQUIRE_FALSE(dm.is_display_sleeping());
    CHECK(refreshes >= 1);
    CHECK(bed_at_first == 550);
    CHECK(text_at_first == "550");
    CHECK(pending_at_first == 0);
}

TEST_CASE_METHOD(AsleepWakeFixture,
                 "a wake inside a batch renders its first frame from the loop, once settled",
                 "[status_hold][display][wake_frame]") {
    sleep_display();
    send_bed(56.0);
    mgr.process_notifications();
    lv_label_set_text(label, "before");
    refreshes = 0;

    ui::queue_update("test::wake_in_batch", [this] {
        dm.wake_display();
        ui::queue_update("test::after_wake", [this] { lv_label_set_text(label, "after"); });
    });
    ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(dm.is_display_sleeping());
    CHECK(refreshes == 0);
    CHECK(lv_obj_is_hidden(lv_screen_active())); // the panel is still dark

    dm.check_display_sleep();
    CHECK(refreshes >= 1);
    CHECK(bed_at_first == 560);
    CHECK(text_at_first == "after");
    CHECK_FALSE(lv_obj_is_hidden(lv_screen_active()));
}
