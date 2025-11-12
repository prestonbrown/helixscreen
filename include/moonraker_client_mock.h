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

#ifndef MOONRAKER_CLIENT_MOCK_H
#define MOONRAKER_CLIENT_MOCK_H

#include "moonraker_client.h"
#include <string>
#include <vector>

/**
 * @brief Mock Moonraker client for testing without real printer connection
 *
 * Simulates printer hardware discovery with configurable test data.
 * Useful for UI development and testing without physical hardware.
 *
 * Inherits from MoonrakerClient to provide drop-in replacement compatibility.
 * Overrides discover_printer() to populate test data without WebSocket connection.
 */
class MoonrakerClientMock : public MoonrakerClient {
public:
    enum class PrinterType {
        VORON_24,           // Voron 2.4 (CoreXY, chamber heating)
        VORON_TRIDENT,      // Voron Trident (3Z, CoreXY)
        CREALITY_K1,        // Creality K1/K1 Max (bed slinger style)
        FLASHFORGE_AD5M,    // FlashForge Adventurer 5M (enclosed)
        GENERIC_COREXY,     // Generic CoreXY printer
        GENERIC_BEDSLINGER, // Generic i3-style printer
        MULTI_EXTRUDER      // Multi-extruder test case (2 extruders)
    };

    MoonrakerClientMock(PrinterType type = PrinterType::VORON_24);
    ~MoonrakerClientMock() = default;

    /**
     * @brief Simulate WebSocket connection (no real network I/O)
     *
     * Overrides base class to simulate successful connection without
     * actual WebSocket establishment. Immediately invokes on_connected callback.
     *
     * @param url WebSocket URL (ignored in mock)
     * @param on_connected Callback invoked immediately
     * @param on_disconnected Callback stored but never invoked in mock
     * @return Always returns 0 (success)
     */
    int connect(const char* url,
                std::function<void()> on_connected,
                std::function<void()> on_disconnected) override;

    /**
     * @brief Simulate printer hardware discovery
     *
     * Overrides base class method to immediately populate hardware lists
     * based on configured printer type and invoke completion callback.
     *
     * @param on_complete Callback invoked after discovery completes
     */
    void discover_printer(std::function<void()> on_complete) override;

    /**
     * @brief Simulate WebSocket disconnection (no real network I/O)
     *
     * Overrides base class to simulate disconnection without actual WebSocket teardown.
     */
    void disconnect() override;

    /**
     * @brief Simulate JSON-RPC request without parameters
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param method RPC method name
     * @return Always returns 0 (success)
     */
    int send_jsonrpc(const std::string& method) override;

    /**
     * @brief Simulate JSON-RPC request with parameters
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param method RPC method name
     * @param params JSON parameters object (ignored in mock)
     * @return Always returns 0 (success)
     */
    int send_jsonrpc(const std::string& method, const json& params) override;

    /**
     * @brief Simulate JSON-RPC request with callback
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param method RPC method name
     * @param params JSON parameters object (ignored in mock)
     * @param cb Callback function (not invoked in mock)
     * @return Always returns 0 (success)
     */
    int send_jsonrpc(const std::string& method,
                     const json& params,
                     std::function<void(json)> cb) override;

    /**
     * @brief Simulate JSON-RPC request with success/error callbacks
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param method RPC method name
     * @param params JSON parameters object (ignored in mock)
     * @param success_cb Success callback (not invoked in mock)
     * @param error_cb Error callback (not invoked in mock)
     * @param timeout_ms Timeout (ignored in mock)
     * @return Always returns 0 (success)
     */
    int send_jsonrpc(const std::string& method,
                     const json& params,
                     std::function<void(json)> success_cb,
                     std::function<void(const MoonrakerError&)> error_cb,
                     uint32_t timeout_ms = 0) override;

    /**
     * @brief Simulate G-code script command
     *
     * Overrides base class to log and return success without network I/O.
     *
     * @param gcode G-code string
     * @return Always returns 0 (success)
     */
    int gcode_script(const std::string& gcode) override;

    /**
     * @brief Set printer type for mock data generation
     *
     * @param type Printer type to simulate
     */
    void set_printer_type(PrinterType type) { printer_type_ = type; }

private:
    /**
     * @brief Populate hardware lists based on configured printer type
     *
     * Directly modifies the protected member variables inherited from
     * MoonrakerClient (heaters_, sensors_, fans_, leds_).
     */
    void populate_hardware();

private:
    PrinterType printer_type_;
};

#endif // MOONRAKER_CLIENT_MOCK_H
