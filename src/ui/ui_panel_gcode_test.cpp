// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_gcode_test.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_gcode_viewer.h"

#include "display_settings_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

using namespace helix;

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <string>

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<GcodeTestPanel> g_gcode_test_panel;

GcodeTestPanel* get_gcode_test_panel(PrinterState& printer_state, IMoonrakerAPI* api) {
    if (!g_gcode_test_panel) {
        g_gcode_test_panel = std::make_unique<GcodeTestPanel>(printer_state, api);
        StaticPanelRegistry::instance().register_destroy("GcodeTestPanel",
                                                         []() { g_gcode_test_panel.reset(); });
    }
    return g_gcode_test_panel.get();
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

GcodeTestPanel::GcodeTestPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    spdlog::debug("[{}] Constructed", get_name());
}

GcodeTestPanel::~GcodeTestPanel() {
    // CRITICAL: Do NOT call LVGL functions here!
    // Static destruction order means LVGL may already be destroyed.
    // The file_picker_overlay_ is part of LVGL's widget tree and will be
    // cleaned up when the screen is destroyed.

    // Just reset internal state
    file_picker_overlay_ = nullptr;
    gcode_viewer_ = nullptr;
    stats_label_ = nullptr;
    gcode_files_.clear();

    // Note: Cannot log here - spdlog may be destroyed
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void GcodeTestPanel::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // No subjects for this panel - it doesn't use reactive data binding
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized (none)", get_name());
}

void GcodeTestPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    PanelBase::setup(panel, parent_screen);

    spdlog::info("[{}] Setting up panel", get_name());

    // Get widget references
    gcode_viewer_ = lv_obj_find_by_name(panel_, "gcode_viewer");
    stats_label_ = lv_obj_find_by_name(panel_, "stats_label");
    layer_slider_ = lv_obj_find_by_name(panel_, "layer_slider");
    layer_value_label_ = lv_obj_find_by_name(panel_, "layer_value_label");

    spdlog::debug("[{}] Widget lookup: viewer={}, stats={}, layer_slider={}, layer_label={}",
                  get_name(), (void*)gcode_viewer_, (void*)stats_label_, (void*)layer_slider_,
                  (void*)layer_value_label_);

    if (!gcode_viewer_) {
        LOG_ERROR_INTERNAL("[{}] Failed to find gcode_viewer widget", get_name());
        return;
    }

    if (!layer_slider_) {
        spdlog::warn("[{}] Failed to find layer_slider widget", get_name());
    }

    // Wire up all callbacks
    setup_callbacks();

    // Apply runtime config camera settings
    apply_runtime_config();

    // Apply render mode - priority: cmdline > env var > settings
    // Note: HELIX_GCODE_MODE env var is handled at widget creation
    const RuntimeConfig* rt_config = get_runtime_config();
    const char* env_mode = std::getenv("HELIX_GCODE_MODE");

    if (rt_config->gcode_render_mode >= 0) {
        // Command line takes highest priority
        auto render_mode = static_cast<GcodeViewerRenderMode>(rt_config->gcode_render_mode);
        ui_gcode_viewer_set_render_mode(gcode_viewer_, render_mode);
        spdlog::info("[{}] Render mode: {} ({}) [cmdline]", get_name(),
                     rt_config->gcode_render_mode,
                     rt_config->gcode_render_mode == 0   ? "Auto"
                     : rt_config->gcode_render_mode == 1 ? "3D"
                                                         : "2D Layers");
    } else if (env_mode) {
        // Env var already applied at widget creation - just log
        spdlog::info("[{}] Render mode: {} [env var HELIX_GCODE_MODE]", get_name(),
                     ui_gcode_viewer_is_using_2d_mode(gcode_viewer_) ? "2D" : "3D");
    } else {
        // No cmdline or env var - apply saved settings
        int render_mode_val = DisplaySettingsManager::instance().get_gcode_render_mode();
        auto render_mode = static_cast<GcodeViewerRenderMode>(render_mode_val);
        ui_gcode_viewer_set_render_mode(gcode_viewer_, render_mode);
        spdlog::info("[{}] Render mode: {} ({}) [settings]", get_name(), render_mode_val,
                     render_mode_val == 0 ? "Auto" : (render_mode_val == 1 ? "3D" : "2D Layers"));
    }

    // Register callback for async load completion
    ui_gcode_viewer_set_load_callback(gcode_viewer_, on_gcode_load_complete_static, this);

    // Auto-load file (either from config or default)
    std::string default_path = std::string(ASSETS_DIR) + "/" + DEFAULT_TEST_FILE;
    const RuntimeConfig* config = get_runtime_config();
    const char* file_to_load =
        config->gcode_test_file ? config->gcode_test_file : default_path.c_str();

    spdlog::info("[{}] Auto-loading file: {}", get_name(), file_to_load);
    load_file(file_to_load);

    spdlog::info("[{}] Panel setup complete", get_name());
}

