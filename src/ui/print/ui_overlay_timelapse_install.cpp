// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_timelapse_install.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_nav.h"
#include "ui_step_progress.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "exception_policy.h"
#include "helix_fs.h"
#include "helix_plugin_installer.h"
#include "helix_thread.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl.h"
#include "macro_manager.h"
#include "moonraker_config_manager.h"
#include "moonraker_types.h"
#include "printer_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <sstream>

using namespace helix;

namespace {
bool job_holds_now() {
    lv_subject_t* job = get_printer_state().print_state().get_job_holds_machine_subject();
    return job && lv_subject_get_int(job) != 0;
}
} // namespace

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

TimelapseInstallOverlay& get_global_timelapse_install() {
    auto* instance = helix::lazy_global_if_exists<TimelapseInstallOverlay>();
    if (!instance) {
        spdlog::error("[Timelapse Install] get_global called before init!");
        throw std::runtime_error("TimelapseInstallOverlay not initialized");
    }
    return *instance;
}

void init_global_timelapse_install(IMoonrakerAPI* api) {
    helix::lazy_global<TimelapseInstallOverlay>("TimelapseInstallOverlay", api);
    spdlog::trace("[Timelapse Install] Initialized");
}

// ============================================================================
// CONSTRUCTOR & LIFECYCLE
// ============================================================================

TimelapseInstallOverlay::TimelapseInstallOverlay(IMoonrakerAPI* api) : api_(api) {}

TimelapseInstallOverlay::~TimelapseInstallOverlay() {
    // The worker's command is ended, so this waits well under a second.
    stop_install_worker();
    // SubjectManager handles LVGL initialization check and cleanup
    subjects_.deinit_all();
}

void TimelapseInstallOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, "", "timelapse_install_status",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(action_text_subject_, action_text_buf_, "",
                                  "timelapse_install_action_text", subjects_);
        UI_MANAGED_SUBJECT_INT(action_visible_subject_, 0, "timelapse_install_action_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(card_subject_, static_cast<int>(Card::None),
                               "timelapse_install_card", subjects_);
        UI_MANAGED_SUBJECT_STRING(ffmpeg_command_subject_, ffmpeg_command_buf_,
                                  kFfmpegInstallCommand, "timelapse_install_ffmpeg_command",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(macro_card_subject_, static_cast<int>(MacroCard::None),
                               "timelapse_install_macro_card", subjects_);
        UI_MANAGED_SUBJECT_STRING(include_line_subject_, include_line_buf_, kTimelapseIncludeLine,
                                  "timelapse_install_include_line", subjects_);
    });
}

void TimelapseInstallOverlay::register_callbacks() {
    register_xml_callbacks({{"on_timelapse_install_action",
                             [](lv_event_t*) { get_global_timelapse_install().press_action(); }},
                            {"on_timelapse_install_restart_klipper", [](lv_event_t*) {
                                 get_global_timelapse_install().restart_klipper_for_macros();
                             }}});
}

void TimelapseInstallOverlay::press_action() {
    if (action_callback_) {
        action_callback_();
    }
}

lv_obj_t* TimelapseInstallOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    // Step progress widget is built programmatically (dynamic content)
    lv_obj_t* step_container =
        helix::ui::find_required(overlay_root_, "step_container", get_name());
    if (step_container) {
        ui_step_t steps[] = {{lv_tr("Checking webcam"), StepState::Pending},
                             {lv_tr("Checking plugin"), StepState::Pending},
                             {lv_tr("Install plugin"), StepState::Pending},
                             {lv_tr("Configure Moonraker"), StepState::Pending},
                             {lv_tr("Restart Moonraker"), StepState::Pending},
                             {lv_tr("Verify"), StepState::Pending}};
        step_progress_ = ui_step_progress_create(step_container, steps, STEP_COUNT, false, nullptr);
    }

    hide_action_button();
    show_card(Card::None);
    show_macro_card(MacroCard::None);

    return overlay_root_;
}

void TimelapseInstallOverlay::on_activate() {
    OverlayBase::on_activate();
    spdlog::debug("[{}] Activated", get_name());
    // A wake resumes the run on the step it reached; its replies still land.
    if (take_wake()) {
        return;
    }
    start_wizard();
}

void TimelapseInstallOverlay::on_deactivating(DeactivateReason reason) {
    spdlog::debug("[{}] Deactivated ({})", get_name(), deactivate_reason_name(reason));
    if (reason != DeactivateReason::Suspended) {
        wizard_active_ = false;
    }
    if (reason == DeactivateReason::Shutdown) {
        install_cancel_.store(true);
    }
}

