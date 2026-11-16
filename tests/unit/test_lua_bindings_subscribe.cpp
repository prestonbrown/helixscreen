// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../lvgl_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/plugin_test_support.h"
#include "core/update_queue.h"
#include "lua_bindings.h"
#include "plugin_backend.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {
void drain() {
    helix::ui::UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "subscribe publishes the plugin's objects",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        h = helix.moonraker.subscribe({ extruder = { "temperature" } }, function(s) end)
    )"));
    REQUIRE(b.fake.object_sets.size() == 1);
    CHECK(b.fake.object_sets.back() ==
          std::make_pair(std::string("test-plugin"), json{{"extruder", {"temperature"}}}));
}

TEST_CASE_METHOD(LVGLTestFixture, "the first callback carries queried values",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        temp = "none"
        target = "unset"
        calls = 0
        snap = "unset"
        helix.moonraker.subscribe({ extruder = { "temperature" } }, function(s, snapshot)
            calls = calls + 1
            temp = s.extruder and s.extruder.temperature
            target = s.extruder and s.extruder.target
            snap = snapshot
        end)
    )"));
    REQUIRE(b.fake.requests.size() == 1);
    CHECK(b.fake.requests[0].a == "printer.objects.query");
    CHECK(b.fake.requests[0].params == json{{"objects", {{"extruder", {"temperature"}}}}});
    b.fake.requests[0].reply(RpcResult{
        true, query_reply({{"extruder", {{"temperature", 210.5}, {"target", 215}}}}), {}});
    drain();
    CHECK(b.t.global("calls") == "1");
    CHECK(b.t.global("temp") == "210.5");
    CHECK(b.t.global("target") == "nil");
    CHECK(b.t.global("snap") == "true");
}

TEST_CASE_METHOD(LVGLTestFixture, "deltas reach only the matching subscription",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        got = {}
        helix.moonraker.subscribe({ extruder = true }, function(s, snapshot)
            got[#got + 1] = "ext:" .. tostring(s.extruder and s.extruder.temperature)
        end)
        helix.moonraker.subscribe({ heater_bed = { "temperature" } }, function(s, snapshot)
            got[#got + 1] = "bed:" .. tostring(s.heater_bed and s.heater_bed.temperature) ..
                            ":" .. tostring(snapshot)
        end)
    )"));
    REQUIRE(b.fake.status.size() == 1);
    CHECK(b.fake.notify.empty());
    for (auto& r : b.fake.requests)
        r.reply(RpcResult{true, query_reply(json::object()), {}});
    drain();
    REQUIRE(b.t.run("got = {}"));

    b.fake.status[0](delta_frame({{"heater_bed", {{"temperature", 60.0}}}}));
    drain();
    REQUIRE(b.t.run("r = table.concat(got, ',')"));
    CHECK(b.t.global("r") == "bed:60.0:false");
}

TEST_CASE_METHOD(LVGLTestFixture, "a true subscription sees whole objects",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        fields = "none"
        helix.moonraker.subscribe({ extruder = true }, function(s)
            fields = s.extruder and (tostring(s.extruder.temperature) .. "/" ..
                                     tostring(s.extruder.power))
        end)
    )"));
    REQUIRE(b.fake.status.size() == 1);
    b.fake.status[0](delta_frame({{"extruder", {{"temperature", 30.0}, {"power", 0.4}}}}));
    drain();
    CHECK(b.t.global("fields") == "30.0/0.4");
}

