// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_bed_mesh.h"

#include "ui_bed_mesh.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"

#include "app_globals.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <limits>

static std::unique_ptr<BedMeshPanel> g_bed_mesh_panel;

BedMeshPanel::BedMeshPanel(PrinterState& printer_state, MoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Initialize buffer contents
    std::memset(profile_name_buf_, 0, sizeof(profile_name_buf_));
    std::strncpy(dimensions_buf_, "No mesh data", sizeof(dimensions_buf_) - 1);
    std::memset(z_range_buf_, 0, sizeof(z_range_buf_));
    std::memset(variance_buf_, 0, sizeof(variance_buf_));
}

BedMeshPanel::~BedMeshPanel() {
    // Widget pointers owned by LVGL - just clear them
    canvas_ = nullptr;
    profile_dropdown_ = nullptr;
}

void BedMeshPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize subjects with default values
    UI_SUBJECT_INIT_AND_REGISTER_INT(bed_mesh_available_, 0, "bed_mesh_available");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(bed_mesh_profile_name_, profile_name_buf_, "",
                                        "bed_mesh_profile_name");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(bed_mesh_dimensions_, dimensions_buf_, "No mesh data",
                                        "bed_mesh_dimensions");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(bed_mesh_z_range_, z_range_buf_, "", "bed_mesh_z_range");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(bed_mesh_variance_, variance_buf_, "", "bed_mesh_variance");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized and registered", get_name());
}

void BedMeshPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::info("[{}] Setting up event handlers...", get_name());

    // Use standard overlay panel setup (wires header, back button, handles responsive padding)
    ui_overlay_panel_setup_standard(panel_, parent_screen_, "overlay_header", "overlay_content");

    lv_obj_t* overlay_content = lv_obj_find_by_name(panel_, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[{}] overlay_content not found!", get_name());
        return;
    }

    // Find canvas widget (created by <bed_mesh> XML widget)
    canvas_ = lv_obj_find_by_name(overlay_content, "bed_mesh_canvas");
    if (!canvas_) {
        spdlog::error("[{}] Canvas widget 'bed_mesh_canvas' not found in XML", get_name());
        return;
    }
    spdlog::debug("[{}] Found canvas widget - rotation controlled by touch drag", get_name());

    // Setup profile dropdown
    setup_profile_dropdown();

    // Setup Moonraker subscription for mesh updates
    setup_moonraker_subscription();

    // Load initial mesh data from MoonrakerClient
    MoonrakerClient* client = get_moonraker_client();
    if (client && client->has_bed_mesh()) {
        const auto& mesh = client->get_active_bed_mesh();
        spdlog::info("[{}] Active mesh: profile='{}', size={}x{}", get_name(), mesh.name,
                     mesh.x_count, mesh.y_count);
        on_mesh_update_internal(mesh);
    } else {
        spdlog::info("[{}] No mesh data available from Moonraker", get_name());
    }

    // Register cleanup handler
    lv_obj_add_event_cb(panel_, on_panel_delete, LV_EVENT_DELETE, this);

    spdlog::info("[{}] Setup complete!", get_name());
}

void BedMeshPanel::set_mesh_data(const std::vector<std::vector<float>>& mesh_data) {
    if (!canvas_) {
        spdlog::error("[{}] Cannot set mesh data - canvas not initialized", get_name());
        return;
    }

    if (mesh_data.empty() || mesh_data[0].empty()) {
        spdlog::error("[{}] Invalid mesh data - empty rows or columns", get_name());
        return;
    }

    int rows = static_cast<int>(mesh_data.size());
    int cols = static_cast<int>(mesh_data[0].size());

    // Convert std::vector to C-style array for widget API
    std::vector<const float*> row_pointers(rows);
    for (int i = 0; i < rows; i++) {
        row_pointers[i] = mesh_data[i].data();
    }

    // Set mesh data in widget (automatically triggers redraw)
    if (!ui_bed_mesh_set_data(canvas_, row_pointers.data(), rows, cols)) {
        spdlog::error("[{}] Failed to set mesh data in widget", get_name());
        return;
    }

    // Update info subjects
    update_info_subjects(mesh_data, cols, rows);
}

void BedMeshPanel::redraw() {
    if (!canvas_) {
        spdlog::warn("[{}] Cannot redraw - canvas not initialized", get_name());
        return;
    }

    ui_bed_mesh_redraw(canvas_);
}

