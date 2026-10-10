// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_panel_home.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/home_panel_test_access.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "../test_helpers/process_async_timers.h"
#include "../test_helpers/scope_exit.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix-xml/src/xml/lv_xml_translation.h"
#include "misc/lv_timer_private.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "plugin_host.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

TEST_CASE("memory budget", "[plugin][host]") {
    CHECK(plugin_memory_budget(uint64_t(128) << 20) == size_t(8) << 20);
    CHECK(plugin_memory_budget(uint64_t(4) << 30) == size_t(64) << 20);
    CHECK(plugin_memory_budget(0) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "plugins are disabled until enabled", "[plugin][host]") {
    HostRig rig;
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello"));
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK(rig.info("require-test") == nullptr); // no manifest: not a plugin
    CHECK(lv_xml_get_subject(nullptr, "hello__status") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin i18n pack registers on load", "[plugin][host]") {
    HostRig rig(enabled("i18n-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("i18n-demo"));
    REQUIRE(rig.info("i18n-demo")->status == PluginStatus::Loaded);

    // The pack is global from load on; the unique tag cannot collide with any
    // app string, so its German value is proof the plugin's file registered.
    lv_translation_set_language("de");
    CHECK(std::string(lv_tr("i18n-demo only string")) == "Nur auf Deutsch");
    lv_translation_set_language("en");
    CHECK(std::string(lv_tr("i18n-demo only string")) == "i18n-demo only string");
}

TEST_CASE_METHOD(LVGLTestFixture, "an enabled plugin loads, binds and reacts", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello")->status == PluginStatus::Loaded);

    lv_subject_t* status = lv_xml_get_subject(nullptr, "hello__status");
    REQUIRE(status);
    CHECK(std::string(lv_subject_get_string(status)) == "hi");

    auto* panel =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "hello__panel", nullptr));
    REQUIRE(panel);
    lv_obj_t* button = lv_obj_find_by_name(panel, "hello__button");
    REQUIRE(button);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    CHECK(std::string(lv_subject_get_string(status)) == "pressed 7");

    rig.host->dispatch_event("hello__home");
    REQUIRE(rig.fake.requests.size() == 1);
    CHECK(rig.fake.requests[0].a == "G28");
    rig.fake.requests[0].reply(RpcResult{true, {}, {}});
    drain();
    CHECK(std::string(lv_subject_get_string(status)) == "homed");
    lv_obj_delete(panel);
}

TEST_CASE_METHOD(LVGLTestFixture, "unload runs on_unload and leaves nothing registered",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello")->status == PluginStatus::Loaded);
    rig.host->unload_all();
    REQUIRE_FALSE(rig.fake.requests.empty());
    CHECK(rig.fake.requests.back().a == "server.info");
    CHECK(lv_xml_get_subject(nullptr, "hello__status") == nullptr);
    CHECK(lv_xml_create(lv_screen_active(), "hello__panel", nullptr) == nullptr);
    rig.host->dispatch_event("hello__press");
    rig.fake.requests.back().reply(RpcResult{true, {}, {}});
    drain();
}

TEST_CASE_METHOD(LVGLTestFixture, "events for other or unknown plugins are ignored",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    lv_subject_t* status = lv_xml_get_subject(nullptr, "hello__status");
    rig.host->dispatch_event("other-plugin_press");
    rig.host->dispatch_event("hello__nosuchhandler");
    rig.host->dispatch_event("garbage");
    rig.host->dispatch_event("");
    CHECK(std::string(lv_subject_get_string(status)) == "hi");
}

TEST_CASE_METHOD(LVGLTestFixture, "fewer granted permissions than requested blocks loading",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {}));
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::NeedsApproval);
    CHECK(rig.info("hello")->reason.find("gcode") != std::string::npos);
    CHECK(lv_xml_get_subject(nullptr, "hello__status") == nullptr);

    CHECK(rig.host->enable("hello"));
    CHECK(rig.info("hello")->status == PluginStatus::Loaded);
    CHECK(rig.block["enabled"]["hello"]["permissions"] == json::array({"gcode"}));
}

