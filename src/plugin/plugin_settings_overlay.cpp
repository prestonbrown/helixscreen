// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_settings_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_utils.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "lua_bindings.h"
#include "plugin_host.h"
#include "plugin_xml_policy.h"
#include "static_panel_registry.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <lvgl.h>
#include <string>
#include <vector>

namespace helix::plugin {

namespace {

/// Attributes the screen itself wires, whose values never come from a plugin:
/// policy would reject them by design (a callback must read "plugin_event").
bool screen_constant_attr(const std::string& name) {
    return name == "callback" || name == "trigger";
}

/// The attr text for an integer value: no trailing zeros by construction.
std::string int_text(long long v) {
    return std::to_string(v);
}

/// Int-slider range/value: whole numbers, straight through.
long long int_of(const json& j) {
    return j.is_number_integer() ? j.get<long long>() : 0;
}

/// Float-slider range/value: scaled by 100 onto the slider's integer range.
long long scaled(double v) {
    return static_cast<long long>(std::lround(v * 100));
}

/// The value label's text for a slider row: an Int setting as-is, a Float setting
/// as its real value (the slider carries it x100), no trailing zeros.
std::string row_value_text(const SettingDecl& d, int32_t units) {
    if (d.type == SettingType::Float) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", units / 100.0);
        std::string s = buf;
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
        return s;
    }
    return std::to_string(units);
}

/// Puts the row's value label in step with its slider, both directions: from the
/// stored value at build/restore, and live during a drag, when the row's
/// callback has not fired yet (it triggers on release).
void sync_value_label(const PluginSettingsOverlay::RowBinding& b) {
    if (b.decl.type != SettingType::Int && b.decl.type != SettingType::Float)
        return;
    // required-names: setting_slider_row
    lv_obj_t* slider = helix::ui::find_required(b.row, "slider", "PluginSettings");
    // required-names: setting_slider_row
    lv_obj_t* label = helix::ui::find_required(b.row, "value_label", "PluginSettings");
    if (slider && label)
        lv_label_set_text(label, row_value_text(b.decl, lv_slider_get_value(slider)).c_str());
}

void slider_value_changed_cb(lv_event_t* e) {
    // Like the row callbacks, trust the binding only while an open screen owns it.
    void* ud = lv_event_get_user_data(e);
    PluginHost* host = PluginHost::live();
    if (host && host->owns_row_binding(ud))
        sync_value_label(*static_cast<PluginSettingsOverlay::RowBinding*>(ud));
}

} // namespace

SettingRowSpec setting_row_spec(const std::string& /*plugin_id*/, const SettingDecl& d,
                                const json& current) {
    SettingRowSpec spec;
    // The declared label is English-source; a plugin translation pack resolves
    // it, else the label passes through unchanged.
    const std::string label = lv_tr(d.label.c_str());
    switch (d.type) {
    case SettingType::Bool:
        spec.component = "setting_toggle_row";
        spec.attrs = {{"label", label}, {"callback", "plugin_setting_changed"}};
        break;
    case SettingType::Int:
        spec.component = "setting_slider_row";
        spec.attrs = {{"label", label},
                      {"min", int_text(static_cast<long long>(d.min))},
                      {"max", int_text(static_cast<long long>(d.max))},
                      {"value", int_text(int_of(current))},
                      {"callback", "plugin_setting_changed"},
                      {"trigger", "released"}};
        break;
    case SettingType::Float:
        spec.component = "setting_slider_row";
        spec.attrs = {{"label", label},
                      {"min", int_text(scaled(d.min))},
                      {"max", int_text(scaled(d.max))},
                      {"value", int_text(scaled(current.is_number() ? current.get<double>() : 0))},
                      {"callback", "plugin_setting_changed"},
                      {"trigger", "released"}};
        break;
    case SettingType::Enum: {
        spec.component = "setting_dropdown_row";
        std::string options;
        for (const auto& o : d.options) {
            if (!options.empty())
                options += '\n';
            options += o;
        }
        spec.attrs = {
            {"label", label}, {"options", options}, {"callback", "plugin_setting_changed"}};
        break;
    }
    case SettingType::String:
        spec.component = "setting_text_row";
        spec.attrs = {{"label", label},
                      {"value", current.is_string() ? current.get_ref<const std::string&>() : ""},
                      {"callback", "plugin_setting_changed"}};
        break;
    case SettingType::Action:
        spec.component = "setting_action_row";
        spec.attrs = {{"label", label}, {"callback", "plugin_setting_action"}};
        break;
    case SettingType::Info:
        spec.component = "setting_info_row";
        spec.attrs = {{"label", label}, {"bind_value", d.subject}};
        break;
    }
    return spec;
}

