// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_step_progress.h"

#include "ui_row_text.h"

#include "helix/xml/indexed_subject_pool.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <vector>

using namespace helix;

namespace {

// Internal widget data stored in user_data. Each bar names its own subjects
// "<prefix>_<i>_state" (0 pending, 1 active, 2 done) and "<prefix>_<i>_label",
// which its step components and connectors bind; a bar's prefix is unique for
// the life of the process, so two bars on screen never share a subject.
struct StepProgressData {
    std::string prefix; // outlives the pools, which hold a pointer to it
    std::unique_ptr<helix::xml::IndexedSubjectPool> states;
    std::unique_ptr<helix::xml::IndexedSubjectPool> labels;
    std::vector<lv_obj_t*> connectors; // step_count - 1, created on the first relayout
    std::vector<lv_obj_t*> step_items;
    int step_count = 0;
    bool horizontal = false;
    int32_t circle_size = 0;     // Responsive circle diameter
    int32_t connector_width = 0; // Responsive connector line thickness
    lv_obj_t* caption = nullptr; // Current-label-only mode: the one label, else nullptr
};

constexpr size_t kLabelCap = 128;

StepProgressData* data_of(lv_obj_t* widget) {
    return widget ? static_cast<StepProgressData*>(lv_obj_get_user_data(widget)) : nullptr;
}

StepState state_at(StepProgressData* data, int i) {
    return static_cast<StepState>(lv_subject_get_int(data->states->at(static_cast<size_t>(i))));
}

void set_state_at(StepProgressData* data, int i, StepState state) {
    if (state_at(data, i) == state)
        return;
    data->states->set_int(static_cast<size_t>(i), static_cast<int>(state));
    // LVGL skips the style refresh of a state change on a widget not yet drawn,
    // and the active label's larger font needs a taller row.
    if (lv_obj_t* item = data->step_items[static_cast<size_t>(i)]) {
        lv_obj_refresh_style(item, LV_PART_ANY, LV_STYLE_PROP_ANY);
    }
}

const char* label_at(StepProgressData* data, int i) {
    return lv_subject_get_string(data->labels->at(static_cast<size_t>(i)));
}

std::string subject_name(const StepProgressData* data, int i, const char* suffix) {
    return data->prefix + "_" + std::to_string(i) + suffix;
}

} // namespace

// The glyphs on the dots start from the palette text color and shift toward
// their pole as 4:1 against each fill needs. A derived color, so it is computed
// here and handed to the step component as a prop.
static std::string contrast_text_on(const char* fill_token) {
    lv_color_t c = theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"),
                                                            theme_manager_get_color(fill_token));
    return fmt::format("0x{:06X}", lv_color_to_u32(c) & 0xFFFFFF);
}

/**
 * Cleanup callback for widget deletion. Resetting the subjects unbinds the step
 * components and connectors before LVGL deletes them.
 */
