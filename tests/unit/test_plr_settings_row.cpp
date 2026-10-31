// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_settings_row.cpp
 * @brief The Printing settings page's power-loss recovery switch
 *
 * Run with: ./build/bin/helix-tests "[plr][settings]"
 *
 * The row is a plugin_feature_toggle_row: a working switch while the plugin's
 * `plr` feature can run, greyed with an Install or update prompt when the
 * plugin is missing or too old, and hidden on a printer the plugin cannot
 * recover. Flipping it writes the user's choice where the plugin reads it.
 */

#include "ui_modal.h"
#include "ui_panel_settings.h"
#include "ui_settings_printing.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "plr_backend.h"
#include "plr_plugin_rpc.h"
#include "plr_profile.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <cstring>
#include <optional>

#include "../catch_amalgamated.hpp"

namespace {

struct PlrSettingsRowFixture : LVGLUITestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, get_printer_state()};
    lv_obj_t* root_ = nullptr;

    PlrSettingsRowFixture() {
        SettingsManager::instance().init_subjects();
        get_global_settings_panel().init_subjects();
        helix::settings::get_printing_settings_overlay().register_callbacks();
        set_moonraker_api(&api);
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_printing_overlay", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    ~PlrSettingsRowFixture() override {
        set_moonraker_api(nullptr);
        auto& plugin = get_printer_state().plugin_status_state();
        plugin.set_features({});
        plugin.set_installed(false);
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        helix::ui::UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    static void set_plr_feature(bool available, bool enabled,
                                std::optional<bool> saving = std::nullopt) {
        auto& plugin = get_printer_state().plugin_status_state();
        helix::PluginFeatureEntry entry;
        entry.version = helix::kPlrFeatureMinVersion;
        entry.available = available;
        entry.enabled = enabled;
        entry.saving = saving;
        plugin.set_installed(true);
        plugin.set_features({{helix::kPlrFeatureName, entry}});
    }

    static bool visible(lv_obj_t* obj) {
        for (; obj != nullptr; obj = lv_obj_get_parent(obj)) {
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return false;
            }
        }
        return true;
    }

    lv_obj_t* find(const char* name) const {
        return lv_obj_find_by_name(root_, name);
    }

    /// Click a widget, pump LVGL and the update queue.
    void tap(lv_obj_t* widget) {
        lv_obj_send_event(widget, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The top modal dialog's text of the named widget, or "" when absent.
    std::string modal_text(const char* widget) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        if (dialog == nullptr) {
            return "(no dialog)";
        }
        lv_obj_t* label = lv_obj_find_by_name(dialog, widget);
        return label != nullptr ? lv_label_get_text(label) : "(missing)";
    }

    /// The top modal dialog's primary button, clicked.
    void confirm_modal() {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* button = lv_obj_find_by_name(dialog, "btn_primary");
        REQUIRE(button != nullptr);
        tap(button);
    }
};

} // namespace