void GcodeTestPanel::on_activate() {
    spdlog::debug("[{}] on_activate()", get_name());

    // Resume G-code viewer rendering
    if (gcode_viewer_) {
        ui_gcode_viewer_set_paused(gcode_viewer_, false);
    }
}

void GcodeTestPanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Pause G-code viewer rendering when panel is hidden (CPU optimization)
    if (gcode_viewer_) {
        ui_gcode_viewer_set_paused(gcode_viewer_, true);
    }
}

// ============================================================================
// PANEL-SPECIFIC API
// ============================================================================

void GcodeTestPanel::show_file_picker() {
    if (file_picker_overlay_) {
        // Already open
        return;
    }

    // Scan for files
    scan_gcode_files();

    if (gcode_files_.empty()) {
        spdlog::warn("[{}] No G-code files found in assets directory", get_name());
        return;
    }

    // Create full-screen overlay
    file_picker_overlay_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(file_picker_overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(file_picker_overlay_, theme_manager_get_color("screen_bg"), 0);
    lv_obj_set_style_bg_opa(file_picker_overlay_, 200, 0); // Semi-transparent
    lv_obj_set_style_pad_all(file_picker_overlay_, 40, 0);

    // Create card for file list
    lv_obj_t* card = lv_obj_create(file_picker_overlay_);
    lv_obj_set_size(card, LV_PCT(80), LV_PCT(80));
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_gap(card, 12, 0);

    // Header
    lv_obj_t* header = lv_label_create(card);
    lv_label_set_text(header, lv_tr("Select G-Code File"));

    // File list container
    lv_obj_t* list_container = lv_obj_create(card);
    lv_obj_set_width(list_container, LV_PCT(100));
    lv_obj_set_flex_grow(list_container, 1);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list_container, 8, 0);
    lv_obj_set_style_pad_gap(list_container, 8, 0);
    lv_obj_set_scroll_dir(list_container, LV_DIR_VER);

    // Add file buttons
    for (size_t i = 0; i < gcode_files_.size(); i++) {
        // Extract filename from path
        size_t last_slash = gcode_files_[i].find_last_of('/');
        std::string filename = (last_slash != std::string::npos)
                                   ? gcode_files_[i].substr(last_slash + 1)
                                   : gcode_files_[i];

        lv_obj_t* btn = lv_button_create(list_container);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 50);

        // Store both 'this' pointer and index in user_data
        // We encode the index in the event user_data
        lv_obj_add_event_cb(btn, on_file_selected_static, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
        // Store 'this' pointer in the button's user_data for the callback
        lv_obj_set_user_data(btn, this);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, filename.c_str());
        lv_obj_center(label);
    }

    // Close button
    lv_obj_t* close_btn = lv_button_create(card);
    lv_obj_set_width(close_btn, LV_PCT(100));
    lv_obj_set_height(close_btn, 50);
    lv_obj_add_event_cb(close_btn, on_file_picker_close_static, LV_EVENT_CLICKED, this);

    lv_obj_t* close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, lv_tr("Cancel"));
    lv_obj_center(close_label);

    spdlog::debug("[{}] File picker shown with {} files", get_name(), gcode_files_.size());
}

