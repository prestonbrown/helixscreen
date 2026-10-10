// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugins_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_utils.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "plugin_permissions.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <utility>

namespace helix::plugin {

void PluginsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"plugin_list_row_clicked",
         [](lv_event_t* e) {
             PluginsOverlay& ov = get_plugins_overlay();
             // The row root carries the binding; the tap may have landed on a child.
             for (lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e)); obj;
                  obj = lv_obj_get_parent(obj)) {
                 if (const std::string* id = ov.binding_at(lv_obj_get_user_data(obj))) {
                     ov.activate(*id);
                     return;
                 }
             }
         }},
    });
}

lv_obj_t* PluginsOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    populate_rows();
    return overlay_root_;
}

void PluginsOverlay::populate_rows() {
    // A consent or Disable callback can outlive the screen's widget tree; with
    // no root there is nothing to repopulate. Searching from NULL would adopt
    // the active screen instead.
    if (!overlay_root_)
        return;
    lv_obj_t* rows = helix::ui::find_required(overlay_root_, "plugins_rows", get_name());
    if (!rows)
        return;
    helix::ui::safe_clean_children(rows);
    const PluginHost* host = PluginHost::live();
    if (!host)
        return;
    for (const PluginInfo& info : host->plugins()) {
        const std::string& label = info.manifest && !info.manifest->name.empty()
                                       ? lv_tr(info.manifest->name.c_str())
                                       : info.dir_name;
        std::string desc = lv_tr(plugin_status_name(info.status));
        if (!info.reason.empty())
            desc += ": " + info.reason;
        const std::string row_name = "row_" + info.dir_name;
        const char* pairs[] = {"name",        row_name.c_str(), "label",
                               label.c_str(), "description",    desc.c_str(),
                               "icon",        "puzzle_outline", "description_min_bp",
                               "0",           "callback",       "plugin_list_row_clicked",
                               nullptr};
        lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(rows, "setting_action_row", pairs));
        if (!row) {
            spdlog::warn("[Plugins] cannot create a row for {}", info.dir_name);
            continue;
        }
        bindings_.push_back(info.dir_name);
        lv_obj_set_user_data(row, &bindings_.back());
    }
}

const std::string* PluginsOverlay::binding_at(const void* ud) {
    for (const std::string& id : bindings_)
        if (&id == ud)
            return &id;
    return nullptr;
}

void PluginsOverlay::set_consent_shower(ConsentShower shower) {
    consent_shower_ = std::move(shower);
}

void PluginsOverlay::show_consent_for(const Manifest& m, const std::vector<Permission>& grown,
                                      std::function<void()> on_yes) {
    if (consent_shower_)
        consent_shower_(m, grown, std::move(on_yes));
    else
        show_consent(m, grown, std::move(on_yes));
}

void PluginsOverlay::activate(const std::string& id) {
    PluginHost* host = PluginHost::live();
    if (!host)
        return;
    const PluginInfo* info = nullptr;
    for (const PluginInfo& p : host->plugins()) {
        if (p.dir_name == id) {
            info = &p;
            break;
        }
    }
    if (!info || !info->manifest)
        return;
    const Manifest& m = *info->manifest;

    switch (info->status) {
    case PluginStatus::Disabled:
    case PluginStatus::NeedsApproval: {
        // An update that grew its permission set re-prompts for the growth
        // only; a first enable asks for the whole set.
        std::vector<Permission> grown;
        if (info->status == PluginStatus::NeedsApproval)
            grown = permission_growth(host->granted(id), m.permissions);
        const auto tok = object_lifetime_.token();
        PluginHost* h = host;
        show_consent_for(m, grown, [this, tok, h, id] {
            if (tok.expired() || PluginHost::live() != h)
                return; // the overlay closed or the host was destroyed meanwhile
            h->enable(id);
            populate_rows();
        });
        break;
    }
    case PluginStatus::Loaded: {
        const auto tok = object_lifetime_.token();
        PluginHost* h = host;
        const std::string name = m.name.empty() ? id : lv_tr(m.name.c_str());
        // Only the Disable button disables: dismissing the dialog (backdrop,
        // ESC) leaves the plugin running, so no on_dismiss action exists.
        helix::ui::ConfirmOptions opts;
        opts.cancel_text = lv_tr("Disable");
        opts.on_cancel = [this, tok, h, id] {
            if (tok.expired() || PluginHost::live() != h)
                return;
            h->disable(id);
            populate_rows();
        };
        helix::ui::modal_confirm(
            name.c_str(), lv_tr("Enabled. Open its settings, or disable it?"), ModalSeverity::Info,
            lv_tr("Settings"),
            [tok, h, id] {
                if (tok.expired() || PluginHost::live() != h)
                    return;
                h->open_settings(id);
            },
            opts);
        break;
    }
    case PluginStatus::Faulted:
    case PluginStatus::OverBudget:
        // Re-enable re-runs the load under the current memory budget; consent
        // already covers the granted permission set.
        host->enable(id);
        populate_rows();
        break;
    case PluginStatus::Invalid:
    case PluginStatus::Incompatible:
        break; // the manifest itself is the problem; a dialog cannot fix it
    }
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
