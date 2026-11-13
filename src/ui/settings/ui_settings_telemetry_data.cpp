// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_telemetry_data.cpp
 * @brief Implementation of TelemetryDataOverlay
 */

#include "ui_settings_telemetry_data.h"

#include "ui_callback_helpers.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "i18n_plural.h"
#include "json_utils.h"
#include "lvgl.h"
#include "system/telemetry_manager.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <optional>

#include "hv/json.hpp"

namespace helix::settings {

TelemetryDataOverlay::~TelemetryDataOverlay() {
    if (subjects_initialized_) {
        deinit_subjects_base(subjects_);
    }
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void TelemetryDataOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // Status text subject: "Telemetry Enabled" / "Telemetry Disabled"
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, lv_tr("Telemetry"),
                                  "telemetry_data_status", subjects_);

        // Detail text subject: "N events queued"
        UI_MANAGED_SUBJECT_STRING(detail_subject_, detail_buf_, lv_tr("0 events queued"),
                                  "telemetry_data_detail", subjects_);

        // Count subject for show/hide empty state vs event list
        UI_MANAGED_SUBJECT_INT(count_subject_, 0, "telemetry_data_count", subjects_);
    });
}

void TelemetryDataOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_telemetry_clear_queue",
         [](lv_event_t*) { get_telemetry_data_overlay().handle_clear_queue(); }},
    });
}

void TelemetryDataOverlay::on_activate() {
    OverlayBase::on_activate();

    update_status();
    populate_events();
}

// ============================================================================
// INTERNAL METHODS
// ============================================================================

void TelemetryDataOverlay::update_status() {
    auto& telemetry = TelemetryManager::instance();
    bool enabled = telemetry.is_enabled();
    size_t count = telemetry.queue_size();

    // Update status text
    const char* status_text = enabled ? lv_tr("Telemetry Enabled") : lv_tr("Telemetry Disabled");
    lv_subject_set_string(&status_subject_, status_text);

    // Update detail text with event count
    if (count == 0) {
        lv_subject_set_string(&detail_subject_, lv_tr("No events queued"));
    } else {
        snprintf(detail_buf_, sizeof(detail_buf_),
                 helix::ui::tr_plural("1 event queued", "%zu events queued", count), count);
        lv_subject_set_string(&detail_subject_, detail_buf_);
    }

    // Update count subject for show/hide logic
    lv_subject_set_int(&count_subject_, static_cast<int32_t>(count));

    spdlog::debug("[{}] Status updated: {} events, enabled={}", get_name(), count, enabled);
}

