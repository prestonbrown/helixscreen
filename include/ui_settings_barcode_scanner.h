// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "bt_discovery_run.h"
#include "core/static_panel_registry.h"
#include "input_device_scanner.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

namespace helix::ui {

/// Persistent settings overlay for barcode scanner selection and configuration.
/// Twins LabelPrinterSettingsOverlay: keyboard-layout dropdown, USB device list,
/// Bluetooth discovery + pairing, Auto-detect option. Reached from the Spoolman
/// settings "Barcode Scanner" row. Replaces the former ScannerPickerModal.
class BarcodeScannerSettingsOverlay : public OverlayBase {
  public:
    ~BarcodeScannerSettingsOverlay() override;

    const char* get_name() const override {
        return "Barcode Scanner Settings";
    }
    const char* xml_component() const override {
        return "barcode_scanner_settings";
    }
    bool destroy_on_close() const override {
        return true;
    }
    HELIX_KIND_TAG(BarcodeScannerSettingsOverlay)

    void init_subjects() override;
    void register_callbacks() override;

  protected:
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

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
    /// Merges BlueZ's known scanners into bt_devices_ from a worker thread.
    void seed_known_bt_devices();
    void update_bt_action_buttons();
    void pair_bt_device(const std::string& mac, const std::string& name);
    void handle_bt_forget(const std::string& mac);
    int selected_bt_index() const;

    // Subjects (global scope — single-instance overlay)
    SubjectManager subjects_; // declared ahead: tears down after the subjects it owns
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

    std::shared_ptr<helix::bluetooth::SharedContext> bt_ctx_ =
        std::make_shared<helix::bluetooth::SharedContext>();
    helix::bluetooth::DiscoveryRun bt_discovery_;
    std::vector<BtDeviceInfo> bt_devices_;
    bool bt_discovering_ = false;
};

inline BarcodeScannerSettingsOverlay& get_barcode_scanner_settings_overlay() {
    return lazy_global<BarcodeScannerSettingsOverlay>("BarcodeScannerSettingsOverlay");
}

} // namespace helix::ui
