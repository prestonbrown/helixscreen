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

#include "ui_panel_controls_temp.h"
#include "ui_component_keypad.h"
#include "ui_component_header_bar.h"
#include "ui_utils.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include "ui_temp_graph.h"
#include <spdlog/spdlog.h>
#include <string.h>
#include <math.h>

// Heater type enumeration
typedef enum {
    HEATER_NOZZLE,
    HEATER_BED
} heater_type_t;

// Heater configuration structure
typedef struct {
    heater_type_t type;
    const char* name;
    const char* title;
    lv_color_t color;
    float temp_range_max;
    int y_axis_increment;
    int default_mock_target;
    struct {
        int off;
        int pla;
        int petg;
        int abs;
    } presets;
    struct {
        float min;
        float max;
    } keypad_range;
} heater_config_t;

// Temperature subjects (reactive data binding)
static lv_subject_t nozzle_current_subject;
static lv_subject_t nozzle_target_subject;
static lv_subject_t bed_current_subject;
static lv_subject_t bed_target_subject;
static lv_subject_t nozzle_display_subject;
static lv_subject_t bed_display_subject;

// Subject storage buffers
static char nozzle_current_buf[16];
static char nozzle_target_buf[16];
static char bed_current_buf[16];
static char bed_target_buf[16];
static char nozzle_display_buf[32];
static char bed_display_buf[32];

// Current temperature state
static int nozzle_current = 25;
static int nozzle_target = 0;
static int bed_current = 25;
static int bed_target = 0;

// Temperature limits (can be updated from Moonraker heater config)
static int nozzle_min_temp = 0;
static int nozzle_max_temp = 500;  // Safe default for most hotends
static int bed_min_temp = 0;
static int bed_max_temp = 150;     // Safe default for most heatbeds

// Panel widgets
static lv_obj_t* nozzle_panel = nullptr;
static lv_obj_t* bed_panel = nullptr;
static lv_obj_t* parent_obj = nullptr;

// Temperature graph widgets
static ui_temp_graph_t* nozzle_graph = nullptr;
static ui_temp_graph_t* bed_graph = nullptr;
static int nozzle_series_id = -1;
static int bed_series_id = -1;

// Heater configurations
static const heater_config_t NOZZLE_CONFIG = {
    .type = HEATER_NOZZLE,
    .name = "Nozzle",
    .title = "Nozzle Temperature",
    .color = lv_color_hex(0xFF4444),     // Red
    .temp_range_max = 320.0f,
    .y_axis_increment = 80,
    .default_mock_target = 210,
    .presets = {0, 210, 240, 250},
    .keypad_range = {0.0f, 350.0f}
};

static const heater_config_t BED_CONFIG = {
    .type = HEATER_BED,
    .name = "Bed",
    .title = "Heatbed Temperature",
    .color = lv_color_hex(0x00CED1),     // Cyan
    .temp_range_max = 140.0f,
    .y_axis_increment = 35,
    .default_mock_target = 60,
    .presets = {0, 60, 80, 100},
    .keypad_range = {0.0f, 150.0f}
};

// Forward declarations for callbacks
static void update_nozzle_display();
static void update_bed_display();

// Generate mock temperature data for realistic heating curve with dramatic changes
static void generate_mock_temp_data(float* temps, int count, float start_temp, float target_temp) {
    const float room_temp = 25.0f;
    const float actual_start = start_temp > 0 ? start_temp : room_temp;

    for (int i = 0; i < count; i++) {
        float progress = (float)i / (float)(count - 1);

        if (target_temp == 0.0f) {
            // Cooling curve (exponential decay to room temp)
            temps[i] = room_temp + (actual_start - room_temp) * expf(-progress * 4.5f);
        } else {
            // Heating curve with more dramatic overshoot and oscillation
            float base_curve = actual_start + (target_temp - actual_start) * (1.0f - expf(-progress * 6.0f));
            float overshoot = (target_temp - actual_start) * 0.12f * expf(-progress * 8.0f) * sinf(progress * 3.14159f * 3.0f);
            temps[i] = base_curve + overshoot;

            // Add some noise for realism
            float noise = ((float)(i % 7) - 3.0f) * 0.5f;
            temps[i] += noise;
        }
    }
}

