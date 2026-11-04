// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "bluetooth_plugin.h"
#include "bt_discovery_run.h"
#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "mdns_discovery.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "usb_printer_detector.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace helix::settings {

class LabelPrinterSettingsOverlay : public OverlayBase {
    friend class LabelPrinterSettingsOverlayTestAccess;

  public:
    ~LabelPrinterSettingsOverlay() override;

    // OverlayBase interface
    void init_subjects() override;
    void register_callbacks() override;
    const char* get_name() const override {
        return "Label Printer";
    }
    const char* xml_component() const override {
        return "label_printer_settings";
    }
    bool destroy_on_close() const override {
        return true;
    }
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    // Event handlers (called by the XML callback lambdas)
    void handle_address_changed();
    void handle_port_changed();
    void handle_label_size_changed(int index);
    void handle_preset_changed(int index);
    void handle_test_print();
    void handle_printer_selected(int index);
    void handle_type_changed(int index);
    void handle_usb_printer_selected(int index);
    void handle_bt_printer_selected(int index);
    void handle_bt_scan();
    void handle_bt_connect();
    void handle_bt_forget();
    void handle_label_count_changed(int index);
    void handle_user_printer_selected(int index);
    void handle_manual_entry_started();

  private:
    /// A discovered network printer with auto-detected protocol
    struct DiscoveredNetworkPrinter {
        DiscoveredPrinter printer;
        std::string protocol; ///< "raw" or "ipp"
        int score;
    };

    void init_address_input();
    void init_port_input();
    void init_label_size_dropdown();
    void init_preset_dropdown();
    void init_discovery_dropdown();
    void init_printer_type_dropdown();
    void init_usb_printer_dropdown();
    void init_bt_printer_dropdown();
    void init_label_count_dropdown();
    void start_label_printer_discovery();
    void stop_label_printer_discovery();
    void start_usb_detection();
    void stop_usb_detection();
    void start_bt_discovery();
    void stop_bt_discovery();
    void merge_and_update_discovery();
    void on_usb_printers_detected(const std::vector<helix::UsbPrinterInfo>& printers);
    void update_ipp_selected_subject();

    /// A rebuilt tree has none of the input callbacks the guard below stands for.
    void on_ui_destroyed() override {
        inputs_initialized_ = false;
    }

    bool inputs_initialized_ = false; ///< Guard against stacking duplicate event callbacks
    /// The saved network printer was chosen by discovery, not the user, so a
    /// better-ranked printer found later replaces it.
    bool auto_picked_ = false;

    // mDNS discovery (raw TCP + IPP, merged into single list)
    std::unique_ptr<IMdnsDiscovery> mdns_discovery_;     ///< _pdl-datastream._tcp
    std::unique_ptr<IMdnsDiscovery> ipp_mdns_discovery_; ///< _ipp._tcp
    std::vector<DiscoveredPrinter> raw_printers_;        ///< temp buffer for raw TCP results
    std::vector<DiscoveredPrinter> ipp_printers_;        ///< temp buffer for IPP results
    std::vector<DiscoveredNetworkPrinter> discovered_network_printers_; ///< merged list
    SubjectManager subjects_;             // declared ahead: tears down after the subjects it owns
    lv_subject_t ipp_selected_subject_{}; ///< 0=not IPP, 1=IPP protocol selected

    // USB printer detection
    std::unique_ptr<helix::UsbPrinterDetector> usb_detector_;
    std::vector<helix::UsbPrinterInfo> detected_usb_printers_;

    // Bluetooth discovery
    /// Info about a discovered BT printer (UI thread copies)
    struct BtDeviceInfo {
        std::string mac;
        std::string name;
        bool paired = false;
        bool connected = false;
        bool is_ble = false;
        bool is_scanner = false; ///< HID barcode scanner — exclude from printer dropdown
    };

    std::shared_ptr<helix::bluetooth::SharedContext> bt_ctx_ =
        std::make_shared<helix::bluetooth::SharedContext>();
    helix::bluetooth::DiscoveryRun bt_discovery_;
    std::vector<BtDeviceInfo> bt_devices_;
    bool bt_discovering_ = false;
    lv_subject_t bt_scanning_subject_{};   ///< 0=idle, 1=scanning
    lv_subject_t test_printing_subject_{}; ///< 0=idle, 1=printing

  public:
    void test_printing_subject_set(int val) {
        lv_subject_set_int(&test_printing_subject_, val);
    }
};

inline LabelPrinterSettingsOverlay& get_label_printer_settings_overlay() {
    return lazy_global<LabelPrinterSettingsOverlay>("LabelPrinterSettingsOverlay");
}

} // namespace helix::settings
