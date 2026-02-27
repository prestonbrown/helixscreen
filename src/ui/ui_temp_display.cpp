// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_temp_display.h"

#include "ui_fonts.h"
#include "ui_temperature_utils.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>

using helix::ui::temperature::centi_to_degrees;
using helix::ui::temperature::centi_to_degrees_f;
using helix::ui::temperature::get_heating_state_color;

// ============================================================================
// Constants
// ============================================================================

/** Magic number to identify temp_display widgets ("TMP1" as ASCII) */
static constexpr uint32_t TEMP_DISPLAY_MAGIC = 0x544D5031;

// ============================================================================
// Per-widget user data
// ============================================================================

/**
 * @brief User data stored on each temp_display widget
 */
struct TempDisplayData {
    uint32_t magic = TEMP_DISPLAY_MAGIC;
    int current_centi = 0; // Centidegrees for precision formatting
    int current_temp = 0;  // Whole degrees (for heating color logic)
    int target_temp = 0;
    bool show_target = false;                 // Default: hide target (opt-in via prop)
    bool has_target_binding = false;          // True if bind_target was set (heater mode)
    bool target_subjects_initialized = false; // True if target subject was created

    // Child label pointers for efficient updates
    lv_obj_t* current_label = nullptr;
    lv_obj_t* separator_label = nullptr;
    lv_obj_t* target_label = nullptr;
    lv_obj_t* unit_label = nullptr;

    // String subjects for reactive text binding
    lv_subject_t current_text_subject;
    lv_subject_t target_text_subject;

    // Observers from lv_label_bind_text (must be removed before freeing subjects)
    lv_observer_t* current_text_observer = nullptr;
    lv_observer_t* target_text_observer = nullptr;

    // Buffers for formatted text
    char current_text_buf[16];
    char target_text_buf[16];

    // Optional click callback name (for XML event_cb prop)
    char event_cb_name[64] = {0};
};

// Static registry for safe cleanup
static std::unordered_map<lv_obj_t*, TempDisplayData*> s_registry;

static TempDisplayData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

// ============================================================================
// Internal helpers
// ============================================================================

/** Get font based on size string using shared helper */
static const lv_font_t* get_font_for_size(const char* size) {
    const char* font_token = theme_manager_size_to_font_token(size, "md");
    const lv_font_t* font = theme_manager_get_font(font_token);
    return font ? font : &noto_sans_18;
}

/** Tolerance for "at temperature" state (±degrees) */
static constexpr int AT_TEMP_TOLERANCE = 2;

/**
 * @brief Update current temp label color based on 4-state thermal logic
 *
 * Uses the shared get_heating_state_color() utility for consistent
 * color-coding across all temperature displays.
 *
 * For sensor-only displays (no bind_target), keeps text_primary color
 * since there's no heating state to indicate.
 */
static void update_heating_color(TempDisplayData* data) {
    if (!data || !data->current_label)
        return;

    // Sensor-only mode: no target binding, so no heating state to show
    // Keep text_primary for readability (e.g., chamber temp sensor)
    if (!data->has_target_binding) {
        lv_obj_set_style_text_color(data->current_label, theme_manager_get_color("text"),
                                    LV_PART_MAIN);
        return;
    }

    lv_color_t color =
        get_heating_state_color(data->current_temp, data->target_temp, AT_TEMP_TOLERANCE);
    lv_obj_set_style_text_color(data->current_label, color, LV_PART_MAIN);
}

/**
 * @brief Format target temp text - shows "--" when heater is off
 *
 * When show_target is true:
 * - target=0: Display "--" (heater off)
 * - target>0: Display actual temperature value
 */
static void format_target_text(TempDisplayData* data) {
    if (!data || !data->target_subjects_initialized)
        return;

    if (data->target_temp == 0) {
        snprintf(data->target_text_buf, sizeof(data->target_text_buf), "—");
    } else {
        snprintf(data->target_text_buf, sizeof(data->target_text_buf), "%d", data->target_temp);
    }
    lv_subject_copy_string(&data->target_text_subject, data->target_text_buf);
}

/** Format centidegrees as "XX.X" with one decimal place */
static void format_centi_temp(char* buf, size_t buf_size, int centi) {
    float deg = centi_to_degrees_f(centi);
    snprintf(buf, buf_size, "%.1f", deg);
}

/** Update the display text based on current values */
static void update_display(TempDisplayData* data) {
    if (!data)
        return;

    // Update current temp via subject
    format_centi_temp(data->current_text_buf, sizeof(data->current_text_buf), data->current_centi);
    lv_subject_copy_string(&data->current_text_subject, data->current_text_buf);

    // Update target temp via subject (shows "--" when heater off)
    format_target_text(data);

    // Update heating accent color
    update_heating_color(data);
}

/** Click event handler - invokes registered callback if set */
static void on_click(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto* data = get_data(obj);
    if (!data || data->event_cb_name[0] == '\0')
        return;

    // Look up the registered callback by name
    lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, data->event_cb_name);
    if (cb) {
        cb(e);
    } else {
        spdlog::warn("[temp_display] Event callback '{}' not found", data->event_cb_name);
    }
}

