// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "plugin_backend.h"
#include "printer_state.h"

#include <chrono>
#include <future>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;

TEST_CASE("app backend reports no connection instead of crashing", "[plugin][backend]") {
    REQUIRE(get_moonraker_api() == nullptr);
    REQUIRE(get_moonraker_client() == nullptr);
    auto b = make_app_backend();
    std::vector<RpcResult> got;
    auto sink = [&](RpcResult r) { got.push_back(std::move(r)); };
    b.gcode("M117 hi", sink);
    b.call("server.info", json::object(), sink);
    b.upload("gcodes", "a.gcode", "G28", sink);
    b.download("gcodes", "a.gcode", 1024, sink);
    REQUIRE(got.size() == 4);
    for (const auto& r : got) {
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("not connected") != std::string::npos);
    }
    auto off = b.on_notify("notify_agent_event", [](const json&) {});
    REQUIRE(off);
    off();
    auto status_off = b.on_status([](const json&) {});
    REQUIRE(status_off);
    status_off();
    // Sound goes to the app's sound manager, which with no audio backend plays nothing.
    REQUIRE(b.play_sound);
    b.play_sound("button_tap", SoundPriority::UI);
}

TEST_CASE("app backend rejects roots other than gcodes and config", "[plugin][backend]") {
    auto b = make_app_backend();
    RpcResult up, down;
    b.upload("logs", "x", "y", [&](RpcResult r) { up = std::move(r); });
    b.download("../etc", "x", 1024, [&](RpcResult r) { down = std::move(r); });
    CHECK_FALSE(up.ok);
    CHECK(up.error.find("root") != std::string::npos);
    CHECK_FALSE(down.ok);
}

TEST_CASE("plan_http_target refuses every spelling of this machine", "[plugin][backend]") {
    const std::vector<std::string> none;
    for (const char* url :
         {"http://:7125/x", "http:///x", "http://evil.com?@127.0.0.1:7125/x",
          "http://evil.com#@127.0.0.1:7125/x", "http://0.0.0.0:7125/x", "http://[::]:7125/x",
          "http://127.1/x", "http://2130706433/x", "http://[::1]/x", "http://[::ffff:127.0.0.1]/x",
          "http://localhost:7125/x", "https://127.0.0.1/"}) {
        INFO(url);
        HttpTarget t = plan_http_target(url, none);
        CHECK_FALSE(t.ok);
        CHECK(t.error.find("printer host") != std::string::npos);
    }
}

TEST_CASE("plan_http_target refuses an address in the forbidden list", "[plugin][backend]") {
    HttpTarget t = plan_http_target("http://93.184.216.34/x", {"10.0.0.2", "93.184.216.34"});
    CHECK_FALSE(t.ok);
    CHECK(t.error.find("printer host") != std::string::npos);
}

TEST_CASE("plan_http_target pins a plain http connect to the checked address",
          "[plugin][backend]") {
    HttpTarget t = plan_http_target("http://93.184.216.34:8080/a?b=1", {"10.0.0.2"});
    REQUIRE(t.ok);
    CHECK(t.connect_url == "http://93.184.216.34:8080/a?b=1");
    CHECK(t.host_header == "93.184.216.34:8080");

    HttpTarget v6 = plan_http_target("http://[2606:4700::1111]/p", {});
    REQUIRE(v6.ok);
    CHECK(v6.connect_url == "http://[2606:4700::1111]:80/p");
}

TEST_CASE("plan_http_target leaves an https URL unpinned", "[plugin][backend]") {
    HttpTarget t = plan_http_target("https://93.184.216.34/x", {});
    REQUIRE(t.ok);
    CHECK(t.connect_url == "https://93.184.216.34/x");
    CHECK(t.host_header.empty());
}

TEST_CASE("app backend http refuses the printer's own host", "[plugin][backend]") {
    // The slow lane is process-wide and already running for every case. Stopping
    // it here would leave the next case's reset to start a fresh worker, which
    // the isolation listener reports as a thread leaked by that next case.
    auto b = make_app_backend();
    std::promise<RpcResult> done;
    auto ready = done.get_future();
    b.http("GET", "http://127.0.0.1:7125/x", "", json::object(), 5000, 1024,
           [&done](RpcResult r) { done.set_value(std::move(r)); });
    REQUIRE(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    RpcResult r = ready.get();
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("printer host") != std::string::npos);
}