std::vector<TelemetryEventLine> describe_telemetry_event(const nlohmann::json& event) {
    using json_util::find_member;
    using json_util::read_field;
    using json_util::read_integer_field;
    using json_util::read_scalar;

    std::vector<TelemetryEventLine> lines;
    auto add = [&lines](std::string text, const char* color_token) {
        lines.push_back({std::move(text), color_token});
    };

    // Event type (heading) — JSON key is "event", not "type"
    std::string event_type = lv_tr("Unknown Event");
    if (auto raw_type = read_field<std::string>(event, "event")) {
        event_type = *raw_type;
        if (event_type == "session") {
            event_type = lv_tr("Session Start");
        } else if (event_type == "print_outcome") {
            event_type = lv_tr("Print Outcome");
        } else if (event_type == "crash") {
            event_type = lv_tr("Crash Report");
        } else if (event_type == "memory_snapshot") {
            event_type = lv_tr("Memory Snapshot");
        } else if (event_type == "hardware_profile") {
            event_type = lv_tr("Hardware Profile");
        } else if (event_type == "settings_snapshot") {
            event_type = lv_tr("Settings Snapshot");
        } else if (event_type == "panel_usage") {
            event_type = lv_tr("Panel Usage");
        } else if (event_type == "connection_stability") {
            event_type = lv_tr("Connection Stability");
        } else if (event_type == "print_start_context") {
            event_type = lv_tr("Print Start");
        } else if (event_type == "error_encountered") {
            event_type = lv_tr("Error");
        } else if (event_type == "update_failed") {
            event_type = lv_tr("Update Failed");
        } else if (event_type == "update_success") {
            event_type = lv_tr("Update Success");
        }
    }

    add(event_type, "text");

    // Timestamp
    if (auto timestamp = read_field<std::string>(event, "timestamp")) {
        add(*timestamp, "text_muted");
    }

    // Key fields based on event type
    const std::string type_str = read_field<std::string>(event, "event").value_or("");

    // Helper lambdas for rendering simple fields
    auto add_field_str = [&](const char* key, const char* display_name) {
        if (auto value = read_field<std::string>(event, key)) {
            add(std::string(display_name) + ": " + *value, "text_subtle");
        }
    };
    auto add_field_num = [&](const char* key, const char* display_name, const char* suffix) {
        char buf[64];
        if (auto whole = read_integer_field(event, key)) {
            snprintf(buf, sizeof(buf), "%s: %d%s", display_name, *whole, suffix);
        } else if (auto real = read_field<double>(event, key)) {
            snprintf(buf, sizeof(buf), "%s: %.1f%s", display_name, *real, suffix);
        } else {
            return;
        }
        add(buf, "text_subtle");
    };
    // A nested section, or nullptr when the event lacks it or it is not an object.
    auto section = [&event](const char* key) -> const nlohmann::json* {
        const auto* v = find_member(event, key);
        return v && v->is_object() ? v : nullptr;
    };

    if (type_str == "session") {
        // Session fields are nested under "app"; read_field yields nothing for a
        // non-object "app", so no object check is needed here.
        const auto* app = find_member(event, "app");
        auto add_app_field = [&](const char* key, const char* display_name) {
            if (!app) {
                return;
            }
            if (auto value = read_field<std::string>(*app, key)) {
                add(std::string(display_name) + ": " + *value, "text_subtle");
            }
        };

        // App info - combine platform and display on one line
        add_app_field("version", lv_tr("Version"));
        if (app) {
            std::string platform_display;
            if (auto platform = read_field<std::string>(*app, "platform")) {
                platform_display = std::string(lv_tr("Platform: ")) + *platform;
            }
            if (auto display = read_field<std::string>(*app, "display")) {
                if (!platform_display.empty())
                    platform_display += " | ";
                platform_display += std::string(lv_tr("Display: ")) + *display;
                if (auto backend = read_field<std::string>(*app, "display_backend")) {
                    platform_display += " (" + *backend + ")";
                }
            }
            if (!platform_display.empty()) {
                add(platform_display, "text_subtle");
            }

            // Theme, locale, and input type
            std::string settings_line;
            if (auto theme = read_field<std::string>(*app, "theme")) {
                settings_line = std::string(lv_tr("Theme: ")) + *theme;
            }
            if (auto locale = read_field<std::string>(*app, "locale")) {
                if (!settings_line.empty())
                    settings_line += " | ";
                settings_line += std::string(lv_tr("Locale: ")) + *locale;
            }
            if (auto input = read_field<std::string>(*app, "input_type")) {
                if (!settings_line.empty())
                    settings_line += " | ";
                settings_line += std::string(lv_tr("Input: ")) + *input;
            }
            if (!settings_line.empty()) {
                add(settings_line, "text_subtle");
            }
        }

        // Printer section
        if (const auto* p = section("printer")) {
            // "Printer: corexy, 350x350x300"
            std::string printer_prefix = lv_tr("Printer:");
            std::string printer_line = printer_prefix;
            if (auto kinematics = read_field<std::string>(*p, "kinematics")) {
                printer_line += " " + *kinematics;
            }
            if (auto volume = read_field<std::string>(*p, "build_volume")) {
                printer_line += ", " + *volume;
            }
            if (printer_line != printer_prefix) {
                add(printer_line, "text_subtle");
            }

            // "MCU: stm32f446 (x2) | 1 extruder"
            std::string mcu_line;
            if (auto mcu = read_field<std::string>(*p, "mcu")) {
                mcu_line = std::string(lv_tr("MCU: ")) + *mcu;
                if (auto mcu_count = read_integer_field(*p, "mcu_count");
                    mcu_count && *mcu_count > 1) {
                    mcu_line += " (x" + std::to_string(*mcu_count) + ")";
                }
            }
            if (auto ext = read_integer_field(*p, "extruder_count")) {
                if (!mcu_line.empty())
                    mcu_line += " | ";
                mcu_line +=
                    std::to_string(*ext) + " " +
                    helix::ui::tr_plural("extruder", "extruders", static_cast<uint64_t>(*ext));
            }
            if (!mcu_line.empty()) {
                add(mcu_line, "text_subtle");
            }

            // Klipper and Moonraker versions
            if (auto klipper = read_field<std::string>(*p, "klipper_version")) {
                add(std::string(lv_tr("Klipper: ")) + *klipper, "text_subtle");
            }
            if (auto moonraker = read_field<std::string>(*p, "moonraker_version")) {
                add(std::string(lv_tr("Moonraker: ")) + *moonraker, "text_subtle");
            }
        }

        // Features array
        if (const auto* features = find_member(event, "features");
            features && features->is_array() && !features->empty()) {
            std::string features_str = lv_tr("Features: ");
            bool first = true;
            for (const auto& f : *features) {
                if (auto name = read_scalar<std::string>(f)) {
                    if (!first)
                        features_str += ", ";
                    features_str += *name;
                    first = false;
                }
            }
            add(features_str, "text_subtle");
        }

        // Host info
        if (const auto* h = section("host")) {
            // "Host: aarch64, 4 cores, 1024 MB RAM"
            std::string host_line;
            if (auto arch = read_field<std::string>(*h, "arch")) {
                host_line = *arch;
            }
            if (auto cores = read_integer_field(*h, "cpu_cores")) {
                if (!host_line.empty())
                    host_line += ", ";
                host_line += std::to_string(*cores) + " " + lv_tr("cores");
            }
            if (auto ram = read_integer_field(*h, "ram_total_mb")) {
                if (!host_line.empty())
                    host_line += ", ";
                host_line += std::to_string(*ram) + " " + lv_tr("MB RAM");
            }
            if (!host_line.empty()) {
                add(std::string(lv_tr("Host: ")) + host_line, "text_subtle");
            }

            if (auto cpu = read_field<std::string>(*h, "cpu_model")) {
                add(std::string(lv_tr("CPU: ")) + *cpu, "text_subtle");
            }

            if (auto os = read_field<std::string>(*h, "os")) {
                add(std::string(lv_tr("OS: ")) + *os, "text_subtle");
            }
        }
    } else if (type_str == "print_outcome") {
        add_field_str("outcome", lv_tr("Outcome"));
        add_field_num("duration_sec", lv_tr("Duration"), "s");
        add_field_str("filament_type", lv_tr("Filament"));
        add_field_num("nozzle_temp", lv_tr("Nozzle"),
                      "\xC2\xB0"
                      "C");
        add_field_num("bed_temp", lv_tr("Bed"),
                      "\xC2\xB0"
                      "C");

    } else if (type_str == "crash") {
        add_field_str("signal_name", lv_tr("Signal"));
        add_field_num("signal", lv_tr("Signal #"), "");
        add_field_str("app_version", lv_tr("Version"));
        add_field_num("uptime_sec", lv_tr("Uptime"), "s");
        if (const auto* backtrace = find_member(event, "backtrace");
            backtrace && backtrace->is_array()) {
            const size_t frames = backtrace->size();
            add(fmt::format(
                    fmt::runtime(helix::ui::tr_plural(nullptr, "Backtrace: {} frames", frames)),
                    frames),
                "text_subtle");
        }

    } else if (type_str == "memory_snapshot") {
        add_field_str("trigger", lv_tr("Trigger"));
        add_field_num("uptime_sec", lv_tr("Uptime"), "s");
        add_field_num("rss_kb", lv_tr("RSS"), " KB");
        add_field_num("vm_size_kb", lv_tr("VM Size"), " KB");
        add_field_num("vm_peak_kb", lv_tr("VM Peak"), " KB");
        add_field_num("vm_hwm_kb", lv_tr("High Water Mark"), " KB");

    } else if (type_str == "hardware_profile") {
        // Show key sections from nested JSON
        if (const auto* p = section("printer")) {
            std::string line;
            if (auto model = read_field<std::string>(*p, "detected_model")) {
                line = *model;
            }
            if (auto kinematics = read_field<std::string>(*p, "kinematics")) {
                if (!line.empty())
                    line += " | ";
                line += *kinematics;
            }
            if (!line.empty()) {
                add(std::string(lv_tr("Printer: ")) + line, "text_subtle");
            }
        }
        if (const auto* m = section("mcus")) {
            std::string line;
            if (auto primary = read_field<std::string>(*m, "primary")) {
                line = std::string(lv_tr("MCU: ")) + *primary;
            }
            if (auto cnt = read_integer_field(*m, "count"); cnt && *cnt > 1) {
                line += " (x" + std::to_string(*cnt) + ")";
            }
            if (!line.empty()) {
                add(line, "text_subtle");
            }
        }
        if (const auto* bv = section("build_volume")) {
            auto x = read_field<int>(*bv, "x_mm");
            auto y = read_field<int>(*bv, "y_mm");
            if (x && y) {
                std::string vol = std::to_string(*x) + "x" + std::to_string(*y);
                if (auto z = read_field<int>(*bv, "z_mm")) {
                    vol += "x" + std::to_string(*z);
                }
                add(std::string(lv_tr("Build Volume: ")) + vol + " mm", "text_subtle");
            }
        }
        if (const auto* e = section("extruders")) {
            std::string line;
            if (auto count = read_integer_field(*e, "count")) {
                line = std::to_string(*count) + " " + lv_tr("extruder(s)");
            }
            if (read_field<bool>(*e, "has_heater_bed").value_or(false)) {
                line += ", " + std::string(lv_tr("heated bed"));
            }
            if (read_field<bool>(*e, "has_chamber_heater").value_or(false)) {
                line += ", " + std::string(lv_tr("chamber heater"));
            }
            if (!line.empty()) {
                add(line, "text_subtle");
            }
        }
        // Fans, steppers, LEDs summary. read_field yields nothing for a missing
        // or non-object section, so these need no object check.
        {
            auto count_in = [&event](const char* key, const char* field) -> std::optional<int> {
                const auto* v = find_member(event, key);
                return v ? read_field<int>(*v, field) : std::nullopt;
            };
            std::string hw_line;
            if (auto fans = count_in("fans", "total")) {
                hw_line += std::to_string(*fans) + " " + lv_tr("fans");
            }
            if (auto steppers = count_in("steppers", "count")) {
                if (!hw_line.empty())
                    hw_line += ", ";
                hw_line += std::to_string(*steppers) + " " + lv_tr("steppers");
            }
            if (auto leds = count_in("leds", "count")) {
                if (!hw_line.empty())
                    hw_line += ", ";
                hw_line += std::to_string(*leds) + " " + lv_tr("LEDs");
            }
            if (!hw_line.empty()) {
                add(hw_line, "text_subtle");
            }
        }
        // Capabilities summary (show true capabilities)
        if (const auto* capabilities = section("capabilities")) {
            std::string caps;
            for (auto it = capabilities->begin(); it != capabilities->end(); ++it) {
                if (read_scalar<bool>(it.value()).value_or(false)) {
                    std::string name = it.key();
                    // Strip "has_" prefix for readability
                    if (name.rfind("has_", 0) == 0) {
                        name = name.substr(4);
                    }
                    if (!caps.empty())
                        caps += ", ";
                    caps += name;
                }
            }
            if (!caps.empty()) {
                add(std::string(lv_tr("Capabilities: ")) + caps, "text_subtle");
            }
        }
        // AMS
        if (const auto* a = section("ams")) {
            std::string line = "AMS:";
            if (auto type = read_field<std::string>(*a, "type")) {
                line += " " + *type;
            }
            if (auto slots = read_integer_field(*a, "total_slots")) {
                line += " (" + std::to_string(*slots) + " " + lv_tr("slots)");
            }
            add(line, "text_subtle");
        }
        // Tools
        if (const auto* t = section("tools")) {
            if (auto count = read_integer_field(*t, "count")) {
                std::string line = std::to_string(*count) + " " + lv_tr("tool(s)");
                if (read_field<bool>(*t, "is_multi_tool").value_or(false)) {
                    line += " " + std::string(lv_tr("(multi-tool)"));
                }
                add(std::string(lv_tr("Tools: ")) + line, "text_subtle");
            }
        }
    } else if (type_str == "settings_snapshot") {
        add_field_str("theme", lv_tr("Theme"));
        add_field_num("brightness_pct", lv_tr("Brightness"), "%");
        add_field_str("locale", lv_tr("Locale"));
        add_field_num("screensaver_timeout_sec", lv_tr("Screensaver"), "s");
        add_field_num("screen_blank_timeout_sec", lv_tr("Screen Blank"), "s");
        add_field_num("auto_update_channel", lv_tr("Update Channel"), "");

    } else if (type_str == "panel_usage") {
        add_field_num("session_duration_sec", lv_tr("Session Duration"), "s");
        add_field_num("overlay_open_count", lv_tr("Overlays Opened"), "");
        // "key: value" pairs of a section's numeric entries, skipping the rest
        auto numeric_pairs = [&section](const char* key, const char* suffix) {
            std::string out;
            const auto* map = section(key);
            if (!map) {
                return out;
            }
            for (auto it = map->begin(); it != map->end(); ++it) {
                auto n = read_scalar<int>(it.value());
                if (!n) {
                    continue;
                }
                if (!out.empty())
                    out += ", ";
                out += it.key() + ": " + std::to_string(*n) + suffix;
            }
            return out;
        };
        // Show panel times
        if (auto panels = numeric_pairs("panel_time_sec", "s"); !panels.empty()) {
            add(std::string(lv_tr("Time: ")) + panels, "text_subtle");
        }
        if (auto overlays = numeric_pairs("overlay_visits", ""); !overlays.empty()) {
            add(std::string(lv_tr("Overlay Visits: ")) + overlays, "text_subtle");
        }
        if (auto visits = numeric_pairs("panel_visits", ""); !visits.empty()) {
            add(std::string(lv_tr("Visits: ")) + visits, "text_subtle");
        }
    } else if (type_str == "connection_stability") {
        add_field_num("session_duration_sec", lv_tr("Session Duration"), "s");
        add_field_num("connect_count", lv_tr("Connects"), "");
        add_field_num("disconnect_count", lv_tr("Disconnects"), "");
        add_field_num("total_connected_sec", lv_tr("Connected Time"), "s");
        add_field_num("total_disconnected_sec", lv_tr("Disconnected Time"), "s");
        add_field_num("longest_disconnect_sec", lv_tr("Longest Disconnect"), "s");
        add_field_num("klippy_error_count", lv_tr("Klippy Errors"), "");
        add_field_num("klippy_shutdown_count", lv_tr("Klippy Shutdowns"), "");

    } else if (type_str == "print_start_context") {
        add_field_str("source", lv_tr("Source"));
        if (auto thumbnail = read_field<bool>(event, "has_thumbnail")) {
            add(std::string(lv_tr("Thumbnail: ")) + (*thumbnail ? lv_tr("Yes") : lv_tr("No")),
                "text_subtle");
        }
        add_field_str("file_size_bucket", lv_tr("File Size"));
        add_field_str("estimated_duration_bucket", lv_tr("Est. Duration"));
        add_field_str("slicer", lv_tr("Slicer"));
        add_field_num("tool_count_used", lv_tr("Tools Used"), "");
        if (auto ams_active = read_field<bool>(event, "ams_active")) {
            add(std::string(lv_tr("AMS Active: ")) + (*ams_active ? lv_tr("Yes") : lv_tr("No")),
                "text_subtle");
        }
    } else if (type_str == "error_encountered") {
        add_field_str("category", lv_tr("Category"));
        add_field_str("code", lv_tr("Code"));
        add_field_str("context", lv_tr("Context"));
        add_field_num("uptime_sec", lv_tr("Uptime"), "s");

    } else if (type_str == "update_failed") {
        add_field_str("reason", lv_tr("Reason"));
        add_field_str("version", lv_tr("Target Version"));
        add_field_str("from_version", lv_tr("From Version"));
        add_field_str("platform", lv_tr("Platform"));
        add_field_num("http_code", lv_tr("HTTP Code"), "");
        add_field_num("exit_code", lv_tr("Exit Code"), "");

    } else if (type_str == "update_success") {
        add_field_str("version", lv_tr("Version"));
        add_field_str("from_version", lv_tr("From Version"));
        add_field_str("platform", lv_tr("Platform"));
    }

    // Show the full hashed device ID (no truncation)
    if (auto device = read_field<std::string>(event, "device_id")) {
        add(std::string(lv_tr("Device: ")) + *device, "text_subtle");
    }

    return lines;
}

