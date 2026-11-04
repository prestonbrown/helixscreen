// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "core/update_queue.h"
#include "lua_bindings.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {
void drain() {
    helix::ui::UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "gcode needs its permission", "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    CHECK_FALSE(b.t.run(R"(helix.gcode("G28"))"));
    CHECK(b.fake.requests.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "gcode suspends until Moonraker answers",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings}, {Permission::Gcode});
    REQUIRE(b.t.run(R"(ok, err = helix.gcode("G28"); done = true)"));
    REQUIRE(b.fake.requests.size() == 1);
    CHECK(b.fake.requests[0].kind == "gcode");
    CHECK(b.fake.requests[0].a == "G28");
    CHECK(b.t.global("done") == "nil");

    b.fake.requests[0].reply(RpcResult{false, {}, "Must home axis first"});
    drain();
    CHECK(b.t.global("done") == "true");
    CHECK(b.t.global("ok") == "nil");
    CHECK(b.t.global("err") == "Must home axis first");
}

TEST_CASE_METHOD(LVGLTestFixture, "call enforces the read-only allowlist",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(helix.moonraker.call("server.info", {}))"));
    CHECK_FALSE(b.t.run(R"(helix.moonraker.call("printer.restart", {}))"));
    REQUIRE(b.fake.requests.size() == 1);
    CHECK(b.fake.requests[0].a == "server.info");
    CHECK(b.fake.requests[0].params == json::object());

    BoundRuntime w({&install_moonraker_bindings}, {Permission::MoonrakerWrite});
    REQUIRE(w.t.run(R"(helix.moonraker.call("printer.restart"))"));
    CHECK(w.fake.requests.size() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "query builds printer.objects.query params",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        r = helix.moonraker.query({ extruder = { "temperature" }, toolhead = true })
        t = r and r.status.extruder.temperature
    )"));
    REQUIRE(b.fake.requests.size() == 1);
    CHECK(b.fake.requests[0].a == "printer.objects.query");
    CHECK(b.fake.requests[0].params ==
          json{{"objects", {{"extruder", {"temperature"}}, {"toolhead", nullptr}}}});
    b.fake.requests[0].reply(
        RpcResult{true, json{{"status", {{"extruder", {{"temperature", 210.5}}}}}}, {}});
    drain();
    CHECK(b.t.global("t") == "210.5");
}

TEST_CASE_METHOD(LVGLTestFixture, "upload and download need moonraker_write",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    CHECK_FALSE(b.t.run(R"(helix.moonraker.upload("gcodes", "a.gcode", "G28"))"));
    CHECK_FALSE(b.t.run(R"(helix.moonraker.download("gcodes", "a.gcode"))"));

    BoundRuntime w({&install_moonraker_bindings}, {Permission::MoonrakerWrite});
    REQUIRE(w.t.run(R"(up = helix.moonraker.upload("gcodes", "a.gcode", "G28\n"))"));
    REQUIRE(w.t.run(R"(body = helix.moonraker.download("gcodes", "a.gcode"))"));
    REQUIRE(w.fake.requests.size() == 2);
    CHECK(w.fake.requests[0].kind == "upload");
    CHECK(w.fake.requests[0].c == "G28\n");
    w.fake.requests[0].reply(RpcResult{true, {}, {}});
    w.fake.requests[1].reply(RpcResult{true, json("G28\n"), {}});
    drain();
    CHECK(w.t.global("up") == "true");
    CHECK(w.t.global("body") == "G28\n");
}

TEST_CASE_METHOD(LVGLTestFixture, "download refuses a body over the plugin memory cap",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime w({&install_moonraker_bindings}, {Permission::MoonrakerWrite});
    REQUIRE(w.t.run(R"(body, err = helix.moonraker.download("gcodes", "big.gcode"))"));
    REQUIRE(w.fake.requests.size() == 1);
    w.fake.requests[0].reply(RpcResult{true, json(std::string(4 << 20, 'x')), {}});
    drain();
    CHECK(w.t.global("body") == "nil");
    CHECK(w.t.global("err").find("memory cap") != std::string::npos);
    CHECK_FALSE(w.t.rt->faulted());
}

