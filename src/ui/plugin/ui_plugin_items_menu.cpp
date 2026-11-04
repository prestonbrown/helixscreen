// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_items_menu.h"

#include "ui_plugin_menu_rows.h"

#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix::ui {

bool PluginItemsMenu::show_for(lv_obj_t* parent, lv_obj_t* anchor, plugin_menu::MenuKind kind,
                               plugin_menu::json context, std::string_view only_plugin) {
    kind_ = kind;
    context_ = std::move(context);
    only_plugin_ = std::string(only_plugin);
    return show_below_widget(parent, anchor);
}

void PluginItemsMenu::on_created(lv_obj_t* backdrop) {
    lv_obj_t* rows = find_required(backdrop, "plugin_rows", "PluginItemsMenu");
    if (!rows)
        return;
    auto entries = plugin_menu::resolve(kind_, context_, only_plugin_);
    // Each row's texts as set, for measuring: a dotted label's own text may carry the dots.
    struct RowTexts {
        std::string text;
        std::string reason;
    };
    std::vector<RowTexts> texts;
    std::string group;
    for (const auto& e : entries) {
        if (e.plugin_id != group) {
            texts.push_back({e.plugin_name, {}});
            group = e.plugin_id;
        }
        texts.push_back({e.label, e.reason});
    }

    if (append_plugin_rows(*this, rows, std::move(entries), context_, false) == 0) {
        const char* text = kind_ == plugin_menu::MenuKind::PrintFile
                               ? lv_tr("No plugin actions for this file")
                               : lv_tr("No actions right now");
        const char* attrs[] = {"text", text, nullptr};
        lv_xml_create(rows, "plugin_menu_empty", attrs);
        texts.assign(1, RowTexts{text, {}});
    }

    lv_obj_t* menu_card = card();
    if (!menu_card)
        return;
    lv_obj_update_layout(menu_card);
    widest_row_ = 0;
    const uint32_t n = lv_obj_get_child_count(rows);
    for (uint32_t i = 0; i < n && i < texts.size(); ++i) {
        lv_obj_t* row = lv_obj_get_child(rows, static_cast<int32_t>(i));
        lv_obj_t* label = lv_obj_find_by_name(row, "plugin_name");
        if (!label)
            label = lv_obj_find_by_name(row, "item_label");
        if (!label)
            label = lv_obj_find_by_name(row, "empty_text");
        if (label)
            widest_row_ =
                std::max(widest_row_, row_width_for_text(row, label, texts[i].text.c_str()));
        lv_obj_t* reason = lv_obj_find_by_name(row, "item_reason");
        if (reason && !texts[i].reason.empty())
            widest_row_ =
                std::max(widest_row_, row_width_for_text(row, reason, texts[i].reason.c_str()));
    }
    if (lv_obj_t* title = lv_obj_find_by_name(menu_card, "card_title")) {
        widest_row_ = std::max(widest_row_, row_width_for_text(lv_obj_get_parent(title), title,
                                                               lv_label_get_text(title)));
    }
}

} // namespace helix::ui
