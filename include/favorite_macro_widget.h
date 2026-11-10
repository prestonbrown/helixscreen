// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "macro_param_cache.h"
#include "macro_param_modal.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"

#include <memory>
#include <string>

namespace helix {
class IMoonrakerAPI;

class FavoriteMacroConfigModal;

/// Home panel widget for one-tap macro execution.
/// Uses the multi_instance system: base ID "favorite_macro" with dynamic
/// instance IDs like "favorite_macro:1", "favorite_macro:3", etc.
/// Tap executes assigned macro; configure button opens config modal.
/// When unconfigured, tap also opens config modal.
class FavoriteMacroWidget : public TiledPanelWidget {
  public:
    /// @param instance_id e.g. "favorite_macro:1"
    explicit FavoriteMacroWidget(const std::string& instance_id);
    ~FavoriteMacroWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;
    std::string get_component_name() const override {
        return "panel_widget_favorite_macro";
    }
    const char* id() const override {
        return widget_id_.c_str();
    }

    /// Event handlers routed from static callbacks
    void handle_clicked();

    // Static event callbacks (XML-registered)
    static void clicked_cb(lv_event_t* e);

  private:
    std::string widget_id_; ///< Instance ID, e.g. "favorite_macro:1"

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* icon_label_ = nullptr;

    std::string macro_name_;           ///< Assigned macro (e.g., "CLEAN_NOZZLE")
    std::string icon_name_;            ///< Custom icon name, empty = "play" default
    uint32_t icon_color_ = 0;          ///< Custom icon color (RGB), 0 = theme secondary
    bool require_confirmation_ = true; ///< Gate runs behind the param modal / confirm dialog
    helix::AsyncLifetimeGuard lifetime_;

    std::unique_ptr<FavoriteMacroConfigModal> config_modal_;

    // This instance's name and badge colour, bound by its component. Owned per
    // instance because two macro tiles can sit on the home grid at once.
    lv_subject_t name_subject_{};
    char name_buffer_[128] = {};
    /// 1 while a macro is assigned; 0 draws the "Configure" prompt.
    lv_subject_t named_subject_{};
    lv_subject_t badge_color_subject_{};
    SubjectManager subjects_;

    IMoonrakerAPI* get_api() const;

    void update_display();
    void save_config();

    void fetch_and_execute();
    void open_config_modal();
};

} // namespace helix
