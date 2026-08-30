// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_help.cpp
 * @brief Implementation of HelpSettingsOverlay
 */

#include "ui_settings_help.h"

#include "ui_callback_helpers.h"
#include "ui_debug_bundle_modal.h"
#include "ui_event_safety.h"
#include "ui_info_qr_modal.h"
#include "ui_nav_manager.h"
#include "ui_settings_about.h"

#include "first_run_tour.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<HelpSettingsOverlay> g_help_settings_overlay;

HelpSettingsOverlay& get_help_settings_overlay() {
    if (!g_help_settings_overlay) {
        g_help_settings_overlay = std::make_unique<HelpSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy("HelpSettingsOverlay",
                                                         []() { g_help_settings_overlay.reset(); });
    }
    return *g_help_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

HelpSettingsOverlay::HelpSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

HelpSettingsOverlay::~HelpSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void HelpSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void HelpSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_replay_tour_clicked", on_replay_tour_clicked},
        {"on_debug_bundle_clicked", on_debug_bundle_clicked},
        {"on_discord_clicked", on_discord_clicked},
        {"on_docs_clicked", on_docs_clicked},
        {"on_about_clicked", on_about_clicked},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* HelpSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_help_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void HelpSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Register for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void HelpSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    spdlog::debug("[{}] Activated", get_name());
}

void HelpSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
    spdlog::debug("[{}] Deactivated", get_name());
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void HelpSettingsOverlay::on_replay_tour_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HelpSettingsOverlay] on_replay_tour_clicked");
    spdlog::info("[HelpSettingsOverlay] Replay Welcome Tour clicked");
    // Dismiss the Help overlay, then switch to the Home panel so the tour's
    // navbar highlights (and the widget tiles) are actually on screen.
    NavigationManager::instance().go_back();
    NavigationManager::instance().set_active(helix::PanelId::Home);
    // Defer start so panel activation + layout settle before the overlay
    // resolves target coordinates. Raw lv_async_call is safe here: the lambda
    // captures no `this` (user_data=nullptr) and only touches the immortal
    // FirstRunTour function-local static singleton.
    lv_async_call([](void*) { helix::tour::FirstRunTour::instance().start(); }, nullptr);
    LVGL_SAFE_EVENT_CB_END();
}

void HelpSettingsOverlay::on_debug_bundle_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HelpSettingsOverlay] on_debug_bundle_clicked");
    spdlog::info("[HelpSettingsOverlay] Upload Debug Bundle clicked");
    DebugBundleModal::show_owned();
    LVGL_SAFE_EVENT_CB_END();
}

void HelpSettingsOverlay::on_discord_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HelpSettingsOverlay] on_discord_clicked");
    spdlog::info("[HelpSettingsOverlay] Discord clicked");
    helix::ui::InfoQrModal::show_owned({
        .icon = "message",
        .title = "Discord Community",
        .message = lv_tr("Join the HelixScreen community on Discord for discussion, "
                         "tips, troubleshooting help, and feature requests."),
        .url = "https://discord.gg/RZCT2StKhr",
        .url_text = "discord.gg/RZCT2StKhr",
    });
    LVGL_SAFE_EVENT_CB_END();
}

void HelpSettingsOverlay::on_docs_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HelpSettingsOverlay] on_docs_clicked");
    spdlog::info("[HelpSettingsOverlay] Documentation clicked");
    helix::ui::InfoQrModal::show_owned({
        .icon = "book",
        .title = lv_tr("Documentation"),
        .message = lv_tr("Browse guides, configuration references, and troubleshooting "
                         "resources for HelixScreen."),
        .url = "https://helixscreen.org/docs/guide/getting-started/",
        .url_text = "helixscreen.org/docs",
    });
    LVGL_SAFE_EVENT_CB_END();
}

void HelpSettingsOverlay::on_about_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HelpSettingsOverlay] on_about_clicked");
    spdlog::debug("[HelpSettingsOverlay] About clicked - opening AboutSettingsOverlay");
    auto& overlay = helix::settings::get_about_settings_overlay();
    overlay.show(lv_screen_active());
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