void ui_panel_controls_temp_init_subjects() {
    // Initialize temperature subjects with default values
    snprintf(nozzle_current_buf, sizeof(nozzle_current_buf), "%d°C", nozzle_current);
    snprintf(nozzle_target_buf, sizeof(nozzle_target_buf), "%d°C", nozzle_target);
    snprintf(bed_current_buf, sizeof(bed_current_buf), "%d°C", bed_current);
    snprintf(bed_target_buf, sizeof(bed_target_buf), "%d°C", bed_target);
    snprintf(nozzle_display_buf, sizeof(nozzle_display_buf), "%d / %d°C", nozzle_current, nozzle_target);
    snprintf(bed_display_buf, sizeof(bed_display_buf), "%d / %d°C", bed_current, bed_target);

    lv_subject_init_string(&nozzle_current_subject, nozzle_current_buf, nullptr, sizeof(nozzle_current_buf), nozzle_current_buf);
    lv_subject_init_string(&nozzle_target_subject, nozzle_target_buf, nullptr, sizeof(nozzle_target_buf), nozzle_target_buf);
    lv_subject_init_string(&bed_current_subject, bed_current_buf, nullptr, sizeof(bed_current_buf), bed_current_buf);
    lv_subject_init_string(&bed_target_subject, bed_target_buf, nullptr, sizeof(bed_target_buf), bed_target_buf);
    lv_subject_init_string(&nozzle_display_subject, nozzle_display_buf, nullptr, sizeof(nozzle_display_buf), nozzle_display_buf);
    lv_subject_init_string(&bed_display_subject, bed_display_buf, nullptr, sizeof(bed_display_buf), bed_display_buf);

    // Register subjects with XML system (global scope)
    lv_xml_register_subject(NULL, "nozzle_current_temp", &nozzle_current_subject);
    lv_xml_register_subject(NULL, "nozzle_target_temp", &nozzle_target_subject);
    lv_xml_register_subject(NULL, "bed_current_temp", &bed_current_subject);
    lv_xml_register_subject(NULL, "bed_target_temp", &bed_target_subject);
    lv_xml_register_subject(NULL, "nozzle_temp_display", &nozzle_display_subject);
    lv_xml_register_subject(NULL, "bed_temp_display", &bed_display_subject);

    spdlog::info("[Temp] Subjects initialized: nozzle={}/{}°C, bed={}/{}°C",
                 nozzle_current, nozzle_target, bed_current, bed_target);
}

// Update nozzle display text
static void update_nozzle_display() {
    snprintf(nozzle_display_buf, sizeof(nozzle_display_buf), "%d / %d°C", nozzle_current, nozzle_target);
    lv_subject_copy_string(&nozzle_display_subject, nozzle_display_buf);
}

// Update bed display text
static void update_bed_display() {
    snprintf(bed_display_buf, sizeof(bed_display_buf), "%d / %d°C", bed_current, bed_target);
    lv_subject_copy_string(&bed_display_subject, bed_display_buf);
}

// ============================================================================
// COMMON HELPER FUNCTIONS
// ============================================================================

// Helper: Create Y-axis temperature labels
static void create_y_axis_labels(lv_obj_t* container, const heater_config_t* config) {
    if (!container) return;

    // Calculate number of labels based on range and increment
    int num_labels = (int)(config->temp_range_max / config->y_axis_increment) + 1;

    // Create labels from top to bottom
    for (int i = num_labels - 1; i >= 0; i--) {
        int temp = i * config->y_axis_increment;
        lv_obj_t* label = lv_label_create(container);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d°", temp);
        lv_label_set_text(label, buf);
        // Theme handles text color
        lv_obj_set_style_text_font(label, UI_FONT_TINY, 0);  // Compact chart axis labels
    }
}

