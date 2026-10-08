// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "../test_helpers/plugin_test_support.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "panel_widget_registry.h"
#include "plugin_host.h"

#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

// Plugins are written to a temp dir and loaded through the real PluginHost, Lua runtime and
// XML registry; only the Moonraker backend is the recording fake.

using namespace helix::plugin;
using namespace helix::plugin::test;
namespace fs = std::filesystem;

namespace {

const char* kPanelXml = R"(<?xml version="1.0"?>
<component>
  <view extends="lv_obj" width="content" height="content">
    <lv_label name="@ID@__label" bind_text="@ID@__status"/>
  </view>
</component>
)";

/// Writes <root>/<id>/{manifest.json,main.lua,ui/<id>__panel.xml} and returns the plugin dir.
void write_plugin(const fs::path& root, const std::string& id, const std::string& main_lua,
                  const std::string& manifest_extra = "") {
    fs::create_directories(root / id / "ui");
    std::ofstream(root / id / "manifest.json")
        << R"({"id": ")" << id << R"(", "name": "T", "version": "1.0.0")" << manifest_extra << "}";
    std::ofstream(root / id / "main.lua") << main_lua;
    std::string xml = kPanelXml;
    for (size_t p; (p = xml.find("@ID@")) != std::string::npos;)
        xml.replace(p, 4, id);
    std::ofstream(root / id / "ui" / (id + "__panel.xml")) << xml;
}

bool nothing_registered(const std::string& id) {
    return lv_xml_get_subject(nullptr, (id + "__status").c_str()) == nullptr &&
           lv_xml_component_find_scope((id + "__panel").c_str()) == nullptr;
}

const char* kFullMain = R"(
    local status = helix.subject.string("status", "up")
    helix.moonraker.subscribe({ extruder = { "temperature" } }, function() end)
    helix.timer.every(1000, function() end)
    helix.widget("tile", {})
)";

const char* kWidgetManifest = R"(, "widgets": [{"id": "wp__tile", "name": "Tile",
    "component": "wp__panel", "colspan": 1, "rowspan": 1}])";

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a loaded plugin registers its subject, component and widget",
                 "[plugin][load]") {
    TempDir tmp;
    write_plugin(tmp.path, "wp", kFullMain, kWidgetManifest);
    HostRig rig(enabled("wp", {}));
    rig.host->load_from(tmp.path.string());

    REQUIRE(rig.info("wp")->status == PluginStatus::Loaded);
    CHECK(lv_xml_get_subject(nullptr, "wp__status") != nullptr);
    CHECK(lv_xml_component_find_scope("wp__panel") != nullptr);
    CHECK(helix::find_widget_def("wp__tile") != nullptr);
    REQUIRE_FALSE(rig.fake.notify.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "unload removes every registration the plugin made",
                 "[plugin][load]") {
    TempDir tmp;
    write_plugin(tmp.path, "wp", kFullMain, kWidgetManifest);
    HostRig rig(enabled("wp", {}));
    rig.host->load_from(tmp.path.string());
    REQUIRE(rig.info("wp")->status == PluginStatus::Loaded);

    rig.host->unload_all();
    drain();

    CHECK(nothing_registered("wp"));
    CHECK(helix::find_widget_def("wp__tile") == nullptr);
    CHECK(rig.host->runtime("wp") == nullptr);
    CHECK(rig.fake.notify_unregistered == 1);
    CHECK(rig.fake.object_sets.back().second == json::object());
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin reloads after it was unloaded", "[plugin][load]") {
    TempDir tmp;
    write_plugin(tmp.path, "wp", kFullMain, kWidgetManifest);
    HostRig rig(enabled("wp", {}));
    rig.host->load_from(tmp.path.string());
    rig.host->disable("wp");
    REQUIRE(nothing_registered("wp"));

    CHECK(rig.host->enable("wp"));
    CHECK(rig.info("wp")->status == PluginStatus::Loaded);
    CHECK(lv_xml_get_subject(nullptr, "wp__status") != nullptr);
    CHECK(helix::find_widget_def("wp__tile") != nullptr);

    rig.host->load_from(tmp.path.string()); // a second full scan reloads cleanly too
    CHECK(rig.info("wp")->status == PluginStatus::Loaded);
    CHECK(rig.fake.notify_unregistered == 2); // each of the two earlier loads unsubscribed
}

TEST_CASE_METHOD(LVGLTestFixture, "a Lua syntax error fails the load and leaves nothing behind",
                 "[plugin][load]") {
    TempDir tmp;
    write_plugin(tmp.path, "wp", "local x = = 1\n");
    HostRig rig(enabled("wp", {}));
    rig.host->load_from(tmp.path.string());
    drain();

    CHECK(rig.info("wp")->status == PluginStatus::Faulted);
    CHECK_FALSE(rig.info("wp")->reason.empty());
    CHECK(rig.host->runtime("wp") == nullptr);
    CHECK(nothing_registered("wp"));
}

TEST_CASE_METHOD(LVGLTestFixture, "a runtime error in main.lua rolls back partial registration",
                 "[plugin][load]") {
    TempDir tmp;
    write_plugin(tmp.path, "wp", std::string(kFullMain) + "error('boom')\n", kWidgetManifest);
    HostRig rig(enabled("wp", {}));
    rig.host->load_from(tmp.path.string());
    drain();

    CHECK(rig.info("wp")->status == PluginStatus::Faulted);
    CHECK(rig.host->runtime("wp") == nullptr);
    CHECK(nothing_registered("wp"));
    CHECK(helix::find_widget_def("wp__tile") == nullptr);
    CHECK(rig.fake.notify_unregistered == 1);

    // The failed load does not poison a retry once the plugin is fixed.
    write_plugin(tmp.path, "wp", kFullMain, kWidgetManifest);
    rig.host->rescan({"wp"});
    drain();
    CHECK(rig.info("wp")->status == PluginStatus::Loaded);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin without a permission cannot use the API behind it",
                 "[plugin][load]") {
    TempDir tmp;
    write_plugin(tmp.path, "wp", R"(
        ok, err = pcall(helix.gcode, "G28")
        helix.subject.string("status", ok and "allowed" or "denied")
    )");
    HostRig rig(enabled("wp", {}));
    rig.host->load_from(tmp.path.string());

    REQUIRE(rig.info("wp")->status == PluginStatus::Loaded);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "wp__status"))) ==
          "denied");
    CHECK(rig.fake.requests.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a manifest naming an unknown permission is invalid",
                 "[plugin][load]") {
    TempDir tmp;
    write_plugin(tmp.path, "wp", "", R"(, "permissions": ["root_shell"])");
    HostRig rig(enabled("wp", {}));
    rig.host->load_from(tmp.path.string());

    CHECK(rig.info("wp")->status == PluginStatus::Invalid);
    CHECK(nothing_registered("wp"));
}

#endif // HELIX_HAS_PLUGINS