void TimelapseInstallOverlay::cleanup() {
    wizard_active_ = false;
    stop_install_worker();
    action_callback_ = nullptr;
    OverlayBase::cleanup();
}

// ============================================================================
// WIZARD FLOW
// ============================================================================

void TimelapseInstallOverlay::start_wizard() {
    wizard_active_ = true;
    ++run_generation_;
    waiting_on_earlier_install_ = false;
    action_callback_ = nullptr;
    ffmpeg_missing_ = false;
    macro_manager_.reset();

    show_card(Card::None);
    show_macro_card(MacroCard::None);
    hide_action_button();

    step_check_webcam();
}

void TimelapseInstallOverlay::set_step(Step step) {
    current_step_ = step;
    if (step_progress_) {
        ui_step_progress_set_current(step_progress_, static_cast<int>(step));
    }
}

void TimelapseInstallOverlay::set_status(const char* text) {
    if (subjects_initialized_) {
        lv_subject_copy_string(&status_subject_, text);
    }
}

void TimelapseInstallOverlay::show_action_button(const char* label,
                                                 std::function<void()> callback) {
    action_callback_ = std::move(callback);
    if (subjects_initialized_) {
        lv_subject_copy_string(&action_text_subject_, label);
        lv_subject_set_int(&action_visible_subject_, 1);
    }
    // The footer takes height from the scroll area the card sits in.
    reveal_card();
}

void TimelapseInstallOverlay::hide_action_button() {
    action_callback_ = nullptr;
    if (subjects_initialized_) {
        lv_subject_set_int(&action_visible_subject_, 0);
    }
}

void TimelapseInstallOverlay::show_card(Card card) {
    if (subjects_initialized_) {
        lv_subject_set_int(&card_subject_, static_cast<int>(card));
    }
    reveal_card();
}

void TimelapseInstallOverlay::show_macro_card(MacroCard card) {
    if (subjects_initialized_) {
        lv_subject_set_int(&macro_card_subject_, static_cast<int>(card));
    }
    reveal_card();
}

void TimelapseInstallOverlay::reveal_card() {
    if (!overlay_root_ || !subjects_initialized_ ||
        (lv_subject_get_int(&card_subject_) == static_cast<int>(Card::None) &&
         lv_subject_get_int(&macro_card_subject_) == static_cast<int>(MacroCard::None))) {
        return;
    }
    // The step list fills the first screen, so the card with commands to run, the
    // last thing in the scroll area, would otherwise sit below the fold.
    if (lv_obj_t* content = lv_obj_find_by_name(overlay_root_, "overlay_content")) {
        lv_obj_update_layout(overlay_root_);
        lv_obj_scroll_to_y(content, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

// ============================================================================
// STEP 1: CHECK WEBCAM
// ============================================================================

void TimelapseInstallOverlay::step_check_webcam() {
    const unsigned gen = run_generation_;
    set_step(Step::CHECKING_WEBCAM);
    set_status(lv_tr("Checking for webcam..."));

    if (!api_) {
        set_status(lv_tr("Not connected to printer"));
        return;
    }

    // If discovery already found a webcam (e.g. via local endpoint probing on
    // devices like AD5M that don't configure Moonraker's webcam list), skip
    // the API query and proceed directly.
    if (get_printer_state().has_webcam()) {
        spdlog::info("[{}] Webcam already detected by discovery, skipping API check", get_name());
        step_check_plugin();
        return;
    }

    api_->timelapse().get_webcam_list(
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::webcams",
            [this, gen](const std::vector<WebcamInfo>& webcams) {
                if (!is_current(gen, Step::CHECKING_WEBCAM))
                    return;
                if (webcams.empty()) {
                    set_status(lv_tr("No webcam detected.\nA webcam is required for timelapse."));
                    show_action_button(lv_tr("Check Again"), [this]() { step_check_webcam(); });
                    return;
                }
                spdlog::info("[{}] Found {} webcam(s)", get_name(), webcams.size());
                step_check_plugin();
            }),
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::webcams_err", [this, gen](const MoonrakerError& err) {
                if (!is_current(gen, Step::CHECKING_WEBCAM))
                    return;
                spdlog::warn("[{}] Webcam check failed: {}", get_name(), err.message);
                set_status(lv_tr("Could not check webcam status.\nCheck printer connection."));
                show_action_button(lv_tr("Check Again"), [this]() { step_check_webcam(); });
            }));
}

