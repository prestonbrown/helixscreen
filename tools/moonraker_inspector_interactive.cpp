// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_inspector_interactive.cpp
 * @brief Interactive TUI mode for moonraker inspector with collapsible tree
 *
 * Features:
 * - Arrow keys to navigate sections
 * - Enter/Space to expand/collapse sections
 * - Color-coded status indicators
 * - Real-time data display
 *
 * Built with cpp-terminal - modern C++ TUI library
 */

#include "moonraker_client_cli.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <json.hpp> // nlohmann/json from libhv
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cpp-terminal/color.hpp"
#include "cpp-terminal/exception.hpp"
#include "cpp-terminal/input.hpp"
#include "cpp-terminal/iostream.hpp"
#include "cpp-terminal/key.hpp"
#include "cpp-terminal/options.hpp"
#include "cpp-terminal/screen.hpp"
#include "cpp-terminal/style.hpp"
#include "cpp-terminal/terminal.hpp"
#include "cpp-terminal/tty.hpp"

using json = nlohmann::json;
using helix::cli::MoonrakerClient;
using helix::cli::MoonrakerError;

// Tree node for hierarchical data display
struct TreeNode {
    std::string key;
    std::string value;
    bool expanded;
    bool is_section; // Section headers vs data items
    int indent_level;
    std::string object_name; // Moonraker object name for querying
    json object_data;        // Detailed data from Moonraker
    bool data_fetched;       // Have we fetched detailed data?
    std::vector<TreeNode> children;

    TreeNode(const std::string& k, const std::string& v = "", bool section = false, int indent = 0,
             const std::string& obj_name = "")
        : key(k), value(v), expanded(false), is_section(section), indent_level(indent),
          object_name(obj_name), data_fetched(false) {}
};

// Global state for interactive mode
struct InteractiveState {
    std::vector<TreeNode> tree;
    size_t selected_index;
    json server_info;
    json printer_info;
    json objects_list;
    bool data_ready;
    MoonrakerClient* client; // For querying object details
    TreeNode* selected_node; // Track actual selected node (not just index)
    bool need_redraw;        // Flag to trigger redraw from async callbacks
    size_t spinner_frame;    // For animated spinner
    int pending_queries;     // Track pending async queries

    InteractiveState()
        : selected_index(0), data_ready(false), client(nullptr), selected_node(nullptr),
          need_redraw(false), spinner_frame(0), pending_queries(0) {}
};

static InteractiveState* g_state = nullptr;

// Format a scalar value (not array/object)
std::string format_scalar(const json& val) {
    if (val.is_number()) {
        if (val.is_number_float()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f", val.get<double>());
            return buf;
        }
        return std::to_string(val.get<int>());
    } else if (val.is_boolean()) {
        return val.get<bool>() ? "true" : "false";
    } else if (val.is_string()) {
        return val.get<std::string>();
    } else if (val.is_null()) {
        return "null";
    }
    return "?";
}

// Create tree nodes recursively for complex data structures
void add_json_to_tree(TreeNode* parent, const std::string& key, const json& val, int indent) {
    if (val.is_array()) {
        size_t size = val.size();
        if (size == 0) {
            parent->children.push_back(TreeNode(key, "[]", false, indent));
        } else if (size <= 3 && size > 0 && val[0].is_number()) {
            // Show small numeric arrays inline (not expandable)
            std::string result = "[";
            for (size_t i = 0; i < size; i++) {
                if (i > 0)
                    result += ", ";
                result += format_scalar(val[i]);
            }
            result += "]";
            parent->children.push_back(TreeNode(key, result, false, indent));
        } else {
            // Create expandable array node
            char summary[64];
            snprintf(summary, sizeof(summary), "[%zu items]", size);
            TreeNode array_node(key, summary, true, indent);

            // Add array items as children
            for (size_t i = 0; i < size; i++) {
                std::string item_key = "[" + std::to_string(i) + "]";
                add_json_to_tree(&array_node, item_key, val[i], indent + 1);
            }

            parent->children.push_back(array_node);
        }
    } else if (val.is_object()) {
        size_t size = val.size();
        if (size == 0) {
            parent->children.push_back(TreeNode(key, "{}", false, indent));
        } else {
            // Create expandable object node
            char summary[64];
            snprintf(summary, sizeof(summary), "{%zu fields}", size);
            TreeNode obj_node(key, summary, true, indent);

            // Add object fields as children
            for (auto it = val.begin(); it != val.end(); ++it) {
                add_json_to_tree(&obj_node, it.key(), it.value(), indent + 1);
            }

            parent->children.push_back(obj_node);
        }
    } else {
        // Scalar value - not expandable
        parent->children.push_back(TreeNode(key, format_scalar(val), false, indent));
    }
}

