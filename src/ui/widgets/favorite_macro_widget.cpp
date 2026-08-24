// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "favorite_macro_widget.h"

#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_modal.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "favorite_macro_config.h"
#include "favorite_macro_config_modal.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "safety_settings_manager.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
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
    lv_xml_register_event_cb(nullptr, "fav_macro_config_skip_cb",
                             FavoriteMacroConfigModal::skip_params_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_tab_macro_cb",
                             FavoriteMacroConfigModal::tab_macro_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_tab_appearance_cb",
                             FavoriteMacroConfigModal::tab_appearance_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_config_tab_options_cb",
                             FavoriteMacroConfigModal::tab_options_cb);
}
} // namespace helix

namespace {

// File-local helper: single shared MacroParamModal instance.
// Using one instance avoids s_active_instance_ stomping when two widget slots
// both try to open param modals (the old code had two separate static locals).
helix::MacroParamModal& get_shared_param_modal() {
    static helix::MacroParamModal modal;
    return modal;
}

// Heap context for confirmation-modal callbacks. Holds only widget-independent
// state (api ptr + macro name + parent screen) so the originating widget may be
// destroyed mid-modal without UAF — none of the callback paths touch `this`.
struct MacroExecCtx {
    std::string macro_name;
    IMoonrakerAPI* api;
    lv_obj_t* parent_screen;
};

// After-confirmation dispatch. Mirrors the original switch in fetch_and_execute()
// but is a free function so it can be called from a static callback without
// needing the widget instance to still exist.
void run_macro_after_confirm(MacroExecCtx ctx) {
    auto cached = helix::MacroParamCache::instance().get(ctx.macro_name);
    switch (cached.knowledge) {
    case helix::MacroParamKnowledge::KNOWN_NO_PARAMS:
        helix::execute_macro_gcode(ctx.api, ctx.macro_name, {}, "[FavoriteMacroWidget]");
        break;
    case helix::MacroParamKnowledge::KNOWN_PARAMS:
        if (ctx.parent_screen) {
            std::string name = ctx.macro_name;
            IMoonrakerAPI* api = ctx.api;
            get_shared_param_modal().show_for_macro(
                ctx.parent_screen, ctx.macro_name, cached.params,
                [api, name](const helix::MacroParamResult& result) {
                    helix::execute_macro_gcode(api, name, result, "[FavoriteMacroWidget]");
                });
        }
        break;
    case helix::MacroParamKnowledge::UNKNOWN:
        if (ctx.parent_screen) {
            std::string name = ctx.macro_name;
            IMoonrakerAPI* api = ctx.api;
            get_shared_param_modal().show_for_unknown_params(
                ctx.parent_screen, ctx.macro_name,
                [api, name](const helix::MacroParamResult& result) {
                    helix::execute_macro_gcode(api, name, result, "[FavoriteMacroWidget]");
                });
        }
        break;
    }
}

// Ownership: the heap MacroExecCtx is bound to the dialog object's LV_EVENT_DELETE
// (see ctx_delete_cb), which fires on every dismissal path — button click,
// backdrop tap, and ESC-key all funnel through Modal::hide → animated exit →
// object delete. Button callbacks therefore must NOT delete the ctx; doing so
// would double-free when the DELETE event fires later.
void ctx_delete_cb(lv_event_t* e) {
    delete static_cast<MacroExecCtx*>(lv_event_get_user_data(e));
}

void dangerous_confirm_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] dangerous_confirm_cb");
    auto* ctx = static_cast<MacroExecCtx*>(lv_event_get_user_data(e));
    Modal::hide(Modal::get_top());
    run_macro_after_confirm(*ctx);
    LVGL_SAFE_EVENT_CB_END();
}

void dangerous_cancel_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] dangerous_cancel_cb");
    auto* ctx = static_cast<MacroExecCtx*>(lv_event_get_user_data(e));
    Modal::hide(Modal::get_top());
    spdlog::debug("[FavoriteMacroWidget] Dangerous macro cancelled: {}", ctx->macro_name);
    LVGL_SAFE_EVENT_CB_END();
}