namespace {

/// A mock client installed as the app's client, so the backend's `call` reaches a recorder.
class PluginCallFixture : public LVGLTestFixture {
  public:
    PluginCallFixture() : mock(MoonrakerClientMock::PrinterType::VORON_24) {
        get_printer_state().init_subjects(false);
        get_printer_state().print_state().set_spool_latch(false);
        previous = get_moonraker_client();
        set_moonraker_client(&mock);
    }
    ~PluginCallFixture() override {
        set_moonraker_client(previous);
        get_printer_state().print_state().set_spool_latch(false);
    }

    RpcResult call(const std::string& method, const json& params) {
        RpcResult out;
        make_app_backend().call(method, params, [&](RpcResult r) { out = std::move(r); });
        return out;
    }

    MoonrakerClientMock mock;
    helix::IMoonrakerClient* previous = nullptr;
};

} // namespace

TEST_CASE_METHOD(PluginCallFixture, "plugin call refuses a print start while spools are on the bed",
                 "[plugin][backend][spool_latch]") {
    get_printer_state().print_state().set_spool_latch(true);
    mock.clear_gcode_script_history();

    const RpcResult r = call("printer.print.start", {{"filename", "benchy.gcode"}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("Spools are on the bed") != std::string::npos);
    CHECK(mock.last_send_method() != "printer.print.start");
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call refuses every job method while the latch is up",
                 "[plugin][backend][spool_latch]") {
    get_printer_state().print_state().set_spool_latch(true);

    for (const char* method : {"printer.print.resume", "server.job_queue.start", "printer.restart",
                               "printer.firmware_restart"}) {
        INFO(method);
        const RpcResult r = call(method, json::object());
        CHECK_FALSE(r.ok);
        CHECK(mock.last_send_method() != method);
    }
    const RpcResult modified =
        call("server.helix.print_modified",
             {{"original_filename", "a.gcode"}, {"temp_file_path", "b.gcode"}});
    CHECK_FALSE(modified.ok);
    CHECK(mock.last_send_method() != "server.helix.print_modified");

    const RpcResult klipper = call("machine.services.restart", {{"service", "klipper"}});
    CHECK_FALSE(klipper.ok);
    CHECK(mock.last_send_method() != "machine.services.restart");
}

TEST_CASE_METHOD(PluginCallFixture,
                 "plugin call gives print_edited the linked-start checks print_modified has",
                 "[plugin][backend][spool_latch][plugin_server_edits]") {
    const json edited = {{"original_filename", "a.gcode"},
                         {"temp_file_path", ".helix_temp/b.gcode"},
                         {"edits", json::array()}};

    get_printer_state().print_state().set_spool_latch(true);
    const RpcResult latched = call("server.helix.print_edited", edited);
    CHECK_FALSE(latched.ok);
    CHECK(latched.error.find("Spools are on the bed") != std::string::npos);
    CHECK(mock.last_send_method() != "server.helix.print_edited");
    get_printer_state().print_state().set_spool_latch(false);

    for (const char* key : {"original_filename", "temp_file_path"}) {
        INFO(key);
        json bad = edited;
        bad[key] = "../etc/passwd";
        const RpcResult r = call("server.helix.print_edited", bad);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("Invalid path") != std::string::npos);
        CHECK(mock.last_send_method() != "server.helix.print_edited");
    }

    call("server.helix.print_edited", edited);
    CHECK(mock.last_send_method() == "server.helix.print_edited");
}

