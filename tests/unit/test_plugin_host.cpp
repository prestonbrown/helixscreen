// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_icon_image.h"
#include "ui_panel_home.h"
#include "ui_temp_display.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/home_panel_test_access.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "../test_helpers/process_async_timers.h"
#include "config.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix-xml/src/xml/lv_xml_translation.h"
#include "lodepng_encode.h"
#include "misc/lv_timer_private.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "plugin_host.h"
#include "scope_exit.h"
#include "translation_loader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

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

namespace {

/// A loadable plugin whose i18n/ folder holds the given (file name, contents) packs.
void write_i18n_plugin(const std::filesystem::path& root, const std::string& id,
                       const std::vector<std::pair<std::string, std::string>>& packs) {
    std::filesystem::create_directories(root / id / "i18n");
    std::ofstream(root / id / "manifest.json")
        << R"({"id":")" << id
        << R"(","name":"I18n Pack","version":"1.0.0","description":"x","settings":[]})";
    std::ofstream(root / id / "main.lua") << "-- no code\n";
    for (const auto& [name, text] : packs)
        std::ofstream(root / id / "i18n" / name) << text;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a plugin pack never overrides the app's own translation",
                 "[plugin][host][i18n]") {
    helix::ScopeExit restore([] { lv_translation_set_language("en"); });
    // The boot language: the app's pack is registered before any plugin loads.
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");
    const std::string app_cancel = lv_tr("Cancel");
    REQUIRE(app_cancel != "Cancel"); // the app translates it

    TempDir tmp;
    // "fr" has an empty slot for Cancel: a head-of-list pack would answer with the English
    // tag and hide the app's French string.
    write_i18n_plugin(
        tmp.path, "i18n-shadow",
        {{"de.xml",
          "<translations languages=\"de fr\">"
          "<translation tag=\"Cancel\" de=\"PLUGIN-ABBRUCH\"/>"
          "<translation tag=\"i18n-shadow only string\" de=\"Nur Plugin\" fr=\"Seulement\"/>"
          "</translations>"},
         {"en.xml", "<translations languages=\"en\">"
                    "<translation tag=\"Cancel\" en=\"PLUGIN-CANCEL\"/>"
                    "</translations>"}});
    HostRig rig(enabled("i18n-shadow", {}));
    rig.host->load_from(tmp.path.string());
    REQUIRE(rig.info("i18n-shadow")->status == PluginStatus::Loaded);

    CHECK(std::string(lv_tr("Cancel")) == app_cancel);
    CHECK(std::string(lv_tr("i18n-shadow only string")) == "Nur Plugin");

    // A language switched to at run time loads its app pack after the plugin's.
    helix::ui::ensure_translation_loaded("fr");
    lv_translation_set_language("fr");
    const std::string fr_cancel = lv_tr("Cancel");
    CHECK(fr_cancel != "Cancel");
    CHECK(fr_cancel != "PLUGIN-ABBRUCH");
    CHECK(std::string(lv_tr("i18n-shadow only string")) == "Seulement");

    // English is the tag itself, so a pack cannot relabel it.
    lv_translation_set_language("en");
    CHECK(std::string(lv_tr("Cancel")) == "Cancel");
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin pack past the size cap is not registered",
                 "[plugin][host][i18n]") {
    helix::ScopeExit restore([] { lv_translation_set_language("en"); });
    TempDir tmp;
    const std::string body = "<translation tag=\"i18n-big only string\" de=\"Gross\"/>";
    const std::string pad(300 * 1024, ' ');
    write_i18n_plugin(tmp.path, "i18n-big",
                      {{"big.xml", "<translations languages=\"de\"><!--" + pad + "-->" + body +
                                       "</translations>"}});
    write_i18n_plugin(tmp.path, "i18n-small",
                      {{"small.xml", "<translations languages=\"de\">"
                                     "<translation tag=\"i18n-small only string\" de=\"Klein\"/>"
                                     "</translations>"}});
    HostRig rig(enabled_all({{"i18n-big", {}}, {"i18n-small", {}}}));
    rig.host->load_from(tmp.path.string());
    REQUIRE(rig.info("i18n-big")->status == PluginStatus::Loaded);

    lv_translation_set_language("de");
    CHECK(std::string(lv_tr("i18n-big only string")) == "i18n-big only string");
    CHECK(std::string(lv_tr("i18n-small only string")) == "Klein");
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
    helix::ScopeExit cleanup([] { lv_xml_component_unregister("shadow__panel"); });
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
    helix::ScopeExit cleanup([] { lv_xml_component_unregister("hello__panel"); });
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
    // The rebuilt home page builds its default temperature tile, whose
    // temp_display component wraps this C++ widget.
    ui_temp_display_init();
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

TEST_CASE_METHOD(LVGLTestFixture, "a plugin's image icons live while it is loaded",
                 "[plugin][host][icons]") {
    TempDir tmp;
    TempDir outside;
    const auto root = tmp.path / "icon-demo";
    std::filesystem::create_directories(root / "icons");
    std::ofstream(root / "manifest.json") << R"({"id":"icon-demo","name":"Icons","version":"1.0.0",
        "icons":{"icon-demo__oil":{"src":"icons/oil.png","fallback":"water"},
                 "icon-demo__lost":{"src":"icons/lost.png"},
                 "icon-demo__escape":{"src":"icons/escape.png"}},
        "widgets":[{"id":"icon-demo__w","name":"W","component":"icon-demo__tile",
                    "icon":"icon-demo__oil"}]})";
    std::ofstream(root / "main.lua") << "-- no code\n";
    std::filesystem::create_directories(root / "ui");
    std::ofstream(root / "ui" / "icon-demo__tile.xml")
        << R"(<component><view extends="lv_obj"><icon src="icon-demo__oil"/></view></component>)";
    // A one-pixel opaque PNG.
    const unsigned char kPixel[] = {0, 0, 0, 255};
    unsigned char* png = nullptr;
    size_t png_size = 0;
    REQUIRE(lodepng_encode32(&png, &png_size, kPixel, 1, 1) == 0);
    for (const auto& path : {(root / "icons" / "oil.png").string(), outside.file("secret.png")})
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(png), static_cast<std::streamsize>(png_size));
    free(png);
    std::filesystem::create_symlink(outside.file("secret.png"), root / "icons" / "escape.png");

    {
        HostRig rig(enabled("icon-demo", {}));
        rig.host->load_from(tmp.path.string());
        REQUIRE(rig.info("icon-demo"));
        REQUIRE(rig.info("icon-demo")->status == PluginStatus::Loaded);
        CHECK(helix::ui::icon::image_icon_available("icon-demo__oil"));
        // Missing artwork, and artwork a symlink places outside the folder, draw the
        // fallback without failing the load.
        CHECK_FALSE(helix::ui::icon::image_icon_available("icon-demo__lost"));
        CHECK_FALSE(helix::ui::icon::image_icon_available("icon-demo__escape"));
        const helix::PanelWidgetDef* def = helix::find_widget_def("icon-demo__w");
        REQUIRE(def);
        CHECK(std::string(def->icon) == "icon-demo__oil");
    }
    CHECK_FALSE(helix::ui::icon::image_icon_available("icon-demo__oil"));
}

