// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_step_progress.h"

#include "ui_row_text.h"
#include "ui_widget_memory.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <stdlib.h>
#include <string.h>
#include <string>

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
    lv_obj_t* caption;       // Current-label-only mode: the one label, else nullptr
} step_progress_data_t;

// The glyphs on the dots start from the palette text color and shift toward
// their pole as 4:1 against each fill needs. A derived color, so it is computed
// here and handed to the step component as a prop.
static std::string contrast_text_on(const char* fill_token) {
    lv_color_t c = theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"),
                                                            theme_manager_get_color(fill_token));
    return fmt::format("0x{:06X}", lv_color_to_u32(c) & 0xFFFFFF);
}

/**
 * Put a step item in the state its styles key on. The state trickles to the
 * dot and the label: LV_STATE_USER_1 while active, LV_STATE_CHECKED once done.
 */
static void apply_step_state(lv_obj_t* step_item, StepState state) {
    if (!step_item)
        return;
    lv_obj_set_state(step_item, LV_STATE_USER_1, state == StepState::Active);
    lv_obj_set_state(step_item, LV_STATE_CHECKED, state == StepState::Completed);
    // A widget not yet drawn takes a new state without re-measuring, and the
    // active label's larger font needs a taller row.
    lv_obj_refresh_style(step_item, LV_PART_ANY, LV_STYLE_PROP_ANY);
}

static void apply_connector_state(lv_obj_t* connector, StepState from_state) {
    if (connector)
        lv_obj_set_state(connector, LV_STATE_CHECKED, from_state == StepState::Completed);
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

        lv_obj_t* current_circle = lv_obj_find_by_name(current_step, "step_circle");
        lv_obj_t* next_circle = lv_obj_find_by_name(next_step, "step_circle");
        if (!current_circle || !next_circle)
            continue;
        lv_obj_t* current_indicator = lv_obj_get_parent(current_circle);
        lv_obj_t* next_indicator = lv_obj_get_parent(next_circle);

        // Absolute circle positions within container (step + indicator + circle offsets)
        auto abs_x = [](lv_obj_t* step, lv_obj_t* ind, lv_obj_t* circ) {
            return lv_obj_get_x(step) + lv_obj_get_x(ind) + lv_obj_get_x(circ);
        };
        auto abs_y = [](lv_obj_t* step, lv_obj_t* ind, lv_obj_t* circ) {
            return lv_obj_get_y(step) + lv_obj_get_y(ind) + lv_obj_get_y(circ);
        };

        int32_t conn_x, conn_y, conn_w, conn_h;

        if (horizontal) {
            // Horizontal: spans right edge of current circle to left edge of next
            int32_t cur_cx = abs_x(current_step, current_indicator, current_circle);
            int32_t next_cx = abs_x(next_step, next_indicator, next_circle);
            conn_x = cur_cx + circle_size;
            conn_w = next_cx - conn_x;
            conn_h = connector_thickness;
            conn_y = abs_y(current_step, current_indicator, current_circle) + circle_radius -
                     (connector_thickness / 2);
            if (conn_w < 0)
                conn_w = 0; // degenerate pass — sized on the next valid reflow
        } else {
            // Vertical: spans bottom edge of current circle to top edge of next
            int32_t cur_cy = abs_y(current_step, current_indicator, current_circle);
            int32_t next_cy = abs_y(next_step, next_indicator, next_circle);
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
            connector = static_cast<lv_obj_t*>(
                lv_xml_create(container, "step_progress_connector", nullptr));
            if (!connector)
                continue;
            apply_connector_state(connector, data->states[i]);
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
                                  bool horizontal) {
    if (!parent || !steps || step_count <= 0) {
        spdlog::error("[Step Progress] Invalid parameters for step progress widget");
        return nullptr;
    }

    // The connectors are placed from these; the components size from the same tokens.
    int32_t circle_size = theme_manager_get_spacing("step_indicator");
    if (circle_size <= 0)
        circle_size = 20; // fallback
    int32_t connector_thickness = theme_manager_get_spacing("step_connector");
    if (connector_thickness <= 0)
        connector_thickness = 2; // fallback

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
    data->caption = nullptr;

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

    const char* frame_attrs[] = {"flow", horizontal ? "row" : "column", "scrollable",
                                 horizontal ? "true" : "false", nullptr};
    lv_obj_t* container =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "step_progress", frame_attrs));
    if (!container) {
        spdlog::error("[Step Progress] Failed to create step_progress component");
        return nullptr;
    }

    // Transfer ownership to LVGL widget
    lv_obj_set_user_data(container, data_ptr.release());

    lv_obj_add_event_cb(container, step_progress_delete_cb, LV_EVENT_DELETE, nullptr);

    const std::string number_pending_color = contrast_text_on("text_subtle");
    const std::string number_active_color = contrast_text_on("primary");
    const std::string check_color = contrast_text_on("success");
    const char* step_component = horizontal ? "step_progress_step_h" : "step_progress_step_v";

    for (int i = 0; i < step_count; i++) {
        const std::string number = std::to_string(i + 1);
        const char* attrs[] = {"number",
                               number.c_str(),
                               "number_pending_color",
                               number_pending_color.c_str(),
                               "number_active_color",
                               number_active_color.c_str(),
                               "check_color",
                               check_color.c_str(),
                               nullptr};
        lv_obj_t* step_item =
            static_cast<lv_obj_t*>(lv_xml_create(container, step_component, attrs));
        data->step_items[i] = step_item;
        if (!step_item)
            continue;
        // Step labels can be printer text: set after creation, never as a prop.
        helix::ui::set_row_label_text(step_item, "step_label", data->label_buffers[i]);
        apply_step_state(step_item, steps[i].state);
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

// NAMESPACE_OK: the widget's C API, beside its siblings
int ui_step_progress_caption_step(const StepState* states, int step_count) {
    if (!states || step_count <= 0)
        return -1;
    int last_completed = -1;
    for (int i = 0; i < step_count; i++) {
        if (states[i] == StepState::Active)
            return i;
        if (states[i] == StepState::Completed)
            last_completed = i;
    }
    return last_completed >= 0 ? last_completed : 0;
}

static void refresh_caption(step_progress_data_t* data) {
    if (!data || !data->caption)
        return;
    const int idx = ui_step_progress_caption_step(data->states, data->step_count);
    helix::ui::set_row_label_text(lv_obj_get_parent(data->caption), "step_caption",
                                  idx >= 0 ? data->label_buffers[idx] : "");
}

// NAMESPACE_OK: the widget's C API, beside its siblings
void ui_step_progress_set_current_label_only(lv_obj_t* widget, bool on) {
    if (!widget)
        return;
    auto* data = (step_progress_data_t*)lv_obj_get_user_data(widget);
    if (!data)
        return;
    for (int i = 0; i < data->step_count; i++) {
        lv_obj_t* label = lv_obj_find_by_name(data->step_items[i], "step_label");
        if (!label)
            continue;
        // DECLARATIVE_OK: a mode switch on generated rows, with no subject per bar to bind
        lv_obj_set_hidden(label, on);
    }
    if (on && !data->caption) {
        lv_obj_t* caption =
            static_cast<lv_obj_t*>(lv_xml_create(widget, "step_progress_caption", nullptr));
        if (!caption)
            return;
        // The widget reserves one line of padding below for the caption.
        lv_label_set_max_lines(caption, 1);
        int32_t label_gap = theme_manager_get_spacing("step_label_gap");
        if (label_gap <= 0)
            label_gap = 8;
        // The floating caption takes no room in the row, so the row reserves it as
        // bottom padding, and the caption stands in that padding.
        const int32_t reserve =
            lv_font_get_line_height(lv_obj_get_style_text_font(caption, LV_PART_MAIN)) + label_gap;
        // DECLARATIVE_OK: measured from the caption font's line height
        lv_obj_set_style_pad_bottom(widget, reserve, 0);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, reserve);
        data->caption = caption;
    } else if (!on && data->caption) {
        lv_obj_delete(data->caption);
        data->caption = nullptr;
        // DECLARATIVE_OK: clears the measured caption reserve
        lv_obj_set_style_pad_bottom(widget, 0, 0);
    }
    refresh_caption(data);
    // Hiding or showing the labels moves the dots without resizing the row, so
    // no SIZE_CHANGED arrives to re-run the connectors.
    lv_obj_update_layout(widget);
    relayout_connectors(data, widget);
}

