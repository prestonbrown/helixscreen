// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// helix.ui.dialog / overlay / confirm through a real host: they wait for app
// dialogs, report when they appear, and say exactly once how they ended.

#if HELIX_HAS_PLUGINS

#include "ui_modal.h"
#include "ui_nav_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "display_settings_manager.h"
#include "lua_runtime.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

constexpr const char* kAppModal = "print_cancel_confirm_modal";

class SurfacesFx : public LVGLUITestFixture {
  public:
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    HostRig rig{enabled("widget-demo", {})};
    LuaRuntime* rt = nullptr;

    SurfacesFx() {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        rig.host->load_from("tests/fixtures/plugins");
        rt = rig.host->runtime("widget-demo");
        // A headless test display never sleeps or locks.
        rig.host->presenter().set_wake([] {});
        REQUIRE(rt);
        REQUIRE(lua(R"(log = {}
            function rec(s) log[#log + 1] = s end
            function joined() return table.concat(log, ",") end)"));
    }
    ~SurfacesFx() override {
        settle();
        DisplaySettingsManager::instance().set_animations_enabled(true);
    }

    bool lua(const char* code) {
        return rt->run_string(code, "t");
    }
    std::string events() {
        REQUIRE(lua("_events = joined()"));
        lua_getglobal(rt->state(), "_events");
        std::string s = lua_tostring(rt->state(), -1);
        lua_pop(rt->state(), 1);
        return s;
    }
    std::string err() {
        lua_getglobal(rt->state(), "err");
        std::string s = lua_isstring(rt->state(), -1) ? lua_tostring(rt->state(), -1) : "";
        lua_pop(rt->state(), 1);
        return s;
    }
    void settle() {
        drain();
        process_lvgl(500);
    }
};

const char* kOpenDialog = R"(h = helix.ui.dialog("widget-demo__ask", {
    on_open = function() rec("open") end,
    on_close = function(how) rec("close:" .. how) end}))";

} // namespace

TEST_CASE_METHOD(SurfacesFx, "dialog: queued under an app modal, on_open after it closes",
                 "[plugin][bindings][ui]") {
    lv_obj_t* app = Modal::show(kAppModal);
    REQUIRE(app != nullptr);
    REQUIRE(lua(kOpenDialog));
    settle();
    CHECK(events().empty());
    REQUIRE(lua("is_open = h.is_open()"));

    Modal::hide(app);
    settle();
    CHECK(events() == "open");
    CHECK(ModalStack::instance().top_visible_tag() == "widget-demo");
}

TEST_CASE_METHOD(SurfacesFx, "dialog: close while queued -> cancelled, no on_open",
                 "[plugin][bindings][ui]") {
    lv_obj_t* app = Modal::show(kAppModal);
    REQUIRE(lua(kOpenDialog));
    REQUIRE(lua("h.close()"));
    Modal::hide(app);
    settle();
    CHECK(events() == "close:cancelled");
}

TEST_CASE_METHOD(SurfacesFx, "dialog: close while shown -> plugin", "[plugin][bindings][ui]") {
    REQUIRE(lua(kOpenDialog));
    settle();
    REQUIRE(lua("shown = h.is_open(); h:close()"));
    settle();
    CHECK(events() == "open,close:plugin");
    lua_getglobal(rt->state(), "shown");
    CHECK(lua_toboolean(rt->state(), -1));
    lua_pop(rt->state(), 1);
}

