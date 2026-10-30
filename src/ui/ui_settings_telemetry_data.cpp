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
#include "lvgl.h"
#include "system/telemetry_manager.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

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
    lv_subject_copy_string(&status_subject_, status_text);

    // Update detail text with event count
    if (count == 0) {
        lv_subject_copy_string(&detail_subject_, lv_tr("No events queued"));
    } else {
        snprintf(detail_buf_, sizeof(detail_buf_),
                 helix::ui::tr_plural("1 event queued", "%zu events queued", count), count);
        lv_subject_copy_string(&detail_subject_, detail_buf_);
    }

    // Update count subject for show/hide logic
    lv_subject_set_int(&count_subject_, static_cast<int32_t>(count));

    spdlog::debug("[{}] Status updated: {} events, enabled={}", get_name(), count, enabled);
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
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Helper: create a label that wraps within the card
        auto make_label = [&](lv_obj_t* parent, const std::string& text,
                              const char* color_token) -> lv_obj_t* {
            lv_obj_t* label = lv_label_create(parent);
            lv_label_set_text(label, text.c_str());
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(label, lv_pct(100));
            lv_obj_set_style_text_color(label, theme_manager_get_color(color_token), 0);
            lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
            return label;
        };

        // Event type (heading) — JSON key is "event", not "type"
        std::string event_type = lv_tr("Unknown Event");
        if (event.contains("event") && event["event"].is_string()) {
            event_type = event["event"].get<std::string>();
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

        make_label(card, event_type, "text");

        // Timestamp
        if (event.contains("timestamp") && event["timestamp"].is_string()) {
            make_label(card, event["timestamp"].get<std::string>(), "text_muted");
        }

        // Key fields based on event type
        std::string type_str;
        if (event.contains("event") && event["event"].is_string()) {
            type_str = event["event"].get<std::string>();
        }

        // Helper lambdas for rendering simple fields
        auto add_field_str = [&](const char* key, const char* display_name) {
            if (event.contains(key) && event[key].is_string()) {
                make_label(card, std::string(display_name) + ": " + event[key].get<std::string>(),
                           "text_subtle");
            }
        };
        auto add_field_num = [&](const char* key, const char* display_name, const char* suffix) {
            if (event.contains(key) && event[key].is_number()) {
                char buf[64];
                if (event[key].is_number_integer()) {
                    snprintf(buf, sizeof(buf), "%s: %d%s", display_name, event[key].get<int>(),
                             suffix);
                } else {
                    snprintf(buf, sizeof(buf), "%s: %.1f%s", display_name, event[key].get<double>(),
                             suffix);
                }
                make_label(card, buf, "text_subtle");
            }
        };

        if (type_str == "session") {
            // Session fields are nested under "app"
            auto add_app_field = [&](const char* key, const char* display_name) {
                if (event.contains("app") && event["app"].contains(key) &&
                    event["app"][key].is_string()) {
                    std::string text =
                        std::string(display_name) + ": " + event["app"][key].get<std::string>();
                    make_label(card, text, "text_subtle");
                }
            };

            // App info - combine platform and display on one line
            add_app_field("version", lv_tr("Version"));
            if (event.contains("app")) {
                const auto& app = event["app"];
                std::string platform_display;
                if (app.contains("platform") && app["platform"].is_string()) {
                    platform_display =
                        std::string(lv_tr("Platform: ")) + app["platform"].get<std::string>();
                }
                if (app.contains("display") && app["display"].is_string()) {
                    if (!platform_display.empty())
                        platform_display += " | ";
                    platform_display +=
                        std::string(lv_tr("Display: ")) + app["display"].get<std::string>();
                    if (app.contains("display_backend") && app["display_backend"].is_string()) {
                        platform_display += " (" + app["display_backend"].get<std::string>() + ")";
                    }
                }
                if (!platform_display.empty()) {
                    make_label(card, platform_display, "text_subtle");
                }

                // Theme, locale, and input type
                std::string settings_line;
                if (app.contains("theme") && app["theme"].is_string()) {
                    settings_line = std::string(lv_tr("Theme: ")) + app["theme"].get<std::string>();
                }
                if (app.contains("locale") && app["locale"].is_string()) {
                    if (!settings_line.empty())
                        settings_line += " | ";
                    settings_line +=
                        std::string(lv_tr("Locale: ")) + app["locale"].get<std::string>();
                }
                if (app.contains("input_type") && app["input_type"].is_string()) {
                    if (!settings_line.empty())
                        settings_line += " | ";
                    settings_line +=
                        std::string(lv_tr("Input: ")) + app["input_type"].get<std::string>();
                }
                if (!settings_line.empty()) {
                    make_label(card, settings_line, "text_subtle");
                }
            }

            // Printer section
            if (event.contains("printer") && event["printer"].is_object()) {
                const auto& p = event["printer"];

                // "Printer: corexy, 350x350x300"
                std::string printer_prefix = lv_tr("Printer:");
                std::string printer_line = printer_prefix;
                if (p.contains("kinematics") && p["kinematics"].is_string()) {
                    printer_line += " " + p["kinematics"].get<std::string>();
                }
                if (p.contains("build_volume") && p["build_volume"].is_string()) {
                    printer_line += ", " + p["build_volume"].get<std::string>();
                }
                if (printer_line != printer_prefix) {
                    make_label(card, printer_line, "text_subtle");
                }

                // "MCU: stm32f446 (x2) | 1 extruder"
                std::string mcu_line;
                if (p.contains("mcu") && p["mcu"].is_string()) {
                    mcu_line = std::string(lv_tr("MCU: ")) + p["mcu"].get<std::string>();
                    if (p.contains("mcu_count") && p["mcu_count"].is_number_integer() &&
                        p["mcu_count"].get<int>() > 1) {
                        mcu_line += " (x" + std::to_string(p["mcu_count"].get<int>()) + ")";
                    }
                }
                if (p.contains("extruder_count") && p["extruder_count"].is_number_integer()) {
                    int ext = p["extruder_count"].get<int>();
                    if (!mcu_line.empty())
                        mcu_line += " | ";
                    mcu_line +=
                        std::to_string(ext) + " " +
                        helix::ui::tr_plural("extruder", "extruders", static_cast<uint64_t>(ext));
                }
                if (!mcu_line.empty()) {
                    make_label(card, mcu_line, "text_subtle");
                }

                // Klipper and Moonraker versions
                if (p.contains("klipper_version") && p["klipper_version"].is_string()) {
                    make_label(card,
                               std::string(lv_tr("Klipper: ")) +
                                   p["klipper_version"].get<std::string>(),
                               "text_subtle");
                }
                if (p.contains("moonraker_version") && p["moonraker_version"].is_string()) {
                    make_label(card,
                               std::string(lv_tr("Moonraker: ")) +
                                   p["moonraker_version"].get<std::string>(),
                               "text_subtle");
                }
            }

            // Features array
            if (event.contains("features") && event["features"].is_array() &&
                !event["features"].empty()) {
                std::string features_str = lv_tr("Features: ");
                bool first = true;
                for (const auto& f : event["features"]) {
                    if (f.is_string()) {
                        if (!first)
                            features_str += ", ";
                        features_str += f.get<std::string>();
                        first = false;
                    }
                }
                make_label(card, features_str, "text_subtle");
            }

            // Host info
            if (event.contains("host") && event["host"].is_object()) {
                const auto& h = event["host"];

                // "Host: aarch64, 4 cores, 1024 MB RAM"
                std::string host_line;
                if (h.contains("arch") && h["arch"].is_string()) {
                    host_line = h["arch"].get<std::string>();
                }
                if (h.contains("cpu_cores") && h["cpu_cores"].is_number_integer()) {
                    if (!host_line.empty())
                        host_line += ", ";
                    host_line += std::to_string(h["cpu_cores"].get<int>()) + " " + lv_tr("cores");
                }
                if (h.contains("ram_total_mb") && h["ram_total_mb"].is_number_integer()) {
                    if (!host_line.empty())
                        host_line += ", ";
                    host_line +=
                        std::to_string(h["ram_total_mb"].get<int>()) + " " + lv_tr("MB RAM");
                }
                if (!host_line.empty()) {
                    make_label(card, std::string(lv_tr("Host: ")) + host_line, "text_subtle");
                }

                if (h.contains("cpu_model") && h["cpu_model"].is_string()) {
                    make_label(card,
                               std::string(lv_tr("CPU: ")) + h["cpu_model"].get<std::string>(),
                               "text_subtle");
                }

                if (h.contains("os") && h["os"].is_string()) {
                    make_label(card, std::string(lv_tr("OS: ")) + h["os"].get<std::string>(),
                               "text_subtle");
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
            if (event.contains("backtrace") && event["backtrace"].is_array()) {
                const size_t frames = event["backtrace"].size();
                make_label(card,
                           fmt::format(fmt::runtime(helix::ui::tr_plural(
                                           nullptr, "Backtrace: {} frames", frames)),
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
            if (event.contains("printer") && event["printer"].is_object()) {
                const auto& p = event["printer"];
                std::string line;
                if (p.contains("detected_model") && p["detected_model"].is_string()) {
                    line = p["detected_model"].get<std::string>();
                }
                if (p.contains("kinematics") && p["kinematics"].is_string()) {
                    if (!line.empty())
                        line += " | ";
                    line += p["kinematics"].get<std::string>();
                }
                if (!line.empty()) {
                    make_label(card, std::string(lv_tr("Printer: ")) + line, "text_subtle");
                }
            }
            if (event.contains("mcus") && event["mcus"].is_object()) {
                const auto& m = event["mcus"];
                std::string line;
                if (m.contains("primary") && m["primary"].is_string()) {
                    line = std::string(lv_tr("MCU: ")) + m["primary"].get<std::string>();
                }
                if (m.contains("count") && m["count"].is_number_integer()) {
                    int cnt = m["count"].get<int>();
                    if (cnt > 1) {
                        line += " (x" + std::to_string(cnt) + ")";
                    }
                }
                if (!line.empty()) {
                    make_label(card, line, "text_subtle");
                }
            }
            if (event.contains("build_volume") && event["build_volume"].is_object()) {
                const auto& bv = event["build_volume"];
                if (bv.contains("x_mm") && bv["x_mm"].is_number() && bv.contains("y_mm") &&
                    bv["y_mm"].is_number()) {
                    std::string vol = std::to_string(bv["x_mm"].get<int>()) + "x" +
                                      std::to_string(bv["y_mm"].get<int>());
                    if (bv.contains("z_mm") && bv["z_mm"].is_number()) {
                        vol += "x" + std::to_string(bv["z_mm"].get<int>());
                    }
                    make_label(card, std::string(lv_tr("Build Volume: ")) + vol + " mm",
                               "text_subtle");
                }
            }
            if (event.contains("extruders") && event["extruders"].is_object()) {
                const auto& e = event["extruders"];
                std::string line;
                if (e.contains("count") && e["count"].is_number_integer()) {
                    line = std::to_string(e["count"].get<int>()) + " " + lv_tr("extruder(s)");
                }
                if (e.contains("has_heater_bed") && e["has_heater_bed"].is_boolean() &&
                    e["has_heater_bed"].get<bool>()) {
                    line += ", " + std::string(lv_tr("heated bed"));
                }
                if (e.contains("has_chamber_heater") && e["has_chamber_heater"].is_boolean() &&
                    e["has_chamber_heater"].get<bool>()) {
                    line += ", " + std::string(lv_tr("chamber heater"));
                }
                if (!line.empty()) {
                    make_label(card, line, "text_subtle");
                }
            }
            // Fans, steppers, LEDs summary
            {
                std::string hw_line;
                if (event.contains("fans") && event["fans"].contains("total") &&
                    event["fans"]["total"].is_number()) {
                    hw_line +=
                        std::to_string(event["fans"]["total"].get<int>()) + " " + lv_tr("fans");
                }
                if (event.contains("steppers") && event["steppers"].contains("count") &&
                    event["steppers"]["count"].is_number()) {
                    if (!hw_line.empty())
                        hw_line += ", ";
                    hw_line += std::to_string(event["steppers"]["count"].get<int>()) + " " +
                               lv_tr("steppers");
                }
                if (event.contains("leds") && event["leds"].contains("count") &&
                    event["leds"]["count"].is_number()) {
                    if (!hw_line.empty())
                        hw_line += ", ";
                    hw_line +=
                        std::to_string(event["leds"]["count"].get<int>()) + " " + lv_tr("LEDs");
                }
                if (!hw_line.empty()) {
                    make_label(card, hw_line, "text_subtle");
                }
            }
            // Capabilities summary (show true capabilities)
            if (event.contains("capabilities") && event["capabilities"].is_object()) {
                std::string caps;
                for (auto it = event["capabilities"].begin(); it != event["capabilities"].end();
                     ++it) {
                    if (it.value().is_boolean() && it.value().get<bool>()) {
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
                    make_label(card, std::string(lv_tr("Capabilities: ")) + caps, "text_subtle");
                }
            }
            // AMS
            if (event.contains("ams") && event["ams"].is_object()) {
                const auto& a = event["ams"];
                std::string line = "AMS:";
                if (a.contains("type") && a["type"].is_string()) {
                    line += " " + a["type"].get<std::string>();
                }
                if (a.contains("total_slots") && a["total_slots"].is_number_integer()) {
                    line +=
                        " (" + std::to_string(a["total_slots"].get<int>()) + " " + lv_tr("slots)");
                }
                make_label(card, line, "text_subtle");
            }
            // Tools
            if (event.contains("tools") && event["tools"].is_object()) {
                const auto& t = event["tools"];
                if (t.contains("count") && t["count"].is_number_integer()) {
                    std::string line =
                        std::to_string(t["count"].get<int>()) + " " + lv_tr("tool(s)");
                    if (t.contains("is_multi_tool") && t["is_multi_tool"].is_boolean() &&
                        t["is_multi_tool"].get<bool>()) {
                        line += " " + std::string(lv_tr("(multi-tool)"));
                    }
                    make_label(card, std::string(lv_tr("Tools: ")) + line, "text_subtle");
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
            // Show panel times
            if (event.contains("panel_time_sec") && event["panel_time_sec"].is_object()) {
                std::string panels;
                for (auto it = event["panel_time_sec"].begin(); it != event["panel_time_sec"].end();
                     ++it) {
                    if (!it.value().is_number()) {
                        continue;
                    }
                    if (!panels.empty())
                        panels += ", ";
                    panels += it.key() + ": " + std::to_string(it.value().get<int>()) + "s";
                }
                if (!panels.empty()) {
                    make_label(card, std::string(lv_tr("Time: ")) + panels, "text_subtle");
                }
            }
            if (event.contains("overlay_visits") && event["overlay_visits"].is_object()) {
                std::string overlays;
                for (auto it = event["overlay_visits"].begin(); it != event["overlay_visits"].end();
                     ++it) {
                    if (!it.value().is_number()) {
                        continue;
                    }
                    if (!overlays.empty())
                        overlays += ", ";
                    overlays += it.key() + ": " + std::to_string(it.value().get<int>());
                }
                if (!overlays.empty()) {
                    make_label(card, std::string(lv_tr("Overlay Visits: ")) + overlays,
                               "text_subtle");
                }
            }
            if (event.contains("panel_visits") && event["panel_visits"].is_object()) {
                std::string visits;
                for (auto it = event["panel_visits"].begin(); it != event["panel_visits"].end();
                     ++it) {
                    if (!it.value().is_number()) {
                        continue;
                    }
                    if (!visits.empty())
                        visits += ", ";
                    visits += it.key() + ": " + std::to_string(it.value().get<int>());
                }
                if (!visits.empty()) {
                    make_label(card, std::string(lv_tr("Visits: ")) + visits, "text_subtle");
                }
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
            if (event.contains("has_thumbnail") && event["has_thumbnail"].is_boolean()) {
                make_label(card,
                           std::string(lv_tr("Thumbnail: ")) +
                               (event["has_thumbnail"].get<bool>() ? lv_tr("Yes") : lv_tr("No")),
                           "text_subtle");
            }
            add_field_str("file_size_bucket", lv_tr("File Size"));
            add_field_str("estimated_duration_bucket", lv_tr("Est. Duration"));
            add_field_str("slicer", lv_tr("Slicer"));
            add_field_num("tool_count_used", lv_tr("Tools Used"), "");
            if (event.contains("ams_active") && event["ams_active"].is_boolean()) {
                make_label(card,
                           std::string(lv_tr("AMS Active: ")) +
                               (event["ams_active"].get<bool>() ? lv_tr("Yes") : lv_tr("No")),
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
        if (event.contains("device_id") && event["device_id"].is_string()) {
            std::string text =
                std::string(lv_tr("Device: ")) + event["device_id"].get<std::string>();
            make_label(card, text, "text_subtle");
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