// Query Moonraker for detailed object data
void query_object_data(TreeNode* node, MoonrakerClient* client) {
    if (!node || node->object_name.empty() || node->data_fetched || !client) {
        return;
    }

    spdlog::debug("Querying Moonraker for object: {}", node->object_name);

    // Add loading indicator with spinner
    node->children.clear();
    node->children.push_back(TreeNode("Loading data...", "", false, 3));
    if (g_state) {
        g_state->need_redraw = true;
        g_state->pending_queries++; // Track pending query
    }

    // Query this specific object
    json params = json::object();
    params["objects"] = json::object();
    params["objects"][node->object_name] = json::value_t::null; // Query all fields

    client->send_jsonrpc(
        "printer.objects.query", params,
        [node](json response) {
            if (g_state) {
                g_state->pending_queries--; // Query completed
            }
            spdlog::debug("Received response for object: {}", node->object_name);
            spdlog::debug("Response JSON: {}", response.dump());

            if (response.contains("result") && response["result"].contains("status")) {
                node->object_data = response["result"]["status"];
                node->data_fetched = true;

                spdlog::debug("Status data: {}", node->object_data.dump());

                // Clear loading indicator and populate with detailed data
                node->children.clear();

                if (node->object_data.contains(node->object_name)) {
                    const auto& obj_data = node->object_data[node->object_name];
                    spdlog::debug("Found object data for '{}', has {} fields", node->object_name,
                                  obj_data.size());

                    // Add each field as a child (using recursive tree builder)
                    for (auto it = obj_data.begin(); it != obj_data.end(); ++it) {
                        std::string key = it.key();
                        const auto& val = it.value();

                        // Add descriptive labels and units for common fields
                        std::string display_key = key;
                        json modified_val = val;
                        bool add_unit = false;
                        std::string unit;

                        if (key == "temperature") {
                            display_key = "🌡️  Current Temp";
                            add_unit = true;
                            unit = "°C";
                        } else if (key == "target") {
                            display_key = "🎯 Target Temp";
                            add_unit = true;
                            unit = "°C";
                        } else if (key == "power" && val.is_number()) {
                            display_key = "⚡ Heater Power";
                            double pct = val.get<double>() * 100.0;
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%.1f%%", pct);
                            node->children.push_back(TreeNode(display_key, buf, false, 3));
                            continue;
                        } else if (key == "speed" && val.is_number()) {
                            display_key = "💨 Fan Speed";
                            double pct = val.get<double>() * 100.0;
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%.0f%%", pct);
                            node->children.push_back(TreeNode(display_key, buf, false, 3));
                            continue;
                        } else if (key == "rpm") {
                            display_key = "🔄 RPM";
                        } else if (key == "run_current") {
                            display_key = "⚡ Run Current";
                            add_unit = true;
                            unit = "A";
                        } else if (key == "hold_current") {
                            display_key = "⏸️  Hold Current";
                            add_unit = true;
                            unit = "A";
                        } else if (key == "microsteps") {
                            display_key = "📐 Microsteps";
                        }

                        // For simple scalars with units, add inline
                        if (add_unit && val.is_number()) {
                            std::string value = format_scalar(val) + unit;
                            node->children.push_back(TreeNode(display_key, value, false, 3));
                        } else {
                            // Use recursive tree builder for everything else
                            add_json_to_tree(node, display_key, val, 3);
                        }
                    }

                    spdlog::debug("Total children added: {}", node->children.size());
                } else {
                    spdlog::debug("Object name '{}' NOT found in status data. Available keys: {}",
                                  node->object_name, node->object_data.dump());
                }

                // Trigger redraw to show new data
                if (g_state) {
                    spdlog::debug("Setting need_redraw flag");
                    g_state->need_redraw = true;
                } else {
                    spdlog::debug("WARNING: g_state is null, cannot trigger redraw!");
                }
            } else {
                spdlog::debug("Response doesn't contain result.status");
            }
        },
        [node](const MoonrakerError&) {
            if (g_state) {
                g_state->pending_queries--; // Query completed (with error)
            }
            // Query failed - show error
            node->children.clear();
            node->children.push_back(TreeNode("❌ Failed to fetch data", "", false, 3));
            if (g_state) {
                g_state->need_redraw = true;
            }
        });
}

