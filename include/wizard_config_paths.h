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
// The hostname the printer's Klipper reports; how mDNS re-finds it at a new address
constexpr const char* kHostname = "hostname";
constexpr const char* PRINTER_TYPE = "type";
// The saved and detected printer types the saved-vs-detected mismatch warning
// (HardwareSetupPrompter::maybe_warn_type_mismatch) was last answered for. A change to
// either one re-arms the warning once.
constexpr const char* TYPE_MISMATCH_SHOWN_FOR = "type_mismatch_shown_for";
constexpr const char* kTypeMismatchShownForDetected = "type_mismatch_shown_for_detected";

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
constexpr const char* kAuxFan = "fans/aux";

// Chamber hardware (sensor and heater — distinct from chamber_fan above)
constexpr const char* CHAMBER_SENSOR = "temp_sensors/chamber";
constexpr const char* CHAMBER_HEATER = "heaters/chamber";

// Tool-changer feeder macro overrides. "auto" means use the macro
// toolchanger_addon detected for this machine. Per-printer: two MedusaHC builds
// on one network can be mid-migration and expose different command names.
constexpr const char* FEEDER_OPEN_MACRO = "toolchanger/feeder_open_macro";
constexpr const char* FEEDER_CLOSE_MACRO = "toolchanger/feeder_close_macro";
// How far a mount-to-feed load purges past the nozzle, in mm. Per-printer: it
// follows that machine's hot ends.
constexpr const char* kMountFeedPurgeMm = "toolchanger/purge_length_mm";

// ACE bypass macros. The ACE Pro has no bypass command, so the master switch
// that disables its filament path is thrown by macros. "auto" detects the
// conventional names; a printer exposing neither reports no bypass. Per-printer,
// because the wrapper macros are the owner's own and differ between machines.
constexpr const char* kAceBypassOnMacro = "ams/ace_bypass_on_macro";
constexpr const char* kAceBypassOffMacro = "ams/ace_bypass_off_macro";

// LED hardware. kLedAutoStateStrips holds the devices the user chose (the wizard's
// LED step stages its pick there). kLedSelectedStrips is the older selection
// LedController still reads. LED_STRIP is the wizard's own record of the user's pick
// (written by the LED step, read by the summary) and LED_SELECTED its array form.
// None are kept in step, so hardware_validator.cpp asks each in that order before
// deciding no LED is configured.
constexpr const char* kLedAutoStateStrips = "leds/auto_state/strips";
constexpr const char* kLedSelectedStrips = "leds/selected_strips";
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
/**
 * @brief The resolved printer type as saved in config, or empty.
 *
 * Config is the only source populated during the discovery callback
 * (init_subsystems_from_hardware). PrinterState's copy is set later, by
 * auto_detect_and_save, so anything constructed or initialised in that callback
 * that reads PrinterState gets "" on every run and silently behaves as though
 * the printer were unknown.
 */
inline std::string get_saved_printer_type() {
    Config* config = Config::get_instance();
    return config->get<std::string>(config->df() + wizard::PRINTER_TYPE, "");
}

/// The active printer's display name (Config::get_printer_display_name()), or @p fallback.
/// Used by both the home screen widget and printer manager overlay.
inline std::string get_printer_display_name(const std::string& fallback = "My Printer") {
    Config* config = Config::get_instance();
    return config->get_printer_display_name(config->get_active_printer_id(), fallback);
}

} // namespace helix
