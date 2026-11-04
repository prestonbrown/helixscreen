// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "label_printer_settings.h"

#include "config.h"
#include "core/static_subject_registry.h"
#include "spdlog/spdlog.h"

using namespace helix;

LabelPrinterSettingsManager& LabelPrinterSettingsManager::instance() {
    static LabelPrinterSettingsManager instance;
    return instance;
}

LabelPrinterSettingsManager::LabelPrinterSettingsManager() {
    spdlog::trace("[LabelPrinterSettings] Constructor");
}

void LabelPrinterSettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[LabelPrinterSettings] Subjects already initialized, skipping");
        return;
    }

    spdlog::debug("[LabelPrinterSettings] Initializing subjects");

    Config* config = Config::get_instance();

    // Printer type: 0=network, 1=usb, 2=bluetooth
    std::string type_str = config->get<std::string>("/label_printer/type", "network");
    // Migrate legacy "ipp" type to "network" with ipp protocol
    if (type_str == "ipp") {
        type_str = "network";
        config->set<std::string>("/label_printer/type", "network");
        config->set<std::string>("/label_printer/protocol", "ipp");
        config->save();
        spdlog::info("[LabelPrinterSettings] Migrated legacy ipp type to network+ipp protocol");
    }
    int type_int = 0;
    if (type_str == "usb")
        type_int = 1;
    else if (type_str == "bluetooth")
        type_int = 2;
    UI_MANAGED_SUBJECT_INT(printer_type_subject_, type_int, "label_printer_type", subjects_);

    // Configured flag: depends on printer type
    int configured = is_configured() ? 1 : 0;
    UI_MANAGED_SUBJECT_INT(printer_configured_subject_, configured, "label_printer_configured",
                           subjects_);

    subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit("LabelPrinterSettingsManager", []() {
        LabelPrinterSettingsManager::instance().deinit_subjects();
    });

    spdlog::debug("[LabelPrinterSettings] Subjects initialized: type='{}', configured={}", type_str,
                  configured);
}

void LabelPrinterSettingsManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[LabelPrinterSettings] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[LabelPrinterSettings] Subjects deinitialized");
}

// =============================================================================
// GETTERS / SETTERS
// =============================================================================

std::string LabelPrinterSettingsManager::get_printer_type() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/type", "network");
}

void LabelPrinterSettingsManager::set_printer_type(const std::string& type) {
    spdlog::info("[LabelPrinterSettings] set_printer_type('{}')", type);

    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/type", type);
    config->save();

    // Update type subject (0=network, 1=usb, 2=bluetooth)
    if (subjects_initialized_) {
        int type_int = 0;
        if (type == "usb")
            type_int = 1;
        else if (type == "bluetooth")
            type_int = 2;
        lv_subject_set_int(&printer_type_subject_, type_int);
        lv_subject_set_int(&printer_configured_subject_, is_configured() ? 1 : 0);
    }
}

std::string LabelPrinterSettingsManager::get_printer_address() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/address", "");
}

void LabelPrinterSettingsManager::set_printer_address(const std::string& addr) {
    spdlog::info("[LabelPrinterSettings] set_printer_address('{}')", addr);

    Config* config = Config::get_instance();
    if (addr != get_printer_address()) {
        config->set<std::string>("/label_printer/resource_path", "");
    }
    config->set<std::string>("/label_printer/address", addr);
    config->save();

    // Update configured subject
    if (subjects_initialized_) {
        lv_subject_set_int(&printer_configured_subject_, addr.empty() ? 0 : 1);
    }
}

std::string LabelPrinterSettingsManager::get_ipp_resource_path() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/resource_path", "");
}

void LabelPrinterSettingsManager::set_ipp_resource_path(const std::string& path) {
    spdlog::info("[LabelPrinterSettings] set_ipp_resource_path('{}')", path);
    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/resource_path", path);
    config->save();
}

std::string LabelPrinterSettingsManager::get_printer_protocol() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/protocol", "raw");
}

void LabelPrinterSettingsManager::set_printer_protocol(const std::string& protocol) {
    spdlog::info("[LabelPrinterSettings] set_printer_protocol('{}')", protocol);
    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/protocol", protocol);
    config->save();
}

int LabelPrinterSettingsManager::get_printer_port() const {
    Config* config = Config::get_instance();
    return config->get<int>("/label_printer/port", 9100);
}

void LabelPrinterSettingsManager::set_printer_port(int port) {
    spdlog::info("[LabelPrinterSettings] set_printer_port({})", port);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/port", port);
    config->save();
}