// Get human-readable description for a Moonraker component
std::string get_component_description(const std::string& component) {
    if (component == "file_manager") {
        return "Manages G-code files and print job queue";
    } else if (component == "update_manager") {
        return "Handles software updates for Moonraker/Klipper/system";
    } else if (component == "machine") {
        return "System info, power control, and service management";
    } else if (component == "webcam") {
        return "Manages webcam streams for print monitoring";
    } else if (component == "history") {
        return "Tracks print history and statistics";
    } else if (component == "authorization") {
        return "Handles API authentication and user permissions";
    } else if (component == "data_store") {
        return "Persistent storage for UI settings and preferences";
    } else if (component == "announcements") {
        return "News and important updates from Moonraker project";
    } else if (component == "octoprint_compat") {
        return "Compatibility layer for OctoPrint plugins/slicers";
    } else if (component == "job_queue") {
        return "Sequential print job queue management";
    } else if (component == "job_state") {
        return "Tracks current print job state and progress";
    } else if (component == "proc_stats") {
        return "System resource monitoring (CPU/memory/disk)";
    } else if (component == "klippy_apis") {
        return "API endpoints for Klipper communication";
    } else if (component == "database") {
        return "Internal database for configuration storage";
    } else if (component == "http_client") {
        return "HTTP client for external requests (updates/notifications)";
    } else if (component == "secrets") {
        return "Secure storage for API keys and credentials";
    } else if (component == "template") {
        return "Jinja2 template processing for dynamic configs";
    } else if (component == "klippy_connection") {
        return "WebSocket connection manager to Klipper";
    } else if (component == "jsonrpc") {
        return "JSON-RPC protocol handler for API requests";
    } else if (component == "internal_transport") {
        return "Internal IPC between Moonraker components";
    } else if (component == "application") {
        return "Core application framework and lifecycle";
    } else if (component == "websockets") {
        return "WebSocket server for realtime client connections";
    } else if (component == "dbus_manager") {
        return "DBus integration for system service control";
    } else if (component == "shell_command") {
        return "Execute shell commands from G-code macros";
    } else if (component == "extensions") {
        return "Third-party plugin extension system";
    }
    return "";
}

