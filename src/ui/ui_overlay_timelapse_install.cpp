// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_timelapse_install.h"

#include "ui_button.h"
#include "ui_emergency_stop.h"
#include "ui_nav_manager.h"
#include "ui_step_progress.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_client.h"
#include "moonraker_config_manager.h"
#include "moonraker_types.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <sstream>

using namespace helix;

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<TimelapseInstallOverlay> g_timelapse_install;
static lv_obj_t* g_timelapse_install_panel = nullptr;

TimelapseInstallOverlay& get_global_timelapse_install() {
    if (!g_timelapse_install) {
        spdlog::error("[Timelapse Install] get_global called before init!");
        throw std::runtime_error("TimelapseInstallOverlay not initialized");
    }
    return *g_timelapse_install;
}

void init_global_timelapse_install(IMoonrakerAPI* api) {
    if (g_timelapse_install) {
        spdlog::warn("[Timelapse Install] Already initialized, skipping");
        return;
    }
    g_timelapse_install = std::make_unique<TimelapseInstallOverlay>(api);
    StaticPanelRegistry::instance().register_destroy("TimelapseInstallOverlay", []() {
        if (g_timelapse_install_panel) {
            NavigationManager::instance().unregister_overlay_instance(g_timelapse_install_panel);
        }
        g_timelapse_install_panel = nullptr;
        g_timelapse_install.reset();
    });
    spdlog::trace("[Timelapse Install] Initialized");
}

// ============================================================================
// ROW CLICK CALLBACK (opens overlay from Advanced panel)
// ============================================================================

/// Opens the install overlay; called from advanced panel timelapse setup row
static void open_timelapse_install_overlay() {
    if (!g_timelapse_install) {
        spdlog::error("[Timelapse Install] Global instance not initialized!");
        return;
    }

    // Lazy-create the panel
    if (!g_timelapse_install_panel) {
        spdlog::debug("[Timelapse Install] Creating install overlay panel...");
        g_timelapse_install_panel =
            g_timelapse_install->create(lv_display_get_screen_active(nullptr));

        if (!g_timelapse_install_panel) {
            spdlog::error("[Timelapse Install] Failed to create timelapse_install_overlay");
            return;
        }
        spdlog::debug("[Timelapse Install] Panel created");
    }

    // Re-register before every push: switch_to_panel_impl() clears the
    // non-persistent overlay_instances_ map on navbar switches, so a cached
    // panel re-opened after a navbar tap would otherwise be pushed unregistered
    // (no on_deactivate on dismiss, leaving the wizard's async wifi-scan
    // callbacks ungated by wizard_active_). Idempotent (keyed by widget).
    NavigationManager::instance().register_overlay_instance(g_timelapse_install_panel,
                                                            g_timelapse_install.get());
    NavigationManager::instance().push_overlay(g_timelapse_install_panel);
}

// ============================================================================
// CONSTRUCTOR & LIFECYCLE
// ============================================================================

TimelapseInstallOverlay::TimelapseInstallOverlay(IMoonrakerAPI* api) : api_(api) {}

void TimelapseInstallOverlay::init_subjects() {
    lv_xml_register_event_cb(nullptr, "on_timelapse_install_action", on_action_clicked);
    spdlog::trace("[{}] Event callbacks registered", get_name());
}

lv_obj_t* TimelapseInstallOverlay::create(lv_obj_t* parent) {
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, get_xml_component_name(), nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find key widgets by name
    status_label_ = lv_obj_find_by_name(overlay_root_, "status_text");
    action_btn_ = lv_obj_find_by_name(overlay_root_, "action_button");
    ssh_container_ = lv_obj_find_by_name(overlay_root_, "ssh_instructions_container");

    // action_btn_ is a ui_button — use ui_button_set_text() to update its label

    // Create step progress widget programmatically (dynamic content)
    lv_obj_t* step_container = lv_obj_find_by_name(overlay_root_, "step_container");
    if (step_container) {
        ui_step_t steps[] = {{lv_tr("Checking webcam"), StepState::Pending},
                             {lv_tr("Checking plugin"), StepState::Pending},
                             {lv_tr("Install plugin"), StepState::Pending},
                             {lv_tr("Configure Moonraker"), StepState::Pending},
                             {lv_tr("Restart Moonraker"), StepState::Pending},
                             {lv_tr("Verify"), StepState::Pending}};
        step_progress_ = ui_step_progress_create(step_container, steps, STEP_COUNT, false, nullptr);
    }

    // Initially hide interactive elements
    hide_action_button();
    if (ssh_container_)
        lv_obj_add_flag(ssh_container_, LV_OBJ_FLAG_HIDDEN);

    spdlog::debug("[{}] create() - widgets found: status={} action={} ssh={} steps={}", get_name(),
                  status_label_ != nullptr, action_btn_ != nullptr, ssh_container_ != nullptr,
                  step_progress_ != nullptr);

    return overlay_root_;
}