// Helper: Create and configure temperature graph
static ui_temp_graph_t* create_temp_graph(lv_obj_t* chart_area, const heater_config_t* config,
                                          int current_temp, int target_temp, int* series_id_out) {
    if (!chart_area) return nullptr;

    ui_temp_graph_t* graph = ui_temp_graph_create(chart_area);
    if (!graph) return nullptr;

    lv_obj_t* chart = ui_temp_graph_get_chart(graph);
    lv_obj_set_size(chart, lv_pct(100), lv_pct(100));

    // Configure temperature range
    ui_temp_graph_set_temp_range(graph, 0.0f, config->temp_range_max);

    // Add series
    int series_id = ui_temp_graph_add_series(graph, config->name, config->color);
    if (series_id_out) {
        *series_id_out = series_id;
    }

    if (series_id >= 0) {
        // Use mock target if current target is 0
        int mock_target = (target_temp == 0) ? config->default_mock_target : target_temp;

        // Set target temperature line
        ui_temp_graph_set_series_target(graph, series_id, (float)mock_target, true);

        // Generate and populate mock temperature data
        const int point_count = 100;
        float temps[point_count];
        generate_mock_temp_data(temps, point_count, (float)current_temp, (float)mock_target);
        ui_temp_graph_set_series_data(graph, series_id, temps, point_count);

        spdlog::debug("[Temp]   ✓ {} graph created with mock data", config->name);
    }

    return graph;
}

// Helper: Get current/target/display values by heater type
static void get_heater_state(heater_type_t type, int* current_out, int* target_out,
                             void (**update_fn_out)()) {
    if (type == HEATER_NOZZLE) {
        if (current_out) *current_out = nozzle_current;
        if (target_out) *target_out = nozzle_target;
        if (update_fn_out) *update_fn_out = update_nozzle_display;
    } else {
        if (current_out) *current_out = bed_current;
        if (target_out) *target_out = bed_target;
        if (update_fn_out) *update_fn_out = update_bed_display;
    }
}

// Helper: Set heater target temperature
static void set_heater_target(heater_type_t type, int temp) {
    if (type == HEATER_NOZZLE) {
        nozzle_target = temp;
        update_nozzle_display();
    } else {
        bed_target = temp;
        update_bed_display();
    }
}

// Helper: Generic preset button callback
static void preset_button_cb_generic(lv_event_t* e, const heater_config_t* config) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    const char* name = lv_obj_get_name(btn);
    if (!name) return;

    int temp = 0;
    if (strcmp(name, "preset_off") == 0) {
        temp = config->presets.off;
    } else if (strcmp(name, "preset_pla") == 0) {
        temp = config->presets.pla;
    } else if (strcmp(name, "preset_petg") == 0) {
        temp = config->presets.petg;
    } else if (strcmp(name, "preset_abs") == 0) {
        temp = config->presets.abs;
    }

    set_heater_target(config->type, temp);
    spdlog::debug("[Temp] {} target set to {}°C via preset", config->name, temp);
}

// Helper: Setup preset buttons with generic callback
static void setup_preset_buttons(lv_obj_t* panel, const heater_config_t* config,
                                 lv_event_cb_t callback) {
    const char* preset_names[] = {"preset_off", "preset_pla", "preset_petg", "preset_abs"};
    for (const char* name : preset_names) {
        lv_obj_t* btn = lv_obj_find_by_name(panel, name);
        if (btn) {
            lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, (void*)config);
        }
    }
}

// Helper: Custom temperature callback wrapper
typedef struct {
    heater_type_t type;
    const heater_config_t* config;
} custom_callback_data_t;

static custom_callback_data_t nozzle_custom_data = {HEATER_NOZZLE, &NOZZLE_CONFIG};
static custom_callback_data_t bed_custom_data = {HEATER_BED, &BED_CONFIG};

static void custom_temp_callback_generic(float value, void* user_data) {
    custom_callback_data_t* data = (custom_callback_data_t*)user_data;
    set_heater_target(data->type, (int)value);
    spdlog::debug("[Temp] {} target set to {}°C via custom input", data->config->name, (int)value);
}

// Helper: Generic custom button callback
static void custom_button_cb_generic(lv_event_t* e, const heater_config_t* config,
                                     custom_callback_data_t* callback_data) {
    (void)e;

    int current_target;
    get_heater_state(config->type, nullptr, &current_target, nullptr);

    spdlog::debug("[Temp] Opening keypad for {} custom temperature", config->name);

    ui_keypad_config_t keypad_config = {
        .initial_value = (float)current_target,
        .min_value = config->keypad_range.min,
        .max_value = config->keypad_range.max,
        .title_label = config->type == HEATER_NOZZLE ? "Nozzle Temp" : "Heat Bed Temp",
        .unit_label = "°C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback = custom_temp_callback_generic,
        .user_data = callback_data
    };

    ui_keypad_show(&keypad_config);
}

