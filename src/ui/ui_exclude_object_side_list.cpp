// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_side_list.h"

#include "ui_gcode_viewer.h"
#include "ui_print_exclude_object_manager.h"
#include "ui_utils.h"

#include "observer_factory.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace helix::ui {

namespace {
constexpr uint32_t SLIDE_IN_DURATION_MS = 220;

// Singleton handle so the static XML close callback can find the live instance.
// Only one side list exists at a time (owned by PrintStatusPanel).
ExcludeObjectSideList* g_active_side_list = nullptr;
} // namespace

ExcludeObjectSideList::ExcludeObjectSideList() = default;

ExcludeObjectSideList::~ExcludeObjectSideList() {
    if (g_active_side_list == this) {
        g_active_side_list = nullptr;
    }
    if (root_) {
        lv_obj_delete_async(root_);
        root_ = nullptr;
    }
}

void ExcludeObjectSideList::create(lv_obj_t* parent, PrinterState* printer_state,
                                   PrintExcludeObjectManager* manager, SideListGeometry geom) {
    if (root_) {
        spdlog::warn("[ExcludeObjectSideList] create() called but already active");
        return;
    }
    if (!parent || !printer_state || !manager) {
        spdlog::error("[ExcludeObjectSideList] create() missing required pointers");
        return;
    }

    printer_state_ = printer_state;
    manager_ = manager;

    // Register the close-button XML callback once. Idempotent on repeat calls.
    static bool s_callbacks_registered = false;
    if (!s_callbacks_registered) {
        lv_xml_register_event_cb(nullptr, "on_exclude_side_list_close", on_close_clicked);
        s_callbacks_registered = true;
    }

    g_active_side_list = this;

    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "exclude_object_side_list", nullptr));
    if (!root_) {
        spdlog::error("[ExcludeObjectSideList] lv_xml_create failed");
        g_active_side_list = nullptr;
        return;
    }

    lv_obj_set_width(root_, lv_pct(geom.width_pct));
    // Bottom-anchored means portrait, which is supposed to arrive measured. The
    // percentage fallback is a guess that cannot track the control stack — the
    // exact failure this sizing replaced — so say so rather than silently
    // shipping a list that eats the object map.
    if (geom.anchor_bottom && geom.height_px <= 0) {
        spdlog::warn("[ExcludeObjectSideList] Portrait list created without measurements; "
                     "falling back to {}% of the column",
                     geom.height_pct);
    }
    // A measured px height wins over the percentage. Portrait sizes the list to
    // the control stack it covers; a percentage there cannot notice the stack
    // shrinking and would keep eating the object map.
    if (geom.height_px > 0) {
        lv_obj_set_height(root_, geom.height_px);
    } else {
        lv_obj_set_height(root_, lv_pct(geom.height_pct));
    }
    lv_obj_set_align(root_, geom.anchor_bottom ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_RIGHT_MID);
    // FLOATING removes us from the parent's flex/layout calculations so we
    // sit on top of sibling columns rather than displacing them.
    lv_obj_add_flag(root_, LV_OBJ_FLAG_FLOATING);

    rows_container_ = lv_obj_find_by_name(root_, "rows_container");
    empty_state_ = lv_obj_find_by_name(root_, "empty_state");

    if (!rows_container_) {
        spdlog::error("[ExcludeObjectSideList] rows_container not found");
    }

    // Force layout so we know the pixel extent to travel for the slide.
    lv_obj_update_layout(parent);
    int slide_distance = geom.anchor_bottom ? lv_obj_get_height(root_) : lv_obj_get_width(root_);
    if (slide_distance <= 0) {
        slide_distance = 200; // fallback for unsized parent
    }

    // Start off-screen past the anchored edge (positive offset relative to
    // RIGHT_MID or BOTTOM_MID), then tween to 0 to sit flush against it.
    if (geom.anchor_bottom) {
        lv_obj_set_y(root_, slide_distance);
    } else {
        lv_obj_set_x(root_, slide_distance);
    }

    populate_rows();

    auto repopulate = [](ExcludeObjectSideList* self, int) {
        if (self->root_) {
            self->populate_rows();
        }
    };
    excluded_version_obs_ = observe_int_sync<ExcludeObjectSideList>(
        printer_state_->get_excluded_objects_version_subject(), this, repopulate);
    defined_version_obs_ = observe_int_sync<ExcludeObjectSideList>(
        printer_state_->get_defined_objects_version_subject(), this, repopulate);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root_);
    lv_anim_set_values(&a, slide_distance, 0);
    lv_anim_set_duration(&a, SLIDE_IN_DURATION_MS);
    lv_anim_set_exec_cb(
        &a, geom.anchor_bottom
                ? [](void* obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(obj), v); }
                : [](void* obj, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t*>(obj), v); });
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    // Report the height that was actually applied, not both candidates — a line
    // that always prints the percentage reads as "the percentage was used".
    if (geom.height_px > 0) {
        spdlog::debug("[ExcludeObjectSideList] Created ({}%x{}px measured, anchor={}, "
                      "slide_distance={}px)",
                      geom.width_pct, geom.height_px, geom.anchor_bottom ? "bottom" : "right",
                      slide_distance);
    } else {
        spdlog::debug("[ExcludeObjectSideList] Created ({}x{}%, anchor={}, slide_distance={}px)",
                      geom.width_pct, geom.height_pct, geom.anchor_bottom ? "bottom" : "right",
                      slide_distance);
    }
}

