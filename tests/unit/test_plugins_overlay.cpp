// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_modal.h"
#include "ui_nav_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "display_settings_manager.h"
#include "plugin_consent.h"
#include "plugins_overlay.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

constexpr const char* PLUGINS_DIR = "tests/fixtures/plugins";

/// The permissions string form of every permission, so an enabled entry always
/// covers the manifest and cannot land in NeedsApproval by accident.
std::vector<std::string> all_perms() {
    return {"gcode", "moonraker_write", "http", "storage"};
}

/// LVGLUITestFixture plus a deterministic navigation seed (animations off so
/// close paths run inline) and the Plugins overlay opened against a HostRig.
class ListFx : public LVGLUITestFixture {
  public:
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    std::unique_ptr<HostRig> rig;
    lv_obj_t* root = nullptr;
    int consent_calls = 0;
    std::vector<Permission> consent_grown;

    ListFx() {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
    }
    ~ListFx() override {
        drain();
        PluginsOverlay& ov = get_plugins_overlay();
        if (lv_obj_t* r = ov.root()) {
            lv_obj_t* held = r;
            ov.destroy_overlay_ui(held);
        }
        ov.set_consent_shower(nullptr); // back to the real consent dialog
        drain();
        process_lvgl(100); // deferred deletes
        DisplaySettingsManager::instance().set_animations_enabled(true);
    }

    /// Creates the host over `initial`, scans the fixtures, then opens the
    /// overlay: the same order the Settings row produces (host exists first).
    void open(json initial = json::object()) {
        rig = std::make_unique<HostRig>(std::move(initial));
        rig->host->load_from(PLUGINS_DIR);
        get_plugins_overlay().register_callbacks(); // show() does this before every create
        root = get_plugins_overlay().create(lv_screen_active());
        REQUIRE(root != nullptr);
    }

    /// Installs a consent shower that records the ask and answers yes.
    void auto_yes_consent() {
        get_plugins_overlay().set_consent_shower([this](const Manifest&,
                                                        const std::vector<Permission>& grown,
                                                        std::function<void()> on_yes) {
            ++consent_calls;
            consent_grown = grown;
            on_yes();
        });
    }

    lv_obj_t* row(const char* dir) {
        return lv_obj_find_by_name(root, (std::string("row_") + dir).c_str());
    }
    static std::string label_text(lv_obj_t* r) {
        lv_obj_t* l = lv_obj_find_by_name(r, "label");
        return l ? lv_label_get_text(l) : std::string();
    }
    static std::string desc_text(lv_obj_t* r) {
        lv_obj_t* d = lv_obj_find_by_name(r, "description");
        return d ? lv_label_get_text(d) : std::string();
    }
    void tap(const char* dir) {
        lv_obj_t* r = row(dir);
        REQUIRE(r != nullptr);
        lv_obj_send_event(r, LV_EVENT_CLICKED, nullptr);
        drain();
    }
    /// The consent/choice dialog's button, clicked.
    void click_btn(const char* btn) {
        lv_obj_t* dialog = helix::ui::modal_get_top();
        REQUIRE(dialog != nullptr);
        lv_obj_t* b = lv_obj_find_by_name(dialog, btn);
        REQUIRE(b != nullptr);
        lv_obj_send_event(b, LV_EVENT_CLICKED, nullptr);
        drain();
        process_lvgl(50);
    }
};

} // namespace

TEST_CASE("one row per discovered plugin, status and reason in the description",
          "[plugin][plugins_overlay]") {
    ListFx fx;
    fx.open();
    lv_obj_t* rows = lv_obj_find_by_name(fx.root, "plugins_rows");
    REQUIRE(rows != nullptr);
    CHECK(lv_obj_get_child_count(rows) == 11); // require-test has no manifest
    CHECK(fx.row("require-test") == nullptr);
    CHECK(fx.label_text(fx.row("hello")) == "Hello");
    CHECK(fx.desc_text(fx.row("hello")).find("disabled") != std::string::npos);
    // bad-name's manifest failed validation, so the label falls back to the
    // directory name and the reason names the mismatch.
    CHECK(fx.label_text(fx.row("bad-name")) == "bad-name");
    CHECK(fx.desc_text(fx.row("bad-name")).find("must match") != std::string::npos);
}

