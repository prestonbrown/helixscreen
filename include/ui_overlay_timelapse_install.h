// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"

#include <functional>
#include <string>
#include <vector>

struct WebcamInfo;
struct TimelapseSettings;
struct MoonrakerError;
class IMoonrakerAPI;

/**
 * @file ui_overlay_timelapse_install.h
 * @brief Step-progress wizard for installing moonraker-timelapse plugin
 *
 * Guides users through timelapse plugin installation with step progress:
 * 1. Check webcam availability
 * 2. Check if plugin is already installed
 * 3. Show SSH install instructions (if plugin not found)
 * 4. Configure moonraker.conf with [timelapse] section
 * 5. Restart Moonraker service
 * 6. Verify plugin is loaded
 */
class TimelapseInstallOverlay : public OverlayBase {
  public:
    explicit TimelapseInstallOverlay(IMoonrakerAPI* api);
    ~TimelapseInstallOverlay() override = default;

    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Timelapse Install";
    }
    const char* get_xml_component_name() const {
        return "timelapse_install_overlay";
    }

    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;

    lv_obj_t* get_panel() const {
        return overlay_root_;
    }
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    // Config file helpers (public for testability)
    static std::string append_timelapse_config(const std::string& content);
    static bool has_timelapse_section(const std::string& content);

  private:
    enum class Step {
        CHECKING_WEBCAM = 0,
        CHECKING_PLUGIN = 1,
        INSTALL_PLUGIN = 2,
        CONFIGURE_MOONRAKER = 3,
        RESTART_MOONRAKER = 4,
        VERIFY = 5
    };
    static constexpr int STEP_COUNT = 6;

    /// Start the wizard from the beginning
    void start_wizard();

    /// Set the active step in the progress indicator
    void set_step(Step step);

    /// Update the status text label
    void set_status(const char* text);

    /// Show the action button with a label and callback
    void show_action_button(const char* label, std::function<void()> callback);

    /// Hide the action button
    void hide_action_button();

    // Wizard step implementations
    void step_check_webcam();
    void step_check_plugin();
    void step_show_install_instructions();
    void step_configure_moonraker();
    void step_restart_moonraker();
    void step_verify();

    /// Re-check plugin after user runs SSH install commands
    void recheck_after_install();

    // Config file modification (private: requires API)
    void download_and_modify_config();
    void write_timelapse_config(const std::string& helix_content,
                                const std::string& moonraker_content);

    // Static event callbacks
    static void on_action_clicked(lv_event_t* e);

    IMoonrakerAPI* api_;
    lv_obj_t* step_progress_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* action_btn_ = nullptr;
    lv_obj_t* ssh_container_ = nullptr;

    Step current_step_ = Step::CHECKING_WEBCAM;
    bool wizard_active_ = false;
    std::function<void()> action_callback_;
};

TimelapseInstallOverlay& get_global_timelapse_install();
void init_global_timelapse_install(IMoonrakerAPI* api);

/// Open the timelapse install wizard overlay (lazy-creates panel on first call)
void open_timelapse_install();
