// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_fixtures.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lua_bindings.h"
#include "plugin_canvas.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

/// The plugin's subject value as a string; empty string when it is missing.
std::string demo_subject(const char* local) {
    std::string full = std::string("canvas-demo__") + local;
    lv_subject_t* s = lv_xml_get_subject(nullptr, full.c_str());
    return s && s->type == LV_SUBJECT_TYPE_STRING ? lv_subject_get_string(s) : "";
}

/// The plugin's int subject value; -1 when it is missing.
int32_t demo_int(const char* local) {
    std::string full = std::string("canvas-demo__") + local;
    lv_subject_t* s = lv_xml_get_subject(nullptr, full.c_str());
    return s && s->type == LV_SUBJECT_TYPE_INT ? lv_subject_get_int(s) : -1;
}

/// Loads canvas-demo with one panel instance on screen, laid out and drained.
struct DemoRig : XMLTestFixture {
    HostRig rig{enabled("canvas-demo", {})};
    lv_obj_t* panel = nullptr;

    DemoRig() {
        LogCapture logs;
        rig.host->load_from("tests/fixtures/plugins");
        REQUIRE(rig.info("canvas-demo"));
        if (rig.info("canvas-demo")->status != PluginStatus::Loaded) {
            std::ostringstream why;
            for (const std::string& line : logs.lines())
                if (line.find("canvas-demo") != std::string::npos)
                    why << "\n" << line;
            FAIL("canvas-demo failed to load: " << rig.info("canvas-demo")->reason << why.str());
        }
        panel = static_cast<lv_obj_t*>(
            lv_xml_create(lv_screen_active(), "canvas-demo__panel", nullptr));
        REQUIRE(panel);
        lv_obj_update_layout(panel);
        drain();
    }

    ~DemoRig() override {
        if (panel)
            lv_obj_delete(panel);
        canvas_set_size_listener("canvas-demo__c", {});
        canvas_commit("canvas-demo__c", nullptr);
    }

    lv_obj_t* canvas() {
        lv_obj_t* obj = lv_obj_find_by_name(panel, "canvas-demo__c");
        REQUIRE(obj);
        return obj;
    }

