// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h" // Need full definition for PowerDevice
#include "subject_managed_panel.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace helix::ui {
struct PowerPanelTestAccess; // test-only friend (tests/test_helpers/)
} // namespace helix::ui

/**
 * @brief Power device control panel
 *
 * Displays configured Moonraker power devices with on/off toggle controls.
 * Each device is shown as a row with the device name and a toggle switch.
 * Devices marked as "locked_while_printing" show a lock indicator during prints.
 */
class PowerPanel : public PanelBase {
  public:
    PowerPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);
    ~PowerPanel() override;

    void init_subjects() override;
    void deinit_subjects();
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Power Devices";
    }
    const char* get_xml_component_name() const override {
        return "power_panel";
    }

    /// Get devices selected for home panel quick-toggle
    const std::vector<std::string>& get_selected_devices() const {
        return selected_devices_;
    }

    /// Set devices selected for home panel quick-toggle (saves to config)
    void set_selected_devices(const std::vector<std::string>& devices);

    /// Load selected devices from config file
    void load_selected_devices();

    /// Called when new devices are discovered to auto-select if no config exists
    void on_devices_discovered(const std::vector<PowerDevice>& devices);

    /**
     * @brief Get or create the overlay widget (singleton overlay creation)
     *
     * Ensures only one overlay lv_obj_t* exists for this panel, shared by
     * all callers (HomePanel long-press, AdvancedPanel row click, etc.).
     *
     * @param parent_screen Screen to create overlay on (used only on first call)
     * @return The overlay lv_obj_t*, or nullptr on failure
     */
    lv_obj_t* get_or_create_overlay(lv_obj_t* parent_screen);

  private:
    lv_obj_t* cached_overlay_ = nullptr; // Single shared overlay widget

    // Guards async API callbacks from accessing a destroyed instance
    helix::AsyncLifetimeGuard lifetime_;

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // Subjects for reactive binding
    lv_subject_t status_subject_;
    char status_buf_[128] = "Loading devices...";

    // Widget references into a tree this panel does not own. forget_widget_tree()
    // drops them when that tree is deleted — see on_panel_deleted().
    lv_obj_t* device_list_container_ = nullptr;
    lv_obj_t* empty_state_container_ = nullptr;
    lv_obj_t* status_label_ = nullptr; ///< Write-only: cached by setup(), never read

    // Device state tracking
    struct DeviceRow {
        lv_obj_t* container = nullptr;
        lv_obj_t* toggle = nullptr;
        std::string device_name;
        bool locked = false;
    };
    std::vector<DeviceRow> device_rows_;

    // Selected devices for home panel quick-toggle
    std::vector<std::string> selected_devices_;
    std::vector<std::string> discovered_devices_; // All discovered device names
    bool config_loaded_ = false;

    // Chip selector widgets
    lv_obj_t* chip_container_ = nullptr;

    /// Drop every cached pointer into the widget tree (called when it is deleted)
    void forget_widget_tree();

    /// LV_EVENT_DELETE hook on the panel root — the only notice this panel gets
    /// that its widget tree died.
    static void on_panel_deleted(lv_event_t* e);

    // Chip selector helpers
    void populate_device_chips();
    void populate_device_chips_impl();
    void handle_chip_clicked(const std::string& device_name);
    bool chips_rebuild_pending_ = false; ///< Coalesces chip rebuild requests

    // Setup helpers
    void fetch_devices();
    void populate_device_list(const std::vector<PowerDevice>& devices);
    void create_device_row(const PowerDevice& device);
    void clear_device_list();

    // Event handlers
    void handle_device_toggle(const std::string& device, bool power_on);

    // Static callback for XML event_cb
    static void on_power_device_toggle(lv_event_t* e);

    friend struct helix::ui::PowerPanelTestAccess;
};

/**
 * @brief Get global power panel instance
 * @return Reference to the singleton power panel
 */
PowerPanel& get_global_power_panel();
