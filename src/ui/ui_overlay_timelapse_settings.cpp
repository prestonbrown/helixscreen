// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_timelapse_settings.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_nav_manager.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "runtime_config.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "timelapse_state.h"

#include <spdlog/spdlog.h>

// Global instance and panel
static std::unique_ptr<TimelapseSettingsOverlay> g_timelapse_settings;
static lv_obj_t* g_timelapse_settings_panel = nullptr;

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
    StaticPanelRegistry::instance().register_destroy("TimelapseSettingsOverlay", []() {
        if (g_timelapse_settings_panel) {
            NavigationManager::instance().unregister_overlay_instance(g_timelapse_settings_panel);
        }
        g_timelapse_settings_panel = nullptr;
        g_timelapse_settings.reset();
    });
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

TimelapseSettingsOverlay::TimelapseSettingsOverlay(IMoonrakerAPI* api) : api_(api) {
    spdlog::debug("[{}] Constructor", get_name());
}

void TimelapseSettingsOverlay::init_subjects() {
    spdlog::debug("[{}] init_subjects()", get_name());
}

lv_obj_t* TimelapseSettingsOverlay::create(lv_obj_t* parent) {
    // Create overlay root from XML
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, get_xml_component_name(), nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] create() - finding widgets", get_name());

    // Find row containers first, then get inner widgets
    // setting_toggle_row contains "toggle", setting_dropdown_row contains "dropdown"
    lv_obj_t* enable_row = lv_obj_find_by_name(overlay_root_, "row_timelapse_enable");
    lv_obj_t* mode_row = lv_obj_find_by_name(overlay_root_, "row_timelapse_mode");
    lv_obj_t* framerate_row = lv_obj_find_by_name(overlay_root_, "row_timelapse_framerate");
    lv_obj_t* autorender_row = lv_obj_find_by_name(overlay_root_, "row_timelapse_autorender");

    // Find inner widgets within rows
    if (enable_row) {
        enable_switch_ = lv_obj_find_by_name(enable_row, "toggle");
    }
    if (mode_row) {
        mode_dropdown_ = lv_obj_find_by_name(mode_row, "dropdown");
    }
    if (framerate_row) {
        framerate_dropdown_ = lv_obj_find_by_name(framerate_row, "dropdown");
    }
    if (autorender_row) {
        autorender_switch_ = lv_obj_find_by_name(autorender_row, "toggle");
    }

    // Mode info text is standalone
    mode_info_text_ = lv_obj_find_by_name(overlay_root_, "mode_info_text");

    // Log widget discovery
    spdlog::debug("[{}] Widgets found: enable={} mode={} info={} framerate={} autorender={}",
                  get_name(), enable_switch_ != nullptr, mode_dropdown_ != nullptr,
                  mode_info_text_ != nullptr, framerate_dropdown_ != nullptr,
                  autorender_switch_ != nullptr);

    // Register event callbacks via XML system
    register_xml_callbacks({
        {"on_timelapse_enabled_changed", on_enabled_changed},
        {"on_timelapse_mode_changed", on_mode_changed},
        {"on_timelapse_framerate_changed", on_framerate_changed},
        {"on_timelapse_autorender_changed", on_autorender_changed},
    });

    return overlay_root_;
}

void TimelapseSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    spdlog::debug("[{}] on_activate() - fetching current settings", get_name());
    fetch_settings();
}

void TimelapseSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
    spdlog::debug("[{}] on_deactivate()", get_name());
}

void TimelapseSettingsOverlay::cleanup() {
    spdlog::debug("[{}] cleanup()", get_name());
    OverlayBase::cleanup();
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
            if (tok.expired())
                return;
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
            if (tok.expired())
                return;
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
        [this]() { spdlog::info("[{}] Timelapse settings saved successfully", get_name()); },
        [this](const MoonrakerError& error) {
            spdlog::error("[{}] Failed to save timelapse settings: {}", get_name(), error.message);
        });
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

// Static event handlers
void TimelapseSettingsOverlay::on_enabled_changed(lv_event_t* e) {
    if (!g_timelapse_settings) {
        return;
    }

    lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

    spdlog::debug("[Timelapse Settings] Enable changed: {}", enabled);
    g_timelapse_settings->current_settings_.enabled = enabled;
    g_timelapse_settings->save_settings();
}

void TimelapseSettingsOverlay::on_mode_changed(lv_event_t* e) {
    if (!g_timelapse_settings) {
        return;
    }

    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int index = lv_dropdown_get_selected(dropdown);

    const char* mode = (index == 1) ? "hyperlapse" : "layermacro";
    spdlog::debug("[Timelapse Settings] Mode changed: {} (index {})", mode, index);

    g_timelapse_settings->current_settings_.mode = mode;
    g_timelapse_settings->update_mode_info(index);
    g_timelapse_settings->save_settings();
}

void TimelapseSettingsOverlay::on_framerate_changed(lv_event_t* e) {
    if (!g_timelapse_settings) {
        return;
    }

    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int index = lv_dropdown_get_selected(dropdown);
    int framerate = index_to_framerate(index);

    spdlog::debug("[Timelapse Settings] Framerate changed: {} fps (index {})", framerate, index);

    g_timelapse_settings->current_settings_.output_framerate = framerate;
    g_timelapse_settings->save_settings();
}

void TimelapseSettingsOverlay::on_autorender_changed(lv_event_t* e) {
    if (!g_timelapse_settings) {
        return;
    }

    lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool autorender = lv_obj_has_state(sw, LV_STATE_CHECKED);

    spdlog::debug("[Timelapse Settings] Autorender changed: {}", autorender);
    g_timelapse_settings->current_settings_.autorender = autorender;
    g_timelapse_settings->save_settings();
}

// ============================================================================
// Row Click Callback (opens this overlay from Advanced panel)
// ============================================================================

void open_timelapse_settings() {
    spdlog::debug("[Timelapse Settings] Timelapse row clicked");

    if (!g_timelapse_settings) {
        spdlog::error("[Timelapse Settings] Global instance not initialized!");
        return;
    }

    // Lazy-create the timelapse settings panel using OverlayBase::create()
    if (!g_timelapse_settings_panel) {
        spdlog::debug("[Timelapse Settings] Creating timelapse settings panel...");
        g_timelapse_settings_panel =
            g_timelapse_settings->create(lv_display_get_screen_active(nullptr));

        if (g_timelapse_settings_panel) {
            // Register with NavigationManager for lifecycle callbacks
            NavigationManager::instance().register_overlay_instance(g_timelapse_settings_panel,
                                                                    g_timelapse_settings.get());
            spdlog::debug("[Timelapse Settings] Panel created and registered");
        } else {
            spdlog::error("[Timelapse Settings] Failed to create timelapse_settings_overlay");
            return;
        }
    }

    // Show the overlay - NavigationManager will call on_activate()
    NavigationManager::instance().push_overlay(g_timelapse_settings_panel);
}
