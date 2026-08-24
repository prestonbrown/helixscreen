// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config.h"

#include <string>

/**
 * @file wizard_config_paths.h
 * @brief Centralized configuration path suffixes for wizard screens
 *
 * Defines JSON configuration path SUFFIXES used by wizard screens.
 * These are relative to the active printer config section.
 *
 * Usage: config->get<T>(config->df() + wizard::BED_HEATER, default_val)
 *
 * The df() method returns the printer prefix (e.g., "/printers/voron/")
 * so the full path resolves to e.g., "/printers/voron/heaters/bed".
 */

namespace helix {
namespace wizard {

// ---- Per-printer settings (suffixes — prepend config->df() for full path) ----
// Example: config->get<T>(config->df() + wizard::BED_HEATER, default_val)
//          resolves to e.g. "/printers/voron/heaters/bed"

// Printer identification
constexpr const char* PRINTER_NAME = "printer_name";
constexpr const char* PRINTER_TYPE = "type";
// Which saved printer type the one-time saved-vs-detected mismatch warning
// (Application::maybe_warn_type_mismatch) was last shown for. Changing the
// saved type re-arms the warning once.
constexpr const char* TYPE_MISMATCH_SHOWN_FOR = "type_mismatch_shown_for";

// Bed hardware
constexpr const char* BED_HEATER = "heaters/bed";
constexpr const char* BED_SENSOR = "temp_sensors/bed";

// Hotend hardware
constexpr const char* HOTEND_HEATER = "heaters/hotend";
constexpr const char* HOTEND_SENSOR = "temp_sensors/hotend";

// Fan hardware
constexpr const char* HOTEND_FAN = "fans/hotend";
constexpr const char* PART_FAN = "fans/part";
constexpr const char* CHAMBER_FAN = "fans/chamber";
constexpr const char* EXHAUST_FAN = "fans/exhaust";

// Chamber hardware (sensor and heater — distinct from chamber_fan above)
constexpr const char* CHAMBER_SENSOR = "temp_sensors/chamber";
constexpr const char* CHAMBER_HEATER = "heaters/chamber";

// LED hardware (legacy — used for migration only in LedController::load_config()
// and hardware_validator.cpp. New code should use LedController::selected_strips())
constexpr const char* LED_STRIP = "leds/strip";
constexpr const char* LED_SELECTED = "leds/selected";

// Network configuration (per-printer)
constexpr const char* MOONRAKER_HOST = "moonraker_host";
constexpr const char* MOONRAKER_PORT = "moonraker_port";

// ---- Device-level settings (absolute paths — do NOT prepend df()) ----
constexpr const char* WIFI_SSID = "/wifi/ssid";
constexpr const char* WIFI_PASSWORD = "/wifi/password";
} // namespace wizard

// Per-printer setting (suffix — prepend config->df() for full path)
constexpr const char* PRINTER_IMAGE = "printer_image";

/// Get the printer display name from config: saved name → model/type → fallback.
/// Used by both the home screen widget and printer manager overlay.
inline std::string get_printer_display_name(const std::string& fallback = "My Printer") {
    Config* config = Config::get_instance();
    if (!config)
        return fallback;

    std::string name = config->get<std::string>(config->df() + wizard::PRINTER_NAME, "");
    if (!name.empty())
        return name;

    std::string type = config->get<std::string>(config->df() + wizard::PRINTER_TYPE, "");
    if (!type.empty())
        return type;

    return fallback;
}

} // namespace helix