void BedMeshPanel::setup_profile_dropdown() {
    lv_obj_t* overlay_content = lv_obj_find_by_name(panel_, "overlay_content");
    if (!overlay_content)
        return;

    profile_dropdown_ = lv_obj_find_by_name(overlay_content, "profile_dropdown");
    if (!profile_dropdown_) {
        spdlog::warn("[{}] Profile dropdown not found in XML", get_name());
        return;
    }

    MoonrakerClient* client = get_moonraker_client();
    if (!client) {
        lv_dropdown_set_options(profile_dropdown_, "(no connection)");
        spdlog::warn("[{}] Cannot populate dropdown - Moonraker client is null", get_name());
        return;
    }

    const auto& profiles = client->get_bed_mesh_profiles();
    if (profiles.empty()) {
        lv_dropdown_set_options(profile_dropdown_, "(no profiles)");
        spdlog::warn("[{}] No bed mesh profiles available", get_name());
        return;
    }

    // Build options string (newline-separated)
    std::string options;
    for (size_t i = 0; i < profiles.size(); i++) {
        if (i > 0)
            options += "\n";
        options += profiles[i];
    }
    lv_dropdown_set_options(profile_dropdown_, options.c_str());
    spdlog::debug("[{}] Profile dropdown populated with {} profiles", get_name(), profiles.size());

    // Set selected index to match active profile
    const auto& active_mesh = client->get_active_bed_mesh();
    if (!active_mesh.name.empty()) {
        for (size_t i = 0; i < profiles.size(); i++) {
            if (profiles[i] == active_mesh.name) {
                lv_dropdown_set_selected(profile_dropdown_, static_cast<uint32_t>(i));
                spdlog::debug("[{}] Set dropdown to active profile: {}", get_name(),
                              active_mesh.name);
                break;
            }
        }
    }

    // Register event callback
    lv_obj_add_event_cb(profile_dropdown_, on_profile_dropdown_changed, LV_EVENT_VALUE_CHANGED,
                        this);
    spdlog::debug("[{}] Profile dropdown event handler registered", get_name());
}

void BedMeshPanel::setup_moonraker_subscription() {
    MoonrakerClient* client = get_moonraker_client();
    if (!client) {
        spdlog::warn("[{}] Cannot subscribe to Moonraker - client is null", get_name());
        return;
    }

    // Note: We capture 'this' in the lambda. This is safe because:
    // 1. Panels are destroyed when the app exits
    // 2. MoonrakerClient doesn't outlive the UI
    // If we had a proper unregister API, we'd use it in the destructor.
    client->register_notify_update([this, client](nlohmann::json notification) {
        // Check if this notification contains bed_mesh updates
        if (notification.contains("params") && notification["params"].is_array() &&
            !notification["params"].empty()) {
            const nlohmann::json& params = notification["params"][0];
            if (params.contains("bed_mesh") && params["bed_mesh"].is_object()) {
                // Mesh data was updated - refresh UI
                on_mesh_update_internal(client->get_active_bed_mesh());
            }
        }
    });
    spdlog::debug("[{}] Registered Moonraker callback for mesh updates", get_name());
}