PluginSettingsOverlay::PluginSettingsOverlay(std::string plugin_id, const Manifest& manifest,
                                             const json& settings, uint64_t load_gen)
    : plugin_id_(std::move(plugin_id)), title_(lv_tr(manifest.name.c_str())),
      settings_decls_(manifest.settings), load_gen_(load_gen), settings_(settings) {}

PluginSettingsOverlay::~PluginSettingsOverlay() {
    // Still open when the host is destroyed (a printer switch drops the queued
    // close): navigation will never tear this screen down, and this class created
    // the root, so it deletes it here.
    if (!overlay_root_ || StaticPanelRegistry::is_destroying_all())
        return; // inside destroy_all the registry's caller owns the widget
    helix::nav::clear_on_close(overlay_root_);
    helix::nav::unregister_overlay(overlay_root_);
    helix::ui::safe_delete_deferred(overlay_root_);
}

lv_obj_t* PluginSettingsOverlay::create(lv_obj_t* parent) {
    const std::string title = title_.empty() ? plugin_id_ : title_;
    const char* pairs[] = {"title", title.c_str(), nullptr};
    overlay_root_ = helix::ui::create_xml_hidden(parent, "plugin_settings_overlay", pairs);
    if (!overlay_root_) {
        spdlog::error("[PluginSettings] {}: cannot create plugin_settings_overlay", plugin_id_);
        return nullptr;
    }
    parent_screen_ = parent;

    lv_obj_t* rows = helix::ui::find_required(overlay_root_, "settings_rows", get_name());
    if (!rows) {
        spdlog::error("[PluginSettings] {}: plugin_settings_overlay has no settings_rows",
                      plugin_id_);
        destroy_overlay_ui(overlay_root_);
        return nullptr;
    }
    for (const SettingDecl& d : settings_decls_)
        build_row(rows, d, effective_setting(settings_, d)); // a refused row is skipped, not fatal

    return overlay_root_;
}

lv_obj_t* PluginSettingsOverlay::row_for(const std::string& key) const {
    for (const auto& b : bindings_)
        if (b.key == key && b.row)
            return b.row;
    return nullptr;
}

PluginSettingsOverlay::RowBinding* PluginSettingsOverlay::binding_at(const void* ud) {
    for (auto& b : bindings_)
        if (&b == ud)
            return &b;
    return nullptr;
}

bool PluginSettingsOverlay::build_row(lv_obj_t* rows, const SettingDecl& d, const json& current) {
    SettingRowSpec spec = setting_row_spec(plugin_id_, d, current);
    for (const auto& [name, value] : spec.attrs) {
        if (screen_constant_attr(name))
            continue;
        if (auto why = check_plugin_attr(plugin_id_, name, value)) {
            spdlog::warn("[PluginSettings] {}: setting '{}': {}", plugin_id_, d.key, *why);
            return false;
        }
    }
    std::vector<const char*> pairs;
    pairs.reserve(spec.attrs.size() * 2 + 1);
    for (const auto& [name, value] : spec.attrs) {
        pairs.push_back(name.c_str());
        pairs.push_back(value.c_str());
    }
    pairs.push_back(nullptr);
    lv_obj_t* row =
        static_cast<lv_obj_t*>(lv_xml_create(rows, spec.component.c_str(), pairs.data()));
    if (!row) {
        spdlog::warn("[PluginSettings] {}: cannot create '{}' for setting '{}'", plugin_id_,
                     spec.component, d.key);
        return false;
    }
    RowBinding& b = bindings_.emplace_back(RowBinding{d.key, d, current, row});
    // The binding must be reachable through user data before any widget state is
    // applied: setting state fires the row's own value_changed, which walks here.
    lv_obj_set_user_data(row, &b);
    if (d.type == SettingType::Int || d.type == SettingType::Float)
        // required-names: setting_slider_row
        if (lv_obj_t* slider = helix::ui::find_required(row, "slider", get_name()))
            lv_obj_add_event_cb(slider, slider_value_changed_cb, LV_EVENT_VALUE_CHANGED, &b);
    apply_row_state(b);
    return true;
}