/** Cleanup callback when widget is deleted */
static void on_delete(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto it = s_registry.find(obj);
    if (it != s_registry.end()) {
        std::unique_ptr<TempDisplayData> data(it->second);
        s_registry.erase(it);

        // Deinitialize subjects to properly remove all attached observers.
        // lv_subject_deinit() removes observers from the subject side, which
        // also removes their unsubscribe_on_delete_cb from child widgets.
        // This is safe because we own these subjects. Manual lv_observer_remove()
        // would free the observer, but LVGL's child-delete would then try to
        // fire unsubscribe_on_delete_cb on the freed observer → crash.
        lv_subject_deinit(&data->current_text_subject);
        if (data->target_subjects_initialized) {
            lv_subject_deinit(&data->target_text_subject);
        }
        // data automatically freed when unique_ptr goes out of scope
    }
}

// ============================================================================
// Subject observer callbacks for reactive binding
// ============================================================================

/** Observer callback for current temperature subject */
static void current_temp_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* label = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    if (!label)
        return;

    // Get the parent container and its data
    lv_obj_t* container = lv_obj_get_parent(label);
    auto* data = get_data(container);
    if (!data)
        return;

    int centi = lv_subject_get_int(subject);
    data->current_centi = centi;
    data->current_temp = centi_to_degrees(centi);

    // Update color since it depends on current vs target comparison
    update_heating_color(data);

    // Update the text subject (which automatically updates the label via binding)
    format_centi_temp(data->current_text_buf, sizeof(data->current_text_buf), centi);
    lv_subject_copy_string(&data->current_text_subject, data->current_text_buf);
}

/** Observer callback for target temperature subject */
static void target_temp_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* label = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    if (!label)
        return;

    // Get the parent container and its data
    lv_obj_t* container = lv_obj_get_parent(label);
    auto* data = get_data(container);
    if (!data)
        return;

    int temp_deg = centi_to_degrees(lv_subject_get_int(subject));

    data->target_temp = temp_deg;

    // Update target text (shows "--" when heater off, actual value when on)
    format_target_text(data);

    // Update color based on 4-state logic
    update_heating_color(data);
}

// ============================================================================
// XML widget callbacks
// ============================================================================

/**
 * XML create callback for <temp_display> widget
 */
