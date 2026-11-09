// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "timelapse_local_installer.h"

#include <functional>
#include <string>
#include <vector>

struct WebcamInfo;
struct TimelapseSettings;
struct MoonrakerError;
namespace helix {
class IMoonrakerAPI;
}

/**
 * @file ui_overlay_timelapse_install.h
 * @brief Step-progress wizard for installing moonraker-timelapse plugin
 *
 * Guides users through timelapse plugin installation with step progress:
 * 1. Check webcam availability
 * 2. Check if plugin is already installed
 * 3. Install the plugin: by itself when Moonraker runs on this host,
 *    through SSH instructions otherwise
 * 4. Configure moonraker.conf with [timelapse] section
 * 5. Restart Moonraker service
 * 6. Verify plugin is loaded
 */
class TimelapseInstallOverlay : public OverlayBase {
  public:
    explicit TimelapseInstallOverlay(helix::IMoonrakerAPI* api);
    ~TimelapseInstallOverlay() override;

    const char* get_name() const override {
        return "Timelapse Install";
    }
    const char* xml_component() const override {
        return "timelapse_install_overlay";
    }

    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void register_callbacks() override;

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;

    lv_obj_t* get_panel() const {
        return overlay_root_;
    }
    void set_api(helix::IMoonrakerAPI* api) {
        api_ = api;
    }

    // Config file helpers (public for testability)
    static std::string append_timelapse_config(const std::string& content);
    static bool has_timelapse_section(const std::string& content);

  private:
    friend struct TimelapseInstallOverlayTestAccess;

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

    /// Take the route decide_timelapse_setup_path() picks for an absent plugin
    void step_install_plugin();

    /// Install the plugin on this host, off the main thread
    void step_install_locally();
    void on_local_install_done(const helix::LocalPluginResult& result);

    /// Re-check plugin after user runs SSH install commands
    void recheck_after_install();

    /// Which instructions card shows (timelapse_install_card)
    enum class Card { None = 0, Ssh = 1, Ffmpeg = 2 };
    void show_card(Card card);
    /// Scroll the card on show into view
    void reveal_card();

    // Config file modification (private: requires API)
    void download_and_modify_config();
    /// Main thread: fetch helixscreen.conf and write the timelapse sections.
    /// `moonraker_found` is false when moonraker.conf was missing.
    void download_helix_config(const std::string& moonraker_content, bool moonraker_found);
    /// Main thread: add the [include helixscreen.conf] line to moonraker.conf.
    void upload_moonraker_include(const std::string& moonraker_content);
    void write_timelapse_config(const std::string& helix_content,
                                const std::string& moonraker_content);

    helix::IMoonrakerAPI* api_;
    lv_obj_t* step_progress_ = nullptr;

    SubjectManager subjects_;
    lv_subject_t status_subject_{};
    char status_buf_[384] = {};
    lv_subject_t action_text_subject_{};
    char action_text_buf_[64] = {};
    lv_subject_t action_visible_subject_{};
    lv_subject_t card_subject_{};
    lv_subject_t ffmpeg_command_subject_{};
    char ffmpeg_command_buf_[64] = {};

    /// The local install left ffmpeg missing; the last screen says how to add it
    bool ffmpeg_missing_ = false;
    /// A local install is running on a worker; a second tap must not start another
    bool installing_ = false;

    Step current_step_ = Step::CHECKING_WEBCAM;
    bool wizard_active_ = false;
    std::function<void()> action_callback_;
};

TimelapseInstallOverlay& get_global_timelapse_install();
void init_global_timelapse_install(helix::IMoonrakerAPI* api);

/// Open the timelapse install wizard overlay (lazy-creates panel on first call)
void open_timelapse_install();
