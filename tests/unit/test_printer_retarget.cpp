// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_retarget.cpp
 * @brief Pointing the live connection at another printer drops what the last one left.
 */

#include "ui_update_queue.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/scoped_moonraker_client.h"
#include "../test_helpers/sound_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "m300_sound_backend.h"
#include "moonraker_manager.h"
#include "print_history_manager.h"
#include "print_start_collector.h"
#include "printer_retarget.h"
#include "printer_state.h"
#include "sound_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

class RetargetFixture : public LVGLTestFixture {
  public:
    RetargetFixture() {
        auto fake = std::make_unique<helix::test::FakeMoonrakerClient>();
        client_ = fake.get();
        helix::MoonrakerManagerTestAccess::install_client(manager_, std::move(fake));
        set_moonraker_manager(&manager_);
        installed_ = std::make_unique<ScopedMoonrakerClient>(client_);

        cfg_ = helix::Config::get_instance();
        saved_data_ = helix::ConfigTestAccess::data(*cfg_);
        saved_active_ = helix::ConfigTestAccess::active_printer_id(*cfg_);
        nlohmann::json data;
        data["config_version"] = 3;
        data["active_printer_id"] = "beta";
        data["printers"]["alpha"]["printer_name"] = "Alpha";
        data["printers"]["beta"]["printer_name"] = "Beta";
        data["printers"]["beta"]["moonraker_host"] = "10.0.0.2";
        data["printers"]["beta"]["moonraker_port"] = 7126;
        helix::ConfigTestAccess::data(*cfg_) = data;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = "beta";

        get_printer_state().init_subjects(false);
        get_printer_state().set_active_printer_name("Alpha");

        auto& ams = helix::AmsState::instance();
        ams.init_subjects(false);
        ams.set_backend(std::make_unique<helix::AmsBackendMock>());
    }

    ~RetargetFixture() override {
        helix::AmsState::instance().set_backend(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        helix::ConfigTestAccess::data(*cfg_) = saved_data_;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = saved_active_;
        installed_.reset();
        set_moonraker_manager(nullptr);
    }

    static std::string shown_name() {
        return lv_subject_get_string(get_printer_state().get_active_printer_name_subject());
    }

    helix::test::FakeMoonrakerClient* client_ = nullptr;
    MoonrakerManager manager_;

  private:
    std::unique_ptr<ScopedMoonrakerClient> installed_;
    helix::Config* cfg_ = nullptr;
    nlohmann::json saved_data_;
    std::string saved_active_;
};

} // namespace

TEST_CASE_METHOD(RetargetFixture, "Retarget: connects to the active printer as a new printer",
                 "[multi-printer][retarget]") {
    REQUIRE(helix::AmsState::instance().backend_count() == 1);
    REQUIRE(shown_name() == "Alpha");

    CHECK(helix::retarget_printer_connection());

    CHECK(client_->get_last_url() == "ws://10.0.0.2:7126/websocket");
    CHECK(helix::AmsState::instance().backend_count() == 0);
    CHECK(shown_name() == "Beta");
}

TEST_CASE_METHOD(RetargetFixture, "Reconnect: the same printer keeps its AMS backends",
                 "[multi-printer][retarget]") {
    CHECK(helix::reconnect_active_printer());

    CHECK(client_->get_last_url() == "ws://10.0.0.2:7126/websocket");
    CHECK(helix::AmsState::instance().backend_count() == 1);
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: a transport that cannot start reports failure",
                 "[multi-printer][retarget]") {
    client_->connect_result = -1;

    CHECK_FALSE(helix::retarget_printer_connection());
    CHECK(client_->get_last_url() == "ws://10.0.0.2:7126/websocket");
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: the active printer's WebSocket URL",
                 "[multi-printer][retarget]") {
    CHECK(helix::active_printer_ws_url() == "ws://10.0.0.2:7126/websocket");
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: the active printer's HTTP base URL",
                 "[multi-printer][retarget]") {
    CHECK(helix::active_printer_http_url() == "http://10.0.0.2:7126");
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: a printer with no saved host takes the default",
                 "[multi-printer][retarget]") {
    helix::ConfigTestAccess::data(*helix::Config::get_instance())["printers"]["beta"].erase(
        "moonraker_host");
    CHECK(helix::active_printer_ws_url() == "ws://:7126/websocket");
    CHECK(helix::active_printer_ws_url("localhost") == "ws://localhost:7126/websocket");
    CHECK(helix::active_printer_http_url("localhost") == "http://localhost:7126");
}

