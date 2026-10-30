// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "favorite_macro_widget.h"

#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_modal.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "favorite_macro_config.h"
#include "favorite_macro_config_modal.h"
#include "home_macro_run.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tile_color.h"

#include <spdlog/spdlog.h>

namespace helix {
void register_favorite_macro_widgets() {
    register_widget_factory("favorite_macro", [](const std::string& id) {
        return std::make_unique<FavoriteMacroWidget>(id);
    });
    // Register XML callbacks early — before any XML is parsed
    lv_xml_register_event_cb(nullptr, "favorite_macro_clicked_cb", FavoriteMacroWidget::clicked_cb);
    helix::register_favorite_macro_config_subjects();
    lv_xml_register_event_cb(nullptr, "fav_macro_config_close_cb",
                             FavoriteMacroConfigModal::close_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_confirm_cb",
                             FavoriteMacroConfigModal::require_confirm_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_defaults_cb",
                             FavoriteMacroConfigModal::defaults_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_tab_macro_cb",
                             FavoriteMacroConfigModal::tab_macro_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_tab_appearance_cb",
                             FavoriteMacroConfigModal::tab_appearance_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_tab_options_cb",
                             FavoriteMacroConfigModal::tab_options_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_color_cb",
                             FavoriteMacroConfigModal::color_swatch_cb);
}
} // namespace helix

using namespace helix;

// The name is a dotted single line, so a representative word measures its
// height; the glyph sits in a disc.
FavoriteMacroWidget::FavoriteMacroWidget(const std::string& widget_id)
    : TiledPanelWidget(widget_id,
                       TileSizing::Content{"", "", "Macro", false, "", /*label_always_drawn=*/false,
                                           TileSizing::IconBox::Disc}),
      widget_id_(widget_id) {}

FavoriteMacroWidget::~FavoriteMacroWidget() {
    detach();
}

void FavoriteMacroWidget::set_config(const nlohmann::json& config) {
    auto c = helix::favorite_macro_config_from_json(config);
    macro_name_ = c.macro;
    // Validate icon against codepoints — reject stale/invalid names
    if (c.icon.empty() || helix::ui::icon::lookup_codepoint(c.icon.c_str())) {
        icon_name_ = c.icon;
    } else {
        spdlog::warn("[FavoriteMacroWidget] Unknown icon '{}' in config, using default", c.icon);
    }
    icon_color_ = c.color;
    require_confirmation_ = c.require_confirmation;
    spdlog::debug("[FavoriteMacroWidget] Config: {}={} icon={} color=0x{:06X} require_confirm={}",
                  widget_id_, macro_name_, icon_name_.empty() ? "default" : icon_name_, icon_color_,
                  require_confirmation_);
}

void FavoriteMacroWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (widget_obj_) {
        // Pressed feedback: dim the widget on touch
        lv_obj_set_style_opa(widget_obj_, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // Cache label pointers from XML
    icon_badge_ = lv_obj_find_by_name(widget_obj_, "fav_macro_badge");
    icon_label_ = lv_obj_find_by_name(widget_obj_, "fav_macro_icon");
    name_label_ = lv_obj_find_by_name(widget_obj_, "fav_macro_name");

    if (!icon_label_ || !name_label_) {
        spdlog::warn("[FavoriteMacroWidget] XML child lookup failed: icon={} name={} badge={} — "
                     "widget will appear blank",
                     icon_label_ != nullptr, name_label_ != nullptr, icon_badge_ != nullptr);
    }

    update_display();

    spdlog::debug("[FavoriteMacroWidget] Attached {} (macro: {})", widget_id_,
                  macro_name_.empty() ? "none" : macro_name_);
}

void FavoriteMacroWidget::detach() {
    lifetime_.invalidate();
    config_modal_.reset();

    if (widget_obj_) {
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    icon_badge_ = nullptr;
    icon_label_ = nullptr;
    name_label_ = nullptr;

    spdlog::debug("[FavoriteMacroWidget] Detached");
}

bool FavoriteMacroWidget::on_edit_configure() {
    open_config_modal();
    return false;
}

void FavoriteMacroWidget::handle_clicked() {
    if (macro_name_.empty()) {
        // No macro assigned — open config modal to configure
        spdlog::info("[FavoriteMacroWidget] {} clicked (unconfigured) - opening config modal",
                     widget_id_);
        open_config_modal();
    } else {
        // Execute assigned macro
        spdlog::info("[FavoriteMacroWidget] {} clicked - executing {}", widget_id_, macro_name_);
        fetch_and_execute();
    }
}

void FavoriteMacroWidget::open_config_modal() {
    config_modal_ = std::make_unique<FavoriteMacroConfigModal>(widget_id_, panel_id(), [this]() {
        auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id());
        set_config(wc.get_widget_config(widget_id_));
        update_display();
    });
    config_modal_->show(parent_screen_);
}

IMoonrakerAPI* FavoriteMacroWidget::get_api() const {
    return get_moonraker_api();
}

void FavoriteMacroWidget::update_display() {
    bool unconfigured = macro_name_.empty();
    // A macro tile has no off state: configured is Active.
    const tile_color::Paint paint =
        tile_color::paint_for(icon_color_, unconfigured ? tile_color::TileState::Unconfigured
                                                        : tile_color::TileState::Active);

    if (name_label_) {
        if (unconfigured) {
            // A tag, not lv_tr() text: the label then re-translates itself.
            lv_label_set_translation_tag(name_label_, "Configure");
        } else {
            std::string display = helix::get_display_name(macro_name_, helix::DeviceType::MACRO);
            lv_label_set_text(name_label_, display.c_str());
        }
    }

    if (icon_label_) {
        // Unconfigured: script_text icon. Configured: custom icon or "play" default.
        const char* effective_icon = "script_text";
        if (!unconfigured)
            effective_icon = icon_name_.empty() ? "play" : icon_name_.c_str();
        helix::ui::icon::set_source(icon_label_, effective_icon);

        if (paint == tile_color::Paint::Custom) {
            helix::ui::icon::set_color(icon_label_, lv_color_hex(icon_color_), LV_OPA_COVER);
        } else {
            helix::ui::icon::set_variant(icon_label_, "secondary");
        }
    }

    if (icon_badge_) {
        lv_obj_set_style_bg_color(icon_badge_,
                                  paint == tile_color::Paint::Custom
                                      ? lv_color_hex(icon_color_)
                                      : theme_manager_get_color("secondary"),
                                  0);
    }
}

void FavoriteMacroWidget::save_config() {
    helix::FavoriteMacroConfig c{macro_name_, icon_name_, icon_color_, require_confirmation_};
    save_widget_config(helix::favorite_macro_config_to_json(c));
    spdlog::debug(
        "[FavoriteMacroWidget] Saved config: {}={} icon={} color=0x{:06X} require_confirm={}",
        widget_id_, macro_name_, icon_name_.empty() ? "default" : icon_name_, icon_color_,
        require_confirmation_);
}

void FavoriteMacroWidget::fetch_and_execute() {
    run_home_macro(get_api(),
                   {macro_name_, require_confirmation_, parent_screen_, "FavoriteMacroWidget"});
}

void FavoriteMacroWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] clicked_cb");
    auto* widget = panel_widget_from_event<FavoriteMacroWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}