// Get human-readable description for a Klipper object
std::string get_object_description(const std::string& obj_name) {
    if (obj_name.find("extruder") != std::string::npos) {
        return "Hotend extruder - heats plastic and pushes filament";
    } else if (obj_name.find("heater_bed") != std::string::npos) {
        return "Heated print bed - keeps prints from warping";
    } else if (obj_name.find("heater_generic") != std::string::npos) {
        return "Generic heater - chamber/other heating element";
    } else if (obj_name.find("temperature_sensor") != std::string::npos) {
        return "Temperature sensor - monitors ambient/component temps";
    } else if (obj_name.find("fan") != std::string::npos) {
        if (obj_name.find("heater_fan") != std::string::npos) {
            return "Heater fan - cools hotend/heatbreak";
        } else if (obj_name.find("controller_fan") != std::string::npos) {
            return "Controller fan - cools MCU/stepper drivers";
        } else if (obj_name.find("fan_generic") != std::string::npos) {
            return "Generic fan - chamber/auxiliary cooling";
        } else {
            return "Part cooling fan - cools printed plastic";
        }
    } else if (obj_name.find("led") != std::string::npos ||
               obj_name.find("neopixel") != std::string::npos) {
        return "LED strip - lighting/status indication";
    } else if (obj_name.find("tmc") != std::string::npos) {
        return "TMC stepper driver - silent motor control with stallguard";
    } else if (obj_name.find("stepper_") != std::string::npos) {
        return "Stepper motor - controls axis movement";
    } else if (obj_name.find("probe") != std::string::npos) {
        return "Z-probe - measures bed height for leveling";
    } else if (obj_name.find("bltouch") != std::string::npos) {
        return "BLTouch probe - servo-based bed leveling sensor";
    } else if (obj_name.find("bed_mesh") != std::string::npos) {
        return "Bed mesh - compensates for uneven bed surface";
    } else if (obj_name.find("filament_switch_sensor") != std::string::npos) {
        return "Filament sensor - detects filament runout";
    } else if (obj_name.find("filament_motion_sensor") != std::string::npos) {
        return "Filament motion sensor - detects jams/clogs";
    } else if (obj_name.find("servo") != std::string::npos) {
        return "Servo motor - precise angular positioning";
    } else if (obj_name.find("gcode_macro") != std::string::npos) {
        return "G-code macro - custom print command";
    } else if (obj_name.find("gcode_button") != std::string::npos) {
        return "Physical button - triggers G-code commands";
    } else if (obj_name.find("firmware_retraction") != std::string::npos) {
        return "Firmware retraction - fast filament retract/prime";
    }
    return ""; // No description
}