TEST_CASE("tapping a disabled plugin consents, then enables with its permissions recorded",
          "[plugin][plugins_overlay]") {
    ListFx fx;
    fx.open();
    fx.auto_yes_consent();
    fx.tap("hello");
    CHECK(fx.consent_calls == 1);
    CHECK(fx.consent_grown.empty()); // first enable: nothing granted yet to grow from
    CHECK(fx.rig->info("hello")->status == PluginStatus::Loaded);
    const json entry = fx.rig->block.at("enabled").at("hello");
    CHECK(entry.at("version") == "1.0.0");
    CHECK(entry.at("permissions") == json::array({"gcode"}));
    CHECK(fx.desc_text(fx.row("hello")).find("loaded") != std::string::npos);
}

TEST_CASE("a plugin whose update asks for more permissions re-prompts for the growth only",
          "[plugin][plugins_overlay]") {
    ListFx fx;
    fx.open(enabled("hello", {})); // granted nothing: gcode is new
    CHECK(fx.rig->info("hello")->status == PluginStatus::NeedsApproval);
    fx.auto_yes_consent();
    fx.tap("hello");
    CHECK(fx.consent_calls == 1);
    REQUIRE(fx.consent_grown.size() == 1);
    CHECK(fx.consent_grown[0] == Permission::Gcode);
    CHECK(fx.rig->info("hello")->status == PluginStatus::Loaded);
    CHECK(fx.rig->block.at("enabled").at("hello").at("permissions") == json::array({"gcode"}));
}

TEST_CASE_METHOD(ListFx, "the loaded row's Disable button unloads and erases the entry",
                 "[plugin][plugins_overlay]") {
    open();
    REQUIRE(rig->host->enable("hello"));
    tap("hello");
    click_btn("btn_secondary");
    CHECK(rig->info("hello")->status == PluginStatus::Disabled);
    CHECK_FALSE(rig->block.at("enabled").contains("hello"));
}

TEST_CASE_METHOD(ListFx, "the loaded row's Settings button opens the plugin's settings",
                 "[plugin][plugins_overlay]") {
    open();
    REQUIRE(rig->host->enable("hello"));
    tap("hello");
    click_btn("btn_primary");
    CHECK(rig->host->settings_screen("hello") != nullptr);
}

TEST_CASE_METHOD(ListFx, "dismissing the loaded row's dialog by backdrop does not disable",
                 "[plugin][plugins_overlay]") {
    open();
    REQUIRE(rig->host->enable("hello"));
    tap("hello");
    lv_obj_t* dialog = helix::ui::modal_get_top();
    REQUIRE(dialog != nullptr);
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
    drain();
    process_lvgl(50);
    CHECK(rig->info("hello")->status == PluginStatus::Loaded);
}

TEST_CASE_METHOD(ListFx, "a faulted row re-enables without asking again",
                 "[plugin][plugins_overlay]") {
    open(enabled("looper", all_perms())); // looper faults at load: time budget
    REQUIRE(rig->info("looper")->status == PluginStatus::Faulted);
    auto_yes_consent();
    const int writes_before = rig->writes;
    tap("looper");
    CHECK(consent_calls == 0); // consent was already given for these permissions
    CHECK(rig->writes > writes_before);
    CHECK(rig->info("looper")->status == PluginStatus::Faulted); // still faults: it loops
}

TEST_CASE_METHOD(ListFx, "the host dying under an open consent dialog leaves the buttons inert",
                 "[plugin][plugins_overlay]") {
    open();
    tap("hello"); // real show_consent: the dialog is up
    lv_obj_t* dialog = helix::ui::modal_get_top();
    REQUIRE(dialog != nullptr);
    rig->host.reset(); // printer switch destroys the host under the dialog
    drain();
    lv_obj_t* enable_btn = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(enable_btn != nullptr);
    lv_obj_send_event(enable_btn, LV_EVENT_CLICKED, nullptr);
    drain();
    process_lvgl(50);
    CHECK(PluginHost::live() == nullptr);
    CHECK_FALSE(rig->block.contains("enabled"));
}

TEST_CASE_METHOD(ListFx, "populate_rows after the screen closed is a no-op",
                 "[plugin][plugins_overlay]") {
    open();
    PluginsOverlay& ov = get_plugins_overlay();
    lv_obj_t* held = ov.root();
    ov.destroy_overlay_ui(held); // what destroy_on_close does when the overlay closes
    // A same-named container on the now-active screen must not be adopted as
    // the rows parent by a callback that outlived the screen.
    lv_obj_t* decoy = lv_obj_create(lv_screen_active());
    lv_obj_set_name(decoy, "plugins_rows");
    ov.populate_rows();
    CHECK(lv_obj_get_child_count(decoy) == 0);
    lv_obj_delete(decoy);
}

#endif // HELIX_HAS_PLUGINS
