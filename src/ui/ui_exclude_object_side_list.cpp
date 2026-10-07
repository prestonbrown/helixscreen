// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_side_list.h"

#include "ui_gcode_viewer.h"
#include "ui_open_instances.h"
#include "ui_row_text.h"
#include "ui_utils.h"

#include "color_utils.h"
#include "observer_factory.h"
#include "printer_excluded_objects_state.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace helix::ui {

namespace {
constexpr uint32_t SLIDE_IN_DURATION_MS = 220;

// Print status keeps its overlay alive while the details view opens its own.
OpenInstances<ExcludeObjectSideList>& open_lists() {
    static OpenInstances<ExcludeObjectSideList> lists;
    return lists;
}
} // namespace

ExcludeObjectSideList::ExcludeObjectSideList() = default;

ExcludeObjectSideList::~ExcludeObjectSideList() {
    open_lists().remove(this);
    if (root_) {
        lv_obj_delete_async(root_);
        root_ = nullptr;
    }
}

void ExcludeObjectSideList::create(lv_obj_t* parent, PrinterExcludedObjectsState* state,
                                   ObjectTapFn on_object_tapped, ExcludeTapMode tap_mode,
                                   SideListGeometry geom) {
    if (root_) {
        spdlog::warn("[ExcludeObjectSideList] create() called but already active");
        return;
    }
    if (!parent || !state) {
        spdlog::error("[ExcludeObjectSideList] create() missing required pointers");
        return;
    }

    state_ = state;
    on_object_tapped_ = std::move(on_object_tapped);
    tap_mode_ = tap_mode;

    // Register the close-button XML callback once. Idempotent on repeat calls.
    static bool s_callbacks_registered = false;
    if (!s_callbacks_registered) {
        lv_xml_register_event_cb(nullptr, "on_exclude_side_list_close", on_close_clicked);
        s_callbacks_registered = true;
    }

    open_lists().add(this);

    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "exclude_object_side_list", nullptr));
    if (!root_) {
        spdlog::error("[ExcludeObjectSideList] lv_xml_create failed");
        open_lists().remove(this);
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

    rows_container_ = helix::ui::find_required(root_, "rows_container", "ExcludeObjectSideList");
    empty_state_ = helix::ui::find_required(root_, "empty_state", "ExcludeObjectSideList");

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

    rebuild_rows();

    excluded_version_obs_ = observe<int>(
        state_->get_excluded_objects_version_subject(), this,
        [](ExcludeObjectSideList* self, int) {
            if (self->root_) {
                self->update_row_states();
            }
        },
        state_->get_subjects_lifetime());
    defined_version_obs_ = observe<int>(
        state_->get_defined_objects_version_subject(), this,
        [](ExcludeObjectSideList* self, int) {
            if (self->root_) {
                self->rebuild_rows();
            }
        },
        state_->get_subjects_lifetime());
    const auto restyle = [](ExcludeObjectSideList* self, int) {
        if (self->root_) {
            self->restyle_rows_if_stale();
        }
    };
    theme_obs_ =
        observe<int>(theme_manager_get_changed_subject(), this, restyle, subject_never_freed());
    breakpoint_obs_ =
        observe<int>(theme_manager_get_breakpoint_subject(), this, restyle, subject_never_freed());

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
    // Drop observers first — they capture `this` and the caller is about to
    // free us. Row click handlers also capture `this`; we delete the widget
    // tree asynchronously below, but the rows are children and will be torn
    // down with their parent before any further input can dispatch.
    excluded_version_obs_.reset();
    defined_version_obs_.reset();
    theme_obs_.reset();
    breakpoint_obs_.reset();
    lifetime_.invalidate();

    // Cancel the slide-in animation (no slide-out — the lv_obj_delete_async
    // handles teardown immediately; animating with stale handlers risks UAF
    // on row taps during the out-anim window).
    if (root_) {
        lv_anim_delete(root_, nullptr);
        lv_obj_delete_async(root_);
    }
    // Deinit detaches the rows still bound to these subjects, so the widgets
    // may outlive them until the async delete runs.
    row_states_.reclaim();
    row_names_.clear();

    root_ = nullptr;
    rows_container_ = nullptr;
    empty_state_ = nullptr;
    on_object_tapped_ = nullptr;

    open_lists().remove(this);
}

