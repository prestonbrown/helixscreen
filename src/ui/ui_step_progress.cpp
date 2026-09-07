// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_step_progress.h"

#include "ui_fonts.h" // For mdi_icons_16 font
#include "ui_widget_memory.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace helix;

// Internal widget data stored in user_data
typedef struct {
    char** label_buffers;  // String storage for labels
    StepState* states;     // Current state for each step
    lv_obj_t** connectors; // Connector line objects (step_count - 1)
    lv_obj_t** step_items; // Step item objects (step_count)
    int step_count;
    bool horizontal;
    int32_t circle_size;     // Responsive circle diameter
    int32_t connector_width; // Responsive connector line thickness
} step_progress_data_t;

// Theme-aware color variables (loaded from component scope or defaults)
static lv_color_t color_pending;
static lv_color_t color_active;
static lv_color_t color_completed;
static lv_color_t color_number_pending;
static lv_color_t color_number_active;
static lv_color_t color_checkmark;
static lv_color_t color_label_active;
static lv_color_t color_label_inactive;

// Helper to read a scope color, falling back to a computed color
static lv_color_t get_step_color(lv_xml_component_scope_t* scope, bool dark,
                                 const char* scope_light, const char* scope_dark,
                                 lv_color_t fallback) {
    if (scope) {
        const char* val = lv_xml_get_const(scope, dark ? scope_dark : scope_light);
        if (val)
            return theme_manager_parse_hex_color(val);
    }
    return fallback;
}

// Helper to read a scope color with theme token fallback
static lv_color_t get_step_color(lv_xml_component_scope_t* scope, bool dark,
                                 const char* scope_light, const char* scope_dark,
                                 const char* token_fallback) {
    return get_step_color(scope, dark, scope_light, scope_dark,
                          theme_manager_get_color(token_fallback));
}

// Initialize colors from component scope or theme tokens
static void init_step_progress_colors(const char* scope_name) {
    lv_xml_component_scope_t* scope = scope_name ? lv_xml_component_get_scope(scope_name) : nullptr;
    bool dark = theme_manager_is_dark_mode();

    color_pending =
        get_step_color(scope, dark, "step_pending_light", "step_pending_dark", "text_subtle");
    color_active = get_step_color(scope, dark, "step_active_light", "step_active_dark", "primary");
    color_completed =
        get_step_color(scope, dark, "step_completed_light", "step_completed_dark", "success");
    // The circles are accent fills, so the glyphs on them are black-or-white by
    // luminance unless the component scope names its own colours.
    color_number_pending =
        get_step_color(scope, dark, "step_number_pending_light", "step_number_pending_dark",
                       theme_manager_get_readable_on(color_pending));
    color_number_active =
        get_step_color(scope, dark, "step_number_active_light", "step_number_active_dark",
                       theme_manager_get_readable_on(color_active));
    color_checkmark =
        get_step_color(scope, dark, "step_number_active_light", "step_number_active_dark",
                       theme_manager_get_readable_on(color_completed));
    color_label_active =
        get_step_color(scope, dark, "step_label_active_light", "step_label_active_dark", "text");
    color_label_inactive = get_step_color(scope, dark, "step_label_inactive_light",
                                          "step_label_inactive_dark", "text_subtle");

    spdlog::debug("[StepProgress] Colors loaded (scope={}, mode={})",
                  scope_name ? scope_name : "none", dark ? "dark" : "light");
}

/**
 * Apply state-based styling to a step item's indicator and label
 */