void TimelapseInstallOverlay::on_activate() {
    OverlayBase::on_activate();
    spdlog::debug("[{}] Activated", get_name());
    start_wizard();
}

void TimelapseInstallOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
    spdlog::debug("[{}] Deactivated", get_name());
    wizard_active_ = false;
}

void TimelapseInstallOverlay::cleanup() {
    wizard_active_ = false;
    action_callback_ = nullptr;
    OverlayBase::cleanup();
}

// ============================================================================
// WIZARD FLOW
// ============================================================================

void TimelapseInstallOverlay::start_wizard() {
    wizard_active_ = true;
    action_callback_ = nullptr;

    // Reset UI state
    if (ssh_container_)
        lv_obj_add_flag(ssh_container_, LV_OBJ_FLAG_HIDDEN);
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
    if (status_label_) {
        lv_label_set_text(status_label_, text);
    }
}

void TimelapseInstallOverlay::show_action_button(const char* label,
                                                 std::function<void()> callback) {
    action_callback_ = std::move(callback);
    if (action_btn_) {
        lv_obj_remove_flag(action_btn_, LV_OBJ_FLAG_HIDDEN);
        ui_button_set_text(action_btn_, label);
    }
}

void TimelapseInstallOverlay::hide_action_button() {
    action_callback_ = nullptr;
    if (action_btn_) {
        lv_obj_add_flag(action_btn_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// STEP 1: CHECK WEBCAM
// ============================================================================

void TimelapseInstallOverlay::step_check_webcam() {
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

    auto tok = lifetime_.token();
    api_->timelapse().get_webcam_list(
        [this, tok](const std::vector<WebcamInfo>& webcams) {
            if (tok.expired() || !wizard_active_)
                return;
            bool empty = webcams.empty();
            size_t count = webcams.size();
            tok.defer([this, empty, count]() {
                if (!wizard_active_)
                    return;
                if (empty) {
                    set_status(lv_tr("No webcam detected.\nA webcam is required for timelapse."));
                    show_action_button(lv_tr("Check Again"), [this]() { step_check_webcam(); });
                    return;
                }
                spdlog::info("[{}] Found {} webcam(s)", get_name(), count);
                step_check_plugin();
            });
        },
        [this, tok](const MoonrakerError& err) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::warn("[{}] Webcam check failed: {}", get_name(), err.message);
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                set_status(lv_tr("Could not check webcam status.\nCheck printer connection."));
                show_action_button(lv_tr("Check Again"), [this]() { step_check_webcam(); });
            });
        });
}

// ============================================================================
// STEP 2: CHECK PLUGIN
// ============================================================================

void TimelapseInstallOverlay::step_check_plugin() {
    set_step(Step::CHECKING_PLUGIN);
    set_status(lv_tr("Checking timelapse plugin..."));

    if (!api_)
        return;

    auto tok = lifetime_.token();
    api_->timelapse().get_timelapse_settings(
        [this, tok](const TimelapseSettings& /*settings*/) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::info("[{}] Timelapse plugin already installed", get_name());
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                set_status(lv_tr("Timelapse plugin is already installed!"));
                if (step_progress_) {
                    for (int i = 0; i < STEP_COUNT; i++) {
                        ui_step_progress_set_completed(step_progress_, i);
                    }
                }
                show_action_button(lv_tr("Close"),
                                   []() { NavigationManager::instance().go_back(); });
            });
        },
        [this, tok](const MoonrakerError& /*err*/) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::info("[{}] Plugin not detected, showing install instructions", get_name());
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                step_show_install_instructions();
            });
        });
}