// ============================================================================
// NOZZLE TEMPERATURE PANEL
// ============================================================================

// Event handler: Back button (nozzle panel)
static void nozzle_back_button_cb(lv_event_t* e) {
    (void)e;

    // Use navigation history to go back to previous panel
    if (!ui_nav_go_back()) {
        // Fallback: If navigation history is empty, manually hide and show controls launcher
        if (nozzle_panel) {
            lv_obj_add_flag(nozzle_panel, LV_OBJ_FLAG_HIDDEN);
        }

        if (parent_obj) {
            lv_obj_t* controls_launcher = lv_obj_find_by_name(parent_obj, "controls_panel");
            if (controls_launcher) {
                lv_obj_clear_flag(controls_launcher, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// Event handler: Confirm button (nozzle panel)
static void nozzle_confirm_button_cb(lv_event_t* e) {
    (void)e;
    spdlog::info("[Temp] Nozzle temperature confirmed: {}°C", nozzle_target);

    // TODO: Send command to printer (moonraker_set_nozzle_temp(nozzle_target))

    // Return to launcher
    nozzle_back_button_cb(e);
}

// Event handler: Nozzle preset buttons (wrapper for generic handler)
static void nozzle_preset_button_cb(lv_event_t* e) {
    const heater_config_t* config = (const heater_config_t*)lv_event_get_user_data(e);
    preset_button_cb_generic(e, config);
}

// Event handler: Nozzle custom button (wrapper for generic handler)
static void nozzle_custom_button_cb(lv_event_t* e) {
    custom_button_cb_generic(e, &NOZZLE_CONFIG, &nozzle_custom_data);
}

// Resize callback for responsive padding (nozzle panel)
static void nozzle_on_resize() {
    if (!nozzle_panel || !parent_obj) {
        return;
    }

    lv_obj_t* temp_content = lv_obj_find_by_name(nozzle_panel, "temp_content");
    if (temp_content) {
        lv_coord_t padding = ui_get_header_content_padding(lv_obj_get_height(parent_obj));
        lv_obj_set_style_pad_all(temp_content, padding, 0);
    }
}

void ui_panel_controls_temp_nozzle_setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    nozzle_panel = panel;
    parent_obj = parent_screen;

    spdlog::info("[Temp] Setting up nozzle panel event handlers...");

    // Create Y-axis labels using common helper
    lv_obj_t* y_axis_labels = lv_obj_find_by_name(panel, "y_axis_labels");
    if (y_axis_labels) {
        create_y_axis_labels(y_axis_labels, &NOZZLE_CONFIG);
    }

    // Create temperature graph using common helper
    lv_obj_t* chart_area = lv_obj_find_by_name(panel, "chart_area");
    if (chart_area) {
        nozzle_graph = create_temp_graph(chart_area, &NOZZLE_CONFIG, nozzle_current, nozzle_target, &nozzle_series_id);
    }

    // Setup header for responsive height
    lv_obj_t* nozzle_temp_header = lv_obj_find_by_name(panel, "nozzle_temp_header");
    if (nozzle_temp_header) {
        ui_component_header_bar_setup(nozzle_temp_header, parent_screen);
    }

    // Set responsive padding for content area
    lv_obj_t* temp_content = lv_obj_find_by_name(panel, "temp_content");
    if (temp_content) {
        lv_coord_t padding = ui_get_header_content_padding(lv_obj_get_height(parent_screen));
        lv_obj_set_style_pad_all(temp_content, padding, 0);
        spdlog::debug("[Temp]   ✓ Content padding: {}px (responsive)", padding);
    }

    // Register resize callback
    ui_resize_handler_register(nozzle_on_resize);

    // Back button
    lv_obj_t* back_btn = lv_obj_find_by_name(panel, "back_button");
    if (back_btn) {
        lv_obj_add_event_cb(back_btn, nozzle_back_button_cb, LV_EVENT_CLICKED, nullptr);
        spdlog::debug("[Temp]   ✓ Back button");
    }

    // Show and wire confirm button
    lv_obj_t* header = lv_obj_find_by_name(panel, "nozzle_temp_header");
    if (header && ui_header_bar_show_right_button(header)) {
        lv_obj_t* confirm_btn = lv_obj_find_by_name(header, "right_button");
        if (confirm_btn) {
            lv_obj_add_event_cb(confirm_btn, nozzle_confirm_button_cb, LV_EVENT_CLICKED, nullptr);
            spdlog::debug("[Temp]   ✓ Confirm button");
        }
    }

    // Preset buttons (using generic helper)
    setup_preset_buttons(panel, &NOZZLE_CONFIG, nozzle_preset_button_cb);
    spdlog::debug("[Temp]   ✓ Preset buttons (4)");

    // Custom button
    lv_obj_t* custom_btn = lv_obj_find_by_name(panel, "btn_custom");
    if (custom_btn) {
        lv_obj_add_event_cb(custom_btn, nozzle_custom_button_cb, LV_EVENT_CLICKED, nullptr);
        spdlog::debug("[Temp]   ✓ Custom button");
    }

    spdlog::info("[Temp] Nozzle panel setup complete!");
}

// ============================================================================
// BED TEMPERATURE PANEL
// ============================================================================

// Event handler: Back button (bed panel)
static void bed_back_button_cb(lv_event_t* e) {
    (void)e;

    // Use navigation history to go back to previous panel
    if (!ui_nav_go_back()) {
        // Fallback: If navigation history is empty, manually hide and show controls launcher
        if (bed_panel) {
            lv_obj_add_flag(bed_panel, LV_OBJ_FLAG_HIDDEN);
        }

        if (parent_obj) {
            lv_obj_t* controls_launcher = lv_obj_find_by_name(parent_obj, "controls_panel");
            if (controls_launcher) {
                lv_obj_clear_flag(controls_launcher, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// Event handler: Confirm button (bed panel)
static void bed_confirm_button_cb(lv_event_t* e) {
    (void)e;
    spdlog::info("[Temp] Bed temperature confirmed: {}°C", bed_target);

    // TODO: Send command to printer (moonraker_set_bed_temp(bed_target))

    // Return to launcher
    bed_back_button_cb(e);
}

// Event handler: Bed preset buttons (wrapper for generic handler)
static void bed_preset_button_cb(lv_event_t* e) {
    const heater_config_t* config = (const heater_config_t*)lv_event_get_user_data(e);
    preset_button_cb_generic(e, config);
}

// Event handler: Bed custom button (wrapper for generic handler)
static void bed_custom_button_cb(lv_event_t* e) {
    custom_button_cb_generic(e, &BED_CONFIG, &bed_custom_data);
}

// Resize callback for responsive padding (bed panel)
static void bed_on_resize() {
    if (!bed_panel || !parent_obj) {
        return;
    }

    lv_obj_t* temp_content = lv_obj_find_by_name(bed_panel, "temp_content");
    if (temp_content) {
        lv_coord_t padding = ui_get_header_content_padding(lv_obj_get_height(parent_obj));
        lv_obj_set_style_pad_all(temp_content, padding, 0);
    }
}

void ui_panel_controls_temp_bed_setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    bed_panel = panel;
    parent_obj = parent_screen;

    spdlog::info("[Temp] Setting up bed panel event handlers...");

    // Create Y-axis labels using common helper
    lv_obj_t* y_axis_labels = lv_obj_find_by_name(panel, "y_axis_labels");
    if (y_axis_labels) {
        create_y_axis_labels(y_axis_labels, &BED_CONFIG);
    }

    // Create temperature graph using common helper
    lv_obj_t* chart_area = lv_obj_find_by_name(panel, "chart_area");
    if (chart_area) {
        bed_graph = create_temp_graph(chart_area, &BED_CONFIG, bed_current, bed_target, &bed_series_id);
    }

    // Setup header for responsive height
    lv_obj_t* bed_temp_header = lv_obj_find_by_name(panel, "bed_temp_header");
    if (bed_temp_header) {
        ui_component_header_bar_setup(bed_temp_header, parent_screen);
    }

    // Set responsive padding for content area
    lv_obj_t* temp_content = lv_obj_find_by_name(panel, "temp_content");
    if (temp_content) {
        lv_coord_t padding = ui_get_header_content_padding(lv_obj_get_height(parent_screen));
        lv_obj_set_style_pad_all(temp_content, padding, 0);
        spdlog::debug("[Temp]   ✓ Content padding: {}px (responsive)", padding);
    }

    // Register resize callback
    ui_resize_handler_register(bed_on_resize);

    // Back button
    lv_obj_t* back_btn = lv_obj_find_by_name(panel, "back_button");
    if (back_btn) {
        lv_obj_add_event_cb(back_btn, bed_back_button_cb, LV_EVENT_CLICKED, nullptr);
        spdlog::debug("[Temp]   ✓ Back button");
    }

    // Show and wire confirm button
    lv_obj_t* header = lv_obj_find_by_name(panel, "bed_temp_header");
    if (header && ui_header_bar_show_right_button(header)) {
        lv_obj_t* confirm_btn = lv_obj_find_by_name(header, "right_button");
        if (confirm_btn) {
            lv_obj_add_event_cb(confirm_btn, bed_confirm_button_cb, LV_EVENT_CLICKED, nullptr);
            spdlog::debug("[Temp]   ✓ Confirm button");
        }
    }

    // Preset buttons (using generic helper)
    setup_preset_buttons(panel, &BED_CONFIG, bed_preset_button_cb);
    spdlog::debug("[Temp]   ✓ Preset buttons (4)");

    // Custom button
    lv_obj_t* custom_btn = lv_obj_find_by_name(panel, "btn_custom");
    if (custom_btn) {
        lv_obj_add_event_cb(custom_btn, bed_custom_button_cb, LV_EVENT_CLICKED, nullptr);
        spdlog::debug("[Temp]   ✓ Custom button");
    }

    spdlog::info("[Temp] Bed panel setup complete!");
}

// ============================================================================
// PUBLIC API
// ============================================================================

void ui_panel_controls_temp_set_nozzle(int current, int target) {
    // Validate temperature ranges using dynamic limits
    if (current < nozzle_min_temp || current > nozzle_max_temp) {
        spdlog::warn("[Temp] Invalid nozzle current temperature {}°C (valid: {}-{}°C), clamping",
                     current, nozzle_min_temp, nozzle_max_temp);
        current = (current < nozzle_min_temp) ? nozzle_min_temp : nozzle_max_temp;
    }
    if (target < nozzle_min_temp || target > nozzle_max_temp) {
        spdlog::warn("[Temp] Invalid nozzle target temperature {}°C (valid: {}-{}°C), clamping",
                     target, nozzle_min_temp, nozzle_max_temp);
        target = (target < nozzle_min_temp) ? nozzle_min_temp : nozzle_max_temp;
    }

    nozzle_current = current;
    nozzle_target = target;
    update_nozzle_display();
}

void ui_panel_controls_temp_set_bed(int current, int target) {
    // Validate temperature ranges using dynamic limits
    if (current < bed_min_temp || current > bed_max_temp) {
        spdlog::warn("[Temp] Invalid bed current temperature {}°C (valid: {}-{}°C), clamping",
                     current, bed_min_temp, bed_max_temp);
        current = (current < bed_min_temp) ? bed_min_temp : bed_max_temp;
    }
    if (target < bed_min_temp || target > bed_max_temp) {
        spdlog::warn("[Temp] Invalid bed target temperature {}°C (valid: {}-{}°C), clamping",
                     target, bed_min_temp, bed_max_temp);
        target = (target < bed_min_temp) ? bed_min_temp : bed_max_temp;
    }

    bed_current = current;
    bed_target = target;
    update_bed_display();
}

int ui_panel_controls_temp_get_nozzle_target() {
    return nozzle_target;
}

int ui_panel_controls_temp_get_bed_target() {
    return bed_target;
}

void ui_panel_controls_temp_set_nozzle_limits(int min_temp, int max_temp) {
    nozzle_min_temp = min_temp;
    nozzle_max_temp = max_temp;
    spdlog::info("[Temp] Nozzle temperature limits updated: {}-{}°C", min_temp, max_temp);
}

void ui_panel_controls_temp_set_bed_limits(int min_temp, int max_temp) {
    bed_min_temp = min_temp;
    bed_max_temp = max_temp;
    spdlog::info("[Temp] Bed temperature limits updated: {}-{}°C", min_temp, max_temp);
}