void GcodeTestPanel::close_file_picker() {
    // SAFETY: Use lv_obj_del_async — the Cancel button that triggered this call
    // is a child of file_picker_overlay_. Synchronous delete causes use-after-free
    // (issue #80).
    if (file_picker_overlay_) {
        lv_obj_del_async(file_picker_overlay_);
        file_picker_overlay_ = nullptr;
        spdlog::debug("[{}] File picker closed", get_name());
    }
}

void GcodeTestPanel::load_file(const char* filepath) {
    if (!gcode_viewer_) {
        LOG_ERROR_INTERNAL("[{}] Cannot load file - viewer not initialized", get_name());
        return;
    }

    // Set stats to "Loading..." immediately
    if (stats_label_) {
        lv_label_set_text(stats_label_, lv_tr("Loading..."));
    }

    ui_gcode_viewer_load_file(gcode_viewer_, filepath);
    // Stats will be updated by on_gcode_load_complete_static callback
}

void GcodeTestPanel::clear_viewer() {
    if (!gcode_viewer_) {
        return;
    }

    spdlog::info("[{}] Clearing viewer", get_name());
    ui_gcode_viewer_clear(gcode_viewer_);

    if (stats_label_) {
        lv_label_set_text(stats_label_, lv_tr("No file loaded"));
    }
}

// ============================================================================
// INTERNAL METHODS
// ============================================================================

void GcodeTestPanel::scan_gcode_files() {
    gcode_files_.clear();

    DIR* dir = opendir(ASSETS_DIR);
    if (!dir) {
        LOG_ERROR_INTERNAL("[{}] Failed to open assets directory: {}", get_name(), ASSETS_DIR);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;

        // Check if file ends with .gcode
        if (filename.length() > 6 && filename.substr(filename.length() - 6) == ".gcode") {
            std::string full_path = std::string(ASSETS_DIR) + "/" + filename;
            gcode_files_.push_back(full_path);
            spdlog::debug("[{}] Found G-code file: {}", get_name(), full_path);
        }
    }
    closedir(dir);

    // Sort files alphabetically
    std::sort(gcode_files_.begin(), gcode_files_.end());

    spdlog::info("[{}] Found {} G-code files", get_name(), gcode_files_.size());
}