void PluginSettingsOverlay::apply_row_state(const RowBinding& b) {
    switch (b.decl.type) {
    case SettingType::Bool:
        // required-names: setting_toggle_row
        if (lv_obj_t* toggle = helix::ui::find_required(b.row, "toggle", get_name())) {
            if (b.stored.is_boolean() && b.stored.get<bool>())
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            else
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        break;
    case SettingType::Int:
    case SettingType::Float:
        // required-names: setting_slider_row
        if (lv_obj_t* slider = helix::ui::find_required(b.row, "slider", get_name())) {
            int32_t v = 0;
            if (b.decl.type == SettingType::Int)
                v = static_cast<int32_t>(int_of(b.stored));
            else if (b.stored.is_number())
                v = static_cast<int32_t>(scaled(b.stored.get<double>()));
            lv_slider_set_value(slider, v, LV_ANIM_OFF);
            sync_value_label(b);
        }
        break;
    case SettingType::Enum:
        // required-names: setting_dropdown_row
        if (lv_obj_t* dropdown = helix::ui::find_required(b.row, "dropdown", get_name())) {
            if (b.stored.is_string()) {
                const std::string& s = b.stored.get_ref<const std::string&>();
                for (size_t i = 0; i < b.decl.options.size(); ++i) {
                    if (b.decl.options[i] == s) {
                        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(i));
                        break;
                    }
                }
            }
        }
        break;
    case SettingType::String:
        // required-names: setting_text_row
        if (lv_obj_t* input = helix::ui::find_required(b.row, "value_input", get_name()))
            lv_textarea_set_text(
                input, b.stored.is_string() ? b.stored.get_ref<const std::string&>().c_str() : "");
        break;
    case SettingType::Action:
    case SettingType::Info:
        break;
    }
}

void PluginSettingsOverlay::on_row_changed(RowBinding& b, lv_obj_t* target) {
    json value;
    switch (b.decl.type) {
    case SettingType::Bool:
        value = lv_obj_has_state(target, LV_STATE_CHECKED);
        break;
    case SettingType::Int:
        value = lv_slider_get_value(target);
        break;
    case SettingType::Float:
        value = lv_slider_get_value(target) / 100.0;
        break;
    case SettingType::Enum: {
        uint32_t sel = lv_dropdown_get_selected(target);
        if (sel >= b.decl.options.size())
            return;
        value = b.decl.options[sel];
        break;
    }
    case SettingType::String:
        value = std::string(lv_textarea_get_text(target));
        break;
    case SettingType::Action:
    case SettingType::Info:
        return;
    }
    // The text row reports ready and then defocused for one confirm; a restored
    // row echoes its own value_changed. Both land here and are not writes.
    if (value == b.stored)
        return;
    PluginHost* host = PluginHost::live();
    if (host && host->set_setting(plugin_id_, b.key, value)) {
        b.stored = std::move(value);
        return;
    }
    spdlog::warn("[PluginSettings] {}: refused write to '{}'; restoring the row", plugin_id_,
                 b.key);
    apply_row_state(b);
}

void PluginSettingsOverlay::on_row_action(const RowBinding& b) {
    if (b.decl.type != SettingType::Action || b.decl.callback.empty())
        return;
    if (PluginHost* host = PluginHost::live())
        host->dispatch_event(b.decl.callback); // the same path plugin_event takes
}

void PluginSettingsOverlay::on_nav_closed() {
    destroy_overlay_ui(overlay_root_);
}

void register_plugin_settings_callbacks() {
    register_xml_callbacks({
        {"plugin_setting_changed",
         [](lv_event_t* e) {
             if (PluginHost* host = PluginHost::live())
                 host->handle_setting_row_event(e, false);
         }},
        {"plugin_setting_action",
         [](lv_event_t* e) {
             if (PluginHost* host = PluginHost::live())
                 host->handle_setting_row_event(e, true);
         }},
    });
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