// ============================================================================
// STEP 2: CHECK PLUGIN
// ============================================================================

void TimelapseInstallOverlay::step_check_plugin() {
    const unsigned gen = run_generation_;
    set_step(Step::CHECKING_PLUGIN);
    set_status(lv_tr("Checking timelapse plugin..."));

    if (!api_)
        return;

    api_->timelapse().get_timelapse_settings(
        lifetime_.bg_cb("TimelapseInstallOverlay::plugin_present",
                        [this, gen](const TimelapseSettings& /*settings*/) {
                            if (!is_current(gen, Step::CHECKING_PLUGIN))
                                return;
                            spdlog::info("[{}] Timelapse plugin already installed", get_name());
                            set_status(lv_tr("Timelapse plugin is already installed!"));
                            if (step_progress_) {
                                for (int i = 0; i < STEP_COUNT; i++) {
                                    ui_step_progress_set_completed(step_progress_, i);
                                }
                            }
                            show_action_button(lv_tr("Close"), []() { helix::nav::go_back(); });
                        }),
        lifetime_.bg_cb("TimelapseInstallOverlay::plugin_absent",
                        [this, gen](const MoonrakerError& /*err*/) {
                            if (!is_current(gen, Step::CHECKING_PLUGIN))
                                return;
                            spdlog::info("[{}] Plugin not detected", get_name());
                            step_install_plugin();
                        }));
}

// ============================================================================
// STEP 3: INSTALL PLUGIN
// ============================================================================

void TimelapseInstallOverlay::step_install_plugin() {
    TimelapseSetupFacts facts;
    facts.plugin_present = false;
    facts.moonraker_local =
        get_printer_state().moonraker_locality() == helix::MoonrakerLocality::Local;
    const bool has_script = !HelixPluginInstaller().get_install_script_path().empty();
    const bool has_home = !timelapse_install_home().empty();
    facts.can_install_locally = has_script && has_home;
    facts.job_holds = job_holds_now();

    const TimelapseSetupPath path = decide_timelapse_setup_path(facts);
    spdlog::info("[{}] Install route: local={} script={} home={} job_holds={} -> {}", get_name(),
                 facts.moonraker_local, has_script, has_home, facts.job_holds,
                 static_cast<int>(path));
    if (facts.moonraker_local && has_script && !has_home) {
        spdlog::warn("[{}] HOME is unset or empty, so there is nowhere to clone the plugin; "
                     "showing the SSH commands",
                     get_name());
    }
    switch (path) {
    case TimelapseSetupPath::AlreadyInstalled:
        step_configure_moonraker();
        return;
    case TimelapseSetupPath::LocalInstall:
        step_install_locally();
        return;
    case TimelapseSetupPath::WaitForJob:
        set_step(Step::INSTALL_PLUGIN);
        show_wait_for_print([this]() { step_check_plugin(); });
        return;
    case TimelapseSetupPath::RemoteInstructions:
        step_show_install_instructions();
        return;
    }
}

void TimelapseInstallOverlay::step_show_install_instructions() {
    set_step(Step::INSTALL_PLUGIN);
    set_status(lv_tr("Install the timelapse plugin via SSH,\nthen tap \"Check Again\"."));
    show_card(Card::Ssh);
    show_action_button(lv_tr("Check Again"), [this]() { recheck_after_install(); });
}

namespace {
const char* local_install_stage_text(LocalPluginStage stage) {
    switch (stage) {
    case LocalPluginStage::Fetch:
        return lv_tr("Downloading the timelapse plugin...");
    case LocalPluginStage::Install:
        return lv_tr("Adding the timelapse plugin to Moonraker...");
    case LocalPluginStage::CheckDependency:
        return lv_tr("Checking for ffmpeg...");
    }
    return "";
}

const char* local_install_failure_text(LocalPluginStage stage) {
    switch (stage) {
    case LocalPluginStage::Fetch:
        return lv_tr("Could not download the timelapse plugin. Check the printer's internet "
                     "connection.");
    case LocalPluginStage::Install:
    case LocalPluginStage::CheckDependency:
        return lv_tr("Could not add the timelapse plugin to Moonraker.");
    }
    return "";
}
} // namespace