void TelemetryDataOverlay::populate_events() {
    lv_obj_t* event_list = helix::ui::find_required(overlay_root_, "event_list", get_name());
    if (!event_list) {
        return;
    }

    // Clear existing children
    helix::ui::safe_clean_children(event_list);

    auto& telemetry = TelemetryManager::instance();
    auto snapshot = telemetry.get_queue_snapshot();

    if (!snapshot.is_array() || snapshot.empty()) {
        spdlog::debug("[{}] No events to display", get_name());
        return;
    }

    for (const auto& event : snapshot) {
        // Create a card for each event
        lv_obj_t* card = lv_obj_create(event_list);
        if (!card) {
            continue;
        }

        // Style the card
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, theme_manager_get_color("card_bg"), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, theme_manager_get_spacing("space_md"), 0);
        lv_obj_set_style_pad_gap(card, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollable(card, false);

        // Helper: create a label that wraps within the card
        auto make_label = [&](lv_obj_t* parent, const std::string& text,
                              const char* color_token) -> lv_obj_t* {
            lv_obj_t* label = lv_label_create(parent);
            lv_label_set_text(label, text.c_str());
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_width(label, lv_pct(100));
            lv_obj_set_style_text_color(label, theme_manager_get_color(color_token), 0);
            lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
            return label;
        };

        for (const auto& line : describe_telemetry_event(event)) {
            make_label(card, line.text, line.color_token);
        }
    }

    spdlog::debug("[{}] Populated {} event cards", get_name(), snapshot.size());
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void TelemetryDataOverlay::handle_clear_queue() {
    auto& telemetry = TelemetryManager::instance();
    telemetry.clear_queue();

    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Telemetry queue cleared"), 2000);
    spdlog::info("[{}] Queue cleared by user", get_name());

    // Refresh display
    update_status();
    populate_events();
}

} // namespace helix::settings
