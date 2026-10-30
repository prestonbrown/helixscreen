// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_settings.h"

#include "ui_callback_helpers.h"
#include "ui_debug_bundle_modal.h"
#include "ui_info_qr_modal.h"
#include "ui_modal.h"
#if HELIX_HAS_PLUGINS
#include "plugins_overlay.h"
#endif
#include "ui_nav.h"
#include "ui_panel_memory_stats.h"
#include "ui_printer_manager_overlay.h"
#include "ui_settings_appearance.h"
#include "ui_settings_connection.h"
#include "ui_settings_display.h"
#include "ui_settings_hardware.h"
#include "ui_settings_hardware_health.h"
#include "ui_settings_help.h"
#include "ui_settings_language_time.h"
#include "ui_settings_printing.h"
#include "ui_settings_safety.h"
#include "ui_settings_sound.h"
#include "ui_settings_system.h"
#include "ui_settings_touch.h"
#include "ui_settings_updates.h"
#include "ui_severity_card.h"
#include "ui_snake_game.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_wizard_hardware_selector.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "ethernet_manager.h"
#include "filament_sensor_manager.h"
#include "format_utils.h"
#include "hardware_validator.h"
#include "helix_version.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "input_settings_manager.h"
#include "lvgl.h"
#include "moonraker_manager.h"
#include "platform_info.h"
#include "printer_hardware.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "settings_root_status.h"
#include "sound_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "wifi_manager.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

using namespace helix;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

SettingsPanel::SettingsPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    spdlog::trace("[{}] Constructor", get_name());
}

SettingsPanel::~SettingsPanel() {
    // Applying [L041]: deinit_subjects() as first line in destructor
    deinit_subjects();

    // Note: Klipper/Moonraker/OS version observers bound declaratively in XML
    // Note: Don't log here - spdlog may be destroyed during static destruction
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void SettingsPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize visibility subjects (controls which settings are shown)
    // Note: show_beta_features subject is initialized globally in app_globals.cpp

    // Platform visibility subjects — hidden on Android where OS manages these
    bool on_android = helix::is_android_platform();

    // Task 13: un-hidden on ESP32 now that WifiBackend over esp_wifi
    // (wifi_backend_esp.cpp) backs wifi_manager.h for real — Ethernet stays
    // out of scope (ethernet_manager.h still resolves to the
    // helixapp_platform_stubs.cpp seam; no ESP32 wired-network HIL exists).
    bool show_network_settings = !on_android;
    UI_MANAGED_SUBJECT_INT(show_network_settings_subject_, show_network_settings ? 1 : 0,
                           "show_network_settings", subjects_);

    // Update checker runs on all platforms — on Android, "Install Update"
    // opens the store or release page the APK came from instead of self-updating.
    //
    // Checking and installing are gated SEPARATELY. Only a firmware opt-out hides
    // the "Check for Updates" row, because checking is a network fetch that a
    // read-only install tree cannot fail; an install tree we cannot write hides
    // only "Install Update" and adds a notice saying so. Gating both on one
    // predicate is what made a false negative unrecoverable — the whole section
    // disappeared, so nothing could tell the user an update existed or what to do
    // about it.
    bool externally_managed = updates_externally_managed();
    bool install_suppressed = update_install_suppressed();
    UI_MANAGED_SUBJECT_INT(show_update_settings_subject_, update_checks_suppressed() ? 0 : 1,
                           "show_update_settings", subjects_);

    UI_MANAGED_SUBJECT_INT(updates_firmware_managed_subject_, externally_managed ? 1 : 0,
                           "updates_firmware_managed", subjects_);

    UI_MANAGED_SUBJECT_INT(updates_unavailable_subject_,
                           (install_suppressed && !externally_managed) ? 1 : 0,
                           "updates_unavailable", subjects_);

    // 0 until Application::init_plugins loads at least one plugin, so the row
    // stays hidden until there is something to show
    UI_MANAGED_SUBJECT_INT(plugins_available_subject_, 0, "settings_plugins_available", subjects_);

    // Live status line under each stateful root row; refresh_status_lines()
    // fills these in, first from setup() and then on every return to the root.
    UI_MANAGED_SUBJECT_STRING(settings_status_display_subject_, settings_status_display_buf_, "",
                              "settings_status_display", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_appearance_subject_, settings_status_appearance_buf_,
                              "", "settings_status_appearance", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_sound_subject_, settings_status_sound_buf_, "",
                              "settings_status_sound", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_devices_subject_, settings_status_devices_buf_, "",
                              "settings_status_devices", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_connection_subject_, settings_status_connection_buf_,
                              "", "settings_status_connection", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_language_time_subject_,
                              settings_status_language_time_buf_, "",
                              "settings_status_language_time", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_updates_subject_, settings_status_updates_buf_, "",
                              "settings_status_updates", subjects_);

    // Note: BedMeshPanel subjects are initialized in main.cpp during startup

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SettingsPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[{}] Deinitializing subjects", get_name());

    // Expire any in-flight Ethernet probe first: get_info_async()'s deferred
    // write targets settings_status_connection_subject_ below, which
    // subjects_.deinit_all() is about to tear down.
    lifetime_.invalidate();

    // Deinit all subjects via SubjectManager (handles 7 string subjects)
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

void SettingsPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::debug("[{}] Setup complete", get_name());
}

