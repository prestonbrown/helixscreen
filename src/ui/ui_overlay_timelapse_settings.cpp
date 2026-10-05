// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_timelapse_settings.h"

#include "ui_callback_helpers.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "runtime_config.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "timelapse_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

// Global instance (constructed with the API, so not a lazy_global)
static std::unique_ptr<TimelapseSettingsOverlay> g_timelapse_settings;

TimelapseSettingsOverlay& get_global_timelapse_settings() {
    if (!g_timelapse_settings) {
        spdlog::error(
            "[Timelapse Settings] get_global_timelapse_settings() called before initialization!");
        throw std::runtime_error("TimelapseSettingsOverlay not initialized");
    }
    return *g_timelapse_settings;
}

void init_global_timelapse_settings(IMoonrakerAPI* api) {
    if (g_timelapse_settings) {
        spdlog::warn("[Timelapse Settings] TimelapseSettingsOverlay already initialized, skipping");
        return;
    }
    g_timelapse_settings = std::make_unique<TimelapseSettingsOverlay>(api);
    StaticPanelRegistry::instance().register_destroy("TimelapseSettingsOverlay",
                                                     []() { g_timelapse_settings.reset(); });
    spdlog::trace("[Timelapse Settings] TimelapseSettingsOverlay initialized");
}

// Framerate mapping
constexpr int TimelapseSettingsOverlay::FRAMERATE_VALUES[];

int TimelapseSettingsOverlay::framerate_to_index(int framerate) {
    for (int i = 0; i < FRAMERATE_COUNT; i++) {
        if (FRAMERATE_VALUES[i] == framerate) {
            return i;
        }
    }
    return 2; // Default to 30fps (index 2)
}

int TimelapseSettingsOverlay::index_to_framerate(int index) {
    if (index >= 0 && index < FRAMERATE_COUNT) {
        return FRAMERATE_VALUES[index];
    }
    return 30; // Default to 30fps
}

TimelapseSettingsOverlay::TimelapseSettingsOverlay(IMoonrakerAPI* api) : api_(api) {}

lv_obj_t* TimelapseSettingsOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    // setting_toggle_row contains "toggle", setting_dropdown_row contains "dropdown"
    auto* enable_row = helix::ui::find_required(overlay_root_, "row_timelapse_enable", get_name());
    auto* mode_row = helix::ui::find_required(overlay_root_, "row_timelapse_mode", get_name());
    auto* framerate_row =
        helix::ui::find_required(overlay_root_, "row_timelapse_framerate", get_name());
    auto* autorender_row =
        helix::ui::find_required(overlay_root_, "row_timelapse_autorender", get_name());

    enable_switch_ = helix::ui::find_required(enable_row, "toggle", get_name());
    mode_dropdown_ = helix::ui::find_required(mode_row, "dropdown", get_name());
    framerate_dropdown_ = helix::ui::find_required(framerate_row, "dropdown", get_name());
    autorender_switch_ = helix::ui::find_required(autorender_row, "toggle", get_name());
    mode_info_text_ = helix::ui::find_required(overlay_root_, "mode_info_text", get_name());

    return overlay_root_;
}

void TimelapseSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_timelapse_enabled_changed",
         [](lv_event_t* e) {
             bool enabled = helix::ui::event_checked(e);
             auto& self = get_global_timelapse_settings();
             spdlog::debug("[{}] Enable changed: {}", self.get_name(), enabled);
             self.current_settings_.enabled = enabled;
             self.save_settings();
         }},
        {"on_timelapse_mode_changed",
         [](lv_event_t* e) {
             int index = helix::ui::event_selected(e);
             auto& self = get_global_timelapse_settings();
             const char* mode = (index == 1) ? "hyperlapse" : "layermacro";
             spdlog::debug("[{}] Mode changed: {} (index {})", self.get_name(), mode, index);
             self.current_settings_.mode = mode;
             self.update_mode_info(index);
             self.save_settings();
         }},
        {"on_timelapse_framerate_changed",
         [](lv_event_t* e) {
             int index = helix::ui::event_selected(e);
             auto& self = get_global_timelapse_settings();
             int framerate = index_to_framerate(index);
             spdlog::debug("[{}] Framerate changed: {} fps (index {})", self.get_name(), framerate,
                           index);
             self.current_settings_.output_framerate = framerate;
             self.save_settings();
         }},
        {"on_timelapse_autorender_changed",
         [](lv_event_t* e) {
             bool autorender = helix::ui::event_checked(e);
             auto& self = get_global_timelapse_settings();
             spdlog::debug("[{}] Autorender changed: {}", self.get_name(), autorender);
             self.current_settings_.autorender = autorender;
             self.save_settings();
         }},
    });
}

void TimelapseSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    spdlog::debug("[{}] on_activate() - fetching current settings", get_name());
    fetch_settings();
}

void TimelapseSettingsOverlay::fetch_settings() {
    if (!api_) {
        spdlog::debug("[{}] No API available, using defaults", get_name());
        // Use defaults for test mode
        current_settings_ = TimelapseSettings{};
        settings_loaded_ = true;
        // Update UI with defaults
        if (enable_switch_) {
            lv_obj_remove_state(enable_switch_, LV_STATE_CHECKED);
        }
        if (mode_dropdown_) {
            lv_dropdown_set_selected(mode_dropdown_, 0); // Layer Macro
        }
        if (framerate_dropdown_) {
            lv_dropdown_set_selected(framerate_dropdown_, 2); // 30fps
        }
        if (autorender_switch_) {
            lv_obj_add_state(autorender_switch_, LV_STATE_CHECKED);
        }
        update_mode_info(0);
        return;
    }

    spdlog::debug("[{}] Fetching timelapse settings from API", get_name());

    auto tok = lifetime_.token();
    api_->timelapse().get_timelapse_settings(
        [this, tok](const TimelapseSettings& settings) {
            tok.defer([this, settings]() {
                spdlog::info("[{}] Got timelapse settings: enabled={} mode={} fps={} autorender={}",
                             get_name(), settings.enabled, settings.mode, settings.output_framerate,
                             settings.autorender);

                current_settings_ = settings;
                settings_loaded_ = true;

                if (enable_switch_) {
                    if (settings.enabled) {
                        lv_obj_add_state(enable_switch_, LV_STATE_CHECKED);
                    } else {
                        lv_obj_remove_state(enable_switch_, LV_STATE_CHECKED);
                    }
                }

                if (mode_dropdown_) {
                    int mode_index = (settings.mode == "hyperlapse") ? 1 : 0;
                    lv_dropdown_set_selected(mode_dropdown_, mode_index);
                    update_mode_info(mode_index);
                }

                if (framerate_dropdown_) {
                    int fps_index = framerate_to_index(settings.output_framerate);
                    lv_dropdown_set_selected(framerate_dropdown_, fps_index);
                }

                if (autorender_switch_) {
                    if (settings.autorender) {
                        lv_obj_add_state(autorender_switch_, LV_STATE_CHECKED);
                    } else {
                        lv_obj_remove_state(autorender_switch_, LV_STATE_CHECKED);
                    }
                }
            });
        },
        [this, tok](const MoonrakerError& error) {
            tok.defer([this, error]() {
                spdlog::error("[{}] Failed to fetch timelapse settings: {}", get_name(),
                              error.message);
                settings_loaded_ = false;
            });
        });
}

void TimelapseSettingsOverlay::save_settings() {
    if (!api_) {
        spdlog::debug("[{}] No API available, not saving", get_name());
        return;
    }

    spdlog::debug("[{}] Saving timelapse settings: enabled={} mode={} fps={} autorender={}",
                  get_name(), current_settings_.enabled, current_settings_.mode,
                  current_settings_.output_framerate, current_settings_.autorender);

    api_->timelapse().set_timelapse_settings(
        current_settings_,
        lifetime_.bg_cb(
            "TimelapseSettingsOverlay::save_settings",
            [this]() { spdlog::info("[{}] Timelapse settings saved successfully", get_name()); }),
        lifetime_.bg_cb("TimelapseSettingsOverlay::save_settings_error",
                        [this](const MoonrakerError& error) {
                            spdlog::error("[{}] Failed to save timelapse settings: {}", get_name(),
                                          error.message);
                        }));
}

void TimelapseSettingsOverlay::update_mode_info(int mode_index) {
    if (!mode_info_text_) {
        return;
    }

    const char* info_text = (mode_index == 1)
                                ? lv_tr("Hyperlapse captures frames at fixed time intervals. "
                                        "Good for very long prints.")
                                : lv_tr("Layer Macro captures one frame per layer change. "
                                        "Best for most prints.");

    lv_label_set_text(mode_info_text_, info_text);
}

void open_timelapse_settings() {
    spdlog::debug("[Timelapse Settings] Timelapse row clicked");
    get_global_timelapse_settings().show(lv_display_get_screen_active(nullptr));
}
