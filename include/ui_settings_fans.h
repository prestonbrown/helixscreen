// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_fans.h
 * @brief Fan Settings overlay - view and rename discovered fans
 *
 * This overlay displays all discovered fans grouped into two sections:
 * - Controllable fans (part cooling, fan_generic, output_pin)
 * - Auto fans (heater_fan, controller_fan, temperature_fan)
 *
 * Each fan row shows the display name, Moonraker object name, type pill,
 * and current speed. Tapping a fan name opens a rename dialog that persists
 * the custom name via Config.
 *
 * @pattern Overlay (lazy init, dynamic row creation)
 * @threading Main thread only
 *
 * @see PrinterFanState for fan discovery and state
 * @see Config for name persistence
 */

#pragma once

#include "ui_widget_ref.h"

#include "core/static_panel_registry.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <string>

namespace helix::settings {

/**
 * @class FanSettingsOverlay
 * @brief Overlay for viewing and renaming discovered fans
 *
 * ## Dynamic Row Creation:
 *
 * Fan rows are created dynamically at runtime using lv_xml_create().
 * This requires using lv_obj_add_event_cb() for the rename click
 * callbacks, which is an acceptable exception to the declarative UI rule.
 */
class FanSettingsOverlay : public OverlayBase {
  public:
    /// Register event callbacks with lv_xml system
    void register_callbacks() override;

    /// Default create() plus the lookups for the dynamic row containers
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "Fans";
    }
    const char* xml_component() const override {
        return "fan_settings_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    /// Called when overlay becomes visible — repopulates fan lists
    void on_activate() override;

    /// Called when overlay is being hidden
    void on_deactivating(DeactivateReason reason) override;

    /// Handle fan rename via keyboard modal (callable from any overlay)
    void handle_fan_rename(const std::string& object_name, const std::string& current_name);

    /// Confirm the rename (called from static callback)
    void confirm_rename();

    /// Cancel/hide the rename modal
    void cancel_rename();

  private:
    /// Populate all fan sections from PrinterFanState
    void populate_fans();

    /// Populate a specific fan list container
    void populate_fan_list(lv_obj_t* list, bool controllable);

    /// Update badge count for a section
    void update_section_count(const char* badge_name, size_t count);

    lv_obj_t* controllable_list_ = nullptr;
    lv_obj_t* auto_list_ = nullptr;
    lv_obj_t* no_fans_placeholder_ = nullptr;

    void on_ui_destroyed() override {
        controllable_list_ = nullptr;
        auto_list_ = nullptr;
        no_fans_placeholder_ = nullptr;
    }

    // Rename modal state
    helix::ui::WidgetRef rename_modal_;
    std::string pending_rename_object_;
    SubjectManager subjects_; // declared ahead: tears down after the subject it owns
    char rename_old_name_buf_[64] = {};
    lv_subject_t fan_rename_old_name_{}; ///< Subject for bind_text in modal
    bool rename_subject_initialized_ = false;
};

inline FanSettingsOverlay& get_fan_settings_overlay() {
    return lazy_global<FanSettingsOverlay>("FanSettingsOverlay");
}

} // namespace helix::settings