int LabelPrinterSettingsManager::get_label_size_index() const {
    Config* config = Config::get_instance();
    return config->get<int>("/label_printer/label_size", 0);
}

void LabelPrinterSettingsManager::set_label_size_index(int index) {
    spdlog::info("[LabelPrinterSettings] set_label_size_index({})", index);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/label_size", index);
    config->save();
}

int LabelPrinterSettingsManager::get_label_preset() const {
    Config* config = Config::get_instance();
    return config->get<int>("/label_printer/preset", 0);
}

void LabelPrinterSettingsManager::set_label_preset(int preset) {
    spdlog::info("[LabelPrinterSettings] set_label_preset({})", preset);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/preset", preset);
    config->save();
}

uint16_t LabelPrinterSettingsManager::get_usb_vid() const {
    Config* config = Config::get_instance();
    return static_cast<uint16_t>(config->get<int>("/label_printer/usb_vid", 0));
}

void LabelPrinterSettingsManager::set_usb_vid(uint16_t vid) {
    spdlog::info("[LabelPrinterSettings] set_usb_vid(0x{:04x})", vid);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/usb_vid", vid);
    config->save();

    if (subjects_initialized_) {
        lv_subject_set_int(&printer_configured_subject_, is_configured() ? 1 : 0);
    }
}

uint16_t LabelPrinterSettingsManager::get_usb_pid() const {
    Config* config = Config::get_instance();
    return static_cast<uint16_t>(config->get<int>("/label_printer/usb_pid", 0));
}

void LabelPrinterSettingsManager::set_usb_pid(uint16_t pid) {
    spdlog::info("[LabelPrinterSettings] set_usb_pid(0x{:04x})", pid);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/usb_pid", pid);
    config->save();

    if (subjects_initialized_) {
        lv_subject_set_int(&printer_configured_subject_, is_configured() ? 1 : 0);
    }
}

std::string LabelPrinterSettingsManager::get_usb_serial() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/usb_serial", "");
}

void LabelPrinterSettingsManager::set_usb_serial(const std::string& serial) {
    spdlog::info("[LabelPrinterSettings] set_usb_serial('{}')", serial);

    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/usb_serial", serial);
    config->save();
}

std::string LabelPrinterSettingsManager::get_bt_address() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/bt_address", "");
}

void LabelPrinterSettingsManager::set_bt_address(const std::string& address) {
    spdlog::info("[LabelPrinterSettings] set_bt_address('{}')", address);

    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/bt_address", address);
    config->save();

    if (subjects_initialized_) {
        lv_subject_set_int(&printer_configured_subject_, is_configured() ? 1 : 0);
    }
}

std::string LabelPrinterSettingsManager::get_bt_name() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/bt_name", "");
}

void LabelPrinterSettingsManager::set_bt_name(const std::string& name) {
    spdlog::info("[LabelPrinterSettings] set_bt_name('{}')", name);

    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/bt_name", name);
    config->save();
}

std::string LabelPrinterSettingsManager::get_bt_transport() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/bt_transport", "spp");
}

void LabelPrinterSettingsManager::set_bt_transport(const std::string& transport) {
    spdlog::info("[LabelPrinterSettings] set_bt_transport('{}')", transport);

    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/bt_transport", transport);
    config->save();
}

int LabelPrinterSettingsManager::get_bt_channel() const {
    Config* config = Config::get_instance();
    return config->get<int>("/label_printer/bt_channel", 0);
}

void LabelPrinterSettingsManager::set_bt_channel(int channel) {
    spdlog::info("[LabelPrinterSettings] set_bt_channel({})", channel);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/bt_channel", channel);
    config->save();
}

int LabelPrinterSettingsManager::get_label_count() const {
    Config* config = Config::get_instance();
    return config->get<int>("/label_printer/label_count", 1);
}

void LabelPrinterSettingsManager::set_label_count(int count) {
    spdlog::info("[LabelPrinterSettings] set_label_count({})", count);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/label_count", count);
    config->save();
}

bool LabelPrinterSettingsManager::is_configured() const {
    const auto type = get_printer_type();
    if (type == "usb") {
        return get_usb_vid() != 0 && get_usb_pid() != 0;
    }
    if (type == "bluetooth") {
        return !get_bt_address().empty();
    }
    // "network" requires an address (covers both raw and ipp protocols)
    return !get_printer_address().empty();
}

#endif // HELIX_HAS_LABEL_PRINTER
