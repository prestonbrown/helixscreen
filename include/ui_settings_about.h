// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_about.h
 * @brief About Settings overlay - version info, easter eggs, contributors
 *
 * This overlay displays:
 * - Branding header with logo and scrolling contributor marquee
 * - Printer name (7-tap snake easter egg)
 * - Version info (7-tap beta features toggle)
 * - Klipper/Moonraker/OS version info
 * - Print hours (opens history dashboard)
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SettingsPanel for parent panel
 */

#pragma once

#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <chrono>
#include <string>

namespace helix {
class IMoonrakerClient;
}

namespace helix::settings {

/**
 * @class AboutSettingsOverlay
 * @brief Overlay for displaying about/version info
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_about_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class AboutSettingsOverlay : public OverlayBase {
  public:
    ~AboutSettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "About Settings";
    }
    const char* xml_component() const override {
        return "about_settings_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

    /// Builds the overlay and its contributor marquee.
    lv_obj_t* create(lv_obj_t* parent) override;

    //
    // === Public Methods ===
    //

    /**
     * @brief Fetch print hours from Moonraker history totals
     *
     * Called after discovery completes (connection is live) and on
     * notify_history_changed events. Updates print_hours_value_subject_.
     */
    void fetch_print_hours();

    /// Fetch print hours now and again whenever a job finishes. Idempotent: a repeat
    /// call (every discovery) replaces the earlier subscription.
    void attach_print_hours(IMoonrakerClient& client);

    /// Drop the subscription attach_print_hours() made. Safe when never attached.
    void detach_print_hours(IMoonrakerClient& client);

    /**
     * @brief Refresh version and printer info subjects
     *
     * Called on activate to ensure info is current.
     */
    void populate_info_rows();

  private:
    //
    // === Contributor Marquee ===
    //

    void setup_contributor_marquee();

    lv_obj_t* marquee_content_ = nullptr;

    void on_ui_destroyed() override {
        marquee_content_ = nullptr;
    }

    //
    // === Reactive Subjects ===
    //

    SubjectManager subjects_;

    lv_subject_t version_value_subject_{};
    lv_subject_t about_version_description_subject_{};
    lv_subject_t printer_value_subject_{};
    lv_subject_t print_hours_value_subject_{};
    lv_subject_t install_root_value_subject_{};
    lv_subject_t config_dir_value_subject_{};
    lv_subject_t cache_dir_value_subject_{};
    lv_subject_t log_dest_value_subject_{};
    lv_subject_t host_arch_value_subject_{};
    lv_subject_t about_copyright_subject_{};

    // Static buffers for string subjects
    char version_value_buf_[32];
    char about_version_description_buf_[48];
    char printer_value_buf_[64];
    char print_hours_value_buf_[32];
    char install_root_value_buf_[256];
    char config_dir_value_buf_[256];
    char cache_dir_value_buf_[256];
    char log_dest_value_buf_[256];
    char host_arch_value_buf_[64];
    char about_copyright_buf_[48];

    // Debounce tracker restart on inadvertent re-open
    std::chrono::steady_clock::time_point last_deactivate_{};
};

inline AboutSettingsOverlay& get_about_settings_overlay() {
    return lazy_global<AboutSettingsOverlay>("AboutSettingsOverlay");
}

} // namespace helix::settings
