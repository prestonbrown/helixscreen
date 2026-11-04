// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_feature_row.h"

#include "ui_event_safety.h"
#include "ui_panel_advanced.h"

#include "app_globals.h"
#include "core/static_subject_registry.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix_plugin_features.h"
#include "i_moonraker_api.h"
#include "plr_plugin_rpc.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::ui {

namespace {

constexpr const char* kRowNamePrefix = "plugin_feature_row_";

/// Constant 1: the greyed copy of a row names it as its `disabled` subject.
lv_subject_t s_greyed_subject{};
bool s_greyed_initialized = false;

void on_switch_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginFeatureRow] on_switch_changed");
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const std::string feature = plugin_feature_of(sw);
    if (feature.empty()) {
        spdlog::warn("[PluginFeatureRow] Switch outside a plugin feature row");
        return;
    }
    write_plugin_feature_opt_in(feature, lv_obj_has_state(sw, LV_STATE_CHECKED));
    LVGL_SAFE_EVENT_CB_END();
}

void on_install_clicked(lv_event_t*) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginFeatureRow] on_install_clicked");
    get_global_advanced_panel().handle_helix_plugin_install_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

void register_plugin_feature_row_callbacks() {
    if (!s_greyed_initialized) {
        lv_subject_init_int(&s_greyed_subject, 1);
        s_greyed_initialized = true;
        StaticSubjectRegistry::instance().register_deinit(
            "PluginFeatureRow",
            []() {
                if (s_greyed_initialized) {
                    lv_subject_deinit(&s_greyed_subject);
                    s_greyed_initialized = false;
                }
            },
            StaticSubjectRegistry::Lifetime::Process);
    }
    lv_xml_register_subject(nullptr, "plugin_feature_greyed", &s_greyed_subject);
    lv_xml_register_event_cb(nullptr, "on_plugin_feature_switch_changed", on_switch_changed);
    lv_xml_register_event_cb(nullptr, "on_plugin_feature_install_clicked", on_install_clicked);
}

std::string plugin_feature_of(lv_obj_t* obj) {
    const size_t prefix_len = std::strlen(kRowNamePrefix);
    for (; obj != nullptr; obj = lv_obj_get_parent(obj)) {
        const char* name = lv_obj_get_name(obj);
        if (name != nullptr && std::strncmp(name, kRowNamePrefix, prefix_len) == 0) {
            return name + prefix_len;
        }
    }
    return {};
}

void write_plugin_feature_opt_in(const std::string& name, bool enabled) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    spdlog::info("[PluginFeatureRow] {} -> {}", name, enabled ? "on" : "off");
    // A dotted key sets one field of the record and leaves the others.
    api->database_post_item(
        kPluginFeaturesNamespace, std::string(kPluginFeaturesKey) + "." + name, enabled,
        [api]() { plr_rpc::refresh_features(*api); },
        [api, name](const MoonrakerError& err) {
            spdlog::warn("[PluginFeatureRow] Writing the opt-in for {} failed: {}", name,
                         err.message);
            plr_rpc::refresh_features(*api);
        });
}

} // namespace helix::ui