// ============================================================================
// STEP 3: SHOW SSH INSTALL INSTRUCTIONS
// ============================================================================

void TimelapseInstallOverlay::step_show_install_instructions() {
    set_step(Step::INSTALL_PLUGIN);
    set_status(lv_tr("Install the timelapse plugin via SSH,\nthen tap \"Check Again\"."));

    // Show SSH instructions container
    if (ssh_container_) {
        lv_obj_remove_flag(ssh_container_, LV_OBJ_FLAG_HIDDEN);
    }

    show_action_button(lv_tr("Check Again"), [this]() { recheck_after_install(); });
}

// ============================================================================
// RECHECK AFTER SSH INSTALL
// ============================================================================

void TimelapseInstallOverlay::recheck_after_install() {
    set_step(Step::CHECKING_PLUGIN);
    set_status(lv_tr("Checking for plugin..."));
    hide_action_button();
    if (ssh_container_)
        lv_obj_add_flag(ssh_container_, LV_OBJ_FLAG_HIDDEN);

    if (!api_)
        return;

    auto tok = lifetime_.token();
    api_->timelapse().get_timelapse_settings(
        [this, tok](const TimelapseSettings& /*settings*/) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::info("[{}] Plugin detected after recheck!", get_name());
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                set_status(lv_tr("Timelapse plugin is installed!"));
                if (step_progress_) {
                    for (int i = 0; i < STEP_COUNT; i++) {
                        ui_step_progress_set_completed(step_progress_, i);
                    }
                }
                show_action_button(lv_tr("Done"),
                                   []() { NavigationManager::instance().go_back(); });
            });
        },
        [this, tok](const MoonrakerError& /*err*/) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::info("[{}] Plugin still not responding, proceeding to configure", get_name());
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                step_configure_moonraker();
            });
        });
}

// ============================================================================
// STEP 4: CONFIGURE MOONRAKER
// ============================================================================

void TimelapseInstallOverlay::step_configure_moonraker() {
    set_step(Step::CONFIGURE_MOONRAKER);
    set_status(lv_tr("Configuring Moonraker..."));
    hide_action_button();
    if (ssh_container_)
        lv_obj_add_flag(ssh_container_, LV_OBJ_FLAG_HIDDEN);

    download_and_modify_config();
}