void GcodeTestPanel::update_stats_label(const char* filename, int layer_count,
                                        const char* filament_type) {
    if (!stats_label_) {
        return;
    }

    // Clean up filament type for multi-tool prints (e.g., "ABS;ABS;ABS;ABS" -> "ABS")
    std::string filament_str;
    if (filament_type && filament_type[0] != '\0') {
        filament_str = filament_type;
        // If it's a semicolon-separated list where all entries are the same, just show once
        size_t first_semi = filament_str.find(';');
        if (first_semi != std::string::npos) {
            std::string first_type = filament_str.substr(0, first_semi);
            bool all_same = true;
            size_t pos = first_semi + 1;
            while (pos < filament_str.length()) {
                size_t next_semi = filament_str.find(';', pos);
                std::string next_type = filament_str.substr(
                    pos, next_semi == std::string::npos ? std::string::npos : next_semi - pos);
                if (next_type != first_type) {
                    all_same = false;
                    break;
                }
                if (next_semi == std::string::npos)
                    break;
                pos = next_semi + 1;
            }
            if (all_same) {
                filament_str = first_type;
            }
        }
    }

    // Build stats string: filename | layers | filament type
    char buf[256];
    if (!filament_str.empty()) {
        snprintf(buf, sizeof(buf), "%s | %d layers | %s", filename, layer_count,
                 filament_str.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s | %d layers", filename, layer_count);
    }

    lv_label_set_text(stats_label_, buf);
    spdlog::debug("[{}] Updated stats label: {}", get_name(), buf);
}

void GcodeTestPanel::setup_callbacks() {
    // Find all buttons
    lv_obj_t* btn_isometric = lv_obj_find_by_name(panel_, "btn_isometric");
    lv_obj_t* btn_top = lv_obj_find_by_name(panel_, "btn_top");
    lv_obj_t* btn_front = lv_obj_find_by_name(panel_, "btn_front");
    lv_obj_t* btn_side = lv_obj_find_by_name(panel_, "btn_side");
    lv_obj_t* btn_reset = lv_obj_find_by_name(panel_, "btn_reset");
    lv_obj_t* btn_zoom_in = lv_obj_find_by_name(panel_, "btn_zoom_in");
    lv_obj_t* btn_zoom_out = lv_obj_find_by_name(panel_, "btn_zoom_out");
    lv_obj_t* btn_load = lv_obj_find_by_name(panel_, "btn_load_test");
    lv_obj_t* btn_clear = lv_obj_find_by_name(panel_, "btn_clear");
    lv_obj_t* btn_travels = lv_obj_find_by_name(panel_, "btn_travels");

    // Find sliders
    lv_obj_t* specular_slider = lv_obj_find_by_name(panel_, "specular_slider");
    lv_obj_t* shininess_slider = lv_obj_find_by_name(panel_, "shininess_slider");

    // Find dropdowns
    lv_obj_t* ghost_mode_dropdown = lv_obj_find_by_name(panel_, "ghost_mode_dropdown");

    // Register view preset callbacks
    if (btn_isometric)
        lv_obj_add_event_cb(btn_isometric, on_view_preset_clicked_static, LV_EVENT_CLICKED, this);
    if (btn_top)
        lv_obj_add_event_cb(btn_top, on_view_preset_clicked_static, LV_EVENT_CLICKED, this);
    if (btn_front)
        lv_obj_add_event_cb(btn_front, on_view_preset_clicked_static, LV_EVENT_CLICKED, this);
    if (btn_side)
        lv_obj_add_event_cb(btn_side, on_view_preset_clicked_static, LV_EVENT_CLICKED, this);
    if (btn_reset)
        lv_obj_add_event_cb(btn_reset, on_view_preset_clicked_static, LV_EVENT_CLICKED, this);
    if (btn_travels)
        lv_obj_add_event_cb(btn_travels, on_view_preset_clicked_static, LV_EVENT_CLICKED, this);

    // Register zoom callbacks
    if (btn_zoom_in)
        lv_obj_add_event_cb(btn_zoom_in, on_zoom_clicked_static, LV_EVENT_CLICKED, this);
    if (btn_zoom_out)
        lv_obj_add_event_cb(btn_zoom_out, on_zoom_clicked_static, LV_EVENT_CLICKED, this);

    // Register file operation callbacks
    if (btn_load)
        lv_obj_add_event_cb(btn_load, on_load_test_file_static, LV_EVENT_CLICKED, this);
    if (btn_clear)
        lv_obj_add_event_cb(btn_clear, on_clear_static, LV_EVENT_CLICKED, this);

    // Register slider callbacks
    if (specular_slider)
        lv_obj_add_event_cb(specular_slider, on_specular_intensity_changed_static,
                            LV_EVENT_VALUE_CHANGED, this);
    if (shininess_slider)
        lv_obj_add_event_cb(shininess_slider, on_shininess_changed_static, LV_EVENT_VALUE_CHANGED,
                            this);
    if (layer_slider_)
        lv_obj_add_event_cb(layer_slider_, on_layer_slider_changed_static, LV_EVENT_VALUE_CHANGED,
                            this);

    // Register dropdown callbacks
    if (ghost_mode_dropdown)
        lv_obj_add_event_cb(ghost_mode_dropdown, on_ghost_mode_changed_static,
                            LV_EVENT_VALUE_CHANGED, this);

    spdlog::debug("[{}] Callbacks registered", get_name());
}

void GcodeTestPanel::apply_runtime_config() {
    if (!gcode_viewer_) {
        return;
    }

    const RuntimeConfig* config = get_runtime_config();

    if (config->gcode_camera_azimuth_set) {
        spdlog::info("[{}] Setting camera azimuth: {}", get_name(), config->gcode_camera_azimuth);
        ui_gcode_viewer_set_camera_azimuth(gcode_viewer_, config->gcode_camera_azimuth);
    }

    if (config->gcode_camera_elevation_set) {
        spdlog::info("[{}] Setting camera elevation: {}", get_name(),
                     config->gcode_camera_elevation);
        ui_gcode_viewer_set_camera_elevation(gcode_viewer_, config->gcode_camera_elevation);
    }

    if (config->gcode_camera_zoom_set) {
        spdlog::info("[{}] Setting camera zoom: {}", get_name(), config->gcode_camera_zoom);
        ui_gcode_viewer_set_camera_zoom(gcode_viewer_, config->gcode_camera_zoom);
    }

    if (config->gcode_debug_colors) {
        spdlog::info("[{}] Enabling debug face colors", get_name());
        ui_gcode_viewer_set_debug_colors(gcode_viewer_, true);
    }
}

// ============================================================================
// STATIC CALLBACKS (TRAMPOLINES)
// ============================================================================

void GcodeTestPanel::on_gcode_load_complete_static(lv_obj_t* /*viewer*/, void* user_data,
                                                   bool success) {
    auto* self = static_cast<GcodeTestPanel*>(user_data);
    if (self) {
        self->handle_gcode_load_complete(success);
    }
}

void GcodeTestPanel::on_file_selected_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_file_selected");

    // Get the button that was clicked
    lv_obj_t* btn = lv_event_get_target_obj(e);
    auto* self = static_cast<GcodeTestPanel*>(lv_obj_get_user_data(btn));

    // Get the index from the event user_data
    uint32_t index = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    if (self) {
        self->handle_file_selected(index);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_file_picker_close_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_file_picker_close");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->close_file_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_view_preset_clicked_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_view_preset_clicked");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    lv_obj_t* btn = lv_event_get_target_obj(e);
    const char* name = lv_obj_get_name(btn);

    if (self && name) {
        self->handle_view_preset(name, btn);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_zoom_clicked_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_zoom_clicked");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    lv_obj_t* btn = lv_event_get_target_obj(e);
    const char* name = lv_obj_get_name(btn);

    if (self && name) {
        self->handle_zoom(name);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_load_test_file_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_load_test_file");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->show_file_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_clear_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_clear");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->clear_viewer();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_specular_intensity_changed_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_specular_intensity_changed");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    lv_obj_t* slider = lv_event_get_target_obj(e);

    if (self) {
        self->handle_specular_change(slider);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_shininess_changed_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_shininess_changed");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    lv_obj_t* slider = lv_event_get_target_obj(e);

    if (self) {
        self->handle_shininess_change(slider);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_layer_slider_changed_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_layer_slider_changed");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    lv_obj_t* slider = lv_event_get_target_obj(e);

    if (self && slider) {
        int32_t value = lv_slider_get_value(slider);
        self->handle_layer_slider_change(value);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void GcodeTestPanel::on_ghost_mode_changed_static(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GcodeTestPanel] on_ghost_mode_changed");
    auto* self = static_cast<GcodeTestPanel*>(lv_event_get_user_data(e));
    lv_obj_t* dropdown = lv_event_get_target_obj(e);

    if (self && dropdown && self->gcode_viewer_) {
        uint32_t selected = lv_dropdown_get_selected(dropdown);
        // Dropdown: 0=Stipple (default), 1=Solid/Dimmed
        int mode = (selected == 0) ? 1 : 0; // Stipple=1, Dimmed=0
        spdlog::debug("[GcodeTestPanel] Ghost mode changed to: {} (dropdown idx {})",
                      (mode == 1) ? "Stipple" : "Solid", selected);
        ui_gcode_viewer_set_ghost_mode(self->gcode_viewer_, mode);
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// INSTANCE METHODS (CALLED BY TRAMPOLINES)
// ============================================================================

void GcodeTestPanel::handle_gcode_load_complete(bool success) {
    if (!success) {
        LOG_ERROR_INTERNAL("[{}] G-code load callback: failed", get_name());
        if (stats_label_) {
            lv_label_set_text(stats_label_, lv_tr("Error loading file"));
        }
        return;
    }

    spdlog::info("[{}] G-code load callback: success", get_name());

    // Get file info from viewer
    const char* full_path = ui_gcode_viewer_get_filename(gcode_viewer_);
    int layer_count = ui_gcode_viewer_get_layer_count(gcode_viewer_);
    const char* filament_type = ui_gcode_viewer_get_filament_type(gcode_viewer_);

    // Extract just the filename from full path
    const char* filename = full_path ? strrchr(full_path, '/') : nullptr;
    filename = filename ? filename + 1 : (full_path ? full_path : "Unknown");

    update_stats_label(filename, layer_count, filament_type);

    // Update layer slider range for ghost layer testing
    update_layer_slider_range();
}

void GcodeTestPanel::handle_file_selected(uint32_t index) {
    if (index >= gcode_files_.size()) {
        LOG_ERROR_INTERNAL("[{}] Invalid file index: {}", get_name(), index);
        return;
    }

    const std::string& filepath = gcode_files_[index];
    spdlog::info("[{}] Loading selected file: {}", get_name(), filepath);

    // Load the file
    load_file(filepath.c_str());

    // Close the file picker
    close_file_picker();
}

void GcodeTestPanel::handle_view_preset(const char* button_name, lv_obj_t* btn) {
    if (!gcode_viewer_) {
        return;
    }

    spdlog::info("[{}] View preset clicked: {}", get_name(), button_name);

    if (strcmp(button_name, "btn_travels") == 0) {
        // Toggle travel moves visibility
        bool is_checked = lv_obj_has_state(btn, LV_STATE_CHECKED);
        ui_gcode_viewer_set_show_travels(gcode_viewer_, is_checked);
        spdlog::info("[{}] Travel moves: {}", get_name(), is_checked ? "shown" : "hidden");
    } else if (strcmp(button_name, "btn_top") == 0) {
        ui_gcode_viewer_set_view(gcode_viewer_, GcodeViewerPresetView::Top);
    } else if (strcmp(button_name, "btn_front") == 0) {
        ui_gcode_viewer_set_view(gcode_viewer_, GcodeViewerPresetView::Front);
    } else if (strcmp(button_name, "btn_side") == 0) {
        ui_gcode_viewer_set_view(gcode_viewer_, GcodeViewerPresetView::Side);
    } else if (strcmp(button_name, "btn_reset") == 0) {
        ui_gcode_viewer_reset_camera(gcode_viewer_);
    }
}

void GcodeTestPanel::handle_zoom(const char* button_name) {
    if (!gcode_viewer_) {
        return;
    }

    float zoom_step = 1.2f; // 20% zoom per click

    if (strcmp(button_name, "btn_zoom_in") == 0) {
        ui_gcode_viewer_zoom(gcode_viewer_, zoom_step);
        spdlog::debug("[{}] Zoom in clicked", get_name());
    } else if (strcmp(button_name, "btn_zoom_out") == 0) {
        ui_gcode_viewer_zoom(gcode_viewer_, 1.0f / zoom_step);
        spdlog::debug("[{}] Zoom out clicked", get_name());
    }
}

void GcodeTestPanel::handle_specular_change(lv_obj_t* slider) {
    if (!gcode_viewer_) {
        return;
    }

    int32_t value = lv_slider_get_value(slider);
    float intensity = value / 100.0f; // 0-20 → 0.0-0.2

    // Update value label
    lv_obj_t* container = lv_obj_get_parent(slider);
    lv_obj_t* label = lv_obj_find_by_name(container, "specular_value_label");
    if (label) {
        lv_label_set_text_fmt(label, "%.2f", intensity);
    }

    // Get current shininess value
    lv_obj_t* shininess_slider = lv_obj_find_by_name(panel_, "shininess_slider");
    float shininess = 15.0f;
    if (shininess_slider) {
        shininess = (float)lv_slider_get_value(shininess_slider);
    }

    // Update material
    ui_gcode_viewer_set_specular(gcode_viewer_, intensity, shininess);
}

void GcodeTestPanel::handle_shininess_change(lv_obj_t* slider) {
    if (!gcode_viewer_) {
        return;
    }

    int32_t value = lv_slider_get_value(slider);

    // Update value label
    lv_obj_t* container = lv_obj_get_parent(slider);
    lv_obj_t* label = lv_obj_find_by_name(container, "shininess_value_label");
    if (label) {
        lv_label_set_text_fmt(label, "%d", (int)value);
    }

    // Get current specular intensity value
    lv_obj_t* intensity_slider = lv_obj_find_by_name(panel_, "specular_slider");
    float intensity = 0.05f;
    if (intensity_slider) {
        intensity = lv_slider_get_value(intensity_slider) / 100.0f;
    }

    // Update material
    ui_gcode_viewer_set_specular(gcode_viewer_, intensity, (float)value);
}

void GcodeTestPanel::handle_layer_slider_change(int32_t value) {
    if (!gcode_viewer_) {
        return;
    }

    int max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
    if (max_layer < 0) {
        return; // No geometry loaded
    }

    // Slider at max = show all layers solid (disable ghost mode)
    // Slider at 0 = layer 0 solid, rest ghost
    // Slider at N = layers 0..N solid, rest ghost
    if (value >= max_layer) {
        // Disable ghost mode - all layers solid
        ui_gcode_viewer_set_print_progress(gcode_viewer_, -1);
    } else {
        // Enable ghost mode at this layer
        ui_gcode_viewer_set_print_progress(gcode_viewer_, value);
    }

    // Update label
    if (layer_value_label_) {
        lv_label_set_text_fmt(layer_value_label_, " %d / %d", value, max_layer);
    }

    spdlog::trace("[{}] Layer slider: {} / {} (ghost={})", get_name(), value, max_layer,
                  value < max_layer);
}

void GcodeTestPanel::update_layer_slider_range() {
    spdlog::info("[{}] update_layer_slider_range() called: viewer={}, slider={}", get_name(),
                 (void*)gcode_viewer_, (void*)layer_slider_);

    if (!gcode_viewer_ || !layer_slider_) {
        spdlog::warn("[{}] update_layer_slider_range: missing widget (viewer={}, slider={})",
                     get_name(), (void*)gcode_viewer_, (void*)layer_slider_);
        return;
    }

    int max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
    spdlog::info("[{}] ui_gcode_viewer_get_max_layer returned: {}", get_name(), max_layer);

    // At load-complete the 2D layer renderer may not be initialized yet (it's set up
    // lazily on the first render pass), so get_max_layer() can report 0 even though the
    // file parsed fine. Fall back to the parsed-file layer count, which is the source of
    // truth and available immediately — otherwise the slider is stuck at 0/0 in 2D mode.
    if (max_layer <= 0) {
        int layer_count = ui_gcode_viewer_get_layer_count(gcode_viewer_);
        if (layer_count > 0) {
            max_layer = layer_count - 1;
        }
    }

    if (max_layer < 0) {
        max_layer = 0;
    }

    // Set slider range and initialize to max (all layers visible, no ghost)
    lv_slider_set_range(layer_slider_, 0, max_layer);
    lv_slider_set_value(layer_slider_, max_layer, LV_ANIM_OFF);

    // Update label
    if (layer_value_label_) {
        lv_label_set_text_fmt(layer_value_label_, " %d / %d", max_layer, max_layer);
    }

    // Disable ghost mode initially
    ui_gcode_viewer_set_print_progress(gcode_viewer_, -1);

    spdlog::info("[{}] Layer slider range updated: 0-{}", get_name(), max_layer);
}

// ============================================================================
// DEPRECATED LEGACY API
// ============================================================================

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

lv_obj_t* ui_panel_gcode_test_create(lv_obj_t* parent) {
    // Get printer state singleton (required by PanelBase)
    extern PrinterState& get_printer_state();
    PrinterState& ps = get_printer_state();

    // Get or create global panel instance
    GcodeTestPanel* panel = get_gcode_test_panel(ps, nullptr);

    // Initialize subjects (no-op for this panel)
    panel->init_subjects();

    // Create XML component
    lv_obj_t* panel_root =
        (lv_obj_t*)lv_xml_create(parent, panel->get_xml_component_name(), nullptr);
    if (!panel_root) {
        LOG_ERROR_INTERNAL("[GcodeTestPanel] Failed to load XML component");
        return nullptr;
    }

    // Setup the panel
    panel->setup(panel_root, parent);

    return panel_root;
}
