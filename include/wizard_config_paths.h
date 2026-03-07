// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

} // namespace helix
