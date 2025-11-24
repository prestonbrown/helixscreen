// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_inspector.cpp
 * @brief Standalone diagnostic tool for querying Moonraker printer metadata
 *
 * Usage: moonraker_inspector <ip_address> [port]
 * Example: moonraker_inspector 192.168.1.100 7125
 *
 * Connects to a Moonraker instance and dumps all hardware/metadata:
 * - Server info (Moonraker/Klippy versions, components)
 * - Printer info (hostname, state, software version)
 * - Discovered objects (heaters, sensors, fans, LEDs)
 * - Configuration details
 */

#include "moonraker_client.h"
#include "ansi_colors.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>

using json = nlohmann::json;

// Global flag for color support (auto-detected or disabled via --no-color)
static bool use_colors = true;

// Simple state machine for async queries
struct InspectorState {
    bool connected = false;
    bool discovery_complete = false;
    bool server_info_received = false;
    bool printer_info_received = false;
    bool objects_received = false;

    json server_info;
    json printer_info;
    json objects_list;

    std::string error_message;
};

static InspectorState state;

void print_header(const std::string& title) {
    if (use_colors) {
        std::cout << "\n" << ansi::BOLD << ansi::BRIGHT_CYAN;
        std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║ " << std::left << std::setw(62) << title << " ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════╝";
        std::cout << ansi::RESET << "\n";
    } else {
        std::cout << "\n";
        std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║ " << std::left << std::setw(62) << title << " ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
    }
}

void print_section(const std::string& title) {
    if (use_colors) {
        std::cout << "\n" << ansi::BOLD << ansi::CYAN << "┌─ " << title << ansi::RESET << "\n";
    } else {
        std::cout << "\n┌─ " << title << "\n";
    }
}

void print_kv(const std::string& key, const std::string& value, int indent = 0) {
    std::string prefix(indent * 2, ' ');
    if (use_colors) {
        std::cout << prefix << "  " << ansi::BRIGHT_BLUE << std::left << std::setw(30 - indent * 2) << key
                  << ansi::RESET << ": " << ansi::WHITE << value << ansi::RESET << "\n";
    } else {
        std::cout << prefix << "  " << std::left << std::setw(30 - indent * 2) << key
                  << ": " << value << "\n";
    }
}

void print_list_item(const std::string& item, int indent = 0) {
    std::string prefix(indent * 2, ' ');
    std::cout << prefix << "  • " << item << "\n";
}

void print_server_info(const json& info) {
    print_section("Server Information");

    if (info.contains("klippy_connected")) {
        bool connected = info["klippy_connected"].get<bool>();
        std::string status;
        if (use_colors) {
            status = connected ? ansi::success("Connected ✓") : ansi::error("Disconnected ✗");
        } else {
            status = connected ? "Connected ✓" : "Disconnected ✗";
        }
        print_kv("Klippy Status", status);
    }

    if (info.contains("klippy_state")) {
        print_kv("Klippy State", info["klippy_state"].get<std::string>());
    }

    if (info.contains("moonraker_version")) {
        print_kv("Moonraker Version", info["moonraker_version"].get<std::string>());
    }

    if (info.contains("klippy_version")) {
        print_kv("Klippy Version", info["klippy_version"].get<std::string>());
    }

    if (info.contains("api_version")) {
        auto api = info["api_version"];
        std::string api_str = "[" + std::to_string(api[0].get<int>()) + "." +
                             std::to_string(api[1].get<int>()) + "." +
                             std::to_string(api[2].get<int>()) + "]";
        print_kv("API Version", api_str);
    }

    if (info.contains("components")) {
        std::cout << "\n  Components:\n";
        for (const auto& comp : info["components"]) {
            print_list_item(comp.get<std::string>(), 1);
        }
    }

    if (info.contains("warnings")) {
        std::cout << "\n  ⚠️  Warnings:\n";
        for (const auto& warning : info["warnings"]) {
            print_list_item(warning.get<std::string>(), 1);
        }
    }
}

void print_printer_info(const json& info) {
    print_section("Printer Information");

    if (info.contains("state")) {
        std::string state_str = info["state"].get<std::string>();
        std::string display;
        if (use_colors) {
            display = (state_str == "ready") ?
                      ansi::success(state_str + " ✓") :
                      ansi::warning(state_str + " ⚠");
        } else {
            display = state_str + ((state_str == "ready") ? " ✓" : " ⚠");
        }
        print_kv("State", display);
    }

    if (info.contains("state_message")) {
        std::string msg = info["state_message"].get<std::string>();
        if (!msg.empty()) {
            print_kv("State Message", msg);
        }
    }

    if (info.contains("hostname")) {
        print_kv("Hostname", info["hostname"].get<std::string>());
    }

    if (info.contains("software_version")) {
        print_kv("Klipper Version", info["software_version"].get<std::string>());
    }

    if (info.contains("cpu_info")) {
        print_kv("CPU Info", info["cpu_info"].get<std::string>());
    }

    if (info.contains("python_version")) {
        print_kv("Python Version", info["python_version"].get<std::string>());
    }
}