// Build tree from collected data (all sections collapsed by default)
void build_tree(InteractiveState* state) {
    state->tree.clear();

    // Server Information section (collapsed by default)
    TreeNode server_section("📡 Server Information", "", true, 0);
    server_section.expanded = false; // Collapsed by default

    if (state->server_info.contains("klippy_connected")) {
        bool connected = state->server_info["klippy_connected"].get<bool>();
        std::string status = connected ? "Connected ✓" : "Disconnected ✗";
        server_section.children.push_back(TreeNode("Klippy Status", status, false, 1));
    }

    if (state->server_info.contains("klippy_state")) {
        server_section.children.push_back(TreeNode(
            "Klippy State", state->server_info["klippy_state"].get<std::string>(), false, 1));
    }

    // Moonraker version field is actually "version" not "moonraker_version"
    if (state->server_info.contains("version")) {
        server_section.children.push_back(TreeNode(
            "Moonraker Version", state->server_info["version"].get<std::string>(), false, 1));
    } else if (state->server_info.contains("moonraker_version")) {
        server_section.children.push_back(
            TreeNode("Moonraker Version",
                     state->server_info["moonraker_version"].get<std::string>(), false, 1));
    }

    if (state->server_info.contains("klippy_version")) {
        server_section.children.push_back(TreeNode(
            "Klippy Version", state->server_info["klippy_version"].get<std::string>(), false, 1));
    }

    if (state->server_info.contains("components")) {
        TreeNode comp_node("🧩 Components (Moonraker Modules)", "", true, 1);
        comp_node.expanded = false; // Collapsible subsection
        for (const auto& comp : state->server_info["components"]) {
            std::string comp_name = comp.get<std::string>();
            std::string desc = get_component_description(comp_name);
            comp_node.children.push_back(TreeNode(comp_name, desc, false, 2));
        }
        server_section.children.push_back(comp_node);
    }

    state->tree.push_back(server_section);

    // Printer Information section (collapsed by default)
    TreeNode printer_section("🖨️  Printer Information", "", true, 0);
    printer_section.expanded = false;

    if (state->printer_info.contains("state")) {
        std::string state_str = state->printer_info["state"].get<std::string>();
        printer_section.children.push_back(TreeNode("State", state_str, false, 1));
    }

    if (state->printer_info.contains("hostname")) {
        printer_section.children.push_back(
            TreeNode("Hostname", state->printer_info["hostname"].get<std::string>(), false, 1));
    }

    // Check multiple possible field names for Klipper version
    if (state->printer_info.contains("software_version")) {
        printer_section.children.push_back(
            TreeNode("Klipper Version", state->printer_info["software_version"].get<std::string>(),
                     false, 1));
    } else if (state->printer_info.contains("klipper_version")) {
        printer_section.children.push_back(
            TreeNode("Klipper Version", state->printer_info["klipper_version"].get<std::string>(),
                     false, 1));
    }

    state->tree.push_back(printer_section);

    // Hardware Objects section
    if (state->objects_list.contains("objects")) {
        TreeNode hw_section("🔧 Hardware Objects", "", true, 0);
        hw_section.expanded = false; // Collapsed by default

        const auto& obj_array = state->objects_list["objects"];

        // Categorize objects (more detailed categorization)
        std::vector<std::string> heaters, sensors, fans, leds, macros, steppers, probes, other;

        for (const auto& obj : obj_array) {
            std::string name = obj.get<std::string>();

            // Check TMC/stepper FIRST before checking for extruder
            // (to avoid "tmc2209 extruder" being categorized as heater)
            if (name.find("stepper") != std::string::npos ||
                name.find("tmc") != std::string::npos) {
                steppers.push_back(name);
            } else if (name.find("gcode_macro") != std::string::npos) {
                macros.push_back(name);
            } else if (name.find("extruder") != std::string::npos ||
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
            } else if (name.find("probe") != std::string::npos ||
                       name.find("bltouch") != std::string::npos ||
                       name.find("bed_mesh") != std::string::npos) {
                probes.push_back(name);
            } else if (name == "gcode" || name == "webhooks" || name == "configfile" ||
                       name == "mcu" || name.find("mcu ") == 0 || name == "heaters" ||
                       name == "gcode_move" || name == "print_stats" || name == "virtual_sdcard" ||
                       name == "display_status" || name == "exclude_object" ||
                       name == "idle_timeout" || name == "pause_resume") {
                // Core Klipper objects - not interesting to expand
                continue;
            } else {
                other.push_back(name);
            }
        }

        // Add categorized subsections (all collapsed by default)
        if (!heaters.empty()) {
            TreeNode heater_node("🔥 Heaters (" + std::to_string(heaters.size()) + ")", "", true,
                                 1);
            heater_node.expanded = false;
            for (const auto& h : heaters) {
                std::string desc = get_object_description(h);
                heater_node.children.push_back(
                    TreeNode(h, desc, true, 2, h)); // Expandable, store object name
            }
            hw_section.children.push_back(heater_node);
        }

        if (!sensors.empty()) {
            TreeNode sensor_node("🌡️  Sensors (" + std::to_string(sensors.size()) + ")", "", true,
                                 1);
            sensor_node.expanded = false;
            for (const auto& s : sensors) {
                std::string desc = get_object_description(s);
                sensor_node.children.push_back(TreeNode(s, desc, true, 2, s));
            }
            hw_section.children.push_back(sensor_node);
        }

        if (!fans.empty()) {
            TreeNode fan_node("💨 Fans (" + std::to_string(fans.size()) + ")", "", true, 1);
            fan_node.expanded = false;
            for (const auto& f : fans) {
                std::string desc = get_object_description(f);
                fan_node.children.push_back(TreeNode(f, desc, true, 2, f));
            }
            hw_section.children.push_back(fan_node);
        }

        if (!leds.empty()) {
            TreeNode led_node("💡 LEDs (" + std::to_string(leds.size()) + ")", "", true, 1);
            led_node.expanded = false;
            for (const auto& l : leds) {
                std::string desc = get_object_description(l);
                led_node.children.push_back(TreeNode(l, desc, true, 2, l));
            }
            hw_section.children.push_back(led_node);
        }

        if (!steppers.empty()) {
            TreeNode stepper_node("🔩 Steppers/Drivers (" + std::to_string(steppers.size()) + ")",
                                  "", true, 1);
            stepper_node.expanded = false;
            for (const auto& s : steppers) {
                std::string desc = get_object_description(s);
                stepper_node.children.push_back(TreeNode(s, desc, true, 2, s));
            }
            hw_section.children.push_back(stepper_node);
        }

        if (!probes.empty()) {
            TreeNode probe_node("📍 Probes/Leveling (" + std::to_string(probes.size()) + ")", "",
                                true, 1);
            probe_node.expanded = false;
            for (const auto& p : probes) {
                std::string desc = get_object_description(p);
                probe_node.children.push_back(TreeNode(p, desc, true, 2, p));
            }
            hw_section.children.push_back(probe_node);
        }

        if (!macros.empty()) {
            TreeNode macro_node("⚙️  G-code Macros (" + std::to_string(macros.size()) + ")", "",
                                true, 1);
            macro_node.expanded = false; // ESPECIALLY collapsed by default
            for (const auto& m : macros) {
                std::string desc = get_object_description(m);
                macro_node.children.push_back(TreeNode(m, desc, true, 2, m));
            }
            hw_section.children.push_back(macro_node);
        }

        if (!other.empty()) {
            TreeNode other_node("🔌 Accessories (" + std::to_string(other.size()) + ")", "", true,
                                1);
            other_node.expanded = false;
            for (const auto& o : other) {
                std::string desc = get_object_description(o);
                other_node.children.push_back(TreeNode(o, desc, true, 2, o));
            }
            hw_section.children.push_back(other_node);
        }

        state->tree.push_back(hw_section);
    }
}

