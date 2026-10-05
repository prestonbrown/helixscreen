// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_update_queue.h"

#include "../test_helpers/log_capture.h"
#include "../test_helpers/plugin_test_support.h"
#include "lua_runtime.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using helix::plugin::test::TestRuntime;

TEST_CASE("runtime runs code and exposes an empty helix table", "[plugin][lua_runtime]") {
    TestRuntime t;
    REQUIRE(t.run("x = 6 * 7; kind = type(helix)"));
    CHECK(t.global("x") == "42");
    CHECK(t.global("kind") == "table");
}

TEST_CASE("sandbox removes host access", "[plugin][lua_runtime]") {
    TestRuntime t;
    REQUIRE(t.run(R"(
        io_t, os_t, pkg_t, dbg_t = type(io), type(os), type(package), type(debug)
        dofile_t, loadfile_t, dump_t = type(dofile), type(loadfile), type(string.dump)
        local f, err = load("\27Lua", "bin")
        binary_rejected = (f == nil and err:find("binary") ~= nil)
        text_ok = load("return 5")() == 5
        env_ok = load("return y", "c", "t", { y = 9 })() == 9
        count_ok = type(collectgarbage("count")) == "number"
        stop_ok = pcall(collectgarbage, "stop")
    )"));
    for (const char* g : {"io_t", "os_t", "pkg_t", "dbg_t", "dofile_t", "loadfile_t", "dump_t"})
        CHECK(t.global(g) == "nil");
    CHECK(t.global("binary_rejected") == "true");
    CHECK(t.global("text_ok") == "true");
    CHECK(t.global("env_ok") == "true");
    CHECK(t.global("count_ok") == "true");
    CHECK(t.global("stop_ok") == "false");
}

TEST_CASE("sandbox print logs its arguments tab-joined", "[plugin][lua_runtime]") {
    TestRuntime t;
    helix::TextLogCapture log;
    REQUIRE(t.run(R"(
        print("a", 1, nil, true)
        print()
        print({})
        printed_ok = true
    )"));
    CHECK(t.global("printed_ok") == "true");
    CHECK(log.get_captured().find("a\t1\tnil\ttrue") != std::string::npos);
}

TEST_CASE("sandbox print caps the logged line", "[plugin][lua_runtime]") {
    TestRuntime t;
    helix::TextLogCapture log;
    REQUIRE(t.run(R"(
        local s = string.rep("x", 64 * 1024)
        local many = {}
        for i = 1, 100 do many[i] = s end
        print(table.unpack(many))
    )"));
    std::string line = log.get_captured();
    CHECK(line.size() < 4200);
    CHECK(line.find("...") != std::string::npos);
}

TEST_CASE("require resolves inside the plugin and caches", "[plugin][lua_runtime]") {
    TestRuntime t;
    REQUIRE(t.run(R"(
        a = require("util").answer
        d1 = require("deep"); d2 = require("deep")
        same = (d1 == d2)
    )"));
    CHECK(t.global("a") == "42");
    CHECK(t.global("same") == "true");
    CHECK(t.global("loads") == "1");
    CHECK_FALSE(t.run(R"(require("missing"))"));
}

TEST_CASE("require names that escape the plugin are refused", "[plugin][lua_runtime]") {
    CHECK(require_candidates("/p", "../x").empty());
    CHECK(require_candidates("/p", "/etc/passwd").empty());
    CHECK(require_candidates("/p", "a..b").empty());
    CHECK(require_candidates("/p", ".a").empty());
    CHECK(require_candidates("/p", "a.").empty());
    CHECK(require_candidates("/p", "").empty());
    CHECK(require_candidates("/p", "a.b") ==
          std::vector<std::string>{"/p/a/b.lua", "/p/lib/a/b.lua"});
}

TEST_CASE("a runtime error is reported, not fatal", "[plugin][lua_runtime]") {
    TestRuntime t;
    CHECK_FALSE(t.run("error('first')"));
    CHECK_FALSE(t.rt->faulted());
    CHECK(t.run("ok = true"));
}

TEST_CASE("three errors inside a minute fault the plugin", "[plugin][lua_runtime]") {
    TestRuntime t;
    t.run("error('1')");
    t.run("error('2')");
    CHECK_FALSE(t.rt->faulted());
    t.run("error('3')");
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("errors") != std::string::npos);
    CHECK_FALSE(t.run("late = true"));
    CHECK(t.global("late") == "nil");
}

TEST_CASE("error window slides", "[plugin][lua_runtime]") {
    ErrorWindow w(3, std::chrono::seconds(60));
    auto t0 = ErrorWindow::Clock::time_point{};
    CHECK_FALSE(w.record(t0));
    CHECK_FALSE(w.record(t0 + std::chrono::seconds(30)));
    CHECK_FALSE(w.record(t0 + std::chrono::seconds(61)));
    CHECK(w.record(t0 + std::chrono::seconds(62)));
}

TEST_CASE("out of memory reaching the entry faults the plugin", "[plugin][lua_runtime]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    CHECK_FALSE(t.run("local t = {} while true do t[#t + 1] = string.rep('x', 1024) end"));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("memory") != std::string::npos);
}

TEST_CASE("out of memory caught by the plugin does not fault it", "[plugin][lua_runtime]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    REQUIRE(t.run(R"(
        caught = not pcall(function()
            local t = {} while true do t[#t + 1] = string.rep('x', 1024) end
        end)
        collectgarbage("collect")
    )"));
    CHECK(t.global("caught") == "true");
    CHECK_FALSE(t.rt->faulted());
    CHECK(t.rt->memory_used() < limits.memory_bytes);
}

TEST_CASE("external reservations share the memory cap", "[plugin][lua_runtime]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    const size_t base = t.rt->memory_used();

    CHECK(t.rt->reserve_external(100 * 1024));
    CHECK(t.rt->memory_used() == base + 100 * 1024);

    // The headroom Lua itself needs counts too: the full remaining cap fails.
    CHECK_FALSE(t.rt->reserve_external(256 * 1024));
    CHECK(t.rt->memory_used() == base + 100 * 1024);

    // Exactly the remaining cap fits; a byte more does not.
    CHECK(t.rt->reserve_external(256 * 1024 - 100 * 1024 - base));
    CHECK_FALSE(t.rt->reserve_external(1));

    t.rt->release_external(100 * 1024);
    CHECK(t.rt->memory_used() == 156 * 1024);
    t.rt->release_external(256 * 1024); // over-release clamps
    CHECK(t.rt->memory_used() == 0);
}

TEST_CASE("closers run in reverse before the state closes", "[plugin][lua_runtime]") {
    std::vector<int> order;
    {
        TestRuntime t;
        t.rt->on_close([&] { order.push_back(1); });
        t.rt->on_close([&] { order.push_back(2); });
    }
    CHECK(order == std::vector<int>{2, 1});
}

TEST_CASE("source larger than the memory cap is refused", "[plugin][lua_runtime]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    std::string ones;
    ones.reserve(1000000);
    for (int i = 0; i < 500000; ++i)
        ones += "1,";
    std::string chunk = "local t = {" + ones + "}";
    CHECK(chunk.size() > limits.memory_bytes);
    CHECK_FALSE(t.run(chunk));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("memory") != std::string::npos);
    CHECK(t.rt->memory_used() <= 256 * 1024);
}

TEST_CASE("run_file refuses a missing file and a directory", "[plugin][lua_runtime]") {
    TestRuntime t;
    CHECK_FALSE(t.rt->run_file("no-such-file.lua"));
    CHECK_FALSE(t.rt->run_file("lib"));
    CHECK_FALSE(t.rt->faulted());
}

TEST_CASE("an infinite loop is stopped and faults the plugin",
          "[plugin][lua_runtime][lua_budget]") {
    TestRuntime t;
    auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(t.run("while true do end"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("time budget") != std::string::npos);
}

TEST_CASE("pcall cannot swallow the time budget", "[plugin][lua_runtime][lua_budget]") {
    TestRuntime t;
    CHECK_FALSE(t.run(R"(
        while true do
            pcall(function() while true do end end)
        end
    )"));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("time budget") != std::string::npos);
}

TEST_CASE("work inside the budget is untouched", "[plugin][lua_runtime][lua_budget]") {
    TestRuntime t;
    REQUIRE(t.run("s = 0 for i = 1, 200000 do s = s + i end"));
    CHECK(t.global("s") == "20000100000");
    CHECK_FALSE(t.rt->faulted());
}

TEST_CASE("each outermost entry gets a fresh budget", "[plugin][lua_runtime][lua_budget]") {
    TestRuntime t; // 50 ms budget; three 20 ms runs exceed one budget but not their own
    lua_pushcfunction(t.rt->state(), [](lua_State* L) -> int {
        using namespace std::chrono;
        lua_pushinteger(
            L, duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        return 1;
    });
    lua_setglobal(t.rt->state(), "now_ms");
    const char* spin = "local t0 = now_ms() while now_ms() - t0 < 20 do end";
    CHECK(t.run(spin));
    CHECK(t.run(spin));
    CHECK(t.run(spin));
    CHECK_FALSE(t.rt->faulted());
}

namespace {
// helix.test_wait() suspends and hands its Pending to the test.
void install_test_wait(LuaRuntime& rt, std::vector<LuaRuntime::Pending>* sink) {
    lua_State* L = rt.state();
    lua_getglobal(L, "helix");
    lua_pushlightuserdata(L, sink);
    lua_pushcclosure(
        L,
        [](lua_State* co) -> int {
            auto* s = static_cast<std::vector<LuaRuntime::Pending>*>(
                lua_touserdata(co, lua_upvalueindex(1)));
            return LuaRuntime::from(co).await_async(
                co, [s](LuaRuntime::Pending p) { s->push_back(p); });
        },
        1);
    lua_setfield(L, -2, "test_wait");
    lua_pop(L, 1);
}

LuaRuntime::PushFn push_int(lua_Integer v) {
    return [v](lua_State* co) {
        lua_pushinteger(co, v);
        return 1;
    };
}

// helix.test_ref_many(f, n) takes n registry refs to f; the registry keeps every entry,
// so the held memory grows with n.
void install_test_ref_many(LuaRuntime& rt) {
    lua_State* L = rt.state();
    lua_getglobal(L, "helix");
    lua_pushcfunction(L, [](lua_State* co) -> int {
        auto& rt = LuaRuntime::from(co);
        int n = luaL_checkinteger(co, 2);
        for (int i = 0; i < n; ++i)
            rt.ref_value(co, 1);
        return 0;
    });
    lua_setfield(L, -2, "test_ref_many");
    lua_pop(L, 1);
}
} // namespace

TEST_CASE("an async call suspends and resumes with its result", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);

    REQUIRE(t.run("before = true; got = helix.test_wait(); after = got + 1"));
    CHECK(t.global("before") == "true");
    CHECK(t.global("after") == "nil");
    REQUIRE(pending.size() == 1);

    pending[0].resolve(push_int(41));
    CHECK(t.global("after") == "nil"); // resumes on the main loop, not inside resolve
    helix::ui::UpdateQueue::instance().drain();
    CHECK(t.global("after") == "42");
}

TEST_CASE("resolve is one-shot", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);
    REQUIRE(t.run("n = 0; helix.test_wait(); n = n + 1"));
    pending[0].resolve(push_int(1));
    pending[0].resolve(push_int(2));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(t.global("n") == "1");
}

TEST_CASE("a reply after the runtime is gone is dropped", "[plugin][lua_runtime][async]") {
    std::vector<LuaRuntime::Pending> pending;
    {
        TestRuntime t;
        install_test_wait(*t.rt, &pending);
        REQUIRE(t.run("helix.test_wait()"));
    }
    REQUIRE(pending.size() == 1);
    pending[0].resolve(push_int(1));
    helix::ui::UpdateQueue::instance().drain();
    SUCCEED(); // ASAN is what proves nothing touched the destroyed runtime
}

TEST_CASE("a reply after a fault is dropped", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);
    REQUIRE(t.run("helix.test_wait(); resumed = true"));
    t.run("error('1')");
    t.run("error('2')");
    t.run("error('3')");
    REQUIRE(t.rt->faulted());
    pending[0].resolve(push_int(1));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(t.global("resumed") == "nil");
}

TEST_CASE("a bare coroutine.yield at entry level is an error", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    CHECK_FALSE(t.run("coroutine.yield()"));
    CHECK(t.run("ok = true"));
}

TEST_CASE("an async call inside a plugin-made coroutine raises", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);
    REQUIRE(t.run(R"(
        local co = coroutine.wrap(function() return helix.test_wait() end)
        ok, err = pcall(co)
        mentions = tostring(err):find("async call not allowed") ~= nil
    )"));
    CHECK(t.global("ok") == "false");
    CHECK(t.global("mentions") == "true");
    CHECK(pending.empty());
}

TEST_CASE("nested entries stop at the depth cap", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    // helix.reenter(f) runs f as a new entry synchronously, the way a subject observer does.
    lua_State* L = t.rt->state();
    lua_getglobal(L, "helix");
    lua_pushcfunction(L, [](lua_State* co) -> int {
        auto& rt = LuaRuntime::from(co);
        int ref = rt.ref_value(co, 1);
        rt.invoke(ref);
        rt.unref(ref);
        return 0;
    });
    lua_setfield(L, -2, "reenter");
    lua_pop(L, 1);

    t.run(R"(
        depth = 0
        local function recurse() depth = depth + 1; helix.reenter(recurse) end
        recurse()
    )");
    CHECK(t.global("depth") == "8");
}

TEST_CASE("registering callbacks at the memory cap faults instead of aborting",
          "[plugin][lua_runtime][async]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    install_test_ref_many(*t.rt);
    CHECK_FALSE(t.run("helix.test_ref_many(function() end, 200000); "
                      "local t = {} for i = 1, 1000 do t[i] = {} end"));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("memory") != std::string::npos);
}

TEST_CASE("an async result larger than the cap faults the plugin", "[plugin][lua_runtime][async]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);
    // Both globals exist before the wait, so the resumed entry performs no allocation
    // and only the explicit post-push check can fault it.
    REQUIRE(t.run("got = false; after = false"));
    REQUIRE(t.run("got = helix.test_wait(); after = true"));
    pending[0].resolve([](lua_State* co) {
        std::string big(1 << 20, 'x');
        lua_pushlstring(co, big.data(), big.size());
        return 1;
    });
    helix::ui::UpdateQueue::instance().drain();
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("memory") != std::string::npos);
    CHECK(t.global("after") == "false"); // the entry never resumed past the wait
}

TEST_CASE("a budget kill swallowed by coroutine.resume still faults",
          "[plugin][lua_runtime][lua_budget]") {
    TestRuntime t;
    CHECK_FALSE(t.run("local co = coroutine.create(function() while true do end end); ok = "
                      "coroutine.resume(co)"));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("time budget") != std::string::npos);
}

TEST_CASE("a budget kill followed by a yield still faults", "[plugin][lua_runtime][lua_budget]") {
    TestRuntime t;
    CHECK_FALSE(t.run("local co = coroutine.create(function() while true do end end); ok = "
                      "coroutine.resume(co); coroutine.yield()"));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("time budget") != std::string::npos);
}