void print_hardware_objects(const json& objects) {
    print_section("Discovered Hardware Objects");

    if (!objects.contains("objects")) {
        std::cout << "  No objects found\n";
        return;
    }

    const auto& obj_array = objects["objects"];

    // Categorize objects
    std::vector<std::string> heaters, sensors, fans, leds, other;

    for (const auto& obj : obj_array) {
        std::string name = obj.get<std::string>();

        if (name.find("extruder") != std::string::npos ||
            name.find("heater_bed") != std::string::npos ||
            name.find("heater_generic") != std::string::npos) {
            heaters.push_back(name);
        } else if (name.find("temperature_sensor") != std::string::npos ||
                   name.find("temperature_") != std::string::npos) {
            sensors.push_back(name);
        } else if (name.find("fan") != std::string::npos) {
            fans.push_back(name);
        } else if (name.find("led") != std::string::npos ||
                   name.find("neopixel") != std::string::npos ||
                   name.find("dotstar") != std::string::npos) {
            leds.push_back(name);
        } else {
            other.push_back(name);
        }
    }

    // Print categorized objects
    if (!heaters.empty()) {
        std::cout << "\n  Heaters (" << heaters.size() << "):\n";
        for (const auto& h : heaters) {
            print_list_item(h, 1);
        }
    }

    if (!sensors.empty()) {
        std::cout << "\n  Temperature Sensors (" << sensors.size() << "):\n";
        for (const auto& s : sensors) {
            print_list_item(s, 1);
        }
    }

    if (!fans.empty()) {
        std::cout << "\n  Fans (" << fans.size() << "):\n";
        for (const auto& f : fans) {
            print_list_item(f, 1);
        }
    }

    if (!leds.empty()) {
        std::cout << "\n  LEDs (" << leds.size() << "):\n";
        for (const auto& l : leds) {
            print_list_item(l, 1);
        }
    }

    if (!other.empty()) {
        std::cout << "\n  Other Objects (" << other.size() << "):\n";
        for (const auto& o : other) {
            print_list_item(o, 1);
        }
    }

    std::cout << "\n  Total Objects: " << obj_array.size() << "\n";
}

int main(int argc, char** argv) {
    // Auto-detect TTY for color support
    use_colors = ansi::is_tty();

    // Parse command line
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <ip_address> [port] [--no-color]\n";
        std::cerr << "Example: " << argv[0] << " 192.168.1.100 7125\n";
        std::cerr << "\nOptions:\n";
        std::cerr << "  --no-color    Disable colored output\n";
        return 1;
    }

    std::string ip = argv[1];
    int port = 7125;

    // Parse remaining arguments
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-color" || arg == "--no-colour") {
            use_colors = false;
        } else if (arg[0] >= '0' && arg[0] <= '9') {
            port = std::atoi(arg.c_str());
        }
    }

    std::string url = "ws://" + ip + ":" + std::to_string(port) + "/websocket";

    // Configure logging
    spdlog::set_level(spdlog::level::warn); // Only show warnings/errors

    print_header("Moonraker Inspector");
    std::cout << "Target: " << ip << ":" << port << "\n";
    std::cout << "WebSocket URL: " << url << "\n";

    // Create Moonraker client
    MoonrakerClient client;

    // Configure timeouts
    client.configure_timeouts(5000, 10000, 10000, 200, 2000);

    // Set up connection callbacks
    std::cout << "\nConnecting";
    std::cout.flush();

    auto on_connect = [&]() {
        state.connected = true;
        std::cout << " ✓\n";

        // Query 1: Server info
        client.send_jsonrpc("server.info", json::object(),
            [&](json response) {
                if (response.contains("result")) {
                    state.server_info = response["result"];
                    state.server_info_received = true;
                }
            },
            [&](const MoonrakerError& error) {
                state.error_message = "server.info failed: " + error.message;
            });

        // Query 2: Printer info
        client.send_jsonrpc("printer.info", json::object(),
            [&](json response) {
                if (response.contains("result")) {
                    state.printer_info = response["result"];
                    state.printer_info_received = true;
                }
            },
            [&](const MoonrakerError& error) {
                state.error_message = "printer.info failed: " + error.message;
            });

        // Query 3: Objects list
        client.send_jsonrpc("printer.objects.list", json::object(),
            [&](json response) {
                if (response.contains("result")) {
                    state.objects_list = response["result"];
                    state.objects_received = true;
                    state.discovery_complete = true;
                }
            },
            [&](const MoonrakerError& error) {
                state.error_message = "printer.objects.list failed: " + error.message;
                state.discovery_complete = true;
            });
    };

    auto on_disconnect = [&]() {
        if (!state.connected) {
            std::cout << " ✗\n";
            std::cerr << "\nError: Failed to connect to " << url << "\n";
            std::cerr << "Check that:\n";
            std::cerr << "  1. The IP address is correct\n";
            std::cerr << "  2. Moonraker is running on the target machine\n";
            std::cerr << "  3. Port " << port << " is not blocked by firewall\n";
        }
    };

    // Connect
    int result = client.connect(url.c_str(), on_connect, on_disconnect);
    if (result != 0) {
        std::cerr << "Failed to initiate connection (error code: " << result << ")\n";
        return 1;
    }

    // Wait for discovery to complete (max 10 seconds)
    auto start = std::chrono::steady_clock::now();
    while (!state.discovery_complete) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed > 10) {
            std::cerr << "\nTimeout: No response from Moonraker after 10 seconds\n";
            return 1;
        }

        // Animated waiting
        if (elapsed % 1 == 0) {
            std::cout << ".";
            std::cout.flush();
        }
    }

    // Check for errors
    if (!state.error_message.empty()) {
        std::cerr << "\n\nError: " << state.error_message << "\n";
        return 1;
    }

    // Print results
    if (state.server_info_received) {
        print_server_info(state.server_info);
    }

    if (state.printer_info_received) {
        print_printer_info(state.printer_info);
    }

    if (state.objects_received) {
        print_hardware_objects(state.objects_list);
    }

    std::cout << "\n";
    print_header("Inspection Complete");

    return 0;
}