// Flatten tree for rendering (only visible nodes) - recursive helper
void flatten_tree_recursive(TreeNode& node, std::vector<TreeNode*>& flat) {
    flat.push_back(&node);

    if (node.expanded && !node.children.empty()) {
        for (auto& child : node.children) {
            flatten_tree_recursive(child, flat);
        }
    }
}

// Flatten tree for rendering (only visible nodes)
std::vector<TreeNode*> flatten_tree(std::vector<TreeNode>& tree) {
    std::vector<TreeNode*> flat;

    for (auto& node : tree) {
        flatten_tree_recursive(node, flat);
    }

    return flat;
}

// Find node in tree by flattened index
TreeNode* find_node_by_index(std::vector<TreeNode>& tree, size_t index) {
    auto flat = flatten_tree(tree);
    if (index < flat.size()) {
        return flat[index];
    }
    return nullptr;
}

// Render the tree using cpp-terminal
std::string render_tree(InteractiveState* state, const Term::Screen& term_size) {
    std::stringstream ss;

    // Move to top-left
    ss << Term::cursor_move(1, 1);

    // Header
    ss << Term::color_fg(Term::Color::Name::Cyan);
    ss << Term::style(Term::Style::Bold);
    ss << "╔══════════════════════════════════════════════════════════════╗\n";
    ss << "║ Moonraker Inspector - Interactive Mode                       ║\n";
    ss << "╚══════════════════════════════════════════════════════════════╝\n";
    ss << Term::style(Term::Style::Reset);
    ss << "\n";

    if (!state->data_ready) {
        ss << Term::color_fg(Term::Color::Name::Yellow);
        ss << "Loading data...";
        ss << Term::color_fg(Term::Color::Name::Default);
        return ss.str();
    }

    auto flat_tree = flatten_tree(state->tree);
    // Uncomment for render debugging:
    // spdlog::debug("Rendering {} nodes from flattened tree", flat_tree.size());

    // Update spinner animation
    const char* spinner_chars[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    const char* current_spinner = spinner_chars[state->spinner_frame % 10];
    state->spinner_frame++;

    // Render visible nodes
    for (size_t i = 0; i < flat_tree.size(); i++) {
        TreeNode* node = flat_tree[i];
        bool selected = (i == state->selected_index);

        // Highlight selected
        if (selected) {
            ss << Term::color_bg(Term::Color::Name::Gray);
            ss << Term::color_fg(Term::Color::Name::White);
            ss << Term::style(Term::Style::Bold);
        }

        // Indent
        for (int j = 0; j < node->indent_level; j++) {
            ss << "  ";
        }

        // Show spinner for loading indicators
        bool is_loading = (node->key.find("Loading data") == 0);
        if (is_loading) {
            ss << current_spinner << " " << node->key;
            if (selected) {
                ss << " ◀";
            }
            ss << Term::style(Term::Style::Reset);
            ss << Term::color_bg(Term::Color::Name::Default);
            ss << Term::color_fg(Term::Color::Name::Default);
            ss << "\n";
            continue;
        }

        // Render node
        if (node->is_section) {
            ss << (node->expanded ? "▼ " : "▶ ");
            ss << Term::color_fg(selected ? Term::Color::Name::White : Term::Color::Name::Cyan);
            ss << Term::style(Term::Style::Bold);
            ss << node->key;
            if (!node->value.empty()) {
                ss << Term::style(Term::Style::Reset);
                if (selected) {
                    ss << Term::color_bg(Term::Color::Name::Gray);
                }
                ss << " - " << node->value;
            }
        } else {
            ss << "  ";
            ss << node->key;
            if (!node->value.empty()) {
                ss << ": " << node->value;
            }
        }

        if (selected) {
            ss << " ◀";
        }

        ss << Term::style(Term::Style::Reset);
        ss << Term::color_bg(Term::Color::Name::Default);
        ss << Term::color_fg(Term::Color::Name::Default);
        ss << "\n";
    }

    // Controls footer
    ss << "\n";
    ss << Term::color_fg(Term::Color::Name::Gray);
    ss << "────────────────────────────────────────────────────────────────\n";
    ss << Term::style(Term::Style::Reset);
    ss << Term::color_fg(Term::Color::Name::Cyan);
    ss << "↑/↓";
    ss << Term::color_fg(Term::Color::Name::Default);
    ss << " Navigate  ";
    ss << Term::color_fg(Term::Color::Name::Cyan);
    ss << "Enter/Space";
    ss << Term::color_fg(Term::Color::Name::Default);
    ss << " Expand/Collapse  ";
    ss << Term::color_fg(Term::Color::Name::Cyan);
    ss << "q";
    ss << Term::color_fg(Term::Color::Name::Default);
    ss << " Quit";

    return ss.str();
}

// Handle keyboard input
void handle_input(InteractiveState* state, Term::Key key) {
    auto flat_tree = flatten_tree(state->tree);
    size_t max_index = flat_tree.empty() ? 0 : flat_tree.size() - 1;

    switch (key) {
    case Term::Key::ArrowUp:
        if (state->selected_index > 0) {
            state->selected_index--;
            // Skip non-sections
            while (state->selected_index > 0) {
                TreeNode* node = find_node_by_index(state->tree, state->selected_index);
                if (node && node->is_section) {
                    state->selected_node = node;
                    break;
                }
                state->selected_index--;
            }
        }
        break;

    case Term::Key::ArrowDown:
        if (state->selected_index < max_index) {
            state->selected_index++;
            // Skip non-sections
            while (state->selected_index < max_index) {
                TreeNode* node = find_node_by_index(state->tree, state->selected_index);
                if (node && node->is_section) {
                    state->selected_node = node;
                    break;
                }
                state->selected_index++;
            }
        }
        break;

    case Term::Key::Enter:
    case Term::Key::Space: {
        TreeNode* node = find_node_by_index(state->tree, state->selected_index);
        spdlog::debug("Enter/Space pressed on node: {} (is_section={}, object_name='{}', "
                      "data_fetched={}, expanded={})",
                      node ? node->key : "null", node ? node->is_section : false,
                      node ? node->object_name : "", node ? node->data_fetched : false,
                      node ? node->expanded : false);

        if (node && node->is_section) {
            bool was_expanded = node->expanded;
            node->expanded = !node->expanded;

            spdlog::debug("Toggled expansion: was_expanded={}, now_expanded={}", was_expanded,
                          node->expanded);

            // If expanding and has object name, query Moonraker for details
            if (!was_expanded && !node->object_name.empty() && !node->data_fetched &&
                state->client) {
                spdlog::debug("Triggering query_object_data for: {}", node->object_name);
                query_object_data(node, state->client);
            } else {
                spdlog::debug(
                    "NOT querying: was_expanded={}, object_name='{}', data_fetched={}, client={}",
                    was_expanded, node->object_name, node->data_fetched, state->client != nullptr);
            }

            state->selected_node = node;
        }
    } break;

    default:
        break;
    }
}

// Interactive main loop
int run_interactive(const std::string& ip, int port) {
    try {
        // Check TTY
        if (!Term::is_stdin_a_tty()) {
            throw Term::Exception("The terminal is not attached to a TTY. Exiting...");
        }

        InteractiveState state;
        g_state = &state;

        // Enable debug logging if MOONRAKER_DEBUG env var is set
        const char* debug_env = std::getenv("MOONRAKER_DEBUG");
        if (debug_env && strcmp(debug_env, "1") == 0) {
            // Create file sink to avoid corrupting TUI
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                "/tmp/moonraker-inspector-debug.log", true);
            auto logger = std::make_shared<spdlog::logger>("moonraker", file_sink);
            logger->set_level(spdlog::level::debug);
            spdlog::set_default_logger(logger);
            spdlog::info("Debug logging enabled - output to /tmp/moonraker-inspector-debug.log");
        } else {
            spdlog::set_level(spdlog::level::off);
        }

        // Set terminal options
        Term::terminal.setOptions(Term::Option::Raw, Term::Option::NoSignalKeys,
                                  Term::Option::ClearScreen, Term::Option::NoCursor);

        Term::Screen term_size = Term::screen_size();
        bool need_to_render = true;

        // Connect to Moonraker
        std::string url = "ws://" + ip + ":" + std::to_string(port) + "/websocket";
        MoonrakerClient client;
        client.configure_timeouts(5000, 10000, 10000, 200, 2000);
        state.client = &client;

        bool connected = false;

        auto on_connect = [&]() {
            connected = true;

            // Query all data
            client.send_jsonrpc(
                "server.info", json::object(),
                [&](json response) {
                    if (response.contains("result")) {
                        state.server_info = response["result"];
                    }
                },
                [](const MoonrakerError&) {});

            client.send_jsonrpc(
                "printer.info", json::object(),
                [&](json response) {
                    if (response.contains("result")) {
                        state.printer_info = response["result"];
                    }
                },
                [](const MoonrakerError&) {});

            client.send_jsonrpc(
                "printer.objects.list", json::object(),
                [&](json response) {
                    if (response.contains("result")) {
                        state.objects_list = response["result"];
                        state.data_ready = true;
                        build_tree(&state);
                        state.need_redraw = true;
                    }
                },
                [](const MoonrakerError&) {});
        };

        auto on_disconnect = []() {};

        int result = client.connect(url.c_str(), on_connect, on_disconnect);
        if (result != 0) {
            Term::cerr << Term::color_fg(Term::Color::Name::Red) << "Failed to connect to " << url
                       << Term::color_fg(Term::Color::Name::Default) << std::endl;
            return 1;
        }

        // Main event loop
        bool running = true;

        while (running) {
            // Render if needed (or if we have pending queries - for spinner animation)
            if (need_to_render || state.need_redraw || state.pending_queries > 0) {
                Term::cout << Term::clear_screen() << render_tree(&state, term_size) << std::flush;
                need_to_render = false;
                state.need_redraw = false;
            }

            // Poll for events with timeout (don't block indefinitely)
            // This allows async callbacks to trigger redraws
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Check for input events (using a small timeout via select/poll internally)
            Term::Event event = Term::read_event();

            switch (event.type()) {
            case Term::Event::Type::Key: {
                Term::Key key(event);
                if (key == Term::Key::q || key == Term::Key::Esc) {
                    running = false;
                } else {
                    handle_input(&state, key);
                    need_to_render = true;
                }
                break;
            }
            case Term::Event::Type::Screen: {
                term_size = Term::Screen(event);
                need_to_render = true;
                break;
            }
            case Term::Event::Type::Empty:
                // No event - continue loop to check for async updates
                break;
            default:
                break;
            }
        }

        Term::cout << "\n"
                   << Term::color_fg(Term::Color::Name::Green) << "Exited interactive mode."
                   << Term::color_fg(Term::Color::Name::Default) << std::endl;

        return 0;
    } catch (const Term::Exception& e) {
        Term::cerr << "cpp-terminal error: " << e.what() << std::endl;
        return 2;
    } catch (...) {
        Term::cerr << "Unknown error." << std::endl;
        return 1;
    }
}