void SettingsPanel::on_activate() {
    PanelBase::on_activate();
    refresh_status_lines();
}

// ============================================================================
// SETUP HELPERS
// ============================================================================

namespace {
// A subject owned by an overlay not yet created (e.g. update_new_version,
// registered by the Updates overlay) may not exist; the caller's fallback
// stands in for it, matching the formatter's neutral input.
int status_int_subject(const char* name, int fallback) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    return s ? lv_subject_get_int(s) : fallback;
}
std::string status_string_subject(const char* name, const char* fallback) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    return s ? std::string(lv_subject_get_string(s)) : std::string(fallback);
}
} // namespace

void SettingsPanel::refresh_status_lines() {
    using namespace helix::settings::status;

    // Formatted here rather than once at init, so it is in the language of the
    // latest return to the settings root.
    lv_subject_copy_string(&settings_status_display_subject_,
                           display(status_int_subject("settings_brightness", 0),
                                   status_int_subject("settings_display_sleep", 0),
                                   status_int_subject("settings_has_dimming", 0) != 0)
                               .c_str());

    lv_subject_copy_string(&settings_status_appearance_subject_,
                           appearance(status_int_subject("settings_dark_mode", 0) != 0,
                                      theme_manager_get_active_theme().name)
                               .c_str());

    lv_subject_copy_string(&settings_status_sound_subject_,
                           sound(status_int_subject("settings_sounds_enabled", 0) != 0,
                                 status_int_subject("settings_volume", 0))
                               .c_str());

    lv_subject_copy_string(&settings_status_devices_subject_,
                           devices(lv_subject_get_int(get_printer_state()
                                                          .hardware_validation_state()
                                                          .get_hardware_status_level_subject()))
                               .c_str());

    if (helix::is_android_platform()) {
        // Android manages Wi-Fi and Ethernet itself — both backends compile to
        // nullptr there (wifi_backend.cpp, ethernet_backend.cpp under
        // __ANDROID__) — so probing either just logs errors/warnings for
        // nothing. Show the printer host instead.
        lv_subject_copy_string(
            &settings_status_connection_subject_,
            helix::settings::ConnectionSettingsOverlay::printer_host_display().c_str());
    } else {
        // Both link probes block (a wpa_supplicant control round trip; sysfs
        // scans or a netd socket round trip for Ethernet), so neither runs on
        // this thread. Show the last resolved states now, then refresh each as
        // its probe lands.
        render_connection_status();

        if (!ethernet_manager_) {
            ethernet_manager_ = std::make_unique<EthernetManager>();
        }
        const uint32_t seq = ++connection_probe_seq_;
        auto tok = lifetime_.token();
        ethernet_manager_->get_info_async([this, tok, seq](const EthernetInfo& info) {
            const bool ethernet_up = info.connected;
            tok.defer("SettingsPanel::apply_ethernet_status", [this, seq, ethernet_up]() {
                if (seq != connection_probe_seq_) {
                    return;
                }
                last_ethernet_up_ = ethernet_up;
                render_connection_status();
            });
        });
        get_wifi_manager()->get_status_async(
            tok, [this, seq](const WifiBackend::ConnectionStatus& status) {
                if (seq != connection_probe_seq_) {
                    return;
                }
                last_wifi_connected_ = status.connected;
                last_wifi_ssid_ = status.ssid;
                render_connection_status();
            });
    }

    lv_subject_copy_string(
        &settings_status_language_time_subject_,
        language_time(SystemSettingsManager::instance().get_language_display_name(),
                      status_int_subject("settings_time_format", 0))
            .c_str());

    lv_subject_copy_string(&settings_status_updates_subject_,
                           updates(status_int_subject("update_status", 0),
                                   status_string_subject("update_new_version", ""), helix_version(),
                                   lv_subject_get_int(&updates_firmware_managed_subject_) != 0)
                               .c_str());
}