TEST_CASE_METHOD(PlrSettingsRowFixture, "PLR settings row follows the plugin's plr feature",
                 "[plr][settings]") {
    lv_obj_t* row = find("row_power_loss_recovery");
    REQUIRE(row != nullptr);
    lv_obj_t* switch_row = lv_obj_find_by_name(row, "plugin_feature_row_plr");
    lv_obj_t* needs_plugin = lv_obj_find_by_name(row, "greyed_row");
    lv_obj_t* install = lv_obj_find_by_name(row, "install_button");
    lv_obj_t* needs_update = lv_obj_find_by_name(row, "needs_update_text");
    REQUIRE(switch_row != nullptr);
    REQUIRE(needs_plugin != nullptr);
    REQUIRE(install != nullptr);
    REQUIRE(needs_update != nullptr);

    SECTION("hidden until the plugin probe answers") {
        CHECK_FALSE(visible(row));
    }
    SECTION("no plugin: greyed, with Install") {
        get_printer_state().plugin_status_state().set_installed(false);
        process_lvgl(5);
        CHECK(visible(row));
        CHECK_FALSE(visible(switch_row));
        CHECK(visible(needs_plugin));
        CHECK(visible(install));
        CHECK_FALSE(visible(needs_update));
    }
    SECTION("a plugin that reports no plr feature: greyed, asking for an update") {
        auto& plugin = get_printer_state().plugin_status_state();
        plugin.set_installed(true);
        plugin.set_features({});
        process_lvgl(5);
        CHECK(visible(needs_plugin));
        CHECK(visible(needs_update));
        CHECK_FALSE(visible(install));
        CHECK_FALSE(visible(switch_row));
    }
    SECTION("a printer the plugin cannot recover: hidden") {
        set_plr_feature(false, false);
        process_lvgl(5);
        CHECK_FALSE(visible(row));
    }
    SECTION("the feature can run: a working switch") {
        set_plr_feature(true, false);
        process_lvgl(5);
        CHECK(visible(switch_row));
        CHECK_FALSE(visible(needs_plugin));
    }
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "PLR settings switch writes the user's choice",
                 "[plr][settings]") {
    set_plr_feature(true, false);
    process_lvgl(5);
    lv_obj_t* row = find("row_power_loss_recovery");
    REQUIRE(row != nullptr);
    lv_obj_t* toggle =
        lv_obj_find_by_name(lv_obj_find_by_name(row, "plugin_feature_row_plr"), "toggle");
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));

    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrUserKey) ==
          helix::plr_user_json(true));

    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrUserKey) ==
          helix::plr_user_json(false));
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "PLR settings switch follows saving, not enabled",
                 "[plr][settings]") {
    lv_obj_t* row = find("row_power_loss_recovery");
    REQUIRE(row != nullptr);
    lv_obj_t* toggle =
        lv_obj_find_by_name(lv_obj_find_by_name(row, "plugin_feature_row_plr"), "toggle");
    REQUIRE(toggle != nullptr);

    SECTION("saving off while a snapshot keeps the feature enabled") {
        set_plr_feature(true, true, false);
        process_lvgl(5);
        CHECK(visible(toggle));
        CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    }
    SECTION("saving on") {
        set_plr_feature(true, true, true);
        process_lvgl(5);
        CHECK(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    }
    SECTION("a plugin without saving: enabled stands in") {
        set_plr_feature(true, true);
        process_lvgl(5);
        CHECK(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    }
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "the mesh-include row follows the feature and the answer",
                 "[plr][settings]") {
    lv_obj_t* add = find("row_plr_mesh_include_add");
    lv_obj_t* remove = find("row_plr_mesh_include_remove");
    REQUIRE(add != nullptr);
    REQUIRE(remove != nullptr);
    auto& plugin = get_printer_state().plugin_status_state();

    SECTION("hidden until the plugin's plr feature answers") {
        CHECK_FALSE(visible(add));
        CHECK_FALSE(visible(remove));
    }
    SECTION("the feature present but no answer yet shows neither row") {
        set_plr_feature(true, false);
        plugin.set_plr_mesh_include(std::nullopt);
        process_lvgl(5);
        CHECK_FALSE(visible(add));
        CHECK_FALSE(visible(remove));
    }
    SECTION("the include missing: the add row, marked Not added") {
        set_plr_feature(true, false);
        plugin.set_plr_mesh_include(false);
        process_lvgl(5);
        CHECK(visible(add));
        CHECK_FALSE(visible(remove));
        CHECK(std::strcmp(lv_label_get_text(lv_obj_find_by_name(add, "value")), "Not added") == 0);
    }
    SECTION("the include there: the remove row, marked Added") {
        set_plr_feature(true, false);
        plugin.set_plr_mesh_include(true);
        process_lvgl(5);
        CHECK_FALSE(visible(add));
        CHECK(visible(remove));
        CHECK(std::strcmp(lv_label_get_text(lv_obj_find_by_name(remove, "value")), "Added") == 0);
    }
    SECTION("a stale answer landing after the feature went absent shows nothing") {
        // The reply guard checks at send time, so a reply can outlive the
        // feature: the container's feature gate is what hides the row then.
        set_plr_feature(true, false);
        set_plr_feature(false, false);
        plugin.set_plr_mesh_include(true);
        process_lvgl(5);
        CHECK_FALSE(visible(add));
        CHECK_FALSE(visible(remove));
    }
    SECTION("a printer the plugin cannot recover: hidden, and the answer resets") {
        set_plr_feature(true, false);
        plugin.set_plr_mesh_include(true);
        set_plr_feature(false, false);
        process_lvgl(5);
        CHECK_FALSE(visible(add));
        CHECK_FALSE(visible(remove));
        CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == -1);
    }
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "opening the page asks the plugin for the include",
                 "[plr][settings]") {
    auto& plugin = get_printer_state().plugin_status_state();

    SECTION("the feature present: the endpoint's answer lands in the row") {
        set_plr_feature(true, false);
        helix::settings::get_printing_settings_overlay().on_activate();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == 0);
        process_lvgl(5);
        CHECK(visible(find("row_plr_mesh_include_add")));
    }
    SECTION("the feature absent: nothing is asked") {
        helix::settings::get_printing_settings_overlay().on_activate();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == -1);
    }
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "the mesh-include row changes printer.cfg behind a confirm",
                 "[plr][settings]") {
    helix::ui::modal_init_subjects();
    auto& plugin = get_printer_state().plugin_status_state();

    SECTION("adding") {
        set_plr_feature(true, false);
        plugin.set_plr_mesh_include(false);
        process_lvgl(5);
        tap(find("row_plr_mesh_include_add"));
        CHECK(modal_text("dialog_title") == "Reload the saved bed mesh?");
        confirm_modal();

        CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == 1);
        process_lvgl(5);
        CHECK(visible(find("row_plr_mesh_include_remove")));
        CHECK(modal_text("dialog_title") == "Restart Firmware?");
        confirm_modal();
    }
    SECTION("removing") {
        set_plr_feature(true, false);
        plugin.set_plr_mesh_include(true);
        process_lvgl(5);
        tap(find("row_plr_mesh_include_remove"));
        CHECK(modal_text("dialog_title") == "Stop reloading the saved bed mesh?");
        confirm_modal();

        CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == 0);
        process_lvgl(5);
        CHECK(visible(find("row_plr_mesh_include_add")));
    }
    SECTION("declining the confirm changes nothing") {
        set_plr_feature(true, false);
        plugin.set_plr_mesh_include(false);
        process_lvgl(5);
        tap(find("row_plr_mesh_include_add"));
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* cancel = lv_obj_find_by_name(dialog, "btn_secondary");
        REQUIRE(cancel != nullptr);
        tap(cancel);

        CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == 0);
        process_lvgl(5);
        CHECK(visible(find("row_plr_mesh_include_add")));
    }
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "the plr feature turning present re-asks for the include",
                 "[plr][settings]") {
    auto& plugin = get_printer_state().plugin_status_state();

    // The overlay (and its feature observer) exists while the feature is
    // absent; the feature arriving afterwards still refreshes the row.
    set_plr_feature(true, false);
    process_lvgl(5);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == 0);
    process_lvgl(5);
    CHECK(visible(find("row_plr_mesh_include_add")));

    SECTION("a stale answer left by a late reply is replaced on the return") {
        set_plr_feature(false, false);     // absent: the answer resets
        plugin.set_plr_mesh_include(true); // a reply already in flight lands
        set_plr_feature(true, false);      // the feature returns: re-ask
        process_lvgl(5);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == 0);
    }
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "a no-op mesh-include change offers no restart",
                 "[plr][settings]") {
    helix::ui::modal_init_subjects();
    auto& plugin = get_printer_state().plugin_status_state();
    set_plr_feature(true, false);
    plugin.set_plr_mesh_include(false);
    process_lvgl(5);

    // The include appears between the ask and the confirm (a hand edit, or a
    // second screen): the action replies changed=false, and proposing a
    // firmware restart then would interrupt an active print for nothing.
    helix::plr_rpc::mesh_include_action(api, true, [](bool, bool) {}, [](const MoonrakerError&) {});
    helix::ui::UpdateQueue::instance().drain();
    plugin.set_plr_mesh_include(false);
    tap(find("row_plr_mesh_include_add"));
    confirm_modal();

    CHECK(lv_subject_get_int(plugin.get_plr_mesh_include_subject()) == 1);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    CHECK((dialog == nullptr ||
           std::strcmp(lv_label_get_text(lv_obj_find_by_name(dialog, "dialog_title")),
                       "Restart Firmware?") != 0));
}