    void resize(int32_t w) {
        lv_obj_set_width(canvas(), w);
        lv_obj_update_layout(panel);
    }
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "commit publishes what was drawn", "[plugin][lua][canvas]") {
    BoundRuntime b({&install_canvas_bindings});
    REQUIRE(b.t.run(R"(
        local c = helix.canvas("c")
        c:line(0, 0, 10, 10, {color = "primary"})
        c:polyline({0, 0, 5, 5, 10, 0})
        c:commit()
    )"));
    const DisplayList* list = canvas_committed("test-plugin__c");
    REQUIRE(list);
    CHECK(list->prims.size() == 2);
    CHECK(list->points.size() == 3);
    CHECK(list->units == 1 + 3);
    CHECK(list->tokens.size() == 2); // "primary" and the default "text"

    // The pending list started fresh at commit, so a second commit blanks.
    REQUIRE(b.t.run("helix.canvas('c'):commit()"));
    list = canvas_committed("test-plugin__c");
    REQUIRE(list);
    CHECK(list->prims.empty());
    CHECK(list->units == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "each text and polyline reads its own bytes",
                 "[plugin][lua][canvas]") {
    BoundRuntime b({&install_canvas_bindings});
    REQUIRE(b.t.run(R"(
        local c = helix.canvas("c")
        c:text(0, 0, "one")
        c:text(0, 9, "two")
        c:polyline({0, 0, 5, 5, 10, 0})
        c:polyline({1, 1, 6, 6, 11, 1})
        c:commit()
    )"));
    const DisplayList* list = canvas_committed("test-plugin__c");
    REQUIRE(list);
    CHECK(list->prims.size() == 4);
    // first/count index the shared buffers, so the second of each kind must
    // not alias the first's bytes.
    std::vector<std::string> texts;
    std::vector<uint32_t> line_firsts;
    for (const CanvasPrim& p : list->prims) {
        if (p.op == CanvasOp::Text)
            texts.push_back(list->text.substr(p.first, p.count));
        if (p.op == CanvasOp::Polyline)
            line_firsts.push_back(p.first);
    }
    REQUIRE(texts.size() == 2);
    CHECK(texts[0] == "one");
    CHECK(texts[1] == "two");
    REQUIRE(line_firsts.size() == 2);
    CHECK(line_firsts[0] == 0);
    CHECK(line_firsts[1] == 3);
}

TEST_CASE_METHOD(XMLTestFixture, "opa sets a primitive's alpha percent", "[plugin][lua][canvas]") {
    BoundRuntime b({&install_canvas_bindings});
    REQUIRE(b.t.run(R"(
        local c = helix.canvas("c")
        c:line(0, 0, 1, 1, {opa = 50})
        c:polyline({0, 0, 5, 5})
        c:rect(0, 0, 2, 2, {fill = "text", opa = 0})
        c:arc(5, 5, 2, 0, 90, {opa = 100})
        c:commit()
    )"));
    const DisplayList* list = canvas_committed("test-plugin__c");
    REQUIRE(list);
    REQUIRE(list->prims.size() == 4);
    CHECK(list->prims[0].opa == 128); // 50 percent, rounded on the 0-255 scale
    CHECK(list->prims[1].opa == 255); // absent opa draws opaque
    CHECK(list->prims[2].opa == 0);
    CHECK(list->prims[3].opa == 255);
}

TEST_CASE_METHOD(XMLTestFixture, "a polyline fill carries its own color, alpha and baseline",
                 "[plugin][lua][canvas]") {
    BoundRuntime b({&install_canvas_bindings});
    REQUIRE(b.t.run(R"(
        local c = helix.canvas("c")
        c:polyline({0, 0, 5, 5, 10, 0}, {fill = "primary", fill_opa = 40, baseline = 42})
        c:polyline({0, 0, 5, 5})
        c:commit()
    )"));
    const DisplayList* list = canvas_committed("test-plugin__c");
    REQUIRE(list);
    REQUIRE(list->prims.size() == 2);
    const CanvasPrim& filled = list->prims[0];
    CHECK(list->tokens[filled.border] == "primary");
    CHECK(filled.a == 42);         // the baseline y rides in a
    CHECK(filled.fill_opa == 102); // 40 percent on the 0-255 scale
    const CanvasPrim& plain = list->prims[1];
    CHECK(plain.border == kNoToken); // no fill staged
    CHECK(plain.fill_opa == LV_OPA_COVER);
    // The fill's triangles are derived at draw time, so units are unchanged:
    // each polyline charges its point count, fill or not.
    CHECK(list->units == 3 + 2);
}

TEST_CASE_METHOD(XMLTestFixture, "each primitive validates its arguments",
                 "[plugin][lua][canvas]") {
    BoundRuntime b({&install_canvas_bindings});
    REQUIRE(b.t.run("c = helix.canvas('c'); c:line(0, 0, 1, 1)"));

    struct Bad {
        const char* code;
        const char* want;
    };
    static const Bad bad[] = {
        {"c:line(0/0, 0, 10, 10)", "finite"},
        {"c:line(1e9, 0, 10, 10)", "within"},
        {"c:line(0, 0, 1, 1, {width = 65})", "width"},
        {"c:line(0, 0, 1, 1, {opa = 101})", "opa"},
        {"c:line(0, 0, 1, 1, {opa = -1})", "opa"},
        {"c:rect(0, 0, 1, 1, {fill = 'text', opa = 101})", "opa"},
        {"c:line(0, 0, 1, 1, {width = -2})", "width"},
        {"c:line(0, 0, 1, 1, {color = 'nope'})", "color token"},
        {"c:line(0, 0, 1, 1, {colour = 'text'})", "unknown option"},
        {"c:polyline({1, 2, 3})", "even-length"},
        {"c:polyline({1, 2})", "even-length"},
        {"c:polyline('xy')", "even-length"},
        {"c:polyline({0, 0, 0/0, 5})", "finite"},
        {"c:polyline({0, 0, 'x', 5})", "number"},
        {"c:polyline({0, 0, 5, 5}, {fill = 'nope', baseline = 9})", "color token"},
        {"c:polyline({0, 0, 5, 5}, {fill = 'text', fill_opa = 101, baseline = 9})", "fill_opa"},
        {"c:polyline({0, 0, 5, 5}, {fill = 'text', baseline = 0/0})", "finite"},
        {"c:polyline({0, 0, 5, 5}, {fill = 'text', baseline = 1e9})", "within"},
        {"c:polyline({0, 0, 5, 5}, {fill = 'text'})", "needs a baseline"},
        {"c:polyline({0, 0, 5, 5}, {baseline = 9})", "needs a fill"},
        {"c:polyline({0, 0, 5, 5}, {filled = 'text', baseline = 9})", "unknown option"},
        {"c:rect(0, 0, 10, 10, {})", "fill or border"},
        {"c:rect(0, 0, 10, 10, {border = 'text', radius = -1})", "radius"},
        {"c:circle(5, 5, -1, {fill = 'text'})", "radius"},
        {"c:circle(5, 5, 4, {})", "fill or border"},
        {"c:arc(5, 5, 4, 0/0, 90)", "angles"},
        {"c:arc(5, 5, -4, 0, 90)", "radius"},
        {"c:text(0, 0, string.rep('a', 257))", "at most 256"},
        {"c:text(0, 0, 'x', {font = 'nope'})", "font token"},
        {"c:text(0, 0, 'x', {font = 'heading_large'})", "must be a base token"},
    };
    for (const Bad& row : bad) {
        CAPTURE(row.code);
        REQUIRE(b.t.run(std::string("ok, err = pcall(function() ") + row.code + " end)"));
        CHECK(b.t.global("ok") == "false");
        CHECK(b.t.global("err").find(row.want) != std::string::npos);
    }

    // A refused call adds nothing: only the first line made it into the list.
    REQUIRE(b.t.run("c:commit()"));
    const DisplayList* list = canvas_committed("test-plugin__c");
    REQUIRE(list);
    CHECK(list->prims.size() == 1);

    // An empty local name has no owned-name form, so it is refused outright.
    REQUIRE(b.t.run("ok, err = pcall(helix.canvas, '')"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("must not be empty") != std::string::npos);

    // The boundary values themselves are accepted.
    REQUIRE(b.t.run(R"(
        local c = helix.canvas("bounds")
        c:line(-16384, -16384, 16384, 16384, {width = 64})
        c:rect(0, 0, 5, 5, {fill = "text", radius = 16384})
        c:circle(5, 5, 0, {border = "text", border_width = 0})
        c:arc(5, 5, 16384, -1e6, 1e6)
        c:text(0, 0, string.rep("b", 256), {font = "body"})
        c:commit()
    )"));
    list = canvas_committed("test-plugin__bounds");
    REQUIRE(list);
    CHECK(list->prims.size() == 5);
}

TEST_CASE_METHOD(XMLTestFixture, "canvas fonts resolve only base tokens", "[plugin][lua][canvas]") {
    // The draw path skips a text prim whose font fails to resolve, and the
    // binding refuses the same name, so a size-suffixed token never reaches a
    // committed list on either path.
    CHECK(canvas_resolve_font("heading_large") == nullptr);
    CHECK(canvas_resolve_font("heading") != nullptr);
    CHECK(canvas_resolve_font("small") != nullptr); // the base token, not _small
    CHECK(canvas_resolve_font("nope") == nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "the unit cap holds and the list stays usable",
                 "[plugin][lua][canvas]") {
    BoundRuntime b({&install_canvas_bindings});
    REQUIRE(b.t.run(R"(
        local c = helix.canvas("u")
        for i = 1, 4096 do c:line(0, 0, 1, 1) end
        ok, err = pcall(c.line, c, 0, 0, 1, 1)
    )"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("at most 4096 primitives") != std::string::npos);

    REQUIRE(b.t.run("c = nil; local c = helix.canvas('u'); c:clear(); c:line(0, 0, 1, 1); "
                    "c:commit()"));
    const DisplayList* list = canvas_committed("test-plugin__u");
    REQUIRE(list);
    CHECK(list->prims.size() == 1);
    CHECK(list->units == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "the canvas count is capped", "[plugin][lua][canvas]") {
    BoundRuntime b({&install_canvas_bindings});
    REQUIRE(b.t.run(R"(
        local names = {}
        for i = 1, 8 do names[i] = helix.canvas("n" .. i) end
        again = helix.canvas("n1")
        names[1]:line(0, 0, 1, 1)
        again:commit()
        ok, err = pcall(helix.canvas, "n9")
    )"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("at most 8 canvases") != std::string::npos);
    // A repeated name returns the same canvas: the handle from the second call
    // committed what the first one drew.
    const DisplayList* list = canvas_committed("test-plugin__n1");
    REQUIRE(list);
    CHECK(list->prims.size() == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "list bytes count against the memory cap",
                 "[plugin][lua][canvas]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 128 * 1024;
    BoundRuntime b({&install_canvas_bindings}, {}, {}, "", limits);
    REQUIRE(b.t.run("collectgarbage()")); // snapshot the base after the ctor's own garbage
    const size_t base = b.t.rt->memory_used();

    // Filling the list raises the runtime's used bytes with the list's own.
    REQUIRE(b.t.run(R"(
        c = helix.canvas("m")
        for i = 1, 100 do c:line(0, 0, 10, 10) end
        c:commit()
        collectgarbage()
    )"));
    const DisplayList* list = canvas_committed("test-plugin__m");
    REQUIRE(list);
    const size_t after_commit = b.t.rt->memory_used();
    CAPTURE(base, list->bytes(), sizeof(DisplayList), sizeof(CanvasPrim), sizeof(std::string));
    CHECK(after_commit >= base + list->bytes());
    CHECK(after_commit <= base + list->bytes() + 2048);

    // Charging stops with a normal error, not a fault, and adds nothing.
    REQUIRE(b.t.run(R"(
        n = 0
        local ok, e
        repeat
            ok, e = pcall(c.line, c, 0, 0, 10, 10)
            if ok then n = n + 1 end
        until not ok
        cap_err = e
    )"));
    CHECK(b.t.global("n") != "0");
    CHECK(b.t.global("cap_err").find("would exceed the plugin memory cap") != std::string::npos);
    CHECK_FALSE(b.t.rt->faulted());

    // Clearing the pending list hands its bytes back; only the loop's own few
    // globals may keep any residue.
    const size_t filled = b.t.rt->memory_used();
    REQUIRE(filled > after_commit);
    REQUIRE(b.t.run("c:clear(); n, cap_err = nil; collectgarbage()"));
    const size_t after_clear = b.t.rt->memory_used();
    CHECK(after_clear >= after_commit);
    CHECK(after_clear <= after_commit + 1024); // residue: the loop's interned strings

    // Repeated draw/commit/clear cycles hand back every per-cycle byte; a leak
    // of even one cycle's charge would grow N-fold past any tolerance.
    const size_t before_cycles = b.t.rt->memory_used();
    REQUIRE(b.t.run(R"(
        for round = 1, 25 do
            for i = 1, 20 do c:line(0, 0, 10, 10) end
            c:commit()
            c:clear()
            collectgarbage()
        end
    )"));
    const size_t after_cycles = b.t.rt->memory_used();
    CHECK(after_cycles <= before_cycles + 1024);
}

TEST_CASE_METHOD(DemoRig, "on_size runs on the main loop with the content size",
                 "[plugin][lua][canvas]") {
    REQUIRE(demo_subject("sz") == "160x80");
    REQUIRE(demo_int("n") == 1);

    resize(200);
    drain();
    CHECK(demo_subject("sz") == "200x80");
    CHECK(demo_int("n") == 2);

    // Two resizes before one drain deliver one call, with the latest size.
    resize(220);
    resize(240);
    drain();
    CHECK(demo_subject("sz") == "240x80");
    CHECK(demo_int("n") == 3);
}

TEST_CASE_METHOD(DemoRig, "an on_size handler can remove itself", "[plugin][lua][canvas]") {
    REQUIRE(demo_int("n") == 1); // the ctor's delivery

    // The handler retires itself once the canvas reaches 220 wide: this
    // delivery runs one last time, and it removes itself.
    resize(240);
    drain();
    CHECK(demo_subject("sz") == "240x80");
    CHECK(demo_int("n") == 2);

    // The removed handler hears nothing further.
    resize(220);
    drain();
    CHECK(demo_subject("sz") == "240x80");
    CHECK(demo_int("n") == 2);
}

TEST_CASE_METHOD(DemoRig, "a size that returns to the last delivered value is not delivered",
                 "[plugin][lua][canvas]") {
    resize(200);
    drain();
    CHECK(demo_subject("sz") == "200x80");
    CHECK(demo_int("n") == 2);

    // 200 -> 160 -> 200 nets no change: the return to the last delivered size
    // is a no-op, not a second delivery.
    resize(160);
    resize(200);
    drain();
    CHECK(demo_subject("sz") == "200x80");
    CHECK(demo_int("n") == 2);
}

TEST_CASE_METHOD(DemoRig, "a reload delivers the known size to the new runtime",
                 "[plugin][lua][canvas]") {
    resize(200);
    drain();
    REQUIRE(demo_subject("sz") == "200x80");

    rig.host->rescan({"canvas-demo"});
    drain();
    CHECK(demo_subject("sz") == "200x80");
    CHECK(demo_int("n") == 1); // the new runtime's only delivery, with no resize
    const DisplayList* list = canvas_committed("canvas-demo__c");
    REQUIRE(list);
    CHECK(list->prims.size() == 1); // the new main.lua re-committed its line
}

TEST_CASE_METHOD(DemoRig, "unload blanks the canvas and drops queued size calls",
                 "[plugin][lua][canvas]") {
    resize(200); // queues a deferred size call
    REQUIRE(canvas_committed("canvas-demo__c") != nullptr);

    rig.host->disable("canvas-demo");
    drain(); // the queued call must not enter the dead runtime
    CHECK(canvas_committed("canvas-demo__c") == nullptr);
    lv_refr_now(nullptr); // the instance is still on screen; a blank canvas draws
}

TEST_CASE_METHOD(DemoRig, "a deleted instance with a queued size call is harmless",
                 "[plugin][lua][canvas]") {
    resize(200);
    lv_obj_delete(panel);
    panel = nullptr;
    drain();
    CHECK(canvas_size("canvas-demo__c") == std::make_pair(int32_t(0), int32_t(0)));
}

TEST_CASE_METHOD(DemoRig, "two instances of one canvas name share the slot",
                 "[plugin][lua][canvas]") {
    lv_obj_t* second =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "canvas-demo__panel", nullptr));
    REQUIRE(second);
    lv_obj_update_layout(second);
    drain();
    CHECK(canvas_instance_count("canvas-demo__c") == 2);
    CHECK(demo_int("n") == 1); // both report 160x80; only the first is a change

    lv_obj_t* second_canvas = lv_obj_find_by_name(second, "canvas-demo__c");
    REQUIRE(second_canvas != nullptr);
    lv_obj_set_width(second_canvas, 220);
    lv_obj_update_layout(second);
    drain();
    CHECK(demo_subject("sz") == "220x80");
    CHECK(demo_int("n") == 2);

    lv_obj_delete(panel);
    panel = second;
    CHECK(canvas_instance_count("canvas-demo__c") == 1);
    CHECK(canvas_committed("canvas-demo__c") != nullptr);
    lv_refr_now(nullptr);
}

#endif // HELIX_HAS_PLUGINS