void ExcludeObjectSideList::on_close_clicked(lv_event_t* e) {
    ExcludeObjectSideList* list = open_lists().owner_of(lv_event_get_current_target_obj(e));
    if (!list) {
        return;
    }
    spdlog::debug("[ExcludeObjectSideList] Close button clicked");
    // A copy: the callback usually destroys this list, and its own
    // std::function with it.
    auto close = list->close_cb_;
    if (close) {
        close();
    }
}

void ExcludeObjectSideList::rebuild_rows() {
    if (!rows_container_ || !state_) {
        return;
    }

    const auto& defined = state_->get_defined_objects();

    if (empty_state_) {
        if (defined.empty()) {
            lv_obj_remove_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (defined == row_names_) {
        update_row_states();
        return;
    }

    // Observers above are observe<int> (deferred via UpdateQueue), so child
    // teardown must go through the async-clean helper to stay outside the batch
    // (CLAUDE.md § "No sync widget deletion in queued callbacks").
    helix::ui::safe_clean_children(rows_container_);

    row_names_ = defined;
    row_states_.ensure_size(row_names_.size());
    // States first, so each row binds to its real state on creation.
    update_row_states();

    rows_look_ = resolve_badge_look({});
    for (const auto& badge : compute_object_badges(*state_, nullptr)) {
        create_row(rows_container_, badge);
    }
}

void ExcludeObjectSideList::restyle_rows_if_stale() {
    if (!rows_container_ || badge_look_current(rows_look_)) {
        return;
    }
    // The container keeps its own scroll offset across the swap of children.
    row_names_.clear(); // forces rebuild_rows() past its unchanged-list shortcut
    rebuild_rows();
}

void ExcludeObjectSideList::update_row_states() {
    if (!state_) {
        return;
    }
    // Rows are matched to objects by position, which only holds while they
    // show the current list. A rebuild for the new list is already queued and
    // publishes the states itself.
    if (state_->get_defined_objects() != row_names_) {
        return;
    }
    const auto badges = compute_object_badges(*state_, nullptr);
    for (size_t i = 0; i < badges.size(); ++i) {
        const int state = badges[i].excluded ? (tap_mode_ == ExcludeTapMode::Toggle ? 3 : 2)
                                             : (badges[i].current ? 1 : 0);
        if (lv_subject_get_int(row_states_.at(i)) != state) {
            row_states_.set_int(i, state);
        }
    }
}

void ExcludeObjectSideList::create_row(lv_obj_t* parent, const ObjectBadge& badge) {
    const std::string& name = badge.name;
    const lv_color_t fill = object_badge_color(badge.defined_index);
    const std::string badge_color = helix::color_to_hex_string(lv_color_to_u32(fill));
    const std::string badge_text_color =
        helix::color_to_hex_string(lv_color_to_u32(object_badge_text_color(fill)));
    const std::string state_subject = "exclude_row_state_" + std::to_string(badge.defined_index);

    const char* attrs[] = {
        "badge_text",        badge.number.c_str(),  "badge_color",
        badge_color.c_str(), "badge_text_color",    badge_text_color.c_str(),
        "state_subject",     state_subject.c_str(), nullptr,
    };
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(parent, "exclude_object_row", attrs));
    if (!row) {
        return;
    }
    helix::ui::set_row_label_text(row, "object_name", name.c_str());

    // L069: the row is a plain lv_obj, so the user_data slot is ours. The
    // helper owns the copy and frees it on LV_EVENT_DELETE. An excluded row
    // drops its clickable flag through state_subject, so it never sees a click.
    if (helix::ui::set_owned_user_string(row, name)) {
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, this);
    }
}

void ExcludeObjectSideList::on_row_clicked(lv_event_t* e) {
    auto* self = static_cast<ExcludeObjectSideList*>(lv_event_get_user_data(e));
    lv_obj_t* target = lv_event_get_target_obj(e);
    if (!self || !self->on_object_tapped_ || !target) {
        return;
    }
    const char* name = helix::ui::get_owned_user_string(target);
    if (!name) {
        return;
    }
    spdlog::info("[ExcludeObjectSideList] Row clicked: '{}'", name);

    // Highlight the matching object in the gcode viewer so the user gets
    // spatial feedback before the confirmation modal appears. A toggle tap
    // has no modal to follow, so it leaves the viewer alone.
    if (self->gcode_viewer_ && self->tap_mode_ == ExcludeTapMode::ExcludeOnly) {
        std::unordered_set<std::string> highlight = {std::string(name)};
        ui_gcode_viewer_set_highlighted_objects(self->gcode_viewer_, highlight);
    }

    self->on_object_tapped_(std::string(name));
}

} // namespace helix::ui