void TimelapseInstallOverlay::step_install_locally() {
    set_step(Step::INSTALL_PLUGIN);
    show_card(Card::None);
    hide_action_button();
    if (show_wait_for_print([this]() { step_install_locally(); })) {
        return;
    }
    if (install_running_.load()) {
        // A run closed mid-install left its worker going; this run starts its own
        // once that one ends, and Check Again lets the user ask sooner.
        waiting_on_earlier_install_ = true;
        set_status(lv_tr("An earlier install is still finishing..."));
        show_action_button(lv_tr("Check Again"), [this]() { step_install_locally(); });
        return;
    }
    waiting_on_earlier_install_ = false;
    join_finished_install_worker();

    const std::string home = timelapse_install_home();
    if (home.empty()) {
        spdlog::warn("[{}] HOME is unset or empty, so there is nowhere to clone the plugin; "
                     "showing the SSH commands",
                     get_name());
        step_show_install_instructions();
        return;
    }
    const std::string script = HelixPluginInstaller().get_install_script_path();
    ProcessRunner runner =
        runner_override_ ? runner_override_ : timelapse_install_runner(install_cancel_);
    set_status(lv_tr("Installing the timelapse plugin..."));

    // The commands block for as long as git takes, so they run on this object's own
    // worker. Stage text is the screen's; the outcome is the object's, so a run
    // reopened while this one is still going hears when it ends.
    const unsigned gen = run_generation_;
    auto screen_tok = lifetime_.token();
    auto object_tok = object_lifetime_.token();
    install_cancel_.store(false);
    install_running_.store(true);
    const bool started = helix::contain_exceptions("[Timelapse Install] install worker", [&]() {
        install_thread_ = helix::make_background_thread([this, gen, screen_tok, object_tok, home,
                                                         script, runner]() {
            const LocalPluginRecipe& recipe = timelapse_plugin_recipe();
            const LocalPluginPlan plan = make_local_plugin_plan(recipe, home, script);
            const LocalPluginResult result = install_plugin_locally(
                recipe, plan, runner, [this, gen, screen_tok](LocalPluginStage stage) {
                    screen_tok.defer("TimelapseInstallOverlay::local_stage", [this, gen, stage]() {
                        if (is_current(gen, Step::INSTALL_PLUGIN)) {
                            set_status(local_install_stage_text(stage));
                        }
                    });
                });
            install_running_.store(false);
            object_tok.defer("TimelapseInstallOverlay::local_done",
                             [this, gen, result]() { on_local_install_finished(gen, result); });
        });
    });
    if (!started) {
        install_running_.store(false);
        set_status(lv_tr("Could not add the timelapse plugin to Moonraker."));
        show_action_button(lv_tr("Retry"), [this]() { step_install_locally(); });
    }
}

void TimelapseInstallOverlay::on_local_install_finished(unsigned gen,
                                                        const LocalPluginResult& result) {
    join_finished_install_worker();
    if (is_current(gen, Step::INSTALL_PLUGIN) && !waiting_on_earlier_install_) {
        on_local_install_done(result);
        return;
    }
    if (wizard_active_ && waiting_on_earlier_install_ && current_step_ == Step::INSTALL_PLUGIN) {
        spdlog::info("[{}] The earlier install ended; starting this run's", get_name());
        step_install_locally();
        return;
    }
    spdlog::debug("[{}] Dropping the result of an earlier run's install", get_name());
}

void TimelapseInstallOverlay::join_finished_install_worker() {
    if (install_thread_.joinable() && !install_running_.load()) {
        install_thread_.join();
    }
}

void TimelapseInstallOverlay::stop_install_worker() {
    install_cancel_.store(true);
    if (install_thread_.joinable()) {
        install_thread_.join();
    }
    install_running_.store(false);
}

bool TimelapseInstallOverlay::is_current(unsigned gen, Step step) const {
    return wizard_active_ && gen == run_generation_ && current_step_ == step;
}

bool TimelapseInstallOverlay::show_wait_for_print(std::function<void()> resume) {
    if (!job_holds_now()) {
        return false;
    }
    spdlog::info("[{}] A job holds the machine; holding the wizard", get_name());
    set_status(lv_tr("Installing restarts Moonraker, so it waits until the print is "
                     "finished or cancelled."));
    show_card(Card::None);
    show_action_button(lv_tr("Check Again"), std::move(resume));
    return true;
}