void ExcludeObjectSideList::destroy() {
    if (!root_) {
        return;
    }

    // Drop observers first — they capture `this` and the caller is about to
    // free us. Row click handlers also capture `this`; we delete the widget
    // tree asynchronously below, but the rows are children and will be torn
    // down with their parent before any further input can dispatch.
    excluded_version_obs_.reset();
    defined_version_obs_.reset();
    lifetime_.invalidate();

    // Cancel the slide-in animation (no slide-out — the lv_obj_delete_async
    // handles teardown immediately; animating with stale handlers risks UAF
    // on row taps during the out-anim window).
    lv_anim_delete(root_, nullptr);
    lv_obj_delete_async(root_);

    root_ = nullptr;
    rows_container_ = nullptr;
    empty_state_ = nullptr;

    if (g_active_side_list == this) {
        g_active_side_list = nullptr;
    }
}

void ExcludeObjectSideList::on_close_clicked(lv_event_t* /*e*/) {
    spdlog::debug("[ExcludeObjectSideList] Close button clicked");
    if (g_active_side_list && g_active_side_list->close_cb_) {
        g_active_side_list->close_cb_();
    }
}

void ExcludeObjectSideList::populate_rows() {
    if (!rows_container_ || !printer_state_) {
        return;
    }

    // Observers above are observe_int_sync (deferred via UpdateQueue), so child
    // teardown must go through the async-clean helper to stay outside the batch
    // (CLAUDE.md § "No sync widget deletion in queued callbacks").
    helix::ui::safe_clean_children(rows_container_);

    const auto& defined = printer_state_->get_defined_objects();
    const auto& excluded = printer_state_->get_excluded_objects();
    const auto& current = printer_state_->get_current_object();

    if (empty_state_) {
        if (defined.empty()) {
            lv_obj_remove_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int index = 0;
    for (const auto& name : defined) {
        bool is_excluded = excluded.count(name) > 0;
        bool is_current = (name == current);
        create_row(rows_container_, index, name, is_excluded, is_current);
        ++index;
    }
}

void ExcludeObjectSideList::create_row(lv_obj_t* parent, int index, const std::string& name,
                                       bool is_excluded, bool is_current) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_sm"), 0);
    lv_obj_set_style_pad_gap(row, theme_manager_get_spacing("space_sm"), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_bg_color(row, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Numbered badge matching the map's index/coloring.
    lv_obj_t* badge = lv_obj_create(row);
    lv_obj_set_size(badge, 24, 24);
    lv_obj_set_style_radius(badge, 12, 0);
    lv_obj_set_style_bg_color(badge, color_for_index(index), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* badge_label = lv_label_create(badge);
    char num_buf[8];
    snprintf(num_buf, sizeof(num_buf), "%d", index + 1);
    lv_label_set_text(badge_label, num_buf);
    lv_obj_set_style_text_color(badge_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(badge_label, theme_manager_get_font("font_small"), 0);
    lv_obj_align(badge_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(badge_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, name.c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* status = lv_label_create(row);
    lv_obj_set_style_text_font(status, theme_manager_get_font("font_small"), 0);
    lv_obj_add_flag(status, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (is_excluded) {
        lv_label_set_text(status, lv_tr("Excluded"));
        lv_obj_set_style_text_color(status, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_opa(row, 150, 0);
    } else if (is_current) {
        lv_label_set_text(status, lv_tr("Printing"));
        lv_obj_set_style_text_color(status, theme_manager_get_color("success"), 0);
    } else {
        lv_label_set_text(status, "");
    }

    if (!is_excluded && manager_) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // L069: row was created via lv_obj_create (not lv_xml_create) so the
        // user_data slot is ours to use. The helper owns the copy and frees it
        // on LV_EVENT_DELETE.
        if (helix::ui::set_owned_user_string(row, name)) {
            lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, this);
        }

        lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, 80, LV_STATE_PRESSED);
    }
}

void ExcludeObjectSideList::on_row_clicked(lv_event_t* e) {
    auto* self = static_cast<ExcludeObjectSideList*>(lv_event_get_user_data(e));
    lv_obj_t* target = lv_event_get_target_obj(e);
    if (!self || !self->manager_ || !target) {
        return;
    }
    const char* name = helix::ui::get_owned_user_string(target);
    if (!name) {
        return;
    }
    spdlog::info("[ExcludeObjectSideList] Row clicked: '{}'", name);

    // Highlight the matching object in the gcode viewer so the user gets
    // spatial feedback before the confirmation modal appears.
    if (self->gcode_viewer_) {
        std::unordered_set<std::string> highlight = {std::string(name)};
        ui_gcode_viewer_set_highlighted_objects(self->gcode_viewer_, highlight);
    }

    self->manager_->request_exclude(std::string(name));
}

lv_color_t ExcludeObjectSideList::color_for_index(int index) {
    return theme_manager_get_object_palette_color(index);
}

} // namespace helix::ui