static void apply_step_styling(lv_obj_t* step_item, StepState state) {
    if (!step_item)
        return;

    // Find widgets by traversing children (known structure)
    lv_obj_t* indicator_column = lv_obj_get_child(step_item, 0);
    if (!indicator_column)
        return;

    lv_obj_t* circle = lv_obj_get_child(indicator_column, 0);
    lv_obj_t* step_number = circle ? lv_obj_get_child(circle, 0) : nullptr;
    lv_obj_t* checkmark = circle ? lv_obj_get_child(circle, 1) : nullptr;
    lv_obj_t* label = lv_obj_get_child(step_item, 1);

    lv_color_t color;
    lv_opa_t fill_opa;

    switch (state) {
    case StepState::Pending:
        color = color_pending;
        fill_opa = LV_OPA_COVER; // Filled gray circle
        break;
    case StepState::Active:
        color = color_active;
        fill_opa = LV_OPA_COVER; // Filled red circle
        break;
    case StepState::Completed:
        color = color_completed;
        fill_opa = LV_OPA_COVER; // Fully filled circle (255)
        break;
    default:
        color = color_pending;
        fill_opa = LV_OPA_COVER;
        break;
    }

    // Apply circle styling
    if (circle) {
        lv_obj_set_style_border_color(circle, color, 0);
        lv_obj_set_style_bg_color(circle, color, 0);
        lv_obj_set_style_bg_opa(circle, fill_opa, 0);
    }

    // Toggle step number and checkmark visibility, and set number color
    if (state == StepState::Completed) {
        // Completed: hide step number, show checkmark
        if (step_number)
            lv_obj_add_flag(step_number, LV_OBJ_FLAG_HIDDEN);
        if (checkmark)
            lv_obj_clear_flag(checkmark, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Pending/Active: show step number, hide checkmark
        if (step_number) {
            lv_obj_clear_flag(step_number, LV_OBJ_FLAG_HIDDEN);
            // Use theme-aware number colors
            lv_color_t number_color =
                (state == StepState::Pending) ? color_number_pending : color_number_active;
            lv_obj_set_style_text_color(step_number, number_color, 0);
        }
        if (checkmark)
            lv_obj_add_flag(checkmark, LV_OBJ_FLAG_HIDDEN);
    }

    // Connector recolor is handled by set_current()/set_completed() against the
    // connector objects owned in data->connectors[] (siblings in the container),
    // not here — connectors are not children of the step item / indicator column.

    // Apply label styling (slightly larger for active, smaller for inactive)
    if (label) {
        if (state == StepState::Active) {
            lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
            lv_obj_set_style_text_color(label, color_label_active, 0); // Theme-aware active color
        } else {
            lv_obj_set_style_text_font(label, theme_manager_get_font("font_small"), 0);
            lv_obj_set_style_text_color(label, color_label_inactive,
                                        0); // Theme-aware inactive color
        }
    }
}

/**
 * Cleanup callback for widget deletion
 */
static void step_progress_delete_cb(lv_event_t* e) {
    lv_obj_t* widget = lv_event_get_target_obj(e);
    // Transfer ownership to RAII wrapper - automatic cleanup
    lvgl_unique_ptr<step_progress_data_t> data((step_progress_data_t*)lv_obj_get_user_data(widget));
    lv_obj_set_user_data(widget, nullptr);

    if (data) {
        // Free nested allocations
        if (data->label_buffers) {
            for (int i = 0; i < data->step_count; i++) {
                lvgl_unique_ptr<char> label(data->label_buffers[i]);
            }
            lvgl_unique_ptr<char*> buffers(data->label_buffers);
        }

        if (data->states) {
            lvgl_unique_ptr<StepState> states(data->states);
        }

        // connectors and step_items are LVGL children — freed by lv_obj_delete, not us
        if (data->connectors) {
            lvgl_unique_ptr<lv_obj_t*> conns(data->connectors);
        }
        if (data->step_items) {
            lvgl_unique_ptr<lv_obj_t*> items(data->step_items);
        }
    }
}

/**
 * Position the connector lines between adjacent step circles from the CURRENT
 * layout, creating each one on first call and updating its size/pos thereafter.
 *
 * Connectors are absolutely-positioned IGNORE_LAYOUT rects, so they do not move
 * with the flex layout on their own. They must be recomputed on every reflow —
 * the widget becoming visible, or a growing live-temp label shifting the circles
 * — otherwise they freeze at their create-time geometry. A degenerate pass
 * (spanning extent <= 0) simply sizes the connector to 0; a later valid reflow
 * grows it, so a connector is never permanently dropped.
 *
 * Caller must have run a layout pass on @p container first (create does this;
 * the SIZE_CHANGED handler fires post-layout).
 */
static void relayout_connectors(step_progress_data_t* data, lv_obj_t* container) {
    if (!data || !container)
        return;

    const bool horizontal = data->horizontal;
    const int32_t circle_size = data->circle_size;
    const int32_t circle_radius = circle_size / 2;
    const int32_t connector_thickness = data->connector_width;

    for (int i = 0; i < data->step_count - 1; i++) {
        lv_obj_t* current_step = data->step_items[i];
        lv_obj_t* next_step = data->step_items[i + 1];
        if (!current_step || !next_step)
            continue;

        lv_obj_t* current_indicator = lv_obj_get_child(current_step, 0);
        lv_obj_t* next_indicator = lv_obj_get_child(next_step, 0);
        if (!current_indicator || !next_indicator)
            continue;

        lv_obj_t* current_circle = lv_obj_get_child(current_indicator, 0);
        lv_obj_t* next_circle = lv_obj_get_child(next_indicator, 0);
        if (!current_circle || !next_circle)
            continue;

        // Absolute circle positions within container (step + indicator + circle offsets)
        auto abs_x = [](lv_obj_t* step, lv_obj_t* ind, lv_obj_t* circ) {
            return lv_obj_get_x(step) + lv_obj_get_x(ind) + lv_obj_get_x(circ);
        };
        auto abs_y = [](lv_obj_t* step, lv_obj_t* ind, lv_obj_t* circ) {
            return lv_obj_get_y(step) + lv_obj_get_y(ind) + lv_obj_get_y(circ);
        };

        lv_coord_t conn_x, conn_y, conn_w, conn_h;

        if (horizontal) {
            // Horizontal: spans right edge of current circle to left edge of next
            lv_coord_t cur_cx = abs_x(current_step, current_indicator, current_circle);
            lv_coord_t next_cx = abs_x(next_step, next_indicator, next_circle);
            conn_x = cur_cx + circle_size;
            conn_w = next_cx - conn_x;
            conn_h = connector_thickness;
            conn_y = abs_y(current_step, current_indicator, current_circle) + circle_radius -
                     (connector_thickness / 2);
            if (conn_w < 0)
                conn_w = 0; // degenerate pass — sized on the next valid reflow
        } else {
            // Vertical: spans bottom edge of current circle to top edge of next
            lv_coord_t cur_cy = abs_y(current_step, current_indicator, current_circle);
            lv_coord_t next_cy = abs_y(next_step, next_indicator, next_circle);
            conn_y = cur_cy + circle_size;
            conn_h = next_cy - conn_y;
            conn_w = connector_thickness;
            conn_x = abs_x(current_step, current_indicator, current_circle) + circle_radius -
                     (connector_thickness / 2);
            if (conn_h < 0)
                conn_h = 0; // degenerate pass — sized on the next valid reflow
        }

        lv_obj_t* connector = data->connectors[i];
        if (!connector) {
            connector = lv_obj_create(container);
            lv_obj_add_flag(connector, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_remove_flag(connector, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(connector, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(connector, 0, 0);
            lv_obj_set_style_pad_all(connector, 0, 0);
            lv_obj_set_style_radius(connector, 0, 0);
            lv_color_t connector_color =
                (data->states[i] == StepState::Completed) ? color_completed : color_pending;
            lv_obj_set_style_bg_color(connector, connector_color, 0);
            // Render behind step items so circles draw on top
            lv_obj_move_to_index(connector, 0);
            data->connectors[i] = connector;
        }

        lv_obj_set_size(connector, conn_w, conn_h);
        lv_obj_set_pos(connector, conn_x, conn_y);

        spdlog::debug("[StepProgress] Connector {}: pos=({},{}), size={}x{}", i, conn_x, conn_y,
                      conn_w, conn_h);
    }
}

/**
 * Recompute connector geometry whenever the widget's size changes (becoming
 * visible, label growth, container resize). Absolutely-positioned connectors
 * don't track flex reflow on their own.
 */
static void step_progress_size_changed_cb(lv_event_t* e) {
    lv_obj_t* container = lv_event_get_target_obj(e);
    step_progress_data_t* data = (step_progress_data_t*)lv_obj_get_user_data(container);
    if (data) {
        relayout_connectors(data, container);
    }
}

lv_obj_t* ui_step_progress_create(lv_obj_t* parent, const ui_step_t* steps, int step_count,
                                  bool horizontal, const char* scope_name) {
    if (!parent || !steps || step_count <= 0) {
        spdlog::error("[Step Progress] Invalid parameters for step progress widget");
        return nullptr;
    }

    // Initialize theme-aware colors from component scope
    init_step_progress_colors(scope_name);

    // Read responsive sizing tokens (with fallbacks)
    int32_t circle_size = theme_manager_get_spacing("step_indicator");
    if (circle_size <= 0)
        circle_size = 20; // fallback
    int32_t connector_thickness = theme_manager_get_spacing("step_connector");
    if (connector_thickness <= 0)
        connector_thickness = 2; // fallback
    int32_t label_gap = theme_manager_get_spacing("step_label_gap");
    if (label_gap <= 0)
        label_gap = 8; // fallback
    int32_t row_gap = theme_manager_get_spacing("step_row_gap");
    if (row_gap <= 0)
        row_gap = 12; // fallback

    int32_t border_width = 2;

    spdlog::debug("[StepProgress] Responsive sizes: circle={}px, connector={}px, label_gap={}px, "
                  "row_gap={}px",
                  circle_size, connector_thickness, label_gap, row_gap);

    // Allocate widget data using RAII
    auto data_ptr = lvgl_make_unique<step_progress_data_t>();
    if (!data_ptr) {
        spdlog::error("[Step Progress] Failed to allocate step progress data");
        return nullptr;
    }

    step_progress_data_t* data = data_ptr.get();
    data->step_count = step_count;
    data->horizontal = horizontal;
    data->circle_size = circle_size;
    data->connector_width = connector_thickness;

    // Allocate arrays using RAII
    auto label_buffers_ptr = lvgl_make_unique_array<char*>(static_cast<size_t>(step_count));
    auto states_ptr = lvgl_make_unique_array<StepState>(static_cast<size_t>(step_count));
    auto connectors_ptr =
        lvgl_make_unique_array<lv_obj_t*>(static_cast<size_t>(step_count > 1 ? step_count - 1 : 0));
    auto step_items_ptr = lvgl_make_unique_array<lv_obj_t*>(static_cast<size_t>(step_count));

    if (!label_buffers_ptr || !states_ptr || !connectors_ptr || !step_items_ptr) {
        spdlog::error("[Step Progress] Failed to allocate step data arrays");
        return nullptr;
    }

    data->label_buffers = label_buffers_ptr.get();
    data->states = states_ptr.get();
    data->connectors = connectors_ptr.get();
    data->step_items = step_items_ptr.get();

    // Copy initial data
    for (int i = 0; i < step_count; i++) {
        auto label_buf = lvgl_make_unique_array<char>(128);
        if (label_buf) {
            snprintf(label_buf.get(), 128, "%s", steps[i].label);
            data->label_buffers[i] = label_buf.release();
        } else {
            data->label_buffers[i] = nullptr;
        }
        data->states[i] = steps[i].state;
    }

    // Release ownership of nested arrays (now owned by data struct)
    label_buffers_ptr.release();
    states_ptr.release();
    connectors_ptr.release();
    step_items_ptr.release();

    // Create container widget
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(container, LV_OPA_0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_flex_flow(container, horizontal ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    if (horizontal) {
        lv_obj_set_style_pad_column(container, 0, 0); // No gap - connectors fill space
    } else {
        lv_obj_set_style_pad_row(container, row_gap, 0);
    }

    // Transfer ownership to LVGL widget
    lv_obj_set_user_data(container, data_ptr.release());

    // Register cleanup callback
    lv_obj_add_event_cb(container, step_progress_delete_cb, LV_EVENT_DELETE, nullptr);

    // Create step items programmatically
    for (int i = 0; i < step_count; i++) {
        // Create step item container
        lv_obj_t* step_item = lv_obj_create(container);
        if (horizontal) {
            lv_obj_set_width(step_item, LV_SIZE_CONTENT); // Auto-size to content
            lv_obj_set_flex_grow(step_item, 1);           // Distribute space evenly across steps
        } else {
            lv_obj_set_width(step_item, LV_PCT(100)); // Vertical: responsive to parent
        }
        lv_obj_set_height(step_item, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(step_item, LV_OPA_0, 0);
        lv_obj_set_style_border_width(step_item, 0, 0);
        lv_obj_set_style_pad_all(step_item, 0, 0);
        lv_obj_set_style_pad_gap(step_item, 0, 0);
        lv_obj_set_scrollbar_mode(step_item, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(step_item, LV_OBJ_FLAG_SCROLLABLE);
        // Horizontal: column layout (circle + label stacked), Vertical: row layout
        lv_obj_set_flex_flow(step_item, horizontal ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(step_item, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, // Center cross-axis for both orientations
                              LV_FLEX_ALIGN_START);

        // Create indicator column (just wraps the circle)
        lv_obj_t* indicator_column = lv_obj_create(step_item);
        lv_obj_set_size(indicator_column, circle_size, horizontal ? LV_SIZE_CONTENT : circle_size);
        lv_obj_set_style_bg_opa(indicator_column, LV_OPA_0, 0);
        lv_obj_set_style_border_width(indicator_column, 0, 0);
        lv_obj_set_style_pad_all(indicator_column, 0, 0);
        lv_obj_set_style_pad_gap(indicator_column, 0, 0);
        lv_obj_set_scrollbar_mode(indicator_column, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(indicator_column, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(indicator_column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(indicator_column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);

        // Create circle indicator
        lv_obj_t* circle = lv_obj_create(indicator_column);
        lv_obj_set_size(circle, circle_size, circle_size);
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(circle, border_width, 0);
        lv_obj_set_style_pad_all(circle, 0, 0);
        lv_obj_set_style_margin_all(circle, 0, 0);
        lv_obj_set_scrollbar_mode(circle, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

        // Create step number label (shown for PENDING/ACTIVE states)
        lv_obj_t* step_number = lv_label_create(circle);
        char num_buf[16];
        snprintf(num_buf, sizeof(num_buf), "%d", i + 1);
        lv_label_set_text(step_number, num_buf);
        lv_obj_align(step_number, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_font(step_number, theme_manager_get_font("font_body"), 0);
        lv_obj_set_style_text_color(step_number, color_number_active, 0);

        // Create checkmark label (shown for COMPLETED state)
        lv_obj_t* checkmark = lv_label_create(circle);
        lv_label_set_text(checkmark, "\xF3\xB0\x84\xAC"); // MDI check icon (F012C)
        lv_obj_align(checkmark, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_font(checkmark, &mdi_icons_16, 0);
        lv_obj_set_style_text_color(checkmark, color_checkmark, 0);
        lv_obj_add_flag(checkmark, LV_OBJ_FLAG_HIDDEN);

        // Create step label
        lv_obj_t* label = lv_label_create(step_item);
        lv_label_set_text(label, data->label_buffers[i]);
        lv_obj_set_style_text_color(label, color_label_inactive, 0);
        if (horizontal) {
            lv_obj_set_width(label, LV_SIZE_CONTENT);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_pad_top(label, label_gap, 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_max_width(label, 120, 0);
        } else {
            lv_obj_set_style_pad_left(label, label_gap, 0);
            lv_obj_set_style_pad_top(label, 0, 0);
            lv_obj_set_flex_grow(label, 1);
        }

        // Apply initial styling based on state
        apply_step_styling(step_item, steps[i].state);

        // Store step item pointer
        data->step_items[i] = step_item;
    }

    // Create connector lines AFTER layout is calculated. Connectors use absolute
    // positioning (IGNORE_LAYOUT) spanning circle-edge to circle-edge; they do
    // not follow the flex reflow, so relayout_connectors() is re-run on every
    // size change (see the SIZE_CHANGED handler below).
    lv_obj_update_layout(container);

    spdlog::debug("[StepProgress] Creating {} connectors for {} steps",
                  horizontal ? "horizontal" : "vertical", step_count);

    relayout_connectors(data, container);

    // Recompute connector geometry when the widget reflows (becoming visible
    // after being created hidden, live-temp label growth, container resize).
    lv_obj_add_event_cb(container, step_progress_size_changed_cb, LV_EVENT_SIZE_CHANGED, nullptr);

    return container;
}

void ui_step_progress_set_current(lv_obj_t* widget, int step_index) {
    if (!widget)
        return;

    step_progress_data_t* data = (step_progress_data_t*)lv_obj_get_user_data(widget);
    if (!data || step_index < 0 || step_index >= data->step_count) {
        spdlog::warn("[Step Progress] Invalid step index: {}", step_index);
        return;
    }

    // Mark all previous steps as completed
    for (int i = 0; i < step_index; i++) {
        data->states[i] = StepState::Completed;
    }

    // Mark current step as active
    data->states[step_index] = StepState::Active;

    // Mark remaining steps as pending
    for (int i = step_index + 1; i < data->step_count; i++) {
        data->states[i] = StepState::Pending;
    }

    // Update step item styling
    for (int i = 0; i < data->step_count; i++) {
        apply_step_styling(data->step_items[i], data->states[i]);
    }

    // Update connector colors — connector i connects FROM step i, colored by step i's state
    for (int i = 0; i < data->step_count - 1; i++) {
        if (data->connectors[i]) {
            lv_color_t connector_color =
                (data->states[i] == StepState::Completed) ? color_completed : color_pending;
            lv_obj_set_style_bg_color(data->connectors[i], connector_color, 0);
        }
    }

    // Scroll the active step into view if the widget lives in a scrollable
    // container (vertical step bars can exceed the AMS sidebar column on small
    // screens). Use the stored step item directly — connectors are moved to
    // child index 0, so lv_obj_get_child(container, step_index) is unreliable.
    if (data->step_items[step_index]) {
        lv_obj_scroll_to_view(data->step_items[step_index], LV_ANIM_ON);
    }
}

void ui_step_progress_set_completed(lv_obj_t* widget, int step_index) {
    if (!widget)
        return;

    step_progress_data_t* data = (step_progress_data_t*)lv_obj_get_user_data(widget);
    if (!data || step_index < 0 || step_index >= data->step_count) {
        spdlog::warn("[Step Progress] Invalid step index: {}", step_index);
        return;
    }

    data->states[step_index] = StepState::Completed;
    apply_step_styling(data->step_items[step_index], StepState::Completed);

    // Update connector from this step (if it exists)
    if (step_index < data->step_count - 1 && data->connectors[step_index]) {
        lv_obj_set_style_bg_color(data->connectors[step_index], color_completed, 0);
    }
}

void ui_step_progress_set_label(lv_obj_t* widget, int step_index, const char* new_label) {
    if (!widget || !new_label)
        return;

    step_progress_data_t* data = (step_progress_data_t*)lv_obj_get_user_data(widget);
    if (!data || step_index < 0 || step_index >= data->step_count) {
        spdlog::warn("[Step Progress] Invalid step index: {}", step_index);
        return;
    }

    // Update buffer
    snprintf(data->label_buffers[step_index], 128, "%s", new_label);

    // Update label widget (second child of step_item: indicator_column=0, label=1)
    lv_obj_t* label = lv_obj_get_child(data->step_items[step_index], 1);
    if (label) {
        lv_label_set_text(label, data->label_buffers[step_index]);
    }
}

lv_obj_t* ui_step_progress_test_get_connector(lv_obj_t* widget, int index) {
    if (!widget)
        return nullptr;
    step_progress_data_t* data = (step_progress_data_t*)lv_obj_get_user_data(widget);
    if (!data || index < 0 || index >= data->step_count - 1)
        return nullptr;
    return data->connectors[index];
}