TEST_CASE_METHOD(LVGLTestFixture, "an old list-shaped enabled key loads nothing",
                 "[plugin][host]") {
    HostRig rig(json{{"enabled", json::array({"hello"})}});
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK(rig.host->enable("hello"));
    CHECK(rig.block["enabled"].is_object());
}

TEST_CASE_METHOD(LVGLTestFixture, "a directory not matching its id is invalid", "[plugin][host]") {
    HostRig rig(enabled("other-id", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("bad-name"));
    CHECK(rig.info("bad-name")->status == PluginStatus::Invalid);
    CHECK_FALSE(rig.host->enable("other-id"));
}

TEST_CASE_METHOD(LVGLTestFixture, "an incompatible helix_version is not loaded", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    PluginHost::Deps d;
    // hello asks for >=0.0.1; an app reporting 0.0.0 fails that.
    d.backend = rig.fake.backend();
    d.read_block = [&] { return rig.block; };
    d.write_block = [&](const json& j) { rig.block = j; };
    d.helix_version = "0.0.0";
    d.memory_budget = size_t(64) << 20;
    rig.host = std::make_unique<PluginHost>(std::move(d));
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::Incompatible);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin over the memory budget is not loaded",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}), size_t(1) << 20);
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::OverBudget);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin that spins in main.lua faults and unloads",
                 "[plugin][host]") {
    HostRig rig(enabled("looper", {}));
    rig.host->load_from("tests/fixtures/plugins");
    drain();
    CHECK(rig.info("looper")->status == PluginStatus::Faulted);
    CHECK(rig.info("looper")->reason.find("time budget") != std::string::npos);
    CHECK(rig.host->runtime("looper") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "a fault queued by an old instance leaves a reload loaded",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* old_rt = rig.host->runtime("hello");
    REQUIRE(old_rt);
    // The budget kill faults the old runtime; its fault handler is deferred to the
    // next drain, and the reload lands before that drain runs it.
    CHECK_FALSE(old_rt->run_string("while true do end", "spin"));
    REQUIRE(rig.host->enable("hello"));
    LuaRuntime* new_rt = rig.host->runtime("hello");
    REQUIRE(new_rt);
    drain();
    CHECK(rig.host->runtime("hello") == new_rt);
    CHECK(rig.info("hello")->status == PluginStatus::Loaded);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin component that shadows an app component is rejected",
                 "[plugin][host]") {
    REQUIRE(lv_xml_register_component_from_data(
        "shadow__panel", "<component><view extends=\"lv_obj\" width=\"content\" height=\"content\">"
                         "<lv_label name=\"app_child\"/></view></component>"));
    helix::test::ScopeExit cleanup([] { lv_xml_component_unregister("shadow__panel"); });
    HostRig rig(enabled("shadow", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("shadow"));
    CHECK(rig.info("shadow")->status == PluginStatus::Invalid);
    CHECK(rig.info("shadow")->reason.find("already exists") != std::string::npos);
    CHECK(rig.host->runtime("shadow") == nullptr);

    auto* panel =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "shadow__panel", nullptr));
    REQUIRE(panel);
    REQUIRE(lv_obj_find_by_name(panel, "app_child"));
    lv_obj_delete(panel);
}

TEST_CASE_METHOD(LVGLTestFixture, "unload leaves an app component that took the plugin's name",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello")->status == PluginStatus::Loaded);
    REQUIRE(lv_xml_register_component_from_data(
        "hello__panel", "<component><view extends=\"lv_obj\" width=\"content\" height=\"content\">"
                        "<lv_label name=\"app_child\"/></view></component>"));
    helix::test::ScopeExit cleanup([] { lv_xml_component_unregister("hello__panel"); });
    rig.host->disable("hello");

    auto* panel =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "hello__panel", nullptr));
    REQUIRE(panel);
    CHECK(lv_obj_find_by_name(panel, "app_child"));
    lv_obj_delete(panel);
}