TEST_CASE_METHOD(LVGLTestFixture, "a whole-object frame re-delivers every subscription a snapshot",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        got = {}
        helix.moonraker.subscribe({ extruder = { "temperature" } }, function(s, snapshot)
            got[#got + 1] = "ext:" .. tostring(s.extruder and s.extruder.temperature) .. "/" ..
                            tostring(s.extruder and s.extruder.target) .. "/" ..
                            tostring(s.toolhead) .. ":" .. tostring(snapshot)
        end)
        helix.moonraker.subscribe({ anl_simulator = true }, function(s, snapshot)
            local a = s.anl_simulator
            got[#got + 1] = "sim:" .. tostring(a and a.mode) .. "/" .. tostring(a and a.rate) ..
                            ":" .. tostring(snapshot)
        end)
    )"));
    REQUIRE(b.fake.requests.size() == 2);
    for (auto& r : b.fake.requests)
        r.reply(RpcResult{true, query_reply(json::object()), {}});
    drain();
    REQUIRE(b.t.run("got = {}"));

    // Discovery after a Klipper restart replays a snapshot it captured earlier; the
    // plugin gets fresh values from its own query instead.
    b.fake.status[0](whole_objects_frame(
        {{"extruder", {{"temperature", 20.0}}}, {"anl_simulator", {{"mode", "stale"}}}}, true));
    drain();
    REQUIRE(b.fake.requests.size() == 3);
    CHECK(b.fake.requests[2].a == "printer.objects.query");
    CHECK(b.fake.requests[2].params ==
          json{{"objects", {{"extruder", {"temperature"}}, {"anl_simulator", nullptr}}}});
    REQUIRE(b.t.run("r = table.concat(got, ',')"));
    CHECK(b.t.global("r") == "");

    b.fake.requests[2].reply(
        RpcResult{true,
                  query_reply({{"extruder", {{"temperature", 215.0}, {"target", 215.0}}},
                               {"anl_simulator", {{"mode", "run"}, {"rate", 3}}},
                               {"toolhead", {{"position", {0, 0, 0, 0}}}}}),
                  {}});
    drain();
    REQUIRE(b.t.run("r = table.concat(got, ',')"));
    CHECK(b.t.global("r") == "ext:215.0/nil/nil:true,sim:run/3:true");
}

TEST_CASE_METHOD(LVGLTestFixture, "a query that fails after the runtime closed logs its plugin",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        helix.moonraker.subscribe({ extruder = { "temperature" } }, function(s) end)
    )"));
    b.fake.status[0](whole_objects_frame({{"extruder", {{"temperature", 20.0}}}}, true));
    drain();
    REQUIRE(b.fake.requests.size() == 2);

    // Both replies land after the runtime is gone; under ASAN a read of its id
    // through the freed runtime fails this case.
    b.t.rt.reset();
    helix::LogCapture logs;
    for (auto& r : b.fake.requests)
        r.reply(RpcResult{false, json(), "klippy disconnected"});
    drain();
    CHECK(logs.count_containing("[plugin test-plugin] initial subscribe query failed") == 1);
    CHECK(logs.count_containing("[plugin test-plugin] subscription snapshot query failed") == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "whole-object frames coalesce into one query at a time",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        n = 0
        helix.moonraker.subscribe({ extruder = true }, function(s, snapshot) n = n + 1 end)
    )"));
    REQUIRE(b.fake.requests.size() == 1); // the first delivery's query
    const json frame = whole_objects_frame({{"extruder", {{"temperature", 1.0}}}}, false);

    b.fake.status[0](frame);
    b.fake.status[0](frame);
    drain();
    REQUIRE(b.fake.requests.size() == 2);

    // Frames while that query is out fold into exactly one more, sent once it settles.
    b.fake.status[0](frame);
    drain();
    b.fake.status[0](frame);
    drain();
    CHECK(b.fake.requests.size() == 2);
    b.fake.requests[1].reply(
        RpcResult{true, query_reply({{"extruder", {{"temperature", 2.0}}}}), {}});
    drain();
    REQUIRE(b.fake.requests.size() == 3);

    // A failed query settles too.
    b.fake.requests[2].reply(RpcResult{false, {}, "Klippy Host not connected"});
    drain();
    b.fake.status[0](frame);
    drain();
    CHECK(b.fake.requests.size() == 4);
}

TEST_CASE_METHOD(LVGLTestFixture, "cancel shrinks the set; closing the runtime clears it",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        a = helix.moonraker.subscribe({ extruder = { "temperature" } }, function(s) end)
        w = helix.moonraker.subscribe({ heater_bed = true }, function(s) end)
    )"));
    REQUIRE(b.fake.object_sets.back().second ==
            json{{"extruder", {"temperature"}}, {"heater_bed", nullptr}});
    REQUIRE(b.t.run("a:cancel()"));
    REQUIRE(b.fake.object_sets.back().second == json{{"heater_bed", nullptr}});
    b.t.rt.reset();
    REQUIRE(b.fake.object_sets.back() ==
            std::make_pair(std::string("test-plugin"), json::object()));
    CHECK(b.fake.status_unregistered == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "a cancelled subscription stops delivering",
                 "[plugin][lua][subscribe]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        n = 0
        h = helix.moonraker.subscribe({ extruder = true }, function(s) n = n + 1 end)
        h:cancel()
        h:cancel()
    )"));
    REQUIRE(b.fake.status.size() == 1);
    b.fake.status[0](delta_frame({{"extruder", {{"temperature", 30.0}}}}));
    drain();
    for (auto& r : b.fake.requests)
        r.reply(RpcResult{true, query_reply({{"extruder", {{"temperature", 30.0}}}}), {}});
    drain();
    b.fake.status[0](whole_objects_frame({{"extruder", {{"temperature", 30.0}}}}, false));
    drain();
    CHECK(b.fake.requests.size() == 1); // nothing left to re-snapshot
    CHECK(b.t.global("n") == "0");
}