void SettingsPanel::render_connection_status() {
    lv_subject_copy_string(&settings_status_connection_subject_,
                           helix::settings::status::connection(
                               last_ethernet_up_, last_wifi_connected_, last_wifi_ssid_)
                               .c_str());
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SettingsPanel::show_restart_prompt() {
    // Already showing
    if (restart_prompt_dialog_) {
        return;
    }

    restart_prompt_dialog_ = helix::ui::modal_show("restart_prompt_dialog");
    if (restart_prompt_dialog_) {
        spdlog::debug("[{}] Restart prompt dialog shown via Modal system", get_name());
        // Clear pending flag so we don't show again until next change
        InputSettingsManager::instance().clear_restart_pending();
    }
}

void SettingsPanel::handle_hardware_health_clicked() {
    spdlog::debug("[{}] Hardware Health clicked - delegating to HardwareHealthOverlay", get_name());

    auto& overlay = helix::settings::get_hardware_health_overlay();
    overlay.set_printer_state(&printer_state_);
    overlay.show(parent_screen_);
}

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

SettingsPanel& get_global_settings_panel() {
    return helix::lazy_global<SettingsPanel>("SettingsPanel", get_printer_state(), nullptr);
}

namespace {

using helix::ui::event_checked;
using helix::ui::event_selected;

lv_obj_t* settings_screen() {
    return get_global_settings_panel().parent_screen();
}

// A root or sub-page row that opens one overlay on the settings screen.
template <auto Getter> auto nav_row() {
    return [](lv_event_t*) { Getter().show(settings_screen()); };
}

} // namespace

// Registered BEFORE settings_panel.xml per [L013]. The one table for every callback the
// settings root names; each sub-page registers its own from OverlayBase::register_callbacks().
void register_settings_panel_callbacks() {
    spdlog::trace("[SettingsPanel] Registering XML callbacks for settings_panel.xml");

    using namespace helix::settings;
    register_xml_callbacks({
        // Root rows
        {"on_display_clicked", nav_row<get_display_settings_overlay>()},
        {"on_appearance_clicked", nav_row<get_appearance_settings_overlay>()},
        {"on_sound_clicked", nav_row<get_sound_settings_overlay>()},
        {"on_language_time_clicked", nav_row<get_language_time_settings_overlay>()},
        {"on_printer_clicked", nav_row<get_printer_manager_overlay>()},
        {"on_printing_clicked", nav_row<get_printing_settings_overlay>()},
        {"on_devices_clicked", nav_row<get_hardware_settings_overlay>()},
        {"on_safety_clicked", nav_row<get_safety_settings_overlay>()},
        {"on_system_clicked", nav_row<get_system_settings_overlay>()},
        {"on_help_clicked", nav_row<get_help_settings_overlay>()},
        {"on_touch_input_clicked", nav_row<get_touch_settings_overlay>()},
        {"on_connection_clicked", nav_row<get_connection_settings_overlay>()},
        {"on_updates_clicked", nav_row<get_updates_settings_overlay>()},
#if HELIX_HAS_PLUGINS
        {"on_plugins_clicked", nav_row<helix::plugin::get_plugins_overlay>()},
#else
        // No plugin host: the row stays hidden (settings_plugins_available = 0) and
        // this only lets the XML resolve its callback.
        {"on_plugins_clicked", [](lv_event_t*) {}},
#endif

        // Devices page
        {"on_hardware_health_clicked",
         [](lv_event_t*) { get_global_settings_panel().handle_hardware_health_clicked(); }},

        // Restart prompt and the shared header back button
        {"on_restart_later_clicked",
         [](lv_event_t*) {
             auto& panel = get_global_settings_panel();
             if (panel.restart_prompt_dialog_) {
                 helix::ui::modal_hide(panel.restart_prompt_dialog_);
                 panel.restart_prompt_dialog_ = nullptr;
             }
         }},
        {"on_restart_now_clicked", [](lv_event_t*) { app_request_restart_service(); }},
        {"on_header_back_clicked", [](lv_event_t*) { helix::nav::go_back(); }},
    });
}
