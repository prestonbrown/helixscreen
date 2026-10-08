// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_error_path_integration.cpp
 * @brief What the user sees after the link, the file or the hardware goes wrong (#1312).
 *
 * Each case drives the public path (status frames, connection edges, Config load)
 * and asserts the user-visible state: subjects, never a backend's internals.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "app_globals.h"
#include "connection_state.h"
#include "printer_state.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/registered_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

void drain() {
    helix::ui::UpdateQueue::instance().drain();
}

nlohmann::json status_frame(const nlohmann::json& status, double eventtime) {
    return {{"method", "notify_status_update"},
            {"params", nlohmann::json::array({status, eventtime})}};
}

// ---------------------------------------------------------------------------
// Link loss during a print
// ---------------------------------------------------------------------------

class LinkLossFixture : public LVGLTestFixture {
  public:
    LinkLossFixture() {
        PrinterStateTestAccess::reset(state());
        state().init_subjects(false);
    }
    ~LinkLossFixture() override {
        drain();
        PrinterStateTestAccess::reset(state());
    }

    PrinterState& state() {
        return get_printer_state();
    }

    void frame(const nlohmann::json& status, double eventtime) {
        auto notification = status_frame(status, eventtime);
        if (auto parsed = parse_status_notification(notification)) {
            state().update_from_status(*parsed->status, parsed->eventtime,
                                       parsed->from_cached_snapshot,
                                       state().network_state().klippy_epoch());
        }
        drain();
    }

    PrintJobState job_state() {
        return state().print_state().get_print_job_state();
    }

    void set_connection(ConnectionState s) {
        state().set_printer_connection_state(static_cast<int>(s), "test");
        drain();
    }

    int connection() {
        return lv_subject_get_int(state().network_state().get_printer_connection_state_subject());
    }
};

} // namespace