static void* ui_temp_display_create_cb(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));

    // Create main container (row layout)
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    // Flex row layout
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(container, 0, LV_PART_MAIN); // No gap between labels

    // Create user data
    auto data_ptr = std::make_unique<TempDisplayData>();

    // Parse size attribute for font selection
    const char* size = lv_xml_get_value_of(attrs, "size");
    const lv_font_t* font = get_font_for_size(size);

    // Look up colors once (theme_manager_get_color involves string lookups)
    lv_color_t text_color = theme_manager_get_color("text");
    lv_color_t muted_color = theme_manager_get_color("text_muted");

    // Parse show_target attribute (default is false, opt-in to show)
    const char* show_target_str = lv_xml_get_value_of(attrs, "show_target");
    if (show_target_str && strcmp(show_target_str, "true") == 0) {
        data_ptr->show_target = true;
    }

    // Create current temp label
    data_ptr->current_label = lv_label_create(container);
    lv_obj_set_style_text_font(data_ptr->current_label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(data_ptr->current_label, text_color, LV_PART_MAIN);

    if (data_ptr->show_target) {
        // Create separator label " / "
        data_ptr->separator_label = lv_label_create(container);
        lv_label_set_text(data_ptr->separator_label, " / ");
        lv_obj_set_style_text_font(data_ptr->separator_label, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(data_ptr->separator_label, muted_color, LV_PART_MAIN);

        // Create target temp label
        data_ptr->target_label = lv_label_create(container);
        lv_obj_set_style_text_font(data_ptr->target_label, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(data_ptr->target_label, text_color, LV_PART_MAIN);

        // Initialize target text subject
        snprintf(data_ptr->target_text_buf, sizeof(data_ptr->target_text_buf), "—");
        lv_subject_init_string(&data_ptr->target_text_subject, data_ptr->target_text_buf, nullptr,
                               sizeof(data_ptr->target_text_buf), data_ptr->target_text_buf);
        data_ptr->target_text_observer =
            lv_label_bind_text(data_ptr->target_label, &data_ptr->target_text_subject, nullptr);
        data_ptr->target_subjects_initialized = true;
    }

    // Create unit label "°C"
    data_ptr->unit_label = lv_label_create(container);
    lv_label_set_text(data_ptr->unit_label, "°C");
    lv_obj_set_style_text_font(data_ptr->unit_label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(data_ptr->unit_label, muted_color, LV_PART_MAIN);

    // Initialize current text subject
    snprintf(data_ptr->current_text_buf, sizeof(data_ptr->current_text_buf), "—");
    lv_subject_init_string(&data_ptr->current_text_subject, data_ptr->current_text_buf, nullptr,
                           sizeof(data_ptr->current_text_buf), data_ptr->current_text_buf);

    // Bind current label to subject for reactive updates
    data_ptr->current_text_observer =
        lv_label_bind_text(data_ptr->current_label, &data_ptr->current_text_subject, nullptr);

    // Register data and cleanup
    s_registry[container] = data_ptr.release();
    lv_obj_add_event_cb(container, on_delete, LV_EVENT_DELETE, nullptr);

    spdlog::trace("[temp_display] Created widget (size={}, show_target={})", size ? size : "md",
                  s_registry[container]->show_target);

    return container;
}

/**
 * XML apply callback for <temp_display> widget
 * Handles bind_current and bind_target for reactive binding
 */
static void ui_temp_display_apply_cb(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* container = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    auto* data = get_data(container);

    // Process custom binding attributes
    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "bind_current") == 0) {
            // Bind current temperature to a subject (NULL = global scope)
            lv_subject_t* subject = lv_xml_get_subject(nullptr, value);
            if (subject && data && data->current_label) {
                lv_subject_add_observer_obj(subject, current_temp_observer_cb, data->current_label,
                                            nullptr);
                // Set initial value
                int centi = lv_subject_get_int(subject);
                data->current_centi = centi;
                data->current_temp = centi_to_degrees(centi);
                format_centi_temp(data->current_text_buf, sizeof(data->current_text_buf), centi);
                lv_subject_copy_string(&data->current_text_subject, data->current_text_buf);
                spdlog::trace("[temp_display] Bound current to subject '{}' ({}°C)", value,
                              data->current_temp);
            } else if (!subject) {
                spdlog::warn("[temp_display] Subject '{}' not found for bind_current", value);
            }
        } else if (strcmp(name, "bind_target") == 0) {
            // Bind target temperature to a subject (NULL = global scope)
            lv_subject_t* subject = lv_xml_get_subject(nullptr, value);
            if (subject && data && data->current_label) {
                data->has_target_binding = true; // Mark as heater mode (not sensor-only)
                // Use current_label as observer target (callback traverses to
                // parent container to find data — works for any child label)
                lv_obj_t* obs_target =
                    data->target_label ? data->target_label : data->current_label;
                lv_subject_add_observer_obj(subject, target_temp_observer_cb, obs_target, nullptr);
                // Set initial value
                data->target_temp = centi_to_degrees(lv_subject_get_int(subject));
                // Update target label text if it exists
                format_target_text(data);
                // Apply initial heating color
                update_heating_color(data);
                spdlog::trace("[temp_display] Bound target to subject '{}' ({}°C)", value,
                              data->target_temp);
            } else if (!subject) {
                spdlog::warn("[temp_display] Subject '{}' not found for bind_target", value);
            }
        } else if (strcmp(name, "event_cb") == 0) {
            // Store callback name and make widget clickable
            if (data && value && value[0] != '\0') {
                strncpy(data->event_cb_name, value, sizeof(data->event_cb_name) - 1);
                data->event_cb_name[sizeof(data->event_cb_name) - 1] = '\0';
                lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(container, on_click, LV_EVENT_CLICKED, nullptr);
                spdlog::trace("[temp_display] Registered click callback '{}'", value);
            }
        }
    }

    // Apply base object properties (width, height, align, style_* etc.)
    lv_xml_obj_apply(state, attrs);
}

// ============================================================================
// Public API
// ============================================================================

void ui_temp_display_init(void) {
    lv_xml_register_widget("temp_display", ui_temp_display_create_cb, ui_temp_display_apply_cb);
    spdlog::trace("[temp_display] Registered temp_display widget");
}

void ui_temp_display_set(lv_obj_t* obj, int current, int target) {
    auto* data = get_data(obj);
    if (!data) {
        spdlog::warn("[temp_display] ui_temp_display_set called on non-temp_display widget");
        return;
    }

    data->current_centi = current * 10; // Approximate from whole degrees
    data->current_temp = current;
    data->target_temp = target;
    update_display(data);
}

void ui_temp_display_set_current(lv_obj_t* obj, int current) {
    auto* data = get_data(obj);
    if (!data) {
        return;
    }

    data->current_temp = current;

    // Update current temp via subject for efficiency
    // Note: this API takes whole degrees, so format as "XX.0"
    snprintf(data->current_text_buf, sizeof(data->current_text_buf), "%.1f",
             static_cast<float>(current));
    lv_subject_copy_string(&data->current_text_subject, data->current_text_buf);
}

int ui_temp_display_get_current(lv_obj_t* obj) {
    auto* data = get_data(obj);
    return data ? data->current_temp : -1;
}

int ui_temp_display_get_target(lv_obj_t* obj) {
    auto* data = get_data(obj);
    return data ? data->target_temp : -1;
}

bool ui_temp_display_is_valid(lv_obj_t* obj) {
    auto* data = get_data(obj);
    return data && data->magic == TEMP_DISPLAY_MAGIC;
}