void TimelapseInstallOverlay::on_local_install_done(const LocalPluginResult& result) {
    if (result.outcome == LocalPluginOutcome::Failed) {
        spdlog::warn("[{}] Local install failed at stage {}: {}", get_name(),
                     static_cast<int>(result.failed_stage), result.detail);
        std::string text = local_install_failure_text(result.failed_stage);
        if (!result.detail.empty()) {
            text += "\n" + result.detail;
        }
        set_status(text.c_str());
        show_action_button(lv_tr("Retry"), [this]() { step_install_locally(); });
        return;
    }
    ffmpeg_missing_ = result.outcome == LocalPluginOutcome::InstalledMissingDependency;
    spdlog::info("[{}] Plugin installed locally (ffmpeg {})", get_name(),
                 ffmpeg_missing_ ? "missing" : "present");
    step_configure_moonraker();
}

// ============================================================================
// RECHECK AFTER SSH INSTALL
// ============================================================================

void TimelapseInstallOverlay::recheck_after_install() {
    const unsigned gen = run_generation_;
    set_step(Step::CHECKING_PLUGIN);
    set_status(lv_tr("Checking for plugin..."));
    hide_action_button();
    show_card(Card::None);

    if (!api_)
        return;

    api_->timelapse().get_timelapse_settings(
        lifetime_.bg_cb("TimelapseInstallOverlay::recheck_present",
                        [this, gen](const TimelapseSettings& /*settings*/) {
                            if (!is_current(gen, Step::CHECKING_PLUGIN))
                                return;
                            spdlog::info("[{}] Plugin detected after recheck!", get_name());
                            set_status(lv_tr("Timelapse plugin is installed!"));
                            if (step_progress_) {
                                for (int i = 0; i < STEP_COUNT; i++) {
                                    ui_step_progress_set_completed(step_progress_, i);
                                }
                            }
                            show_action_button(lv_tr("Done"), []() { helix::nav::go_back(); });
                        }),
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::recheck_absent", [this, gen](const MoonrakerError& /*err*/) {
                if (!is_current(gen, Step::CHECKING_PLUGIN))
                    return;
                spdlog::info("[{}] Plugin still not responding, proceeding to configure",
                             get_name());
                step_configure_moonraker();
            }));
}

// ============================================================================
// STEP 4: CONFIGURE MOONRAKER
// ============================================================================

void TimelapseInstallOverlay::step_configure_moonraker() {
    set_step(Step::CONFIGURE_MOONRAKER);
    hide_action_button();
    show_card(Card::None);
    if (show_wait_for_print([this]() { step_configure_moonraker(); })) {
        return;
    }
    set_status(lv_tr("Configuring Moonraker..."));

    download_and_modify_config();
}

void TimelapseInstallOverlay::download_and_modify_config() {
    const unsigned gen = run_generation_;
    if (!api_) {
        set_status(lv_tr("Not connected to printer"));
        return;
    }

    // Download moonraker.conf — check for existing [timelapse] section (migration: leave it alone)
    api_->transfers().download_file(
        "config", "moonraker.conf",
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::moonraker_conf",
            [this, gen](const std::string& moonraker_content) {
                if (!is_current(gen, Step::CONFIGURE_MOONRAKER))
                    return;

                // If [timelapse] already exists in moonraker.conf, config is done
                if (has_timelapse_section(moonraker_content) ||
                    helix::MoonrakerConfigManager::has_section(moonraker_content, "timelapse")) {
                    spdlog::info("[{}] moonraker.conf already has [timelapse] section", get_name());
                    set_status(lv_tr("Configuration already present."));
                    step_restart_moonraker();
                    return;
                }
                download_helix_config(moonraker_content, true);
            }),
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::moonraker_conf_err", [this, gen](const MoonrakerError& err) {
                if (!is_current(gen, Step::CONFIGURE_MOONRAKER))
                    return;
                if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                    // moonraker.conf not found (e.g. mock mode) — proceed with empty content
                    spdlog::info("[{}] moonraker.conf not found, proceeding with empty",
                                 get_name());
                    download_helix_config("", false);
                    return;
                }
                spdlog::error("[{}] Failed to download config: {}", get_name(), err.message);
                set_status(lv_tr("Failed to download moonraker.conf.\nCheck printer connection."));
                show_action_button(lv_tr("Retry"), [this]() { download_and_modify_config(); });
            }));
}