// NAMESPACE_OK: the widget's C API, beside its siblings
lv_obj_t* ui_step_progress_get_caption(lv_obj_t* widget) {
    auto* data = widget ? (step_progress_data_t*)lv_obj_get_user_data(widget) : nullptr;
    return data ? data->caption : nullptr;
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

    for (int i = 0; i < data->step_count; i++) {
        apply_step_state(data->step_items[i], data->states[i]);
    }
    refresh_caption(data);

    // Connector i leaves step i and takes its state.
    for (int i = 0; i < data->step_count - 1; i++) {
        apply_connector_state(data->connectors[i], data->states[i]);
    }

    // Scroll the active step into view if the widget lives in a scrollable
    // container (vertical step bars can exceed the AMS sidebar column on small
    // screens). The scroller is an ancestor, not the step's own parent, so the
    // scroll walks up to it. Use the stored step item directly — connectors are
    // moved to child index 0, so lv_obj_get_child(container, step_index) is
    // unreliable.
    if (data->step_items[step_index]) {
        lv_obj_scroll_to_view_recursive(data->step_items[step_index], LV_ANIM_ON);
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
    apply_step_state(data->step_items[step_index], StepState::Completed);

    if (step_index < data->step_count - 1) {
        apply_connector_state(data->connectors[step_index], StepState::Completed);
    }
    refresh_caption(data);
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

    helix::ui::set_row_label_text(data->step_items[step_index], "step_label",
                                  data->label_buffers[step_index]);
    refresh_caption(data);
}

lv_obj_t* ui_step_progress_test_get_connector(lv_obj_t* widget, int index) {
    if (!widget)
        return nullptr;
    step_progress_data_t* data = (step_progress_data_t*)lv_obj_get_user_data(widget);
    if (!data || index < 0 || index >= data->step_count - 1)
        return nullptr;
    return data->connectors[index];
}