void TimelapseInstallOverlay::download_and_modify_config() {
    if (!api_) {
        set_status(lv_tr("Not connected to printer"));
        return;
    }

    auto tok = lifetime_.token();

    // Download moonraker.conf — check for existing [timelapse] section (migration: leave it alone)
    api_->transfers().download_file(
        "config", "moonraker.conf",
        [this, tok](const std::string& moonraker_content) {
            if (tok.expired() || !wizard_active_)
                return;

            // If [timelapse] already exists in moonraker.conf, config is done
            if (has_timelapse_section(moonraker_content) ||
                helix::MoonrakerConfigManager::has_section(moonraker_content, "timelapse")) {
                spdlog::info("[{}] moonraker.conf already has [timelapse] section", get_name());
                tok.defer([this]() {
                    if (!wizard_active_)
                        return;
                    set_status(lv_tr("Configuration already present."));
                    step_restart_moonraker();
                });
                return;
            }

            // Download helixscreen.conf to add our timelapse config there
            api_->transfers().download_file(
                "config", "helixscreen.conf",
                [this, tok, moonraker_content](const std::string& helix_content) {
                    // Check if helixscreen.conf already has [timelapse] (pure
                    // function on local strings — safe on bg thread)
                    if (helix::MoonrakerConfigManager::has_section(helix_content, "timelapse")) {
                        spdlog::info("[{}] helixscreen.conf already has [timelapse] section",
                                     get_name());
                        tok.defer("TimelapseInstallOverlay::already_present", [this]() {
                            if (!wizard_active_)
                                return;
                            set_status(lv_tr("Configuration already present."));
                            step_restart_moonraker();
                        });
                        return;
                    }
                    tok.defer(
                        "TimelapseInstallOverlay::write_helix_with_moonraker",
                        [this, helix = helix_content, moonraker = moonraker_content]() mutable {
                            if (!wizard_active_)
                                return;
                            write_timelapse_config(helix, moonraker);
                        });
                },
                [this, tok, moonraker_content](const MoonrakerError& err) {
                    if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                        // helixscreen.conf doesn't exist yet — start fresh
                        spdlog::info("[{}] helixscreen.conf not found, creating new", get_name());
                        tok.defer("TimelapseInstallOverlay::write_fresh_helix",
                                  [this, moonraker = moonraker_content]() mutable {
                                      if (!wizard_active_)
                                          return;
                                      write_timelapse_config("", moonraker);
                                  });
                    } else {
                        spdlog::error("[{}] Failed to download helixscreen.conf: {}", get_name(),
                                      err.message);
                        tok.defer("TimelapseInstallOverlay::helix_download_err", [this]() {
                            if (!wizard_active_)
                                return;
                            set_status(lv_tr(
                                "Failed to download helixscreen.conf.\nCheck printer connection."));
                            show_action_button(lv_tr("Retry"),
                                               [this]() { download_and_modify_config(); });
                        });
                    }
                });
        },
        [this, tok](const MoonrakerError& err) {
            if (tok.expired() || !wizard_active_)
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                // moonraker.conf not found (e.g. mock mode) — proceed with empty content
                spdlog::info("[{}] moonraker.conf not found, proceeding with empty", get_name());
                // Download helixscreen.conf (may also not exist)
                api_->transfers().download_file(
                    "config", "helixscreen.conf",
                    [this, tok](const std::string& helix_content) {
                        tok.defer("TimelapseInstallOverlay::write_helix_no_moonraker",
                                  [this, helix = helix_content]() mutable {
                                      if (!wizard_active_)
                                          return;
                                      write_timelapse_config(helix, "");
                                  });
                    },
                    [this, tok](const MoonrakerError& err2) {
                        if (err2.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                            tok.defer("TimelapseInstallOverlay::write_empty", [this]() {
                                if (!wizard_active_)
                                    return;
                                write_timelapse_config("", "");
                            });
                        } else {
                            tok.defer("TimelapseInstallOverlay::read_config_err", [this]() {
                                if (!wizard_active_)
                                    return;
                                set_status(lv_tr("Failed to read config."));
                                show_action_button(lv_tr("Retry"),
                                                   [this]() { download_and_modify_config(); });
                            });
                        }
                    });
            } else {
                spdlog::error("[{}] Failed to download config: {}", get_name(), err.message);
                tok.defer([this]() {
                    if (!wizard_active_)
                        return;
                    set_status(
                        lv_tr("Failed to download moonraker.conf.\nCheck printer connection."));
                    show_action_button(lv_tr("Retry"), [this]() { download_and_modify_config(); });
                });
            }
        });
}

void TimelapseInstallOverlay::write_timelapse_config(const std::string& helix_content,
                                                     const std::string& moonraker_content) {
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

    auto tok = lifetime_.token();

    // Upload helixscreen.conf
    api_->transfers().upload_file(
        "config", "helixscreen.conf", new_helix,
        [this, tok, moonraker_content]() {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::info("[{}] helixscreen.conf updated with timelapse config", get_name());

            // Ensure [include helixscreen.conf] is in moonraker.conf
            if (helix::MoonrakerConfigManager::has_include_line(moonraker_content)) {
                spdlog::info("[{}] moonraker.conf already has include line", get_name());
                tok.defer([this]() {
                    if (!wizard_active_)
                        return;
                    set_status(lv_tr("Configuration added successfully."));
                    step_restart_moonraker();
                });
                return;
            }

            // Add include line and upload moonraker.conf
            std::string updated_moonraker =
                helix::MoonrakerConfigManager::add_include_line(moonraker_content);
            api_->transfers().upload_file(
                "config", "moonraker.conf", updated_moonraker,
                [this, tok]() {
                    if (tok.expired() || !wizard_active_)
                        return;
                    spdlog::info("[{}] moonraker.conf updated with include line", get_name());
                    tok.defer([this]() {
                        if (!wizard_active_)
                            return;
                        set_status(lv_tr("Configuration added successfully."));
                        step_restart_moonraker();
                    });
                },
                [this, tok](const MoonrakerError& err) {
                    if (tok.expired() || !wizard_active_)
                        return;
                    spdlog::error("[{}] Failed to upload moonraker.conf: {}", get_name(),
                                  err.message);
                    tok.defer([this]() {
                        if (!wizard_active_)
                            return;
                        set_status(
                            lv_tr("Failed to update moonraker.conf.\nCheck printer connection."));
                        show_action_button(lv_tr("Retry"),
                                           [this]() { download_and_modify_config(); });
                    });
                });
        },
        [this, tok](const MoonrakerError& err) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::error("[{}] Failed to upload helixscreen.conf: {}", get_name(), err.message);
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                set_status(lv_tr("Failed to update configuration.\nCheck printer connection."));
                show_action_button(lv_tr("Retry"), [this]() { download_and_modify_config(); });
            });
        });
}

