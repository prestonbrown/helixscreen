// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "input_device_scanner.h"
#include "overlay_base.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct helix_bt_context;

namespace helix::ui {

/// Persistent settings overlay for barcode scanner selection and configuration.
/// Twins LabelPrinterSettingsOverlay: keyboard-layout dropdown, USB device list,
/// Bluetooth discovery + pairing, Auto-detect option. Reached from the Spoolman
/// settings "Barcode Scanner" row. Replaces the former ScannerPickerModal.
class BarcodeScannerSettingsOverlay : public OverlayBase {
  public:
    BarcodeScannerSettingsOverlay();
    ~BarcodeScannerSettingsOverlay() override;

    const char* get_name() const override {
        return "Barcode Scanner Settings";
    }

    void init_subjects() override;
    void register_callbacks() override;

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

  protected:
    void on_activate() override;
    void on_deactivate() override;

  private:
    void refresh_current_selection_label();
    void populate_device_list();
    void add_usb_row(lv_obj_t* container, const std::string& label, const std::string& sublabel,
                     const std::string& vendor_product);
    void add_auto_detect_row(lv_obj_t* container);

    void handle_device_selected(const std::string& vendor_product, const std::string& device_name,
                                const std::string& bt_mac);
    void handle_keymap_changed(int dropdown_index);

    // Bluetooth
    void start_bt_discovery();
    void stop_bt_discovery();
    void populate_bt_dropdown();
    void update_bt_action_buttons();
    void pair_bt_device(const std::string& mac, const std::string& name);
    void handle_bt_forget(const std::string& mac);
    int selected_bt_index() const;

    // XML event handlers
    static void on_bs_scan_bluetooth(lv_event_t* e);
    static void on_bs_refresh_usb(lv_event_t* e);
    static void on_bs_keymap_changed(lv_event_t* e);
    static void on_bs_row_clicked(lv_event_t* e);
    static void on_bs_row_forget(lv_event_t* e);
    static void on_bs_bt_scanner_selected(lv_event_t* e);
    static void on_bs_bt_pair(lv_event_t* e);
    static void on_bs_bt_forget(lv_event_t* e);
    static void on_bs_pair_confirm(lv_event_t* e);
    static void on_bs_pair_cancel(lv_event_t* e);

    // Subjects (global scope — single-instance overlay)
    lv_subject_t bt_available_subject_{};
    lv_subject_t bt_discovering_subject_{};
    lv_subject_t keymap_index_subject_{};
    lv_subject_t current_device_label_subject_{};
    char current_device_label_buf_[128]{};

    // Widget references (set in on_activate)
    lv_obj_t* usb_list_ = nullptr;
    lv_obj_t* bt_dropdown_ = nullptr;
    lv_obj_t* btn_bt_pair_ = nullptr;
    lv_obj_t* btn_bt_forget_ = nullptr;

    // BT state (lifted from ScannerPickerModal)
    struct BtDeviceInfo {
        std::string mac;
        std::string name;
        std::string vendor_product;
        bool paired = false;
        bool is_ble = false;
    };

    struct BtDiscoveryContext {
        std::atomic<bool> alive{true};
        BarcodeScannerSettingsOverlay* overlay = nullptr;
        std::optional<LifetimeToken> token;
    };

    helix_bt_context* bt_ctx_ = nullptr;
    std::shared_ptr<BtDiscoveryContext> bt_discovery_ctx_;
    std::vector<BtDeviceInfo> bt_devices_;
    bool bt_discovering_ = false;

    static BarcodeScannerSettingsOverlay* s_active_instance_;

    friend BarcodeScannerSettingsOverlay& get_barcode_scanner_settings_overlay();
};

BarcodeScannerSettingsOverlay& get_barcode_scanner_settings_overlay();

} // namespace helix::ui