void TimelapseInstallOverlay::download_helix_config(const std::string& moonraker_content,
                                                    bool moonraker_found) {
    const unsigned gen = run_generation_;
    if (!api_)
        return;

    // Our timelapse config goes in helixscreen.conf, which may not exist yet.
    api_->transfers().download_file(
        "config", "helixscreen.conf",
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::helix_conf",
            [this, gen, moonraker_content](const std::string& helix_content) {
                if (!is_current(gen, Step::CONFIGURE_MOONRAKER))
                    return;
                if (helix::MoonrakerConfigManager::has_section(helix_content, "timelapse")) {
                    spdlog::info("[{}] helixscreen.conf already has [timelapse] section",
                                 get_name());
                    set_status(lv_tr("Configuration already present."));
                    step_restart_moonraker();
                    return;
                }
                write_timelapse_config(helix_content, moonraker_content);
            }),
        lifetime_.bg_cb("TimelapseInstallOverlay::helix_conf_err", [this, gen, moonraker_content,
                                                                    moonraker_found](
                                                                       const MoonrakerError& err) {
            if (!is_current(gen, Step::CONFIGURE_MOONRAKER))
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                spdlog::info("[{}] helixscreen.conf not found, creating new", get_name());
                write_timelapse_config("", moonraker_content);
                return;
            }
            spdlog::error("[{}] Failed to download helixscreen.conf: {}", get_name(), err.message);
            const char* msg =
                moonraker_found
                    ? lv_tr("Failed to download helixscreen.conf.\nCheck printer connection.")
                    : lv_tr("Failed to read config.");
            set_status(msg);
            show_action_button(lv_tr("Retry"), [this]() { download_and_modify_config(); });
        }));
}

void TimelapseInstallOverlay::write_timelapse_config(const std::string& helix_content,
                                                     const std::string& moonraker_content) {
    const unsigned gen = run_generation_;
    // Add [timelapse] and [update_manager timelapse] sections to helixscreen.conf
    std::string new_helix = helix::MoonrakerConfigManager::add_section(
        helix_content, "timelapse", {}, "Timelapse - added by HelixScreen");
    new_helix = helix::MoonrakerConfigManager::add_section(
        new_helix, "update_manager timelapse",
        {{"type", "git_repo"},
         {"primary_branch", "main"},
         {"path", "~/moonraker-timelapse"},
         {"origin", "https://github.com/mainsail-crew/moonraker-timelapse.git"},
         {"managed_services", "klipper moonraker"}});

    // Upload helixscreen.conf
    api_->transfers().upload_file(
        "config", "helixscreen.conf", new_helix,
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::helix_uploaded",
            [this, gen, moonraker_content]() {
                if (!is_current(gen, Step::CONFIGURE_MOONRAKER))
                    return;
                spdlog::info("[{}] helixscreen.conf updated with timelapse config", get_name());

                // Ensure [include helixscreen.conf] is in moonraker.conf
                if (helix::MoonrakerConfigManager::has_include_line(moonraker_content)) {
                    spdlog::info("[{}] moonraker.conf already has include line", get_name());
                    set_status(lv_tr("Configuration added successfully."));
                    step_restart_moonraker();
                    return;
                }
                upload_moonraker_include(moonraker_content);
            }),
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::helix_upload_err", [this, gen](const MoonrakerError& err) {
                if (!is_current(gen, Step::CONFIGURE_MOONRAKER))
                    return;
                spdlog::error("[{}] Failed to upload helixscreen.conf: {}", get_name(),
                              err.message);
                set_status(lv_tr("Failed to update configuration.\nCheck printer connection."));
                show_action_button(lv_tr("Retry"), [this]() { download_and_modify_config(); });
            }));
}

void TimelapseInstallOverlay::upload_moonraker_include(const std::string& moonraker_content) {
    const unsigned gen = run_generation_;
    if (!api_)
        return;

    std::string updated_moonraker =
        helix::MoonrakerConfigManager::add_include_line(moonraker_content);
    api_->transfers().upload_file(
        "config", "moonraker.conf", updated_moonraker,
        lifetime_.bg_cb("TimelapseInstallOverlay::moonraker_uploaded",
                        [this, gen]() {
                            if (!is_current(gen, Step::CONFIGURE_MOONRAKER))
                                return;
                            spdlog::info("[{}] moonraker.conf updated with include line",
                                         get_name());
                            set_status(lv_tr("Configuration added successfully."));
                            step_restart_moonraker();
                        }),
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::moonraker_upload_err",
            [this, gen](const MoonrakerError& err) {
                if (!is_current(gen, Step::CONFIGURE_MOONRAKER))
                    return;
                spdlog::error("[{}] Failed to upload moonraker.conf: {}", get_name(), err.message);
                set_status(lv_tr("Failed to update moonraker.conf.\nCheck printer connection."));
                show_action_button(lv_tr("Retry"), [this]() { download_and_modify_config(); });
            }));
}