TEST_CASE_METHOD(LVGLTestFixture, "plugin XML naming an app callback is rejected at load",
                 "[plugin][host]") {
    HostRig rig(enabled("app-callback", {}));
    rig.host->load_from("tests/fixtures/plugins");
    const PluginInfo* info = rig.info("app-callback");
    REQUIRE(info);
    CHECK(info->status == PluginStatus::Invalid);
    CHECK(info->reason.find("on_estop_clicked") != std::string::npos);
    CHECK(lv_xml_component_get_scope("app-callback__panel") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "disable unloads and forgets consent", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    rig.host->disable("hello");
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK_FALSE(rig.block["enabled"].contains("hello"));
    CHECK(lv_xml_get_subject(nullptr, "hello__status") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "a loaded plugin's widget is in the registry until unload",
                 "[plugin][host]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("widget-demo")->status == PluginStatus::Loaded);
    const helix::PanelWidgetDef* def = helix::find_widget_def("widget-demo__tile");
    REQUIRE(def);
    CHECK(def->category == helix::WidgetCategory::Plugins);
    CHECK(def->colspan == 2);     // one cell, in tracks
    CHECK(def->max_colspan == 5); // two and a half cells
    CHECK(def->supports_half_col);
    CHECK(def->supports_half_row);
    // A two-cell by one-cell manifest widget registers four by two tracks.
    const helix::PanelWidgetDef* wide = helix::find_widget_def("widget-demo__wide");
    REQUIRE(wide);
    CHECK(wide->colspan == 4);
    CHECK(wide->rowspan == 2);
    CHECK_FALSE(wide->supports_half_col);
    CHECK_FALSE(wide->supports_half_row);

    auto w = def->factory("widget-demo__tile");
    REQUIRE(w);
    lv_obj_t* root = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), w->get_component_name().c_str(), nullptr));
    REQUIRE(root);
    w->attach_tile(root, lv_screen_active());
    w->notify_size_changed(4, 2, 200, 100);
    drain();
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(root, "widget-demo__size_label"))) ==
          "2x1");
    // A half-cell span reaches Lua as a fractional cell count.
    w->notify_size_changed(3, 2, 150, 100);
    drain();
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(root, "widget-demo__size_label"))) ==
          "1.5x1");

    rig.host->disable("widget-demo");
    CHECK(helix::find_widget_def("widget-demo__tile") == nullptr);
    // The plugin is gone: no hook runs against freed Lua state, nothing crashes.
    w->on_activate();
    w->notify_size_changed(2, 2, 100, 100);
    w->detach_tile();
    lv_obj_delete(root);
    w.reset();
}

TEST_CASE_METHOD(LVGLTestFixture, "a reloaded plugin's widget reaches only a fresh instance",
                 "[plugin][host]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    const helix::PanelWidgetDef* def = helix::find_widget_def("widget-demo__tile");
    REQUIRE(def);
    std::unique_ptr<helix::PanelWidget> stale = def->factory("widget-demo__tile");
    stale->notify_size_changed(4, 2, 200, 100);
    lv_subject_t* size = lv_xml_get_subject(nullptr, "widget-demo__size");
    REQUIRE(size);
    CHECK(std::string(lv_subject_get_string(size)) == "2x1");

    rig.host->disable("widget-demo");
    CHECK(rig.host->enable("widget-demo"));
    size = lv_xml_get_subject(nullptr, "widget-demo__size");
    REQUIRE(size);
    CHECK(std::string(lv_subject_get_string(size)) == ""); // the new runtime starts fresh

    // The instance built against the destroyed runtime is inert: driving it must
    // not reach the new runtime either.
    stale->on_activate();
    stale->notify_size_changed(2, 2, 100, 100);
    CHECK(std::string(lv_subject_get_string(size)) == "");

    // A fresh instance from the re-registered factory reaches the new runtime.
    def = helix::find_widget_def("widget-demo__tile");
    REQUIRE(def);
    std::unique_ptr<helix::PanelWidget> fresh = def->factory("widget-demo__tile");
    fresh->notify_size_changed(2, 2, 100, 100);
    CHECK(std::string(lv_subject_get_string(size)) == "1x1");
}