TEST_CASE_METHOD(LinkLossFixture,
                 "a printer that went idle during an outage does not read as printing",
                 "[integration][error_path][reconnect]") {
    frame({{"webhooks", {{"state", "ready"}}},
           {"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}},
           {"virtual_sdcard", {{"progress", 0.5}}}},
          100.0);
    set_connection(ConnectionState::CONNECTED);
    REQUIRE(job_state() == PrintJobState::PRINTING);

    // The socket drops; the print finishes (or is cancelled at the printer)
    // while nobody is listening.
    set_connection(ConnectionState::RECONNECTING);
    CHECK(connection() == static_cast<int>(ConnectionState::RECONNECTING));

    // Reconnect: the discovery snapshot reports an idle printer.
    set_connection(ConnectionState::CONNECTED);
    frame({{"webhooks", {{"state", "ready"}}},
           {"print_stats", {{"state", "standby"}, {"filename", ""}}},
           {"virtual_sdcard", {{"progress", 0.0}}}},
          101.0);

    CHECK(connection() == static_cast<int>(ConnectionState::CONNECTED));
    CHECK(job_state() == PrintJobState::STANDBY);
    CHECK_FALSE(printer_has_job(job_state()));
    CHECK(lv_subject_get_int(state().print_state().get_print_progress_subject()) == 0);
}

TEST_CASE_METHOD(LinkLossFixture,
                 "a print that is still running after the outage keeps reading as printing",
                 "[integration][error_path][reconnect]") {
    frame({{"webhooks", {{"state", "ready"}}},
           {"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}},
          100.0);
    set_connection(ConnectionState::RECONNECTING);
    set_connection(ConnectionState::CONNECTED);

    frame({{"webhooks", {{"state", "ready"}}},
           {"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}},
           {"virtual_sdcard", {{"progress", 0.8}}}},
          101.0);

    CHECK(job_state() == PrintJobState::PRINTING);
    CHECK(lv_subject_get_int(state().print_state().get_print_progress_subject()) == 80);
}

// ---------------------------------------------------------------------------
// AMS: the hardware disagrees with what the UI holds
// ---------------------------------------------------------------------------

namespace {

struct AmsDesyncFixture : LVGLTestFixture {
    AmsDesyncFixture() {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);
    }
    ~AmsDesyncFixture() override {
        drain();
        AmsState::instance().clear_backends();
        drain();
        AmsState::instance().deinit_subjects();
    }

    static void afc_frame(AmsBackendAfc& backend, const nlohmann::json& params) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        AfcTestAccess::handle_status_update(backend, notification);
        drain();
    }

    static int value(lv_subject_t* subject) {
        return lv_subject_get_int(subject);
    }
};

} // namespace

TEST_CASE_METHOD(AmsDesyncFixture,
                 "AMS: firmware unloading behind a loaded UI clears the loaded lane",
                 "[integration][error_path][ams][afc]") {
    helix::test::RegisteredBackend<AmsBackendAfc> afc(nullptr, nullptr);
    AfcTestAccess::initialize_slots(*afc, std::vector<std::string>{"lane1", "lane2"});
    auto& ams = AmsState::instance();

    afc_frame(*afc, {{"AFC", {{"current_load", "lane2"}, {"current_state", "Idle"}}}});
    REQUIRE(value(ams.get_current_slot_subject()) == 1);
    REQUIRE(value(ams.get_filament_loaded_subject()) == 1);

    afc_frame(*afc, {{"AFC", {{"current_load", nullptr}, {"current_state", "Idle"}}}});
    CHECK(value(ams.get_current_slot_subject()) == -1);
    CHECK(value(ams.get_filament_loaded_subject()) == 0);
}

TEST_CASE_METHOD(AmsDesyncFixture,
                 "AMS: a frame naming a different lane moves the loaded lane, not both",
                 "[integration][error_path][ams][afc]") {
    helix::test::RegisteredBackend<AmsBackendAfc> afc(nullptr, nullptr);
    AfcTestAccess::initialize_slots(*afc, std::vector<std::string>{"lane1", "lane2"});
    auto& ams = AmsState::instance();

    afc_frame(*afc, {{"AFC", {{"current_load", "lane1"}}}});
    REQUIRE(value(ams.get_current_slot_subject()) == 0);

    afc_frame(*afc, {{"AFC", {{"current_load", "lane2"}}}});
    CHECK(value(ams.get_current_slot_subject()) == 1);
    CHECK(value(ams.get_filament_loaded_subject()) == 1);
}

TEST_CASE_METHOD(AmsDesyncFixture, "AMS: a busy UI returns to idle when firmware reports idle",
                 "[integration][error_path][ams][afc]") {
    helix::test::RegisteredBackend<AmsBackendAfc> afc(nullptr, nullptr);
    AfcTestAccess::initialize_slots(*afc, std::vector<std::string>{"lane1", "lane2"});
    auto& ams = AmsState::instance();

    afc_frame(*afc, {{"AFC", {{"current_state", "Loading"}, {"current_load", "lane1"}}}});
    REQUIRE(value(ams.get_ams_action_subject()) == static_cast<int>(AmsAction::LOADING));

    // The load never completed: firmware is idle and nothing is loaded.
    afc_frame(*afc, {{"AFC", {{"current_state", "Idle"}, {"current_load", nullptr}}}});
    CHECK(value(ams.get_ams_action_subject()) == static_cast<int>(AmsAction::IDLE));
    CHECK(value(ams.get_filament_loaded_subject()) == 0);
}

TEST_CASE_METHOD(AmsDesyncFixture,
                 "AMS: a frame for a lane the UI never discovered changes nothing",
                 "[integration][error_path][ams][afc]") {
    helix::test::RegisteredBackend<AmsBackendAfc> afc(nullptr, nullptr);
    AfcTestAccess::initialize_slots(*afc, std::vector<std::string>{"lane1", "lane2"});
    auto& ams = AmsState::instance();
    afc_frame(*afc, {{"AFC_stepper lane1", {{"prep", true}, {"status", "Loaded"}}}});
    const int slots_before = value(ams.get_slot_count_subject());
    REQUIRE(slots_before == 2);

    afc_frame(*afc, {{"AFC_stepper lane9", {{"prep", true}, {"status", "Loaded"}}},
                     {"AFC", {{"current_load", "lane9"}}}});

    CHECK(value(ams.get_slot_count_subject()) == slots_before);
    CHECK(value(ams.get_current_slot_subject()) < slots_before);
}

TEST_CASE_METHOD(AmsDesyncFixture, "AMS: losing the whole backend resets the UI to empty",
                 "[integration][error_path][ams]") {
    auto& ams = AmsState::instance();
    {
        helix::test::RegisteredBackend<AmsBackendAfc> afc(nullptr, nullptr);
        AfcTestAccess::initialize_slots(*afc, std::vector<std::string>{"lane1", "lane2"});
        afc_frame(*afc, {{"AFC", {{"current_load", "lane1"}, {"current_state", "Loading"}}}});
        REQUIRE(value(ams.get_filament_loaded_subject()) == 1);
    }

    CHECK(value(ams.get_slot_count_subject()) == 0);
    CHECK(value(ams.get_filament_loaded_subject()) == 0);
    CHECK(value(ams.get_current_slot_subject()) == -1);
    CHECK(value(ams.get_ams_action_subject()) == static_cast<int>(AmsAction::IDLE));
}