void BedMeshPanel::on_mesh_update_internal(const BedMeshProfile& mesh) {
    spdlog::debug("[{}] on_mesh_update_internal called, probed_matrix.size={}", get_name(),
                  mesh.probed_matrix.size());

    if (mesh.probed_matrix.empty()) {
        lv_subject_set_int(&bed_mesh_available_, 0);
        lv_subject_copy_string(&bed_mesh_dimensions_, "No mesh data");
        lv_subject_copy_string(&bed_mesh_z_range_, "");
        lv_subject_copy_string(&bed_mesh_variance_, "");
        spdlog::warn("[{}] No mesh data available", get_name());
        return;
    }

    // Update availability subject
    lv_subject_set_int(&bed_mesh_available_, 1);

    // Update profile name
    lv_subject_copy_string(&bed_mesh_profile_name_, mesh.name.c_str());
    spdlog::debug("[{}] Set profile name: {}", get_name(), mesh.name);

    // Format and update dimensions
    std::snprintf(dimensions_buf_, sizeof(dimensions_buf_), "%dx%d points", mesh.x_count,
                  mesh.y_count);
    lv_subject_copy_string(&bed_mesh_dimensions_, dimensions_buf_);
    spdlog::debug("[{}] Set dimensions: {}", get_name(), dimensions_buf_);

    // Calculate Z range and variance, track coordinates of min/max points
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    int min_row = 0, min_col = 0;
    int max_row = 0, max_col = 0;

    for (size_t row = 0; row < mesh.probed_matrix.size(); row++) {
        for (size_t col = 0; col < mesh.probed_matrix[row].size(); col++) {
            float z = mesh.probed_matrix[row][col];
            if (z < min_z) {
                min_z = z;
                min_row = static_cast<int>(row);
                min_col = static_cast<int>(col);
            }
            if (z > max_z) {
                max_z = z;
                max_row = static_cast<int>(row);
                max_col = static_cast<int>(col);
            }
        }
    }

    // Convert mesh indices to real-world coordinates (matching bed_mesh_renderer coordinate system)
    // X: (col - (cols-1)/2.0) * 50.0
    // Y: ((rows-1-row) - (rows-1)/2.0) * 50.0
    float min_x = (min_col - (mesh.x_count - 1) / 2.0f) * 50.0f;
    float min_y = ((mesh.y_count - 1 - min_row) - (mesh.y_count - 1) / 2.0f) * 50.0f;
    float max_x = (max_col - (mesh.x_count - 1) / 2.0f) * 50.0f;
    float max_y = ((mesh.y_count - 1 - max_row) - (mesh.y_count - 1) / 2.0f) * 50.0f;

    // Calculate variance (range)
    float variance = max_z - min_z;

    // Format and update Z range with coordinates (Mainsail format)
    std::snprintf(z_range_buf_, sizeof(z_range_buf_),
                  "Max [%.1f, %.1f] = %.3f mm\nMin [%.1f, %.1f] = %.3f mm", max_x, max_y, max_z,
                  min_x, min_y, min_z);
    lv_subject_copy_string(&bed_mesh_z_range_, z_range_buf_);
    spdlog::debug("[{}] Set Z range: {}", get_name(), z_range_buf_);

    // Format and update variance
    std::snprintf(variance_buf_, sizeof(variance_buf_), "Range: %.3f mm", variance);
    lv_subject_copy_string(&bed_mesh_variance_, variance_buf_);
    spdlog::debug("[{}] Set variance: {}", get_name(), variance_buf_);

    // Update renderer with new mesh data
    set_mesh_data(mesh.probed_matrix);

    spdlog::info("[{}] Mesh updated: {} ({}x{}, Z: {:.3f} to {:.3f})", get_name(), mesh.name,
                 mesh.x_count, mesh.y_count, min_z, max_z);
}

void BedMeshPanel::update_info_subjects(const std::vector<std::vector<float>>& mesh_data, int cols,
                                        int rows) {
    // Update dimensions subject
    std::snprintf(dimensions_buf_, sizeof(dimensions_buf_), "%dx%d points", cols, rows);
    lv_subject_copy_string(&bed_mesh_dimensions_, dimensions_buf_);

    // Calculate Z range from mesh data
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    for (const auto& row : mesh_data) {
        for (float val : row) {
            min_z = std::min(min_z, val);
            max_z = std::max(max_z, val);
        }
    }

    std::snprintf(z_range_buf_, sizeof(z_range_buf_), "Z: %.3f to %.3f mm", min_z, max_z);
    lv_subject_copy_string(&bed_mesh_z_range_, z_range_buf_);
}

void BedMeshPanel::on_panel_delete(lv_event_t* e) {
    auto* self = static_cast<BedMeshPanel*>(lv_event_get_user_data(e));
    if (!self)
        return;

    spdlog::debug("[{}] Panel delete event - cleaning up resources", self->get_name());

    // Clear widget pointers (owned by LVGL)
    self->canvas_ = nullptr;
    self->profile_dropdown_ = nullptr;
}

void BedMeshPanel::on_profile_dropdown_changed(lv_event_t* e) {
    auto* self = static_cast<BedMeshPanel*>(lv_event_get_user_data(e));
    if (!self)
        return;

    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));

    // Get selected profile name
    char buf[64];
    lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));

    spdlog::info("[{}] Profile selected: {}", self->get_name(), buf);

    // Load the selected profile via G-code command
    MoonrakerClient* client = get_moonraker_client();
    if (client) {
        std::string cmd = "BED_MESH_PROFILE LOAD=" + std::string(buf);
        client->gcode_script(cmd);
        spdlog::debug("[{}] Sent G-code: {}", self->get_name(), cmd);
        // UI will update automatically via Moonraker notification callback
    } else {
        spdlog::warn("[{}] Cannot load profile - Moonraker client is null", self->get_name());
    }
}

BedMeshPanel& get_global_bed_mesh_panel() {
    if (!g_bed_mesh_panel) {
        // Create with dummy PrinterState - legacy API doesn't have proper DI
        extern PrinterState& get_printer_state();
        g_bed_mesh_panel = std::make_unique<BedMeshPanel>(get_printer_state(), nullptr);
    }
    return *g_bed_mesh_panel;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