void run_confirm_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] run_confirm_cb");
    auto* ctx = static_cast<MacroExecCtx*>(lv_event_get_user_data(e));
    Modal::hide(Modal::get_top());
    helix::execute_macro_gcode(ctx->api, ctx->macro_name, {}, "[FavoriteMacroWidget]");
    LVGL_SAFE_EVENT_CB_END();
}

void run_cancel_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] run_cancel_cb");
    (void)e;
    Modal::hide(Modal::get_top());
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

using namespace helix;

FavoriteMacroWidget::FavoriteMacroWidget(const std::string& widget_id) : widget_id_(widget_id) {}

FavoriteMacroWidget::~FavoriteMacroWidget() {
    detach();
}

void FavoriteMacroWidget::set_config(const nlohmann::json& config) {
    auto c = helix::favorite_macro_config_from_json(config);
    macro_name_ = c.macro;
    // Validate icon against codepoints — reject stale/invalid names
    if (c.icon.empty() || ui_icon::lookup_codepoint(c.icon.c_str())) {
        icon_name_ = c.icon;
    } else {
        spdlog::warn("[FavoriteMacroWidget] Unknown icon '{}' in config, using default", c.icon);
    }
    icon_color_ = c.color;
    skip_param_prompt_ = c.skip_param_prompt;
    spdlog::debug("[FavoriteMacroWidget] Config: {}={} icon={} color=0x{:06X} skip_params={}",
                  widget_id_, macro_name_, icon_name_.empty() ? "default" : icon_name_, icon_color_,
                  skip_param_prompt_);
}

void FavoriteMacroWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);

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
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    icon_badge_ = nullptr;
    icon_label_ = nullptr;
    name_label_ = nullptr;

    spdlog::debug("[FavoriteMacroWidget] Detached");
}

void FavoriteMacroWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                          int height_px) {
    if (!widget_obj_)
        return;

    bool tall = (height_px >= widget_size::H_TALL);
    bool wide = (width_px >= widget_size::W_NORMAL);

    // Scale badge and icon: 48px/md at 1×1, 64px/lg when tall or 2×2
    int badge_size = tall ? 64 : 48;
    if (icon_badge_) {
        lv_obj_set_size(icon_badge_, badge_size, badge_size);
        lv_obj_set_style_radius(icon_badge_, badge_size / 2, 0);
    }
    if (icon_label_) {
        const lv_font_t* icon_font = tall ? &mdi_icons_48 : &mdi_icons_32;
        lv_obj_set_style_text_font(icon_label_, icon_font, 0);
    }

    // Scale text: font_xs at 1×1, font_small when tall or wide
    if (name_label_) {
        const char* font_token = (tall || wide) ? "font_small" : "font_xs";
        const lv_font_t* text_font = theme_manager_get_font(font_token);
        if (text_font)
            lv_obj_set_style_text_font(name_label_, text_font, 0);
    }
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

    if (name_label_) {
        if (unconfigured) {
            lv_label_set_text(name_label_, lv_tr("Configure"));
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
        ui_icon_set_source(icon_label_, effective_icon);

        // Apply custom color when configured, muted when unconfigured
        if (icon_color_ != 0 && !unconfigured) {
            ui_icon_set_color(icon_label_, lv_color_hex(icon_color_), LV_OPA_COVER);
        } else if (unconfigured) {
            ui_icon_set_variant(icon_label_, "secondary");
        } else {
            ui_icon_set_variant(icon_label_, "secondary");
        }
    }

    // Badge background: muted when unconfigured, tinted when configured
    if (icon_badge_) {
        if (unconfigured) {
            lv_obj_set_style_bg_color(icon_badge_, theme_manager_get_color("secondary"), 0);
        } else if (icon_color_ != 0) {
            lv_obj_set_style_bg_color(icon_badge_, lv_color_hex(icon_color_), 0);
        } else {
            lv_obj_set_style_bg_color(icon_badge_, theme_manager_get_color("secondary"), 0);
        }
    }
}

void FavoriteMacroWidget::save_config() {
    helix::FavoriteMacroConfig c{macro_name_, icon_name_, icon_color_, skip_param_prompt_};
    save_widget_config(helix::favorite_macro_config_to_json(c));
    spdlog::debug("[FavoriteMacroWidget] Saved config: {}={} icon={} color=0x{:06X} skip_params={}",
                  widget_id_, macro_name_, icon_name_.empty() ? "default" : icon_name_, icon_color_,
                  skip_param_prompt_);
}

void FavoriteMacroWidget::fetch_and_execute() {
    IMoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[FavoriteMacroWidget] No API available");
        return;
    }

    // Dangerous macros always require a confirmation modal — the home-screen tile
    // is one accidental tap away from EMERGENCY_STOP / FIRMWARE_RESTART, and the
    // MacrosPanel already enforces this; the home-screen widget previously
    // bypassed it entirely (#925).
    if (helix::is_dangerous_macro(macro_name_)) {
        if (!parent_screen_) {
            spdlog::warn("[FavoriteMacroWidget] No parent screen for dangerous-macro confirm");
            return;
        }
        spdlog::warn("[FavoriteMacroWidget] Dangerous macro requested: {}", macro_name_);
        auto* ctx = new MacroExecCtx{macro_name_, api, parent_screen_};
        std::string display = helix::get_display_name(macro_name_, helix::DeviceType::MACRO);
        std::string msg =
            fmt::format(lv_tr("{} may cause unintended changes. Are you sure?"), display);
        lv_obj_t* dialog = helix::ui::modal_show_confirmation(
            lv_tr("Run Dangerous Macro?"), msg.c_str(), ::ModalSeverity::Warning, lv_tr("Run"),
            dangerous_confirm_cb, dangerous_cancel_cb, ctx);
        if (dialog) {
            lv_obj_add_event_cb(dialog, ctx_delete_cb, LV_EVENT_DELETE, ctx);
        } else {
            delete ctx; // dialog construction failed — clean up to avoid leak
        }
        return;
    }

    auto cached = MacroParamCache::instance().get(macro_name_);

    // "Run without parameter prompt" favorites — and macros with genuinely no
    // params — execute with empty params and never show the parameter-entry
    // modal. Macros registered via Klipper's register_command report UNKNOWN
    // params (no gcode_macro template to parse), which would otherwise force the
    // popup on every tap regardless of the confirm setting; the per-widget
    // toggle lets the user opt out of it.
    bool run_without_params =
        skip_param_prompt_ || cached.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS;

    // Optional run-confirmation gate (Settings → Safety toggle, default on).
    // Only applies when running without a param modal — for KNOWN_PARAMS /
    // UNKNOWN the param modal is itself the implicit confirmation step. Mirrors
    // MacrosPanel logic.
    if (run_without_params && parent_screen_ &&
        helix::SafetySettingsManager::instance().get_macro_require_confirmation()) {
        auto* ctx = new MacroExecCtx{macro_name_, api, parent_screen_};
        std::string display = helix::get_display_name(macro_name_, helix::DeviceType::MACRO);
        std::string msg = fmt::format(lv_tr("Run {}?"), display);
        lv_obj_t* dialog = helix::ui::modal_show_confirmation(lv_tr("Run Macro?"), msg.c_str(),
                                                              ::ModalSeverity::Info, lv_tr("Run"),
                                                              run_confirm_cb, run_cancel_cb, ctx);
        if (dialog) {
            lv_obj_add_event_cb(dialog, ctx_delete_cb, LV_EVENT_DELETE, ctx);
        } else {
            delete ctx;
        }
        return;
    }

    if (run_without_params) {
        helix::execute_macro_gcode(api, macro_name_, {}, "[FavoriteMacroWidget]");
        return;
    }

    // Has params (and not skipped) — show the parameter-entry modal.
    run_macro_after_confirm({macro_name_, api, parent_screen_});
}

void FavoriteMacroWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] clicked_cb");
    auto* widget = panel_widget_from_event<FavoriteMacroWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}