TEST_CASE_METHOD(LVGLTestFixture, "call results larger than the cap return an error",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(r, err = helix.moonraker.call("server.info", {}))"));
    REQUIRE(b.fake.requests.size() == 1);
    b.fake.requests[0].reply(RpcResult{true, json{{"blob", std::string(4 << 20, 'x')}}, {}});
    drain();
    CHECK(b.t.global("r") == "nil");
    CHECK(b.t.global("err").find("memory cap") != std::string::npos);
    CHECK_FALSE(b.t.rt->faulted());
}

TEST_CASE_METHOD(LVGLTestFixture, "agent event data over the cap arrives as nil",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        seen = {}
        helix.moonraker.on_agent_event("x", function(agent, data)
            seen[#seen + 1] = agent .. ":" .. tostring(data)
        end)
    )"));
    b.fake.notify[0].second(json{
        {"params",
         {{{"agent", "a"}, {"event", "x"}, {"data", {{"blob", std::string(4 << 20, 'x')}}}}}}});
    drain();
    REQUIRE(b.t.run("r = table.concat(seen, ',')"));
    CHECK(b.t.global("r") == "a:nil");
    CHECK_FALSE(b.t.rt->faulted());
}

TEST_CASE_METHOD(LVGLTestFixture, "agent events are filtered by event name",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        got = {}
        helix.moonraker.on_agent_event("result", function(agent, data)
            got[#got + 1] = agent .. "=" .. data.temp
        end)
    )"));
    REQUIRE(b.fake.notify.size() == 1);
    CHECK(b.fake.notify[0].first == "notify_agent_event");
    auto& handler = b.fake.notify[0].second;
    handler(json{{"params", {{{"agent", "orca"}, {"event", "other"}, {"data", {{"temp", 1}}}}}}});
    handler(
        json{{"params", {{{"agent", "orca"}, {"event", "result"}, {"data", {{"temp", 210}}}}}}});
    handler(json{{"params", "malformed"}});
    handler(json::object());
    drain();
    REQUIRE(b.t.run("r = table.concat(got, ',')"));
    CHECK(b.t.global("r") == "orca=210");
}

TEST_CASE_METHOD(LVGLTestFixture, "download asks for one byte more than the remaining cap",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime w({&install_moonraker_bindings}, {Permission::MoonrakerWrite});
    REQUIRE(w.t.run(R"(body = helix.moonraker.download("gcodes", "a.gcode"))"));
    REQUIRE(w.fake.requests.size() == 1);
    CHECK(w.fake.requests[0].cap == w.t.rt->memory_cap() - w.t.rt->memory_used() + 1);
    w.fake.requests[0].reply(RpcResult{true, json("x"), {}});
    drain();
}

TEST_CASE_METHOD(LVGLTestFixture, "on_agent_event bounds handlers per plugin",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        for i = 1, 16 do helix.moonraker.on_agent_event("evt", function() end) end
        ok, err = pcall(helix.moonraker.on_agent_event, "evt", function() end)
    )"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("at most 16") != std::string::npos);
    CHECK(b.fake.notify.size() == 16);
}

TEST_CASE_METHOD(LVGLTestFixture, "agent handlers unregister when the runtime closes",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(helix.moonraker.on_agent_event("x", function() end))"));
    CHECK(b.fake.notify_unregistered == 0);
    b.t.rt.reset();
    CHECK(b.fake.notify_unregistered == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "an agent event after unload is dropped",
                 "[plugin][bindings][moonraker]") {
    std::function<void(const json&)> handler;
    {
        BoundRuntime b({&install_moonraker_bindings});
        REQUIRE(b.t.run(R"(helix.moonraker.on_agent_event("x", function() end))"));
        handler = b.fake.notify[0].second;
    }
    handler(json{{"params", {{{"agent", "a"}, {"event", "x"}}}}});
    drain();
    SUCCEED(); // ASAN is what proves the closed runtime was not touched
}

TEST_CASE_METHOD(LVGLTestFixture, "a late reply after unload is dropped",
                 "[plugin][bindings][moonraker]") {
    RpcCallback late;
    {
        BoundRuntime b({&install_moonraker_bindings}, {Permission::Gcode});
        REQUIRE(b.t.run(R"(helix.gcode("G28"))"));
        late = b.fake.requests[0].reply;
    }
    late(RpcResult{true, {}, {}});
    drain();
    SUCCEED();
}

#endif // HELIX_HAS_PLUGINS
