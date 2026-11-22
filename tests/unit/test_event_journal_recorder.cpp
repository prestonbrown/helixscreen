// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/loopback_http_server.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "event_journal_recorder.h"
#include "gcode_response_routing.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_event_journal.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include <cstdio>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace helix {
struct EventJournalRecorderTestAccess {
    static void connected(EventJournalRecorder& r) {
        r.on_connected();
    }
};
} // namespace helix

namespace {

json notify(const json& payload) {
    return json{{"params", json::array({payload})}};
}

json history(const char* action, const char* status = "in_progress") {
    return notify(
        {{"action", action},
         {"job", {{"job_id", "00002A"}, {"filename", "benchy.gcode"}, {"status", status}}}});
}

/// A FlashForge printer running ZMOD, whose IFS faults only its own wording rows read.
PrinterDiscovery zmod_printer() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"zmod_ifs", "extruder", "heater_bed"}));
    return hw;
}

class RecorderFixture : public LVGLTestFixture {
  public:
    RecorderFixture()
        : path_("/tmp/helix_event_journal_recorder_" + std::to_string(::getpid()) + ".json") {
        std::remove(path_.c_str());
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        handlers_before_ = client_.live_handlers();
        recorder_ = std::make_unique<EventJournalRecorder>(&client_, journal_, path_);
        settle();
    }

    ~RecorderFixture() override {
        recorder_.reset();
        get_printer_state().set_hardware(PrinterDiscovery{});
        auto& plugin = get_printer_state().plugin_status_state();
        plugin.set_installed(false);
        plugin.set_features({});
        test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        settle();
        std::remove(path_.c_str());
    }

    static void settle() {
        ui::UpdateQueue::instance().drain_until_empty();
    }

    std::vector<std::string> kinds() const {
        std::vector<std::string> out;
        for (const auto& e : journal_.snapshot()) {
            out.push_back(e.kind);
        }
        return out;
    }

    const test::FakeMoonrakerClient::RpcCall* rpc(const std::string& method) const {
        for (const auto& call : client_.rpc_calls) {
            if (call.method == method) {
                return &call;
            }
        }
        return nullptr;
    }

    std::string path_;
    test::FakeMoonrakerClient client_;
    LocalEventJournal journal_;
    std::vector<std::pair<std::string, std::string>> handlers_before_;
    std::unique_ptr<EventJournalRecorder> recorder_;
};

} // namespace

TEST_CASE_METHOD(RecorderFixture, "recorder keeps live events with the open job",
                 "[plugin_event_journal][event_journal_recorder]") {
    REQUIRE(client_.fire_notification("notify_history_changed", history("added")));
    REQUIRE(client_.fire_notification("notify_gcode_response",
                                      json{{"params", {"!! Move out of range: 1 2 3", "ok"}}}));
    REQUIRE(client_.fire_notification("notify_klippy_disconnected", json::object()));
    REQUIRE(client_.fire_notification("notify_klippy_ready", json::object()));
    REQUIRE(client_.fire_notification("notify_history_changed", history("finished", "cancelled")));
    REQUIRE(client_.fire_notification("notify_gcode_response",
                                      json{{"params", {"!! Must home axis first"}}}));
    settle();

    REQUIRE(kinds() == std::vector<std::string>{"gcode_error", "klippy_disconnect", "klippy_ready",
                                                "job_end", "gcode_error"});
    const auto held = journal_.snapshot();
    CHECK(held[0].text == "Move out of range: 1 2 3");
    CHECK(held[0].cls == "move");
    CHECK(held[0].job_id == "00002A");
    CHECK(held[3].detail == "cancelled");
    CHECK(held[3].filename == "benchy.gcode");
    CHECK(held[4].job_id.empty());

    LocalEventJournal reloaded;
    REQUIRE(reloaded.load_file(path_, held.back().time));
    CHECK(reloaded.snapshot().size() == held.size());
}

TEST_CASE_METHOD(RecorderFixture, "recorder reads the shutdown reason Klipper reports",
                 "[plugin_event_journal][event_journal_recorder]") {
    client_.fire_notification("notify_history_changed", history("added"));
    REQUIRE(client_.fire_notification("notify_klippy_shutdown", json::object()));
    settle();
    const auto* query = rpc("printer.objects.query");
    REQUIRE(query != nullptr);
    CHECK(query->params["objects"].contains("webhooks"));
    // History closes the job before the query answers.
    client_.fire_notification("notify_history_changed", history("finished", "klippy_shutdown"));
    settle();

    json webhooks = {{"state_message", "Lost communication with MCU 'mcu'"}};
    json status = {{"webhooks", webhooks}};
    query->success_cb(json{{"result", {{"status", status}}}});
    settle();

    const auto held = journal_.snapshot();
    REQUIRE(held.size() == 2);
    const auto& fault = held[0].kind == journal_kind::kJobEnd ? held[1] : held[0];
    CHECK(fault.kind == journal_kind::kMcuDisconnect);
    CHECK(fault.text == "Lost communication with MCU 'mcu'");
    CHECK(fault.job_id == "00002A");
}

