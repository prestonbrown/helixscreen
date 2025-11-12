// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

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

#include "moonraker_client_mock.h"
#include <spdlog/spdlog.h>

MoonrakerClientMock::MoonrakerClientMock(PrinterType type)
    : printer_type_(type) {
    spdlog::info("[MoonrakerClientMock] Created with printer type: {}",
                static_cast<int>(type));
}

int MoonrakerClientMock::connect(const char* url,
                                 std::function<void()> on_connected,
                                 std::function<void()> on_disconnected) {
    spdlog::info("[MoonrakerClientMock] Simulating connection to: {}", url ? url : "(null)");

    // Immediately invoke connection callback (no async delay in mock)
    if (on_connected) {
        spdlog::info("[MoonrakerClientMock] Simulated connection successful");
        on_connected();
    }

    // Store disconnect callback (never invoked in mock, but stored for consistency)
    // Note: Not needed for this simple mock implementation

    return 0;  // Success
}

void MoonrakerClientMock::discover_printer(std::function<void()> on_complete) {
    spdlog::info("[MoonrakerClientMock] Simulating hardware discovery");

    // Populate hardware based on printer type
    populate_hardware();

    // Log discovered hardware
    spdlog::info("[MoonrakerClientMock] Discovered: {} heaters, {} sensors, {} fans, {} LEDs",
                heaters_.size(), sensors_.size(), fans_.size(), leds_.size());

    // Invoke completion callback immediately (no async delay in mock)
    if (on_complete) {
        on_complete();
    }
}

void MoonrakerClientMock::populate_hardware() {
    // Clear existing data (inherited from MoonrakerClient)
    heaters_.clear();
    sensors_.clear();
    fans_.clear();
    leds_.clear();

    // Populate based on printer type
    switch (printer_type_) {
        case PrinterType::VORON_24:
            // Voron 2.4 configuration
            heaters_ = {
                "heater_bed",
                "extruder"
            };
            sensors_ = {
                "temperature_sensor chamber",
                "temperature_sensor raspberry_pi",
                "temperature_sensor mcu_temp"
            };
            fans_ = {
                "heater_fan hotend_fan",
                "fan",  // Part cooling fan
                "fan_generic nevermore",
                "controller_fan controller_fan"
            };
            leds_ = {
                "neopixel chamber_light",
                "neopixel status_led"
            };
            break;

        case PrinterType::VORON_TRIDENT:
            // Voron Trident configuration
            heaters_ = {
                "heater_bed",
                "extruder"
            };
            sensors_ = {
                "temperature_sensor chamber",
                "temperature_sensor raspberry_pi",
                "temperature_sensor mcu_temp",
                "temperature_sensor z_thermal_adjust"
            };
            fans_ = {
                "heater_fan hotend_fan",
                "fan",
                "fan_generic exhaust_fan",
                "controller_fan electronics_fan"
            };
            leds_ = {
                "neopixel sb_leds",
                "neopixel chamber_leds"
            };
            break;

        case PrinterType::CREALITY_K1:
            // Creality K1/K1 Max configuration
            heaters_ = {
                "heater_bed",
                "extruder"
            };
            sensors_ = {
                "temperature_sensor mcu_temp",
                "temperature_sensor host_temp"
            };
            fans_ = {
                "heater_fan hotend_fan",
                "fan",
                "fan_generic auxiliary_fan"
            };
            leds_ = {
                "neopixel logo_led"
            };
            break;

        case PrinterType::FLASHFORGE_AD5M:
            // FlashForge Adventurer 5M configuration
            heaters_ = {
                "heater_bed",
                "extruder"
            };
            sensors_ = {
                "temperature_sensor chamber",
                "temperature_sensor mcu_temp"
            };
            fans_ = {
                "heater_fan hotend_fan",
                "fan",
                "fan_generic chamber_fan"
            };
            leds_ = {
                "led chamber_light"
            };
            break;

        case PrinterType::GENERIC_COREXY:
            // Generic CoreXY printer
            heaters_ = {
                "heater_bed",
                "extruder"
            };
            sensors_ = {
                "temperature_sensor raspberry_pi"
            };
            fans_ = {
                "heater_fan hotend_fan",
                "fan"
            };
            leds_ = {};
            break;

        case PrinterType::GENERIC_BEDSLINGER:
            // Generic i3-style bedslinger
            heaters_ = {
                "heater_bed",
                "extruder"
            };
            sensors_ = {};
            fans_ = {
                "heater_fan hotend_fan",
                "fan"
            };
            leds_ = {};
            break;

        case PrinterType::MULTI_EXTRUDER:
            // Multi-extruder test case
            heaters_ = {
                "heater_bed",
                "extruder",
                "extruder1"
            };
            sensors_ = {
                "temperature_sensor chamber",
                "temperature_sensor mcu_temp"
            };
            fans_ = {
                "heater_fan hotend_fan",
                "heater_fan hotend_fan1",
                "fan",
                "fan_generic exhaust_fan"
            };
            leds_ = {
                "neopixel chamber_light"
            };
            break;
    }

    spdlog::debug("[MoonrakerClientMock] Populated hardware:");
    for (const auto& h : heaters_) spdlog::debug("  Heater: {}", h);
    for (const auto& s : sensors_) spdlog::debug("  Sensor: {}", s);
    for (const auto& f : fans_) spdlog::debug("  Fan: {}", f);
    for (const auto& l : leds_) spdlog::debug("  LED: {}", l);
}

void MoonrakerClientMock::disconnect() {
    spdlog::info("[MoonrakerClientMock] Simulating disconnection (no-op)");
    // Mock does nothing for disconnect - no real connection to tear down
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method) {
    spdlog::debug("[MoonrakerClientMock] Mock send_jsonrpc: {}", method);
    return 0;  // Success
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method, const json& params) {
    spdlog::debug("[MoonrakerClientMock] Mock send_jsonrpc: {} (with params)", method);
    return 0;  // Success
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method,
                                       const json& params,
                                       std::function<void(json)> cb) {
    spdlog::debug("[MoonrakerClientMock] Mock send_jsonrpc: {} (with callback)", method);
    // Note: callback is not invoked in mock
    return 0;  // Success
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method,
                                       const json& params,
                                       std::function<void(json)> success_cb,
                                       std::function<void(const MoonrakerError&)> error_cb,
                                       uint32_t timeout_ms) {
    spdlog::debug("[MoonrakerClientMock] Mock send_jsonrpc: {} (with success/error callbacks)", method);
    // Note: callbacks are not invoked in mock
    return 0;  // Success
}

int MoonrakerClientMock::gcode_script(const std::string& gcode) {
    spdlog::debug("[MoonrakerClientMock] Mock gcode_script: {}", gcode);
    return 0;  // Success
}