TEST_CASE_METHOD(LVGLTestFixture, "a widget whose component file is missing is invalid",
                 "[plugin][host]") {
    // Fixture widget-missing: manifest declares component widget-missing__tile, no ui/ file.
    HostRig rig(enabled("widget-missing", {}));
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("widget-missing")->status == PluginStatus::Invalid);
    CHECK(rig.info("widget-missing")->reason.find("not in ui/") != std::string::npos);
    CHECK(helix::find_widget_def("widget-missing__tile") == nullptr);
}

namespace {} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "an enabled reload rebuilds a placed home tile onto the new runtime",
                 "[plugin][host]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("widget-demo")->status == PluginStatus::Loaded);

    // A persisted two-page home layout: page 0 carries no widgets key, so the
    // registry defaults are not appended anywhere, and page 1 holds only the
    // plugin tile. The placement must live in the Config JSON rather than the
    // parsed cache: the def-change notify marks every panel config dirty and
    // the rebuild re-reads from disk.
    auto* cfg = helix::Config::get_instance();
    nlohmann::json tile = {{"id", "widget-demo__tile"},
                           {"enabled", true},
                           {"col", 0},
                           {"row", 0},
                           {"colspan", 2},
                           {"rowspan", 2}};
    nlohmann::json plug_page_cfg = {{"id", "plug"}, {"widgets", nlohmann::json::array({tile})}};
    nlohmann::json home_cfg = {{"main_page_index", 0},
                               {"next_page_id", 2},
                               {"pages", nlohmann::json::array({{{"id", "main"}}, plug_page_cfg})}};
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/home", home_cfg);

    auto& mgr = helix::PanelWidgetManager::instance();
    mgr.get_widget_config("home").mark_dirty();
    mgr.clear_panel_config("home");

    HomePanel& panel = get_global_home_panel();
    lv_obj_t* main_page = lv_obj_create(lv_screen_active());
    lv_obj_t* plug_page = lv_obj_create(lv_screen_active());
    lv_obj_set_size(main_page, 400, 300);
    lv_obj_set_size(plug_page, 400, 300);
    HomePanelTestAccess::set_page_containers(panel, {main_page, plug_page});
    HomePanelTestAccess::setup_gate_observers(panel);
    panel.populate_widgets();
    drain();
    lv_subject_t* size = lv_xml_get_subject(nullptr, "widget-demo__size");
    REQUIRE(size);
    CHECK(std::string(lv_subject_get_string(size)) == "1x1"); // the placed tile is live

    // enable() is unload + load of the same id: both def notifies coalesce into
    // one async rebuild whose visible id list is identical to the cached one.
    // The fresh runtime restarts its size subject empty, so only a rebuilt tile
    // can write a value back.
    CHECK(rig.host->enable("widget-demo"));
    drain();
    process_lvgl(50);
    process_async_timers();
    drain();

    size = lv_xml_get_subject(nullptr, "widget-demo__size");
    REQUIRE(size);
    CHECK(std::string(lv_subject_get_string(size)) == "1x1");

    helix::PanelWidgetManager::clear_gate_observers("home");
    HomePanelTestAccess::clear_page_containers(panel);
    // A null panel_widgets node reads as absent everywhere, restoring the
    // default-placement view for later tests in this process.
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/home", nlohmann::json());
    mgr.get_widget_config("home").mark_dirty();
    mgr.clear_panel_config("home");
    lv_obj_delete(main_page);
    lv_obj_delete(plug_page);
    drain();
}

#endif // HELIX_HAS_PLUGINS
