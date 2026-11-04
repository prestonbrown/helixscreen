// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_base.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "subject_managed_panel.h" // For SubjectManager

#include <memory>
#include <string>
#include <vector>

class ChangeHostModal;
class EthernetManager; // NAMESPACE_OK: matches its own definition (ethernet_manager.h), global by
                       // design

/**
 * @file ui_panel_settings.h
 * @brief Settings panel - Scrolling list of app and printer settings
 *
 * A comprehensive settings panel with sections for Appearance, Printer,
 * Notifications, System, and About information.
 *
 * ## Key Features:
 * - Grouped root list (Screen, Printer, HelixScreen) navigating to sub-panel overlays
 * - Live one-line status under each stateful row, refreshed on return via on_activate()
 * - LED light control (via Moonraker)
 * - System info display (version, printer, Klipper)
 *
 * ## Architecture:
 * Uses SettingsManager for reactive data binding and persistence. Each domain
 * settings manager (Display, Audio, System, ...) owns its own subjects; this
 * panel only reads them to compose the root rows' status text.
 *
 * @see SettingsManager for data layer
 * @see PanelBase for base class documentation
 */
class SettingsPanel : public PanelBase {
  public:
    /**
     * @brief Construct SettingsPanel with injected dependencies
     *
     * @param printer_state Reference to PrinterState
     * @param api Pointer to IMoonrakerAPI
     */
    SettingsPanel(helix::PrinterState& printer_state, helix::IMoonrakerAPI* api);

    ~SettingsPanel() override;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Initialize SettingsManager subjects
     *
     * Must be called BEFORE XML creation to enable data binding.
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Calls lv_subject_deinit() on all local lv_subject_t members.
     * Must be called before lv_deinit() to prevent dangling observers.
     * Follows [L041] pattern for subject init/deinit symmetry.
     */
    void deinit_subjects();

    /**
     * @brief Setup the settings panel with event handlers and bindings
     *
     * Wires up toggle switches, dropdown, and action row click handlers.
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen (for overlay panel creation)
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    /**
     * @brief Refresh every root row's live status line
     *
     * Called on every return to the root (on_activate()), and directly by
     * tests. Reads each domain's current values and writes the formatted
     * one-liner into that row's settings_status_* subject.
     */
    void on_activate() override;
    void refresh_status_lines();

    const char* get_name() const override {
        return "Settings Panel";
    }
    const char* get_xml_component_name() const override {
        return "settings_panel";
    }

    /// Screen the settings overlays are built on.
    lv_obj_t* parent_screen() const {
        return parent_screen_;
    }

    friend class SettingsPanelTestAccess;

  private:
    //
    // === Widget References ===
    //

    // Restart prompt dialog
    helix::ui::WidgetRef restart_prompt_dialog_;

    // Change host modal is owned by helix::ui::show_change_host_modal(); the
    // connection-failed prompt reaches the same dialog, and ChangeHostModal publishes
    // its subjects under fixed XML names, so a second owner here would fight it.

    //
    // === Reactive Subjects ===
    //

    /// RAII manager for automatic subject cleanup
    SubjectManager subjects_;

    // Platform visibility subjects (Android hides these)
    lv_subject_t show_network_settings_subject_{};
    lv_subject_t show_update_settings_subject_{};
    // 1 when updates are managed by the device firmware (HELIX_DISABLE_AUTO_UPDATES):
    // hides the in-app check/install controls and shows a static notice instead.
    lv_subject_t updates_firmware_managed_subject_{};
    // 1 when in-app updates are suppressed for a NON-firmware reason (self-update is
    // physically impossible because the install tree isn't writable). Drives a
    // neutral "updates aren't available" notice, mutually exclusive with the
    // firmware-managed notice above.
    lv_subject_t updates_unavailable_subject_{};
    // 1 once a plugin host exists: unhides the Plugins row (settings_panel.xml)
    lv_subject_t plugins_available_subject_{};

    // Live status line shown under each stateful root row (settings_panel.xml),
    // refreshed by refresh_status_lines().
    lv_subject_t settings_status_display_subject_{};
    lv_subject_t settings_status_appearance_subject_{};
    lv_subject_t settings_status_sound_subject_{};
    lv_subject_t settings_status_devices_subject_{};
    lv_subject_t settings_status_connection_subject_{};
    lv_subject_t settings_status_language_time_subject_{};
    lv_subject_t settings_status_updates_subject_{};
    char settings_status_display_buf_[64];
    char settings_status_appearance_buf_[64];
    char settings_status_sound_buf_[64];
    char settings_status_devices_buf_[64];
    char settings_status_connection_buf_[64];
    char settings_status_language_time_buf_[64];
    char settings_status_updates_buf_[64];

    // Ethernet's status probe blocks (sysfs scans, or a netd Unix-socket
    // round-trip on daemon-managed firmwares), so refresh_status_lines() never
    // calls it synchronously; get_info_async() hands the result back on an
    // HttpExecutor worker thread. lifetime_ gates the deferred write so a probe
    // that outlives this panel's subjects (e.g. across a deinit_subjects() /
    // init_subjects() cycle) is safely dropped instead of writing stale data.
    std::unique_ptr<EthernetManager> ethernet_manager_;
    helix::AsyncLifetimeGuard lifetime_;
    // Last resolved link states, so the provisional (pre-probe) status on a
    // later refresh reads "Ethernet" or the Wi-Fi network instead of guessing
    // "Not connected" until the async probes land again.
    void render_connection_status();
    bool last_ethernet_up_ = false;
    bool last_wifi_connected_ = false;
    std::string last_wifi_ssid_;
    // Bumped per refresh; a probe carrying an older value is dropped, so a
    // superseded refresh cannot overwrite a newer one's result.
    uint32_t connection_probe_seq_ = 0;

  public:
    /// Shown after any "requires restart" setting changes.
    void show_restart_prompt();

    /// Opens the Hardware Health overlay; also the target of the hardware-issue toast action.
    void handle_hardware_health_clicked();

    // The root rows' callback table lives in register_settings_panel_callbacks()
    // and reaches the handlers above.
    friend void register_settings_panel_callbacks();
};

// Global instance accessor (needed by main.cpp)
SettingsPanel& get_global_settings_panel();

// Register SettingsPanel callbacks for XML parsing (call before settings_panel.xml registration)
// This ensures callbacks exist when LVGL parses the XML component [L013]
void register_settings_panel_callbacks();
