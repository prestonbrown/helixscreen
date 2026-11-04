// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_menu_rows.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_row_text.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <utility>

namespace helix::ui {

namespace {

/// What a tap on one plugin row needs, owned by the row.
struct RowTap {
    ContextMenu* menu;
    plugin_menu::Entry entry;
    plugin_menu::json context;
};

void on_row_deleted(lv_event_t* e) {
    delete static_cast<RowTap*>(lv_event_get_user_data(e));
}

LVGL_SAFE_EVENT_CB_WITH_EVENT(plugin_menu_row_cb, ev, {
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(ev));
    auto* tap = row ? static_cast<RowTap*>(lv_obj_get_user_data(row)) : nullptr;
    if (!tap || lv_obj_has_state(row, LV_STATE_DISABLED))
        return;
    // The payload dies with the row, which hide() deletes.
    plugin_menu::Entry entry = tap->entry;
    plugin_menu::json context = tap->context;
    if (ContextMenu::active() == tap->menu)
        tap->menu->hide();
    plugin_menu::activate(entry, context);
})

} // namespace

void register_plugin_menu_row_callbacks() {
    static bool registered = false;
    if (registered)
        return;
    register_xml_callbacks({{"plugin_menu_row_cb", plugin_menu_row_cb}});
    registered = true;
}

int append_plugin_rows(ContextMenu& menu, lv_obj_t* container,
                       std::vector<plugin_menu::Entry> entries, plugin_menu::json context,
                       bool divider, PluginRowLayout layout) {
    if (!container || entries.empty())
        return 0;
    register_plugin_menu_row_callbacks();
    if (divider) {
        const char* attrs[] = {"color", "#text_muted", "opacity", "38", nullptr};
        lv_xml_create(container, "divider_horizontal", attrs);
    }
    int added = 0;
    const std::string* group = nullptr;
    for (auto& e : entries) {
        const bool column = layout == PluginRowLayout::Column;
        if (!group || *group != e.plugin_id) {
            auto* heading = static_cast<lv_obj_t*>(lv_xml_create(
                container, column ? "context_menu_column_heading" : "plugin_menu_heading",
                nullptr));
            set_row_label_text(heading, column ? "heading_text" : "plugin_name",
                               e.plugin_name.c_str());
            group = &e.plugin_id;
        }
        const char* attrs[] = {"icon",      e.icon.empty() ? "puzzle_outline" : e.icon.c_str(),
                               "disabled",  e.enabled ? "false" : "true",
                               "no_reason", e.reason.empty() ? "true" : "false",
                               nullptr};
        auto* row = static_cast<lv_obj_t*>(
            lv_xml_create(container, column ? "plugin_menu_column_row" : "plugin_menu_row", attrs));
        if (!row)
            continue;
        if (column) {
            ui_button_set_text(lv_obj_find_by_name(row, "item_button"), e.label.c_str());
        } else {
            set_row_label_text(row, "item_label", e.label.c_str());
        }
        set_row_label_text(row, "item_reason", e.reason.c_str());
        auto tap = std::make_unique<RowTap>(RowTap{&menu, e, context});
        lv_obj_set_user_data(row, tap.get());
        lv_obj_add_event_cb(row, on_row_deleted, LV_EVENT_DELETE, tap.release());
        ++added;
    }
    spdlog::debug("[PluginMenuRows] {} plugin row(s) added", added);
    return added;
}

} // namespace helix::ui
