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

#include "ui_panel_bed_mesh.h"

#include "ui_bed_mesh.h"
#include "ui_nav.h"

#include "app_globals.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <limits>

#include "hv/json.hpp"

using json = nlohmann::json;

// Rotation constants are now in ui_bed_mesh.h

// Static state
static lv_obj_t* canvas = nullptr;
static lv_obj_t* bed_mesh_panel = nullptr;
static lv_obj_t* parent_obj = nullptr;
static lv_obj_t* profile_dropdown = nullptr;

// Reactive subjects for bed mesh data
static lv_subject_t bed_mesh_available;    // 0 = no mesh, 1 = mesh loaded
static lv_subject_t bed_mesh_profile_name; // String: active profile name
static lv_subject_t bed_mesh_dimensions;   // String: "10x10 points"
static lv_subject_t bed_mesh_z_range;      // String: "Z: 0.05 to 0.35mm"
static lv_subject_t bed_mesh_variance;     // String: "Range: 0.457 mm"

// String buffers for subjects (LVGL requires persistent buffers)
static char profile_name_buf[64] = "";
static char dimensions_buf[64] = "No mesh data";
static char z_range_buf[64] = "";
static char variance_buf[64] = "";

// Cleanup handler for panel deletion
static void panel_delete_cb(lv_event_t* e) {
    (void)e;

    spdlog::debug("[BedMesh] Panel delete event - cleaning up resources");

    // Widget cleanup (renderer cleanup is handled by widget delete callback)
    canvas = nullptr;
    profile_dropdown = nullptr;
    bed_mesh_panel = nullptr;
    parent_obj = nullptr;
}