TEST_CASE_METHOD(LVGLTestFixture, "a menu check cannot cancel a subscription",
                 "[plugin][lua][subscribe][check]") {
    BoundRuntime b({&install_ui_bindings, &install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        h = helix.moonraker.subscribe({ extruder = true }, function(s) end)
        helix.ui.on("check", function() h:cancel(); return true end)
    )"));
    const size_t sets = b.fake.object_sets.size();
    helix::LogCapture logs;
    CHECK_FALSE(call_ui_handler_sync(*b.t.rt, "check", {}, {}));
    CHECK(logs.count_containing("subscription:cancel is not allowed in a menu check") == 1);
    CHECK(b.fake.object_sets.size() == sets); // the plugin's object set is unchanged
    CHECK_FALSE(b.t.rt->faulted());
}

TEST_CASE_METHOD(LVGLTestFixture, "subscribe enforces its limits", "[plugin][lua][subscribe]") {
    BoundRuntime o({&install_moonraker_bindings});
    REQUIRE(o.t.run(R"(
        t = {}
        for i = 1, 17 do t["obj" .. i] = true end
        ok, err = pcall(helix.moonraker.subscribe, t, function() end)
    )"));
    CHECK(o.t.global("ok") == "false");
    CHECK(o.t.global("err").find("at most 16") != std::string::npos);
    CHECK(o.fake.object_sets.empty());

    BoundRuntime s({&install_moonraker_bindings});
    REQUIRE(s.t.run(R"(
        for i = 1, 8 do helix.moonraker.subscribe({ ["o" .. i] = true }, function() end) end
        ok, err = pcall(helix.moonraker.subscribe, { z = true }, function() end)
    )"));
    CHECK(s.t.global("ok") == "false");
    CHECK(s.t.global("err").find("at most 8") != std::string::npos);

    BoundRuntime n({&install_moonraker_bindings});
    REQUIRE(n.t.run(R"(
        ok, err = pcall(helix.moonraker.subscribe,
                        { [string.rep("x", 65)] = true }, function() end)
    )"));
    CHECK(n.t.global("ok") == "false");
    CHECK(n.t.global("err").find("at most 64") != std::string::npos);

    BoundRuntime v({&install_moonraker_bindings});
    REQUIRE(v.t.run(R"(
        fields = {}
        for i = 1, 33 do fields[i] = "f" .. i end
        ok, err = pcall(helix.moonraker.subscribe, { extruder = fields }, function() end)
        ok2, err2 = pcall(helix.moonraker.subscribe, { extruder = { 123 } }, function() end)
        ok3, err3 = pcall(helix.moonraker.subscribe, { extruder = false }, function() end)
        ok4, err4 = pcall(helix.moonraker.subscribe, "extruder", function() end)
    )"));
    CHECK(v.t.global("ok") == "false");
    CHECK(v.t.global("err").find("at most 32 fields") != std::string::npos);
    CHECK(v.t.global("ok2") == "false");
    CHECK(v.t.global("err2").find("field names must be strings") != std::string::npos);
    CHECK(v.t.global("ok3") == "false");
    CHECK(v.t.global("err3").find("values are true or a list of field names") != std::string::npos);
    CHECK(v.t.global("ok4") == "false");
    CHECK(v.fake.object_sets.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "the object registry unions plugins",
                 "[plugin][lua][subscribe]") {
    publish_plugin_objects("plugin-a", json{{"extruder", {"temperature"}}});
    publish_plugin_objects("plugin-b", json{{"heater_bed", nullptr}, {"toolhead", {"position"}}});
    json u = plugin_objects_union();
    CHECK(u.contains("extruder"));
    CHECK(u.contains("heater_bed"));
    CHECK(u["heater_bed"].is_null());
    CHECK(u.contains("toolhead"));
    publish_plugin_objects("plugin-a", json::object());
    u = plugin_objects_union();
    CHECK_FALSE(u.contains("extruder"));
    CHECK(u.contains("heater_bed"));
    publish_plugin_objects("plugin-b", json::object());
    CHECK(plugin_objects_union().empty());
}

#endif // HELIX_HAS_PLUGINS