TEST_CASE_METHOD(SurfacesFx, "dialog: backdrop tap -> user", "[plugin][bindings][ui]") {
    REQUIRE(lua(kOpenDialog));
    settle();
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_send_event(ModalStack::instance().backdrop_for(dialog), LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK(events() == "open,close:user");
}

TEST_CASE_METHOD(SurfacesFx, "dialog: a second dialog is an error naming the limit",
                 "[plugin][bindings][ui]") {
    REQUIRE(lua(kOpenDialog));
    REQUIRE(lua(R"(ok, err = pcall(helix.ui.dialog, "widget-demo__ask"))"));
    CHECK(err().find("at most 1 dialog per plugin") != std::string::npos);
}

TEST_CASE_METHOD(SurfacesFx, "dialog: an unowned component is an error", "[plugin][bindings][ui]") {
    REQUIRE(lua(R"(ok, err = pcall(helix.ui.dialog, "print_cancel_confirm_modal"))"));
    CHECK(err().find("not owned") != std::string::npos);
}

TEST_CASE_METHOD(SurfacesFx, "overlay: on_open runs; plugin close reports plugin",
                 "[plugin][bindings][ui]") {
    REQUIRE(lua(R"(o = helix.ui.overlay("widget-demo__panel", {
        on_open = function() rec("open") end,
        on_close = function(how) rec("close:" .. how) end}))"));
    settle();
    REQUIRE(lua("o:close()"));
    settle();
    CHECK(events() == "open,close:plugin");
}

TEST_CASE_METHOD(SurfacesFx, "overlay: navigation back reports user", "[plugin][bindings][ui]") {
    REQUIRE(lua(R"(o = helix.ui.overlay("widget-demo__panel", {
        on_close = function(how) rec("close:" .. how) end}))"));
    settle();
    REQUIRE(NavigationManager::instance().has_open_overlays());
    NavigationManager::instance().go_back();
    settle();
    CHECK(events() == "close:user");
}

TEST_CASE_METHOD(SurfacesFx, "overlay: re-request while queued returns the same handle",
                 "[plugin][bindings][ui]") {
    lv_obj_t* app = Modal::show(kAppModal);
    REQUIRE(lua(R"(a = helix.ui.overlay("widget-demo__panel", {on_open = function() rec("a") end})
        b = helix.ui.overlay("widget-demo__panel", {on_open = function() rec("b") end}))"));
    Modal::hide(app);
    settle();
    CHECK(events() == "a");
}

TEST_CASE_METHOD(SurfacesFx, "confirm: waits behind an app modal", "[plugin][bindings][ui]") {
    lv_obj_t* app = Modal::show(kAppModal);
    REQUIRE(lua(R"(helix.ui.confirm("Sure?", "Really"))"));
    settle();
    CHECK(ModalStack::instance().top_dialog() == app);

    Modal::hide(app);
    settle();
    lv_obj_t* top = ModalStack::instance().top_dialog();
    CHECK(top != nullptr);
    CHECK(ModalStack::instance().top_visible_tag() == "widget-demo");
}

TEST_CASE_METHOD(SurfacesFx, "unload: shown and queued surfaces close, no callbacks",
                 "[plugin][bindings][ui]") {
    REQUIRE(lua(kOpenDialog));
    settle();
    REQUIRE(lua(R"(o = helix.ui.overlay("widget-demo__panel", {
        on_open = function() rec("overlay") end,
        on_close = function(how) rec("oclose:" .. how) end}))"));
    settle();
    CHECK(events() == "open,overlay"); // a plugin's own overlay opens over its own dialog
    rig.host->disable("widget-demo");
    settle();
    CHECK(ModalStack::instance().top_visible_tag().empty());
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
}

TEST_CASE_METHOD(SurfacesFx, "a plugin dialog's primary button reaches the plugin handler",
                 "[plugin][bindings][ui]") {
    REQUIRE(lua(R"(helix.ui.on("pick", function(arg) rec("pick:" .. tostring(arg)) end)
        d = helix.ui.dialog("widget-demo__choose"))"));
    settle();
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* ok = lv_obj_find_by_name(dialog, "widget-demo__ok");
    REQUIRE(ok != nullptr);
    CHECK(lv_obj_find_by_name(dialog, "btn_primary") == nullptr);
    CHECK(lv_obj_find_by_name(dialog, "widget-demo__htitle") != nullptr);
    CHECK(lv_obj_find_by_name(dialog, "header_title") == nullptr);
    lv_obj_send_event(ok, LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK(events() == "pick:2");
}

#endif // HELIX_HAS_PLUGINS