bool TimelapseInstallOverlay::has_timelapse_section(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos)
            continue;
        std::string trimmed = line.substr(start);
        // Trim trailing whitespace and carriage returns (Windows line endings)
        size_t end = trimmed.find_last_not_of(" \t\r");
        if (end != std::string::npos)
            trimmed = trimmed.substr(0, end + 1);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        if (trimmed == "[timelapse]")
            return true;
    }
    return false;
}

std::string TimelapseInstallOverlay::append_timelapse_config(const std::string& content) {
    std::string result = content;
    // Ensure trailing newline
    if (!result.empty() && result.back() != '\n') {
        result += '\n';
    }
    result += "\n";
    result += "# Timelapse - added by HelixScreen\n";
    result += "[timelapse]\n";
    result += "\n";
    result += "[update_manager timelapse]\n";
    result += "type: git_repo\n";
    result += "primary_branch: main\n";
    result += "path: ~/moonraker-timelapse\n";
    result += "origin: https://github.com/mainsail-crew/moonraker-timelapse.git\n";
    result += "managed_services: klipper moonraker\n";
    return result;
}

// ============================================================================
// STEP 5: RESTART MOONRAKER
// ============================================================================

void TimelapseInstallOverlay::step_restart_moonraker() {
    const unsigned gen = run_generation_;
    set_step(Step::RESTART_MOONRAKER);
    hide_action_button();
    if (show_wait_for_print([this]() { step_restart_moonraker(); })) {
        return;
    }
    set_status(lv_tr("Restarting Moonraker..."));

    if (!api_)
        return;

    // Suppress recovery modal during intentional restart
    EmergencyStopOverlay::instance().suppress_recovery_dialog(recovery_suppression::LONG);

    api_->restart_moonraker(
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::restart_sent",
            [this, gen]() {
                if (!is_current(gen, Step::RESTART_MOONRAKER))
                    return;
                spdlog::info("[{}] Moonraker restart initiated", get_name());
                set_status(lv_tr("Moonraker restarting...\nWaiting for reconnection..."));

                // Wait 8 seconds then try to verify; the run's generation rides in the
                // timer, so a run reopened since does not jump to its verify step.
                lv_timer_create(
                    [](lv_timer_t* timer) {
                        const auto timer_gen = static_cast<unsigned>(
                            reinterpret_cast<uintptr_t>(lv_timer_get_user_data(timer)));
                        lv_timer_delete(timer);
                        auto* overlay = helix::lazy_global_if_exists<TimelapseInstallOverlay>();
                        if (!overlay || !overlay->is_current(timer_gen, Step::RESTART_MOONRAKER))
                            return;
                        overlay->step_verify();
                    },
                    8000, reinterpret_cast<void*>(static_cast<uintptr_t>(gen)));
            }),
        lifetime_.bg_cb(
            "TimelapseInstallOverlay::restart_err", [this, gen](const MoonrakerError& err) {
                if (!is_current(gen, Step::RESTART_MOONRAKER))
                    return;
                spdlog::error("[{}] Moonraker restart failed: {}", get_name(), err.message);
                set_status(lv_tr("Failed to restart Moonraker."));
                show_action_button(lv_tr("Retry"), [this]() { step_restart_moonraker(); });
            }));
}

// ============================================================================
// STEP 6: VERIFY
// ============================================================================

void TimelapseInstallOverlay::step_verify() {
    const unsigned gen = run_generation_;
    set_step(Step::VERIFY);
    set_status(lv_tr("Verifying timelapse plugin..."));

    if (!api_)
        return;

    api_->timelapse().get_timelapse_settings(
        lifetime_.bg_cb("TimelapseInstallOverlay::verified",
                        [this, gen](const TimelapseSettings& /*settings*/) {
                            if (!is_current(gen, Step::VERIFY))
                                return;
                            spdlog::info("[{}] Timelapse plugin verified!", get_name());
                            if (ffmpeg_missing_) {
                                set_status(lv_tr("The timelapse plugin is installed. Making "
                                                 "videos also needs ffmpeg, which is not "
                                                 "installed on this printer."));
                                show_card(Card::Ffmpeg);
                            } else {
                                set_status(lv_tr("Timelapse plugin installed successfully!"));
                            }
                            if (step_progress_) {
                                ui_step_progress_set_completed(step_progress_,
                                                               static_cast<int>(Step::VERIFY));
                            }
                            // Update capability state so UI reflects timelapse availability
                            get_printer_state().set_timelapse_available(true);
                            show_action_button(lv_tr("Done"), []() { helix::nav::go_back(); });
                            ToastManager::instance().show(
                                ToastSeverity::SUCCESS, lv_tr("Timelapse plugin installed!"), 3000);
                            setup_timelapse_macros(gen);
                        }),
        lifetime_.bg_cb("TimelapseInstallOverlay::verify_err", [this, gen](const MoonrakerError&) {
            if (!is_current(gen, Step::VERIFY))
                return;
            spdlog::warn("[{}] Verification failed - plugin not responding", get_name());
            set_status(
                lv_tr("Plugin not responding after restart.\nIt may need more time to load."));
            show_action_button(lv_tr("Check Again"), [this]() { step_verify(); });
        }));
}