TEST_CASE_METHOD(LVGLTestFixture, "a loaded plugin fills its plugin_table from Lua",
                 "[plugin][host][lua_bindings_table]") {
    TempDir tmp;
    const auto root = tmp.path / "table-demo";
    std::filesystem::create_directories(root / "ui");
    std::ofstream(root / "manifest.json")
        << R"({"id":"table-demo","name":"Tables","version":"1.0.0"})";
    std::ofstream(root / "ui" / "table-demo__list.xml")
        << R"(<component><view extends="lv_obj"><plugin_table name="table-demo__t"/></view></component>)";
    std::ofstream(root / "main.lua")
        << "helix.ui.table('t', {columns = {{title = 'Task'}}}):set_rows({{key = 'k', 'Belts'}})\n";
    HostRig rig(enabled("table-demo", {}));
    rig.host->load_from(tmp.path.string());
    REQUIRE(rig.info("table-demo"));
    REQUIRE(rig.info("table-demo")->status == PluginStatus::Loaded);
    // The host registers the element: the plugin's own component builds an lv_table.
    auto* view = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "table-demo__list", nullptr));
    REQUIRE(view);
    lv_obj_t* table = lv_obj_find_by_name(view, "table-demo__t");
    REQUIRE(table);
    REQUIRE(lv_obj_check_type(table, &lv_table_class));
    CHECK(lv_table_get_row_count(table) == 2);
    CHECK(std::string(lv_table_get_cell_value(table, 1, 0)) == "Belts");
    lv_obj_delete(view);
}

#endif // HELIX_HAS_PLUGINS