TEST_CASE_METHOD(PluginCallFixture,
                 "plugin call refuses every HelixPrint action that is not read-only",
                 "[plugin][backend][plugin_server_edits]") {
    for (const char* method : {"server.helix.plr.commit", "server.helix.plr.prepare",
                               "server.helix.config_edit", "server.helix.print_hooks.screen_start",
                               "server.helix.storage_cleanup", "server.helix.foo"}) {
        INFO(method);
        const RpcResult r = call(method, json::object());
        CHECK_FALSE(r.ok);
        CHECK(r.error.find(method) != std::string::npos);
        CHECK(mock.last_send_method() != method);
    }

    for (const char* method : {"server.helix.status", "server.helix.journal",
                               "server.helix.plr.status", "server.helix.analysis"}) {
        INFO(method);
        call(method, json::object());
        CHECK(mock.last_send_method() == method);
    }
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call applies the gcode guards to printer.gcode.script",
                 "[plugin][backend][spool_latch]") {
    get_printer_state().print_state().set_spool_latch(true);
    CHECK_FALSE(call("printer.gcode.script", {{"script", "G28"}}).ok);
    CHECK(mock.last_send_method() != "printer.gcode.script");
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call refuses an unsafe print start path",
                 "[plugin][backend]") {
    for (const char* name :
         {"a.gcode\nM104 S300", "../etc/passwd", "/abs.gcode", "", "a\rb.gcode"}) {
        INFO(name);
        const RpcResult r = call("printer.print.start", {{"filename", name}});
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("Invalid path") != std::string::npos);
        CHECK(mock.last_send_method() != "printer.print.start");
    }
    CHECK_FALSE(call("printer.print.start", json::object()).ok);
    CHECK_FALSE(call("printer.print.start", {{"filename", 7}}).ok);
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call still sends a normal print start",
                 "[plugin][backend][spool_latch]") {
    const RpcResult r = call("printer.print.start", {{"filename", "sub/benchy.gcode"}});
    CHECK(mock.last_send_method() == "printer.print.start");
    CHECK(r.error.find("Spools") == std::string::npos);
    CHECK(r.error.find("Invalid path") == std::string::npos);

    call("server.info", json::object());
    CHECK(mock.last_send_method() == "server.info");
}

namespace {

/// Calls @p method, which must come back refused by name with nothing sent.
void check_helix_method_refused(PluginCallFixture& f, const char* method) {
    INFO(method);
    f.call("server.info", json::object());
    const RpcResult r = f.call(method, json::object());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find(method) != std::string::npos);
    CHECK(r.error.find("not available to plugins") != std::string::npos);
    CHECK(f.mock.last_send_method() == "server.info");
}

} // namespace

TEST_CASE_METHOD(PluginCallFixture, "plugin call refuses server.helix.plr.plan",
                 "[plugin][backend][plugin_rpc]") {
    check_helix_method_refused(*this, "server.helix.plr.plan");
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call refuses the PLR heat-and-move steps",
                 "[plugin][backend][plugin_rpc]") {
    check_helix_method_refused(*this, "server.helix.plr.prepare");
    check_helix_method_refused(*this, "server.helix.plr.commit");
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call refuses PLR abort and discard",
                 "[plugin][backend][plugin_rpc]") {
    check_helix_method_refused(*this, "server.helix.plr.abort");
    check_helix_method_refused(*this, "server.helix.plr.discard");
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call refuses an unknown server.helix method",
                 "[plugin][backend][plugin_rpc]") {
    check_helix_method_refused(*this, "server.helix.foo");
    check_helix_method_refused(*this, "server.helix.status.extra");
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call sends the read-only server.helix methods",
                 "[plugin][backend][plugin_rpc]") {
    for (const char* method : {"server.helix.status", "server.helix.plr.status"}) {
        INFO(method);
        call("server.info", json::object());
        const RpcResult r = call(method, json::object());
        CHECK(r.error.find("not available to plugins") == std::string::npos);
        CHECK(mock.last_send_method() == method);
    }
}

TEST_CASE_METHOD(PluginCallFixture, "plugin call keeps the print_modified path guard",
                 "[plugin][backend][plugin_rpc]") {
    const RpcResult bad = call("server.helix.print_modified", {{"original_filename", "../a.gcode"},
                                                               {"temp_file_path", "b.gcode"}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("Invalid path") != std::string::npos);
    CHECK(mock.last_send_method() != "server.helix.print_modified");

    call("server.helix.print_modified",
         {{"original_filename", "a.gcode"}, {"temp_file_path", ".helix_temp/b.gcode"}});
    CHECK(mock.last_send_method() == "server.helix.print_modified");
}

#endif // HELIX_HAS_PLUGINS

TEST_CASE("app backend set_plugin_objects reaches the object union", "[plugin][backend]") {
    auto b = make_app_backend();

    // The registry behind plugin_objects_union() is process-wide, so the probe
    // entry is cleared before the case ends whatever its assertions did.
    b.set_plugin_objects("union_probe_plugin", json{{"union_probe_object", {"field"}}});
    const json unioned = plugin_objects_union();
    CHECK(unioned.contains("union_probe_object"));

    // An empty object clears the plugin's entry.
    b.set_plugin_objects("union_probe_plugin", json::object());
    CHECK_FALSE(plugin_objects_union().contains("union_probe_object"));

    // Publishing schedules a coalesced subscription refresh on the UI queue.
    helix::ui::UpdateQueue::instance().drain();
}