bool TimelapseInstallOverlay::has_timelapse_section(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos)
            continue;
        std::string trimmed = line.substr(start);
        // Trim trailing whitespace and carriage returns (Windows line endings)
        size_t end = trimmed.find_last_not_of(" \t\r");
        if (end != std::string::npos)
            trimmed = trimmed.substr(0, end + 1);
        // Skip comments
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
    set_step(Step::RESTART_MOONRAKER);
    set_status(lv_tr("Restarting Moonraker..."));
    hide_action_button();

    if (!api_)
        return;

    auto tok = lifetime_.token();

    // Suppress recovery modal during intentional restart
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);

    api_->restart_moonraker(
        [this, tok]() {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::info("[{}] Moonraker restart initiated", get_name());
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                set_status(lv_tr("Moonraker restarting...\nWaiting for reconnection..."));

                // Wait 8 seconds then try to verify
                lv_timer_create(
                    [](lv_timer_t* timer) {
                        lv_timer_delete(timer);
                        if (!g_timelapse_install || !g_timelapse_install->wizard_active_)
                            return;
                        g_timelapse_install->step_verify();
                    },
                    8000, nullptr);
            });
        },
        [this, tok](const MoonrakerError& err) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::error("[{}] Moonraker restart failed: {}", get_name(), err.message);
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                set_status(lv_tr("Failed to restart Moonraker."));
                show_action_button(lv_tr("Retry"), [this]() { step_restart_moonraker(); });
            });
        });
}

// ============================================================================
// STEP 6: VERIFY
// ============================================================================

void TimelapseInstallOverlay::step_verify() {
    set_step(Step::VERIFY);
    set_status(lv_tr("Verifying timelapse plugin..."));

    if (!api_)
        return;

    auto tok = lifetime_.token();
    api_->timelapse().get_timelapse_settings(
        [this, tok](const TimelapseSettings& /*settings*/) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::info("[{}] Timelapse plugin verified!", get_name());
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                set_status(lv_tr("Timelapse plugin installed successfully!"));
                if (step_progress_) {
                    ui_step_progress_set_completed(step_progress_, static_cast<int>(Step::VERIFY));
                }
                // Update capability state so UI reflects timelapse availability
                get_printer_state().set_timelapse_available(true);
                show_action_button(lv_tr("Done"),
                                   []() { NavigationManager::instance().go_back(); });
                ToastManager::instance().show(ToastSeverity::SUCCESS,
                                              lv_tr("Timelapse plugin installed!"), 3000);
            });
        },
        [this, tok](const MoonrakerError& /*err*/) {
            if (tok.expired() || !wizard_active_)
                return;
            spdlog::warn("[{}] Verification failed - plugin not responding", get_name());
            tok.defer([this]() {
                if (!wizard_active_)
                    return;
                set_status(
                    lv_tr("Plugin not responding after restart.\nIt may need more time to load."));
                show_action_button(lv_tr("Check Again"), [this]() { step_verify(); });
            });
        });
}

// ============================================================================
// EVENT CALLBACKS
// ============================================================================

void TimelapseInstallOverlay::on_action_clicked(lv_event_t* /*e*/) {
    if (!g_timelapse_install)
        return;
    if (g_timelapse_install->action_callback_) {
        g_timelapse_install->action_callback_();
    }
}

// ============================================================================
// PUBLIC OPENER (called from advanced panel)
// ============================================================================

void open_timelapse_install() {
    open_timelapse_install_overlay();
}