TEST_CASE("plugin-relative paths stay inside the plugin", "[plugin][lua_runtime]") {
    CHECK(is_plugin_relative_path("main.lua"));
    CHECK(is_plugin_relative_path("lib/util.lua"));
    CHECK_FALSE(is_plugin_relative_path(""));
    CHECK_FALSE(is_plugin_relative_path("/etc/x.lua"));
    CHECK_FALSE(is_plugin_relative_path("../x.lua"));
    CHECK_FALSE(is_plugin_relative_path("lib/../../x.lua"));
    CHECK_FALSE(is_plugin_relative_path("lib//x.lua"));
    CHECK_FALSE(is_plugin_relative_path("./x.lua"));
    CHECK_FALSE(is_plugin_relative_path("main.txt"));
    CHECK_FALSE(is_plugin_relative_path("a b.lua"));
    CHECK_FALSE(is_plugin_relative_path(std::string(125, 'a') + ".lua"));
}

// ../require-test/util.lua names a real file through a .. hop, and its content runs
// cleanly, so only the path check can refuse it.
TEST_CASE("run_file refuses a path outside the plugin", "[plugin][lua_runtime]") {
    TestRuntime t;
    CHECK_FALSE(t.rt->run_file("../require-test/util.lua"));
}

#endif // HELIX_HAS_PLUGINS
