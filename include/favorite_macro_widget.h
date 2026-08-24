// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "macro_param_cache.h"
#include "macro_param_modal.h"
#include "panel_widget.h"

#include <memory>
#include <string>

class IMoonrakerAPI;

namespace helix {

class FavoriteMacroConfigModal;

/// Home panel widget for one-tap macro execution.
/// Uses the multi_instance system: base ID "favorite_macro" with dynamic
/// instance IDs like "favorite_macro:1", "favorite_macro:3", etc.
/// Tap executes assigned macro; configure button opens config modal.
/// When unconfigured, tap also opens config modal.
class FavoriteMacroWidget : public PanelWidget {
  public:
    /// @param instance_id e.g. "favorite_macro:1"
    explicit FavoriteMacroWidget(const std::string& instance_id);
    ~FavoriteMacroWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
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
    lv_obj_t* icon_badge_ = nullptr;
    lv_obj_t* icon_label_ = nullptr;
    lv_obj_t* name_label_ = nullptr;

    std::string macro_name_;         ///< Assigned macro (e.g., "CLEAN_NOZZLE")
    std::string icon_name_;          ///< Custom icon name, empty = "play" default
    uint32_t icon_color_ = 0;        ///< Custom icon color (RGB), 0 = theme secondary
    bool skip_param_prompt_ = false; ///< Run directly without the parameter-entry modal
    helix::AsyncLifetimeGuard lifetime_;

    std::unique_ptr<FavoriteMacroConfigModal> config_modal_;

    IMoonrakerAPI* get_api() const;

    void update_display();
    void save_config();

    void fetch_and_execute();
    void open_config_modal();
};

} // namespace helix