// ============================================================================
// KLIPPER MACROS
// ============================================================================

void TimelapseInstallOverlay::setup_timelapse_macros(unsigned gen) {
    const auto& discovery = get_printer_state().get_discovery();
    const MacroInstallStatus helix_status = MacroManager::evaluate_status(discovery);
    const bool helix_loaded = helix_status == MacroInstallStatus::INSTALLED ||
                              helix_status == MacroInstallStatus::OUTDATED;
    const bool has_frame_macro = discovery.has_macro(kTimelapseFrameMacro);
    const TimelapseMacroSetup setup = decide_timelapse_macro_setup(has_frame_macro, helix_loaded);
    spdlog::info("[{}] Timelapse macros: {}={} helix_macros={} -> {}", get_name(),
                 kTimelapseFrameMacro, has_frame_macro, helix_loaded, static_cast<int>(setup));

    switch (setup) {
    case TimelapseMacroSetup::Loaded:
        show_macro_card(MacroCard::None);
        return;
    case TimelapseMacroSetup::ShowIncludeLine:
        show_macro_card(MacroCard::IncludeLine);
        return;
    case TimelapseMacroSetup::IncludeFromHelixMacros:
        break;
    }

    if (!api_) {
        show_macro_card(MacroCard::IncludeLine);
        return;
    }
    macro_manager_ = std::make_unique<MacroManager>(*api_, discovery);
    // MacroManager lands both callbacks on the main thread.
    macro_manager_->include_timelapse_macros(
        [this, gen]() {
            if (!is_current(gen, Step::VERIFY))
                return;
            spdlog::info("[{}] {} includes the timelapse macros; they load at the next Klipper "
                         "restart",
                         get_name(), kHelixMacrosFilename);
            show_macro_card(MacroCard::RestartKlipper);
        },
        [this, gen](const MoonrakerError& err) {
            if (!is_current(gen, Step::VERIFY))
                return;
            spdlog::warn("[{}] Could not add the timelapse macros to {} ({}); showing the line "
                         "to add",
                         get_name(), kHelixMacrosFilename, err.message);
            show_macro_card(MacroCard::IncludeLine);
        });
}

void TimelapseInstallOverlay::restart_klipper_for_macros() {
    // The button greys while a job holds the machine (moves_machine); this is the backstop.
    if (job_holds_now()) {
        ToastManager::instance().show(
            ToastSeverity::INFO, lv_tr("A print is in progress - restart still pending"), 3000);
        return;
    }
    if (!macro_manager_) {
        return;
    }
    spdlog::info("[{}] Restarting Klipper to load the timelapse macros", get_name());
    show_macro_card(MacroCard::None);
    // The restart passes through SHUTDOWN on its way back to READY.
    EmergencyStopOverlay::instance().suppress_recovery_dialog(recovery_suppression::NORMAL);
    const unsigned gen = run_generation_;
    // The same restart the helper macros activate with; both callbacks land on main.
    macro_manager_->request_restart(
        [this, gen]() {
            if (!is_current(gen, Step::VERIFY))
                return;
            set_status(
                lv_tr("Klipper is restarting. Timelapse frames are captured from the next print."));
        },
        [this, gen](const MoonrakerError& err) {
            spdlog::error("[{}] Klipper restart request failed: {}", get_name(), err.message);
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to restart Klipper"),
                                          4000);
            if (is_current(gen, Step::VERIFY))
                show_macro_card(MacroCard::RestartKlipper);
        });
}

// ============================================================================
// PUBLIC OPENER (called from advanced panel)
// ============================================================================

void open_timelapse_install() {
    get_global_timelapse_install().show(lv_display_get_screen_active(nullptr));
}
