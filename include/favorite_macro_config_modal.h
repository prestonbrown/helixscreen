// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include <functional>
#include <string>

class IMoonrakerAPI;

namespace helix {

/// Edit-mode configuration modal for a FavoriteMacroWidget.
/// Tabs: Macro | Appearance | Options. Immediate-apply (no Save/Cancel).
class FavoriteMacroConfigModal : public Modal {
  public:
    using ChangeCallback = std::function<void()>;

    FavoriteMacroConfigModal(const std::string& widget_id, const std::string& panel_id,
                             ChangeCallback on_change = nullptr);
    ~FavoriteMacroConfigModal() override;

    const char* get_name() const override {
        return "Favorite Macro Config";
    }
    const char* component_name() const override {
        return "favorite_macro_config_modal";
    }

    // Static XML event callbacks (registered in register_favorite_macro_widgets()).
    static void tab_macro_cb(lv_event_t* e);
    static void tab_appearance_cb(lv_event_t* e);
    static void tab_options_cb(lv_event_t* e);
    static void close_cb(lv_event_t* e);
    static void skip_params_cb(lv_event_t* e);
    static void macro_row_cb(lv_event_t* e);
    static void icon_cell_cb(lv_event_t* e);
    static void color_swatch_cb(lv_event_t* e);

  protected:
    void on_show() override;

  private:
    void load_config();
    void persist();
    void populate_macro_list();
    void populate_icon_grid();
    void populate_color_grid();
    void refresh_highlights();
    void select_macro(const std::string& name);
    void select_icon(const std::string& name);
    void select_color(uint32_t color);
    void select_skip_param(bool enabled);
    IMoonrakerAPI* get_api() const;

    std::string widget_id_;
    std::string panel_id_;
    ChangeCallback on_change_;

    std::string macro_name_;
    std::string icon_name_;
    uint32_t icon_color_ = 0;
    bool skip_param_prompt_ = false;

    lv_obj_t* macro_list_ = nullptr;
    lv_obj_t* icon_grid_ = nullptr;
    lv_obj_t* color_grid_ = nullptr;

    static FavoriteMacroConfigModal* s_active_;
};

/// Registers the two C++-owned subjects (process-lifetime) used by the modal XML.
/// Called from register_favorite_macro_widgets(). Idempotent.
void register_favorite_macro_config_subjects();

} // namespace helix
