// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "timelapse_local_installer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct WebcamInfo;
struct TimelapseSettings;
struct MoonrakerError;
namespace helix {
class IMoonrakerAPI;
class MacroManager;
} // namespace helix

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
 * 6. Verify plugin is loaded, then get its Klipper macros loaded: through
 *    helix_macros.cfg when Klipper has HelixScreen's macros, or by showing the
 *    line to add to printer.cfg
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

    /// Install the plugin on this host, on the install worker
    void step_install_locally();
    /// Main thread: the worker started by run @p gen ended with @p result
    void on_local_install_finished(unsigned gen, const helix::LocalPluginResult& result);
    void on_local_install_done(const helix::LocalPluginResult& result);
    /// Join a worker that has ended; never waits on one still running
    void join_finished_install_worker();
    /// End the worker's command and join it: well under a second
    void stop_install_worker();

    /// Whether a reply for run @p gen at @p step still belongs to the run on screen
    bool is_current(unsigned gen, Step step) const;

    /// While a job holds the machine, say the wizard waits for it and offer
    /// Check Again, which runs @p resume. Returns whether it is waiting.
    bool show_wait_for_print(std::function<void()> resume);

    /// Re-check plugin after user runs SSH install commands
    void recheck_after_install();

    /// Which instructions card shows (timelapse_install_card)
    enum class Card { None = 0, Ssh = 1, Ffmpeg = 2 };
    void show_card(Card card);

    /// Which macro card shows (timelapse_install_macro_card)
    enum class MacroCard { None = 0, IncludeLine = 1, RestartKlipper = 2 };
    void show_macro_card(MacroCard card);

    /// After verify: get TIMELAPSE_TAKE_FRAME loaded (decide_timelapse_macro_setup)
    void setup_timelapse_macros(unsigned gen);
    /// The macro card's Restart Klipper button; refused while a job holds the machine
    void restart_klipper_for_macros();
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
    lv_subject_t macro_card_subject_{};
    lv_subject_t include_line_subject_{};
    char include_line_buf_[64] = {};

    /// Adds the timelapse include to helix_macros.cfg; its callbacks die with it
    std::unique_ptr<helix::MacroManager> macro_manager_;

    /// The local install left ffmpeg missing; the last screen says how to add it
    bool ffmpeg_missing_ = false;

    /// The local install's commands. Owned and joined here, never detached:
    /// install_cancel_ ends its running command, so a join is short.
    std::thread install_thread_;
    /// Set by the worker itself, so no closed screen can leave it stuck true.
    std::atomic<bool> install_running_{false};
    std::atomic<bool> install_cancel_{false};
    /// A run that found an earlier run's install going starts its own when that ends
    bool waiting_on_earlier_install_ = false;
    /// The commands a local install runs; tests set it, empty means the real runner
    helix::ProcessRunner runner_override_;

    Step current_step_ = Step::CHECKING_WEBCAM;
    bool wizard_active_ = false;
    /// Bumped by every start_wizard(); a reply from an earlier run carries an older one
    unsigned run_generation_ = 0;
    std::function<void()> action_callback_;
};

TimelapseInstallOverlay& get_global_timelapse_install();
void init_global_timelapse_install(helix::IMoonrakerAPI* api);

/// Open the timelapse install wizard overlay (lazy-creates panel on first call)
void open_timelapse_install();
