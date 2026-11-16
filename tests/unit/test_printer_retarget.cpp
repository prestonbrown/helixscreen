// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_retarget.cpp
 * @brief Pointing the live connection at another printer drops what the last one left.
 */

#include "ui_print_start_controller.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/scoped_moonraker_client.h"
#include "../test_helpers/sound_manager_test_access.h"
#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "core/data_root_resolver.h"
#include "core/update_queue.h"
#include "m300_sound_backend.h"
#include "moonraker_manager.h"
#include "print_history_manager.h"
#include "print_start_collector.h"
#include "print_stop_reason.h"
#include "printer_discovery.h"
#include "printer_retarget.h"
#include "printer_state.h"
#include "sound_manager.h"

#include <filesystem>
#include <fstream>
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

    /// The client's notify_gcode_response taps named @p handler.
    size_t gcode_taps(const std::string& handler) const {
        size_t n = 0;
        for (const auto& cb : client_->method_callbacks["notify_gcode_response"]) {
            n += cb.handler_name == handler ? 1 : 0;
        }
        return n;
    }

    /// The taps that are not the stop-reason tracker's.
    size_t collector_taps() const {
        return client_->method_callbacks["notify_gcode_response"].size() -
               gcode_taps(helix::PrintStopReasonTracker::kHandlerName);
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
    REQUIRE(collector_taps() == 1);
    REQUIRE(helix::retarget_printer_connection());
    CHECK(manager_.print_start_collector() != nullptr);
    CHECK(manager_.print_start_collector() != first);
    CHECK_FALSE(first->is_active());
    // The new collector listens from connect on, for console lines that announce a
    // printer-side start; the old one's tap is gone, or there would be two.
    CHECK(collector_taps() == 1);
}

TEST_CASE_METHOD(RetargetFixture, "Connect: every connection tracks why its prints stop",
                 "[multi-printer][retarget][stop_reason]") {
    REQUIRE(gcode_taps(helix::PrintStopReasonTracker::kHandlerName) == 0);

    REQUIRE(helix::connect_active_printer());
    CHECK(gcode_taps(helix::PrintStopReasonTracker::kHandlerName) == 1);

    // A switch replaces the tracker; the old one's tap goes with it.
    REQUIRE(helix::retarget_printer_connection());
    CHECK(gcode_taps(helix::PrintStopReasonTracker::kHandlerName) == 1);
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
    helix::ui::UpdateQueue::instance().drain_until_empty();
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
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(ps.print_state().get_print_job_state() == helix::PrintJobState::STANDBY);
    CHECK(
        std::string(lv_subject_get_string(ps.print_state().get_display_message_subject())).empty());
    CHECK(lv_subject_get_int(ps.temperature_state().get_bed_temp_subject()) == 0);
    CHECK(history.get_jobs().empty());
    set_print_history_manager(nullptr);
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: the previous printer's chamber air controls go",
                 "[multi-printer][retarget][chamber_air]") {
    auto& ps = get_printer_state();
    helix::PrinterDiscovery u1;
    u1.parse_objects(nlohmann::json::array({"purifier", "fm175xx_reader", "tool"}));
    ps.chamber_air().set_hardware(u1);
    ps.chamber_air().apply(helix::chamber_air::read_status(
        u1, nlohmann::json{{"purifier", {{"power_detected", true}, {"mode", 1}}}}));
    REQUIRE(lv_subject_get_int(ps.chamber_air().available_subject()) == 1);

    helix::retarget_printer_connection();
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(lv_subject_get_int(ps.chamber_air().available_subject()) == 0);
    CHECK(lv_subject_get_int(ps.chamber_air().mode_subject()) == -1);
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

namespace {
/// An AFC backend that counts the tool-map writes reaching it, in a count that
/// outlives the backend.
class TallyAfcBackend : public helix::AmsBackendAfc {
  public:
    explicit TallyAfcBackend(std::shared_ptr<int> writes)
        : AmsBackendAfc(nullptr, nullptr), writes_(std::move(writes)) {}
    helix::AmsError can_set_tool_mapping(int, int) const override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError set_tool_mapping_impl(int, int) override {
        ++*writes_;
        return helix::AmsErrorHelper::success();
    }
    std::vector<int> get_tool_mapping() const override {
        return {1, 2};
    }

  private:
    std::shared_ptr<int> writes_;
};
} // namespace

TEST_CASE_METHOD(RetargetFixture,
                 "Retarget: a replay waiting on a report never writes to the previous printer",
                 "[multi-printer][retarget][remap-restart]") {
    helix::ConfigDirGuard config_dir{"retarget_remap"};
    const auto record = std::filesystem::path(helix::get_user_config_dir()) / "pending_remap.json";
    std::filesystem::create_directories(record.parent_path());
    {
        std::ofstream ofs(record);
        ofs << R"({"backend_index":0,"backend_type":2,"tool_mapping":[2,1]})";
    }
    auto writes = std::make_shared<int>(0);
    helix::AmsState::instance().set_backend(std::make_unique<TallyAfcBackend>(writes));
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    helix::ui::PrintStartController controller{get_printer_state(), nullptr};
    controller.recover_pending_remap(helix::ReportedPrintActivity::Unknown);

    // The neutral standby the switch writes is not the previous printer's report;
    // its backends are gone before that report can restore onto them.
    helix::retarget_printer_connection();
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(*writes == 0);
    helix::AmsState::instance().clear_pending_tool_map_restore();
}