TEST_CASE_METHOD(RetargetFixture, "Connect: every connection gets a print-start collector",
                 "[multi-printer][retarget]") {
    REQUIRE(manager_.print_start_collector() == nullptr);

    REQUIRE(helix::connect_active_printer());
    const auto first = manager_.print_start_collector();
    CHECK(first != nullptr);
    CHECK(client_->get_last_url() == "ws://10.0.0.2:7126/websocket");

    // A switch's connect replaces it: the collector reads the new printer's profile. The
    // old one is stopped and unhooked from the client, which a retarget keeps: left
    // registered, a collector replaced mid-PRINT_START would keep collecting from the
    // next printer's gcode.
    first->start();
    REQUIRE(first->is_active());
    REQUIRE(client_->method_callbacks["notify_gcode_response"].size() == 1);
    REQUIRE(helix::retarget_printer_connection());
    CHECK(manager_.print_start_collector() != nullptr);
    CHECK(manager_.print_start_collector() != first);
    CHECK_FALSE(first->is_active());
    CHECK(client_->method_callbacks["notify_gcode_response"].empty());
}

TEST_CASE_METHOD(RetargetFixture, "Connect: a transport that cannot start gets no collector",
                 "[multi-printer][retarget]") {
    client_->connect_result = -1;

    CHECK_FALSE(helix::connect_active_printer());
    CHECK(manager_.print_start_collector() == nullptr);
}

TEST_CASE_METHOD(RetargetFixture,
                 "Retarget: the previous printer's job, message, temperatures and history go",
                 "[multi-printer][retarget]") {
    // An unreachable new printer never sends a status, so what A left must not stand in for it.
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, helix::PrintJobState::COMPLETE);
    ps.update_from_status(
        nlohmann::json{{"display_status", {{"message", "Your part is finished!"}}},
                       {"heater_bed", {{"temperature", 61.0}, {"target", 60.0}}}});
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(ps.print_state().get_print_job_state() == helix::PrintJobState::COMPLETE);
    REQUIRE(std::string(lv_subject_get_string(ps.print_state().get_display_message_subject())) ==
            "Your part is finished!");
    REQUIRE(lv_subject_get_int(ps.temperature_state().get_bed_temp_subject()) != 0);

    PrintHistoryManager history(nullptr, nullptr);
    PrintHistoryJob job;
    job.filename = "benchy.gcode";
    helix::PrintHistoryManagerTestAccess::set_loaded_jobs(history, {job});
    set_print_history_manager(&history);
    REQUIRE_FALSE(history.get_jobs().empty());

    helix::retarget_printer_connection();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(ps.print_state().get_print_job_state() == helix::PrintJobState::STANDBY);
    CHECK(
        std::string(lv_subject_get_string(ps.print_state().get_display_message_subject())).empty());
    CHECK(lv_subject_get_int(ps.temperature_state().get_bed_temp_subject()) == 0);
    CHECK(history.get_jobs().empty());
    set_print_history_manager(nullptr);
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: the previous printer's M300 beeper is dropped",
                 "[multi-printer][retarget]") {
    auto& sound = helix::SoundManager::instance();
    sound.set_moonraker_client(client_);
    helix::SoundManagerTestAccess::install_backend(
        sound, std::make_shared<M300SoundBackend>([](const std::string&) { return 0; }));
    REQUIRE(helix::SoundManagerTestAccess::backend(sound)->needs_moonraker_client());

    CHECK(helix::retarget_printer_connection());

    auto after = helix::SoundManagerTestAccess::backend(sound);
    CHECK((after == nullptr || !after->needs_moonraker_client()));
    sound.shutdown();
    sound.set_moonraker_client(nullptr);
}