TEST_CASE_METHOD(RecorderFixture, "recorder records a pause with its cause",
                 "[plugin_event_journal][event_journal_recorder]") {
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    settle();
    client_.fire_notification(
        "notify_gcode_response",
        json{{"params", {"!! Heater extruder not heating at expected rate"}}});
    settle();
    test::set_wire_state(get_printer_state(), PrintJobState::PAUSED);
    settle();

    REQUIRE(kinds() == std::vector<std::string>{"gcode_error", "pause"});
    CHECK(journal_.snapshot()[1].detail == "error");
}

TEST_CASE_METHOD(RecorderFixture, "recorder stands down while the plugin's journal is ready",
                 "[plugin_event_journal][event_journal_recorder]") {
    auto& plugin = get_printer_state().plugin_status_state();
    PluginFeatureMap features;
    features[kJournalFeatureName] = PluginFeatureEntry{1, true, true, {}, ""};
    plugin.set_features(features);
    plugin.set_installed(true);
    REQUIRE(plugin.plugin_feature(kJournalFeatureName, kJournalFeatureMinVersion) ==
            PluginFeatureState::Ready);

    client_.fire_notification("notify_klippy_ready", json::object());
    client_.fire_notification("notify_gcode_response", json{{"params", {"!! Must home"}}});
    settle();
    EventJournalRecorderTestAccess::connected(*recorder_);

    CHECK(journal_.snapshot().empty());
    CHECK(rpc("server.history.list") == nullptr);
}

TEST_CASE_METHOD(RecorderFixture, "recorder backfills the reconnect gap from history",
                 "[plugin_event_journal][event_journal_recorder]") {
    client_.fire_notification("notify_klippy_ready", json::object());
    settle();
    const double since = journal_.latest_time();

    EventJournalRecorderTestAccess::connected(*recorder_);
    const auto* list = rpc("server.history.list");
    REQUIRE(list != nullptr);
    CHECK(list->params["since"].get<double>() == since);

    json job = {{"job_id", "00002B"},
                {"filename", "gap.gcode"},
                {"status", "completed"},
                {"end_time", since + 30}};
    json result = {{"jobs", json::array({job})}};
    list->success_cb(json{{"result", result}});
    settle();

    const auto held = journal_.snapshot();
    REQUIRE(held.size() == 2);
    CHECK(held[1].kind == journal_kind::kJobEnd);
    CHECK(held[1].recovered);
    CHECK(held[1].filename == "gap.gcode");
}

TEST_CASE_METHOD(RecorderFixture, "recorder backfills from a bounded slice of the console history",
                 "[plugin_event_journal][event_journal_recorder][2053]") {
    client_.fire_notification("notify_klippy_ready", json::object());
    settle();
    EventJournalRecorderTestAccess::connected(*recorder_);

    // Moonraker returns only the latest N entries, so N bounds the reply size: a whole
    // 1000-entry store of verbose macro output can outgrow the client's frame cap.
    REQUIRE(client_.last_gcode_store_count > 0);
    CHECK(client_.last_gcode_store_count <= 200);
}

TEST_CASE_METHOD(RecorderFixture, "recorder classifies a live error by the printer's firmware",
                 "[plugin_event_journal][event_journal_recorder][error-wording]") {
    get_printer_state().set_hardware(zmod_printer());
    REQUIRE(client_.fire_notification("notify_gcode_response",
                                      json{{"params", {"!! No filament 2 in IFS"}}}));
    settle();

    const auto held = journal_.snapshot();
    REQUIRE(held.size() == 1);
    CHECK(held[0].kind == journal_kind::kGcodeError);
    CHECK(held[0].cls == "filament");
}

TEST_CASE_METHOD(RecorderFixture, "recorder classifies a shutdown reason by the printer's firmware",
                 "[plugin_event_journal][event_journal_recorder][error-wording]") {
    get_printer_state().set_hardware(zmod_printer());
    REQUIRE(client_.fire_notification("notify_klippy_shutdown", json::object()));
    settle();
    const auto* query = rpc("printer.objects.query");
    REQUIRE(query != nullptr);

    json status = {{"webhooks", {{"state_message", "IFS driver failure"}}}};
    query->success_cb(json{{"result", {{"status", status}}}});
    settle();

    const auto held = journal_.snapshot();
    REQUIRE(held.size() == 1);
    CHECK(held[0].kind == journal_kind::kShutdown);
    CHECK(held[0].cls == "filament");
}