static void step_progress_delete_cb(lv_event_t* e) {
    lv_obj_t* widget = lv_event_get_target_obj(e);
    std::unique_ptr<StepProgressData> data(data_of(widget));
    lv_obj_set_user_data(widget, nullptr);
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
static void relayout_connectors(StepProgressData* data, lv_obj_t* container) {
    if (!data || !container)
        return;

    const bool horizontal = data->horizontal;
    const int32_t circle_size = data->circle_size;
    const int32_t circle_radius = circle_size / 2;
    const int32_t connector_thickness = data->connector_width;

    for (int i = 0; i < data->step_count - 1; i++) {
        lv_obj_t* current_step = data->step_items[static_cast<size_t>(i)];
        lv_obj_t* next_step = data->step_items[static_cast<size_t>(i + 1)];
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

        lv_obj_t* connector = data->connectors[static_cast<size_t>(i)];
        if (!connector) {
            // Connector i leaves step i and binds its state.
            const std::string state = subject_name(data, i, "_state");
            const char* attrs[] = {"state_subject", state.c_str(), nullptr};
            connector =
                static_cast<lv_obj_t*>(lv_xml_create(container, "step_progress_connector", attrs));
            if (!connector)
                continue;
            // Render behind step items so circles draw on top
            lv_obj_move_to_index(connector, 0);
            data->connectors[static_cast<size_t>(i)] = connector;
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
    if (StepProgressData* data = data_of(container)) {
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

    // Holds std:: members, so it is new'd: lvgl_unique_ptr frees without destroying.
    auto data_ptr = std::make_unique<StepProgressData>();
    StepProgressData* data = data_ptr.get();
    static unsigned next_bar = 0;
    data->prefix = "step_progress_" + std::to_string(next_bar++);
    data->step_count = step_count;
    data->horizontal = horizontal;
    data->circle_size = circle_size;
    data->connector_width = connector_thickness;
    data->connectors.assign(static_cast<size_t>(step_count - 1), nullptr);
    data->step_items.assign(static_cast<size_t>(step_count), nullptr);
    using helix::xml::IndexedSubjectPool;
    data->states = std::make_unique<IndexedSubjectPool>(data->prefix.c_str(),
                                                        IndexedSubjectPool::Type::Int, 0, "_state");
    data->labels = std::make_unique<IndexedSubjectPool>(
        data->prefix.c_str(), IndexedSubjectPool::Type::String, kLabelCap, "_label");
    data->states->ensure_size(static_cast<size_t>(step_count));
    data->labels->ensure_size(static_cast<size_t>(step_count));
    for (int i = 0; i < step_count; i++) {
        set_state_at(data, i, steps[i].state);
        // Step labels can be printer text: they travel in a subject, never as a prop.
        data->labels->set_string(static_cast<size_t>(i), steps[i].label ? steps[i].label : "");
    }

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
    const char* step_component = horizontal ? "step_progress_step_h" : "step_progress_step_v";

    for (int i = 0; i < step_count; i++) {
        const std::string number = std::to_string(i + 1);
        const std::string state = subject_name(data, i, "_state");
        const std::string label = subject_name(data, i, "_label");
        const char* attrs[] = {"number",
                               number.c_str(),
                               "state_subject",
                               state.c_str(),
                               "label_subject",
                               label.c_str(),
                               "number_pending_color",
                               number_pending_color.c_str(),
                               "number_active_color",
                               number_active_color.c_str(),
                               nullptr};
        data->step_items[static_cast<size_t>(i)] =
            static_cast<lv_obj_t*>(lv_xml_create(container, step_component, attrs));
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

static void refresh_caption(StepProgressData* data) {
    if (!data || !data->caption)
        return;
    std::vector<StepState> states;
    for (int i = 0; i < data->step_count; i++)
        states.push_back(state_at(data, i));
    const int idx = ui_step_progress_caption_step(states.data(), data->step_count);
    helix::ui::set_row_label_text(lv_obj_get_parent(data->caption), "step_caption",
                                  idx >= 0 ? label_at(data, idx) : "");
}

// NAMESPACE_OK: the widget's C API, beside its siblings
void ui_step_progress_set_current_label_only(lv_obj_t* widget, bool on) {
    StepProgressData* data = data_of(widget);
    if (!data)
        return;
    for (lv_obj_t* item : data->step_items) {
        lv_obj_t* label = item ? lv_obj_find_by_name(item, "step_label") : nullptr;
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
    StepProgressData* data = data_of(widget);
    return data ? data->caption : nullptr;
}

static StepProgressData* data_for_step(lv_obj_t* widget, int step_index) {
    StepProgressData* data = data_of(widget);
    if (!data || step_index < 0 || step_index >= data->step_count) {
        spdlog::warn("[Step Progress] Invalid step index: {}", step_index);
        return nullptr;
    }
    return data;
}

void ui_step_progress_set_current(lv_obj_t* widget, int step_index) {
    StepProgressData* data = data_for_step(widget, step_index);
    if (!data)
        return;

    // Earlier steps are done, later ones pending again.
    for (int i = 0; i < data->step_count; i++) {
        set_state_at(data, i,
                     i < step_index    ? StepState::Completed
                     : i == step_index ? StepState::Active
                                       : StepState::Pending);
    }
    refresh_caption(data);

    // Scroll the active step into view if the widget lives in a scrollable
    // container (vertical step bars can exceed the AMS sidebar column on small
    // screens). The scroller is an ancestor, not the step's own parent, so the
    // scroll walks up to it. Use the stored step item directly: connectors are
    // moved to child index 0, so lv_obj_get_child(container, step_index) is
    // unreliable.
    if (lv_obj_t* item = data->step_items[static_cast<size_t>(step_index)]) {
        lv_obj_scroll_to_view_recursive(item, LV_ANIM_ON);
    }
}

void ui_step_progress_set_completed(lv_obj_t* widget, int step_index) {
    StepProgressData* data = data_for_step(widget, step_index);
    if (!data)
        return;
    set_state_at(data, step_index, StepState::Completed);
    refresh_caption(data);
}

void ui_step_progress_set_label(lv_obj_t* widget, int step_index, const char* new_label) {
    StepProgressData* data = new_label ? data_for_step(widget, step_index) : nullptr;
    if (!data)
        return;
    data->labels->set_string(static_cast<size_t>(step_index), new_label);
    refresh_caption(data);
}

lv_obj_t* ui_step_progress_test_get_connector(lv_obj_t* widget, int index) {
    StepProgressData* data = data_of(widget);
    if (!data || index < 0 || index >= data->step_count - 1)
        return nullptr;
    return data->connectors[static_cast<size_t>(index)];
}
