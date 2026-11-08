// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <string>

namespace helix {

/**
 * @brief Domain-specific manager for label printer settings
 *
 * Owns label printer configuration and persistence:
 * - printer_type ("network", "usb", or "bluetooth")
 * - printer_address (IP/hostname of Brother QL printer or IPP printer)
 * - printer_port (raw socket port, default 9100; IPP uses 631)
 * - printer_protocol ("raw" or "ipp") — auto-detected from mDNS discovery
 * - ipp_resource_path (IPP queue path from mDNS TXT "rp"; belongs to printer_address)
 * - usb_vid, usb_pid, usb_serial (USB device identification)
 * - bt_address (Bluetooth MAC address)
 * - bt_transport ("spp" or "ble")
 * - label_size_index (index into supported label sizes)
 * - label_preset (0=Standard, 1=Compact, 2=Minimal)
 * - printer_configured subject (1 if address non-empty, USB VID/PID set, or BT address set)
 * - printer_type subject (0=network, 1=usb, 2=bluetooth)
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class LabelPrinterSettingsManager {
  public:
    static LabelPrinterSettingsManager& instance();

    LabelPrinterSettingsManager(const LabelPrinterSettingsManager&) = delete;
    LabelPrinterSettingsManager& operator=(const LabelPrinterSettingsManager&) = delete;

    /** @brief Initialize LVGL subjects and load from Config */
    void init_subjects();

    /** @brief Deinitialize LVGL subjects (called by StaticSubjectRegistry) */
    void deinit_subjects();

    // =========================================================================
    // GETTERS / SETTERS
    // =========================================================================

    /** @brief Get printer type ("network", "usb", or "bluetooth") */
    std::string get_printer_type() const;

    /** @brief Set printer type (updates type + configured subjects, persists) */
    void set_printer_type(const std::string& type);

    /** @brief Get network printer protocol ("raw" or "ipp") */
    std::string get_printer_protocol() const;

    /** @brief Set network printer protocol (persists to config) */
    void set_printer_protocol(const std::string& protocol);

    /** @brief Get printer IP address or hostname */
    std::string get_printer_address() const;

    /** @brief Set printer address (updates configured subject + persists).
     *  A different address clears the IPP resource path, which belongs to the old one. */
    void set_printer_address(const std::string& addr);

    /** @brief Get the IPP resource path as stored (empty = printer advertised none) */
    std::string get_ipp_resource_path() const;

    /** @brief Set the IPP resource path (persists to config) */
    void set_ipp_resource_path(const std::string& path);

    /** @brief Get raw socket port (default 9100) */
    int get_printer_port() const;

    /** @brief Set raw socket port (persists to config) */
    void set_printer_port(int port);

    /** @brief Get label size index (into supported sizes list) */
    int get_label_size_index() const;

    /** @brief Set label size index (persists to config) */
    void set_label_size_index(int index);

    /** @brief Get label preset (0=Standard, 1=Compact, 2=Minimal) */
    int get_label_preset() const;

    /** @brief Set label preset (persists to config) */
    void set_label_preset(int preset);

    uint16_t get_usb_vid() const;

    /** @brief Set USB vendor ID (updates configured subject, persists) */
    void set_usb_vid(uint16_t vid);

    uint16_t get_usb_pid() const;

    /** @brief Set USB product ID (updates configured subject, persists) */
    void set_usb_pid(uint16_t pid);

    std::string get_usb_serial() const;

    /** @brief Set USB serial number (persists to config) */
    void set_usb_serial(const std::string& serial);

    // Bluetooth settings

    [[nodiscard]] std::string get_bt_address() const;

    /** @brief Set Bluetooth MAC address (updates configured subject, persists) */
    void set_bt_address(const std::string& address);

    /** @brief Get Bluetooth device name (for display without scanning) */
    [[nodiscard]] std::string get_bt_name() const;

    /** @brief Set Bluetooth device name (persists to config) */
    void set_bt_name(const std::string& name);

    /** @brief Get Bluetooth transport type ("spp" or "ble") */
    [[nodiscard]] std::string get_bt_transport() const;

    /** @brief Set Bluetooth transport type ("spp" or "ble", persists) */
    void set_bt_transport(const std::string& transport);

    /** @brief Get cached Bluetooth RFCOMM channel (0 = unknown, do SDP lookup) */
    [[nodiscard]] int get_bt_channel() const;

    /** @brief Set cached Bluetooth RFCOMM channel (persists; 0 clears cache) */
    void set_bt_channel(int channel);

    /** @brief Get label count per print job (IPP sheet labels, default 1) */
    int get_label_count() const;

    /** @brief Set label count per print job (persists to config) */
    void set_label_count(int count);

    /** @brief True if printer is configured (network: address non-empty; USB: VID+PID set; BT:
     * address non-empty) */
    bool is_configured() const;

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief Printer configured subject (integer: 0=not configured, 1=configured) */
    lv_subject_t* subject_printer_configured() {
        return &printer_configured_subject_;
    }

    /** @brief Printer type subject (integer: 0=network, 1=usb, 2=bluetooth) */
    lv_subject_t* subject_printer_type() {
        return &printer_type_subject_;
    }

  private:
    LabelPrinterSettingsManager();
    ~LabelPrinterSettingsManager() = default;

    SubjectManager subjects_;

    lv_subject_t printer_configured_subject_{}; // int: 0/1
    lv_subject_t printer_type_subject_{};       // int: 0=network, 1=usb, 2=bluetooth

    bool subjects_initialized_ = false;
};

/**
 * @brief The subject XML binds as `label_printer_configured`
 *
 * Builds without label-printer support get a permanent 0, so a
 * `bind_flag_if_eq subject="label_printer_configured" flag="hidden"` resolves and
 * hides the label controls instead of failing to bind and leaving them visible.
 */
inline lv_subject_t* label_printer_configured_subject() {
#if HELIX_HAS_LABEL_PRINTER
    auto& settings = LabelPrinterSettingsManager::instance();
    settings.init_subjects();
    return settings.subject_printer_configured();
#else
    static lv_subject_t unsupported{};
    static bool initialized = false;
    if (!initialized) {
        lv_subject_init_int(&unsupported, 0);
        initialized = true;
    }
    return &unsupported;
#endif
}

} // namespace helix