// Back button event handler
static void back_button_cb(lv_event_t* e) {
    (void)e;

    // Use navigation history to go back to previous panel
    if (!ui_nav_go_back()) {
        // Fallback: If navigation history is empty, manually hide panel
        if (bed_mesh_panel) {
            lv_obj_add_flag(bed_mesh_panel, LV_OBJ_FLAG_HIDDEN);
        }

        // Show settings panel (typical parent)
        if (parent_obj) {
            lv_obj_t* settings_launcher = lv_obj_find_by_name(parent_obj, "settings_panel");
            if (settings_launcher) {
                lv_obj_clear_flag(settings_launcher, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// Profile dropdown event handler
static void profile_dropdown_cb(lv_event_t* e) {
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);

    // Get selected profile name
    char buf[64];
    lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));

    spdlog::info("[BedMesh] Profile selected: {}", buf);

    // Load the selected profile via G-code command
    MoonrakerClient* client = get_moonraker_client();
    if (client) {
        std::string cmd = "BED_MESH_PROFILE LOAD=" + std::string(buf);
        client->gcode_script(cmd);
        spdlog::debug("[BedMesh] Sent G-code: {}", cmd);
        // UI will update automatically via Moonraker notification callback
    } else {
        spdlog::warn("[BedMesh] Cannot load profile - Moonraker client is null");
    }
}

// Update UI subjects when bed mesh data changes
static void on_bed_mesh_update(const MoonrakerClient::BedMeshProfile& mesh) {
    spdlog::debug("[BedMesh] on_bed_mesh_update called, probed_matrix.size={}",
                  mesh.probed_matrix.size());

    if (mesh.probed_matrix.empty()) {
        lv_subject_set_int(&bed_mesh_available, 0);
        lv_subject_copy_string(&bed_mesh_dimensions, "No mesh data");
        lv_subject_copy_string(&bed_mesh_z_range, "");
        spdlog::warn("[BedMesh] No mesh data available");
        return;
    }

    // Update subjects
    lv_subject_set_int(&bed_mesh_available, 1);

    // Update profile name
    lv_subject_copy_string(&bed_mesh_profile_name, mesh.name.c_str());
    spdlog::debug("[BedMesh] Set profile name: {}", mesh.name);

    // Format and update dimensions
    snprintf(dimensions_buf, sizeof(dimensions_buf), "%dx%d points", mesh.x_count, mesh.y_count);
    lv_subject_copy_string(&bed_mesh_dimensions, dimensions_buf);
    spdlog::debug("[BedMesh] Set dimensions: {}", dimensions_buf);

    // Calculate Z range and variance
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    for (const auto& row : mesh.probed_matrix) {
        for (float z : row) {
            min_z = std::min(min_z, z);
            max_z = std::max(max_z, z);
        }
    }

    // Calculate variance (range)
    float variance = max_z - min_z;

    // Format and update Z range
    snprintf(z_range_buf, sizeof(z_range_buf), "Min: %.3f mm | Max: %.3f mm", min_z, max_z);
    lv_subject_copy_string(&bed_mesh_z_range, z_range_buf);
    spdlog::debug("[BedMesh] Set Z range: {}", z_range_buf);

    // Format and update variance
    snprintf(variance_buf, sizeof(variance_buf), "Range: %.3f mm", variance);
    lv_subject_copy_string(&bed_mesh_variance, variance_buf);
    spdlog::debug("[BedMesh] Set variance: {}", variance_buf);

    // Update renderer with new mesh data
    ui_panel_bed_mesh_set_data(mesh.probed_matrix);

    spdlog::info("[BedMesh] Mesh updated: {} ({}x{}, Z: {:.3f} to {:.3f})", mesh.name, mesh.x_count,
                 mesh.y_count, min_z, max_z);
}

void ui_panel_bed_mesh_init_subjects() {
    lv_subject_init_int(&bed_mesh_available, 0);
    lv_subject_init_string(&bed_mesh_profile_name, profile_name_buf, nullptr,
                           sizeof(profile_name_buf), "");
    lv_subject_init_string(&bed_mesh_dimensions, dimensions_buf, nullptr, sizeof(dimensions_buf),
                           "No mesh data");
    lv_subject_init_string(&bed_mesh_z_range, z_range_buf, nullptr, sizeof(z_range_buf), "");
    lv_subject_init_string(&bed_mesh_variance, variance_buf, nullptr, sizeof(variance_buf), "");

    // Register subjects for XML bindings
    lv_xml_register_subject(NULL, "bed_mesh_available", &bed_mesh_available);
    lv_xml_register_subject(NULL, "bed_mesh_profile_name", &bed_mesh_profile_name);
    lv_xml_register_subject(NULL, "bed_mesh_dimensions", &bed_mesh_dimensions);
    lv_xml_register_subject(NULL, "bed_mesh_z_range", &bed_mesh_z_range);
    lv_xml_register_subject(NULL, "bed_mesh_variance", &bed_mesh_variance);

    spdlog::debug("[BedMesh] Subjects initialized and registered");
}

void ui_panel_bed_mesh_setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    bed_mesh_panel = panel;
    parent_obj = parent_screen;

    spdlog::info("[BedMesh] Setting up event handlers...");

    // Find canvas widget (created by <bed_mesh> XML widget)
    canvas = lv_obj_find_by_name(panel, "bed_mesh_canvas");
    if (!canvas) {
        spdlog::error("[BedMesh] Canvas widget not found in XML");
        return;
    }
    spdlog::debug("[BedMesh] Found canvas widget - rotation controlled by touch drag");

    // Find and setup back button
    lv_obj_t* back_btn = lv_obj_find_by_name(panel, "back_button");
    if (back_btn) {
        lv_obj_add_event_cb(back_btn, back_button_cb, LV_EVENT_CLICKED, nullptr);
        spdlog::debug("[BedMesh] Back button configured");
    } else {
        spdlog::warn("[BedMesh] Back button not found in XML");
    }

    // Find and setup profile dropdown
    profile_dropdown = lv_obj_find_by_name(panel, "profile_dropdown");
    if (profile_dropdown) {
        // Get Moonraker client to fetch profile list
        MoonrakerClient* client = get_moonraker_client();
        if (client) {
            const auto& profiles = client->get_bed_mesh_profiles();
            if (!profiles.empty()) {
                // Build options string (newline-separated)
                std::string options;
                for (size_t i = 0; i < profiles.size(); i++) {
                    if (i > 0) options += "\n";
                    options += profiles[i];
                }
                lv_dropdown_set_options(profile_dropdown, options.c_str());
                spdlog::debug("[BedMesh] Profile dropdown populated with {} profiles", profiles.size());

                // Set selected index to match active profile
                const auto& active_mesh = client->get_active_bed_mesh();
                if (!active_mesh.name.empty()) {
                    for (size_t i = 0; i < profiles.size(); i++) {
                        if (profiles[i] == active_mesh.name) {
                            lv_dropdown_set_selected(profile_dropdown, i);
                            spdlog::debug("[BedMesh] Set dropdown to active profile: {}", active_mesh.name);
                            break;
                        }
                    }
                }
            } else {
                lv_dropdown_set_options(profile_dropdown, "(no profiles)");
                spdlog::warn("[BedMesh] No bed mesh profiles available");
            }

            // Register event callback
            lv_obj_add_event_cb(profile_dropdown, profile_dropdown_cb, LV_EVENT_VALUE_CHANGED, nullptr);
            spdlog::debug("[BedMesh] Profile dropdown event handler registered");
        } else {
            lv_dropdown_set_options(profile_dropdown, "(no connection)");
            spdlog::warn("[BedMesh] Cannot populate dropdown - Moonraker client is null");
        }
    } else {
        spdlog::warn("[BedMesh] Profile dropdown not found in XML");
    }

    // Canvas buffer and renderer already created by <bed_mesh> widget
    // Widget is initialized with default rotation angles
    // Rotation is now controlled by touch drag (no sliders/labels needed)

    // Register Moonraker callback for bed mesh updates
    MoonrakerClient* client = get_moonraker_client();
    if (client) {
        client->register_notify_update([client](json notification) {
            // Check if this notification contains bed_mesh updates
            if (notification.contains("params") && notification["params"].is_array() &&
                !notification["params"].empty()) {
                const json& params = notification["params"][0];
                if (params.contains("bed_mesh") && params["bed_mesh"].is_object()) {
                    // Mesh data was updated - refresh UI
                    on_bed_mesh_update(client->get_active_bed_mesh());
                }
            }
        });
        spdlog::debug("[BedMesh] Registered Moonraker callback for mesh updates");
    }

    // Load initial mesh data from MoonrakerClient (mock or real)
    if (client) {
        bool has_mesh = client->has_bed_mesh();
        spdlog::info("[BedMesh] Moonraker client found, has_bed_mesh={}", has_mesh);
        if (has_mesh) {
            const auto& mesh = client->get_active_bed_mesh();
            spdlog::info("[BedMesh] Active mesh: profile='{}', size={}x{}, rows={}", mesh.name,
                         mesh.x_count, mesh.y_count, mesh.probed_matrix.size());
            on_bed_mesh_update(mesh);
        } else {
            spdlog::info("[BedMesh] No mesh data available from Moonraker");
            // Panel will show "No mesh data" via subjects initialized in init_subjects()
        }
    } else {
        spdlog::warn("[BedMesh] Moonraker client is null!");
    }

    // Register cleanup handler
    lv_obj_add_event_cb(panel, panel_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::info("[BedMesh] Setup complete!");
}

void ui_panel_bed_mesh_set_data(const std::vector<std::vector<float>>& mesh_data) {
    if (!canvas) {
        spdlog::error("[BedMesh] Cannot set mesh data - canvas not initialized");
        return;
    }

    if (mesh_data.empty() || mesh_data[0].empty()) {
        spdlog::error("[BedMesh] Invalid mesh data - empty rows or columns");
        return;
    }

    int rows = mesh_data.size();
    int cols = mesh_data[0].size();

    // Convert std::vector to C-style array for widget API
    std::vector<const float*> row_pointers(rows);
    for (int i = 0; i < rows; i++) {
        row_pointers[i] = mesh_data[i].data();
    }

    // Set mesh data in widget (automatically triggers redraw)
    if (!ui_bed_mesh_set_data(canvas, row_pointers.data(), rows, cols)) {
        spdlog::error("[BedMesh] Failed to set mesh data in widget");
        return;
    }

    // Update subjects for info labels
    snprintf(dimensions_buf, sizeof(dimensions_buf), "%dx%d points", cols, rows);
    lv_subject_copy_string(&bed_mesh_dimensions, dimensions_buf);

    // Calculate Z range from mesh data
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    for (const auto& row : mesh_data) {
        for (float val : row) {
            min_z = std::min(min_z, val);
            max_z = std::max(max_z, val);
        }
    }

    snprintf(z_range_buf, sizeof(z_range_buf), "Z: %.3f to %.3f mm", min_z, max_z);
    lv_subject_copy_string(&bed_mesh_z_range, z_range_buf);
}

void ui_panel_bed_mesh_redraw() {
    if (!canvas) {
        spdlog::warn("[BedMesh] Cannot redraw - canvas not initialized");
        return;
    }

    // Trigger redraw via widget API
    ui_bed_mesh_redraw(canvas);
}
