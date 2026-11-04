// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

#include "plugin_menu.h"

#include <string>
#include <string_view>

#include "hv/json.hpp"

namespace helix::ui {

/// A context menu holding only plugin items: the print details more button and a
/// plugin tile's gear. Items resolve (checks run) when it opens; when none is
/// visible it says so instead.
class PluginItemsMenu : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(PluginItemsMenu)

  public:
    /// Opens below `anchor`. `only_plugin` limits the items to one plugin's.
    bool show_for(lv_obj_t* parent, lv_obj_t* anchor, plugin_menu::MenuKind kind,
                  plugin_menu::json context, std::string_view only_plugin = {});

  protected:
    const char* xml_component_name() const override {
        return "plugin_items_menu";
    }
    void on_created(lv_obj_t* backdrop) override;

    /// As wide as the longest row, up to half a landscape screen or a whole portrait one.
    CardWidth card_width() const override {
        return {50, 160, 0, true, 100};
    }
    int32_t content_width() const override {
        return widest_row_;
    }

  private:
    plugin_menu::MenuKind kind_ = plugin_menu::MenuKind::PrintFile;
    plugin_menu::json context_;
    std::string only_plugin_;
    int32_t widest_row_ = 0;
};

} // namespace helix::ui