TEST_CASE_METHOD(RecorderFixture,
                 "recorder classifies a console backfill by the printer's firmware",
                 "[plugin_event_journal][event_journal_recorder][error-wording]") {
    get_printer_state().set_hardware(zmod_printer());
    client_.fire_notification("notify_klippy_ready", json::object());
    settle();
    const double since = journal_.latest_time();
    EventJournalRecorderTestAccess::connected(*recorder_);
    REQUIRE(client_.last_gcode_store_cb);

    client_.last_gcode_store_cb({{"!! No filament 2 in IFS", since + 5, "response"}});
    settle();

    const auto held = journal_.snapshot();
    REQUIRE(held.size() == 2);
    CHECK(held[1].kind == journal_kind::kGcodeError);
    CHECK(held[1].recovered);
    CHECK(held[1].cls == "filament");
}

namespace {

/// Moonraker's file endpoint serving one klippy.log body.
class KlippyLogServer : public test::LoopbackHttpServer {
  public:
    explicit KlippyLogServer(std::string log) : log_(std::move(log)) {
        service().GET("/server/files/klippy.log", [this](HttpRequest*, HttpResponse* resp) {
            resp->body = log_;
            return 200;
        });
        start();
    }

  private:
    std::string log_;
};

/// Installs an API as the app's for the life of the object.
class ScopedMoonrakerApi {
  public:
    explicit ScopedMoonrakerApi(IMoonrakerAPI* api) : previous_(get_moonraker_api()) {
        set_moonraker_api(api);
    }
    ~ScopedMoonrakerApi() {
        set_moonraker_api(previous_);
    }
    ScopedMoonrakerApi(const ScopedMoonrakerApi&) = delete;
    ScopedMoonrakerApi& operator=(const ScopedMoonrakerApi&) = delete;

  private:
    IMoonrakerAPI* previous_;
};

} // namespace

TEST_CASE_METHOD(RecorderFixture,
                 "recorder classifies a klippy.log backfill by the printer's firmware",
                 "[plugin_event_journal][event_journal_recorder][error-wording]") {
    get_printer_state().set_hardware(zmod_printer());
    client_.fire_notification("notify_klippy_ready", json::object());
    settle();
    const double since = journal_.latest_time();

    // Klipper started after the last entry, so its shutdown falls in the gap.
    const std::string start_wall = std::to_string(since + 10);
    KlippyLogServer server("Start printer at Tue Oct 21 10:00:00 2026 (" + start_wall +
                           " 1000.0)\n"
                           "Stats 1005.0: gcodein=0 mcu: mcu_awake=0.001\n"
                           "Transition to shutdown state: IFS driver failure\n");
    REQUIRE(server.started());
    MoonrakerClientMock api_client;
    PrinterState api_state;
    MoonrakerAPIMock api(api_client, api_state);
    api.set_http_base_url(server.url(""));
    ScopedMoonrakerApi installed(&api);

    EventJournalRecorderTestAccess::connected(*recorder_);
    const auto* list = rpc("server.history.list");
    REQUIRE(list != nullptr);
    json job = {{"job_id", "00002B"},
                {"filename", "gap.gcode"},
                {"status", "klippy_shutdown"},
                {"end_time", since + 20}};
    list->success_cb(json{{"result", {{"jobs", json::array({job})}}}});

    const auto shutdown_recorded = [this] {
        settle();
        for (const auto& e : journal_.snapshot()) {
            if (e.kind == journal_kind::kShutdown) {
                return true;
            }
        }
        return false;
    };
    REQUIRE(wait_until(shutdown_recorded));

    for (const auto& e : journal_.snapshot()) {
        if (e.kind == journal_kind::kShutdown) {
            CHECK(e.text == "IFS driver failure");
            CHECK(e.cls == "filament");
        }
    }
}

TEST_CASE_METHOD(RecorderFixture, "recorder leaves no handler behind",
                 "[plugin_event_journal][event_journal_recorder]") {
    CHECK(client_.handler_count("notify_gcode_response", "event_journal") == 1);
    recorder_.reset();
    CHECK(client_.live_handlers() == handlers_before_);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a printer scope's routing records the journal and stops before the client goes",
                 "[plugin_event_journal][event_journal_recorder]") {
    auto& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    // The routing's recorder keeps its journal in the config dir.
    const std::string dir = "/tmp/helix_routing_journal_" + std::to_string(::getpid());
    ScopedEnv config_dir{"HELIX_CONFIG_DIR", dir.c_str()};
    test::FakeMoonrakerClient client;
    {
        GcodeResponseRouting routing;
        routing.attach(&client, nullptr);
        CHECK(client.handler_count("notify_klippy_ready", "event_journal") == 1);

        routing.detach_handlers(&client);
        routing.release_routers();
        CHECK(client.handler_count("notify_klippy_ready", "event_journal") == 0);
    }
    ui::UpdateQueue::instance().drain_until_empty();
}
