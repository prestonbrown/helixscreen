/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "ui_wizard_summary.h"
#include "ui_wizard.h"
#include "app_globals.h"
#include "config.h"
#include "lvgl/lvgl.h"
#include <spdlog/spdlog.h>
#include <string>
#include <sstream>

// ============================================================================
// Static Data & Subjects
// ============================================================================

// Subject declarations (module scope)
static lv_subject_t summary_printer_name;
static lv_subject_t summary_printer_type;
static lv_subject_t summary_network;
static lv_subject_t summary_bed;
static lv_subject_t summary_hotend;
static lv_subject_t summary_part_fan;

// String buffers (must be persistent)
static char printer_name_buffer[128];
static char printer_type_buffer[128];
static char network_buffer[256];
static char bed_buffer[256];
static char hotend_buffer[256];
static char part_fan_buffer[256];

// Screen instance
static lv_obj_t* summary_screen_root = nullptr;

// ============================================================================
// Helper Functions
// ============================================================================

static std::string format_network_summary(Config* config) {
    std::stringstream ss;

    std::string connection_type = config->get<std::string>("/network/connection_type", "None");
    if (connection_type == "wifi") {
        std::string ssid = config->get<std::string>("/network/wifi_ssid", "");
        if (!ssid.empty()) {
            ss << "WiFi: " << ssid;
        } else {
            ss << "WiFi: Not configured";
        }
    } else if (connection_type == "ethernet") {
        std::string ip = config->get<std::string>("/network/eth_ip", "");
        if (!ip.empty()) {
            ss << "Ethernet: " << ip;
        } else {
            ss << "Ethernet: DHCP";
        }
    } else {
        ss << "Not configured";
    }

    return ss.str();
}

static std::string format_bed_summary(Config* config) {
    std::stringstream ss;

    std::string heater = config->get<std::string>("/printer/bed_heater", "None");
    std::string sensor = config->get<std::string>("/printer/bed_sensor", "None");

    ss << "Heater: " << (heater == "None" ? "None" : heater);
    ss << ", Sensor: " << (sensor == "None" ? "None" : sensor);

    return ss.str();
}

static std::string format_hotend_summary(Config* config) {
    std::stringstream ss;

    std::string heater = config->get<std::string>("/printer/hotend_heater", "None");
    std::string sensor = config->get<std::string>("/printer/hotend_sensor", "None");

    ss << "Heater: " << (heater == "None" ? "None" : heater);
    ss << ", Sensor: " << (sensor == "None" ? "None" : sensor);

    return ss.str();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void ui_wizard_summary_init_subjects() {
    spdlog::debug("[Wizard Summary] Initializing subjects");

    // Load all values from config
    Config* config = Config::get_instance();

    // Printer name
    std::string printer_name = config ? config->get<std::string>("/printer/name", "Unnamed Printer") : "Unnamed Printer";
    strncpy(printer_name_buffer, printer_name.c_str(), sizeof(printer_name_buffer) - 1);
    printer_name_buffer[sizeof(printer_name_buffer) - 1] = '\0';
    lv_subject_init_string(&summary_printer_name, printer_name_buffer, nullptr,
                          sizeof(printer_name_buffer), printer_name_buffer);
    lv_xml_register_subject(nullptr, "summary_printer_name", &summary_printer_name);

    // Printer type
    std::string printer_type = config ? config->get<std::string>("/printer/type", "Unknown") : "Unknown";
    strncpy(printer_type_buffer, printer_type.c_str(), sizeof(printer_type_buffer) - 1);
    printer_type_buffer[sizeof(printer_type_buffer) - 1] = '\0';
    lv_subject_init_string(&summary_printer_type, printer_type_buffer, nullptr,
                          sizeof(printer_type_buffer), printer_type_buffer);
    lv_xml_register_subject(nullptr, "summary_printer_type", &summary_printer_type);

    // Network configuration
    std::string network_summary = config ? format_network_summary(config) : "Not configured";
    strncpy(network_buffer, network_summary.c_str(), sizeof(network_buffer) - 1);
    network_buffer[sizeof(network_buffer) - 1] = '\0';
    lv_subject_init_string(&summary_network, network_buffer, nullptr,
                          sizeof(network_buffer), network_buffer);
    lv_xml_register_subject(nullptr, "summary_network", &summary_network);

    // Bed configuration
    std::string bed_summary = config ? format_bed_summary(config) : "Not configured";
    strncpy(bed_buffer, bed_summary.c_str(), sizeof(bed_buffer) - 1);
    bed_buffer[sizeof(bed_buffer) - 1] = '\0';
    lv_subject_init_string(&summary_bed, bed_buffer, nullptr,
                          sizeof(bed_buffer), bed_buffer);
    lv_xml_register_subject(nullptr, "summary_bed", &summary_bed);

    // Hotend configuration
    std::string hotend_summary = config ? format_hotend_summary(config) : "Not configured";
    strncpy(hotend_buffer, hotend_summary.c_str(), sizeof(hotend_buffer) - 1);
    hotend_buffer[sizeof(hotend_buffer) - 1] = '\0';
    lv_subject_init_string(&summary_hotend, hotend_buffer, nullptr,
                          sizeof(hotend_buffer), hotend_buffer);
    lv_xml_register_subject(nullptr, "summary_hotend", &summary_hotend);

    // Part cooling fan
    std::string part_fan = config ? config->get<std::string>("/printer/part_fan", "None") : "None";
    strncpy(part_fan_buffer, part_fan.c_str(), sizeof(part_fan_buffer) - 1);
    part_fan_buffer[sizeof(part_fan_buffer) - 1] = '\0';
    lv_subject_init_string(&summary_part_fan, part_fan_buffer, nullptr,
                          sizeof(part_fan_buffer), part_fan_buffer);
    lv_xml_register_subject(nullptr, "summary_part_fan", &summary_part_fan);

    spdlog::info("[Wizard Summary] Subjects initialized with config values");
}

// ============================================================================
// Callback Registration
// ============================================================================

void ui_wizard_summary_register_callbacks() {
    spdlog::debug("[Wizard Summary] No callbacks to register (read-only screen)");
    // No interactive callbacks for summary screen
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* ui_wizard_summary_create(lv_obj_t* parent) {
    spdlog::info("[Wizard Summary] Creating summary screen");

    if (summary_screen_root) {
        spdlog::warn("[Wizard Summary] Screen already exists, destroying old instance");
        lv_obj_del(summary_screen_root);
        summary_screen_root = nullptr;
    }

    // Refresh subjects with latest config values before creating UI
    ui_wizard_summary_init_subjects();

    // Create screen from XML
    summary_screen_root = (lv_obj_t*)lv_xml_create(parent, "wizard_summary", nullptr);
    if (!summary_screen_root) {
        spdlog::error("[Wizard Summary] Failed to create screen from XML");
        return nullptr;
    }

    spdlog::info("[Wizard Summary] Screen created successfully");
    return summary_screen_root;
}

// ============================================================================
// Cleanup
// ============================================================================

void ui_wizard_summary_cleanup() {
    spdlog::debug("[Wizard Summary] Cleaning up resources");

    if (summary_screen_root) {
        lv_obj_del(summary_screen_root);
        summary_screen_root = nullptr;
    }
}

// ============================================================================
// Validation
// ============================================================================

bool ui_wizard_summary_is_validated() {
    // Summary screen is always validated (no user input required)
    return true;
}