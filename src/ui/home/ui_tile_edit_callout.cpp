// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_tile_edit_callout.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"

#include "core/static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace helix::ui {

namespace {

constexpr const char* kTitleSubject = "tile_callout_title";
constexpr const char* kConfigureSubject = "tile_callout_configure";

std::array<char, 96> s_title_buf{};
lv_subject_t s_title_subject{};
lv_subject_t s_configure_subject{};
bool s_subjects_ready = false;

void ensure_subjects() {
    if (s_subjects_ready) {
        return;
    }
    lv_subject_init_string(&s_title_subject, s_title_buf.data(), nullptr, s_title_buf.size(), "");
    lv_xml_register_subject(nullptr, kTitleSubject, &s_title_subject);
    lv_subject_init_int(&s_configure_subject, 0);
    lv_xml_register_subject(nullptr, kConfigureSubject, &s_configure_subject);
    s_subjects_ready = true;
    StaticSubjectRegistry::instance().register_deinit(
        "TileEditCallout",
        []() {
            if (s_subjects_ready) {
                lv_xml_unregister_subject(nullptr, kTitleSubject);
                lv_xml_unregister_subject(nullptr, kConfigureSubject);
                lv_subject_deinit(&s_title_subject);
                lv_subject_deinit(&s_configure_subject);
                s_subjects_ready = false;
            }
        },
        StaticSubjectRegistry::Lifetime::Process);
}

/// Whether the press that produced @p e lifted over the object that handles it.
/// LVGL press-locks a row, so its CLICKED fires wherever the finger lifts; a
/// press that slid off the row is not a tap on it. A click with no input device
/// behind it (remote control, tests) counts.
bool released_over_target(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return true;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_current_target(e)), &area);
    return point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 && point.y <= area.y2;
}

} // namespace

TileCalloutRows tile_callout_rows(bool has_edit_configure) {
    TileCalloutRows rows;
    rows.configure = has_edit_configure;
    return rows;
}

void TileEditCallout::register_callbacks() {
    ensure_subjects();
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_xml_callbacks({
        {"tile_callout_configure_cb", [](lv_event_t* e) { pick(e, TileCalloutAction::Configure); }},
        {"tile_callout_remove_cb", [](lv_event_t* e) { pick(e, TileCalloutAction::Remove); }},
        {"tile_callout_row_pressed_cb", row_pressed},
        {"tile_callout_row_press_lost_cb", row_press_lost},
    });
}

bool TileEditCallout::show_for(lv_obj_t* screen, lv_obj_t* tile, const std::string& title,
                               TileCalloutRows rows, ActionHandler on_action) {
    register_callbacks();
    if (!screen || !tile) {
        return false;
    }
    lv_subject_copy_string(&s_title_subject, title.c_str());
    lv_subject_set_int(&s_configure_subject, rows.configure ? 1 : 0);
    lv_obj_get_coords(tile, &tile_area_);
    lockout_.reset(lv_tick_get());
    set_action_callback([on_action = std::move(on_action)](int action, int /*item*/) {
        if (action < 0 || !on_action) {
            return;
        }
        on_action(static_cast<TileCalloutAction>(action));
    });
    return show_around_widget(screen, -1, tile);
}

void TileEditCallout::on_created(lv_obj_t* backdrop) {
    if (lv_obj_t* ring = lv_obj_find_by_name(backdrop, "tile_callout_ring")) {
        lv_area_t backdrop_area;
        lv_obj_get_coords(backdrop, &backdrop_area);
        // DECLARATIVE_OK: measured anchor, the ring sits on the tile it names.
        lv_obj_set_pos(ring, tile_area_.x1 - backdrop_area.x1, tile_area_.y1 - backdrop_area.y1);
        lv_obj_set_size(ring, lv_area_get_width(&tile_area_), lv_area_get_height(&tile_area_));
    }
    lv_obj_t* menu_card = card();
    if (!menu_card) {
        return;
    }
    // The lockout runs from the frame that shows the card.
    // DECLARATIVE_OK: draw hook, no declarative equivalent.
    lv_obj_add_event_cb(
        menu_card,
        [](lv_event_t*) {
            if (auto* self = ContextMenu::active_as<TileEditCallout>()) {
                self->lockout_.on_drawn(lv_tick_get());
            }
        },
        LV_EVENT_DRAW_POST, nullptr);

    // Rows and title as wide as their text asks, measured from the caller's
    // strings: a dotted label's own text may already carry the dots.
    lv_obj_update_layout(menu_card);
    widest_row_ = 0;
    for (const char* name : {"tile_callout_configure", "tile_callout_remove"}) {
        lv_obj_t* row = lv_obj_find_by_name(menu_card, name);
        if (!row || lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        if (lv_obj_t* label = ui_button_get_label(row)) {
            widest_row_ =
                std::max(widest_row_, row_width_for_text(row, label, lv_label_get_text(label)));
        }
    }
    if (lv_obj_t* title = lv_obj_find_by_name(menu_card, "card_title")) {
        widest_row_ = std::max(
            widest_row_, row_width_for_text(lv_obj_get_parent(title), title, s_title_buf.data()));
    }
}

ContextMenu::CardWidth TileEditCallout::card_width() const {
    CardWidth width;
    width.pct = 40;
    width.min = 112;
    width.fit_content = true;
    return width;
}

int32_t TileEditCallout::content_width() const {
    return widest_row_;
}

void TileEditCallout::pick(lv_event_t* e, TileCalloutAction action) {
    auto* self = ContextMenu::active_as<TileEditCallout>();
    if (!self) {
        spdlog::warn("[TileEditCallout] No active instance for a row tap");
        return;
    }
    // A click with no input device behind it (helix-screen ctl, tests) has no
    // finger that could have landed on the card as it appeared.
    const bool from_touch = lv_indev_active() != nullptr;
    if (from_touch && !self->lockout_.release_acts(released_over_target(e), lv_tick_get())) {
        spdlog::info("[TileEditCallout] Row tap ignored: pressed within {} ms of the card "
                     "appearing, or lifted off the row",
                     TapLockout::kDefaultMs);
        return;
    }
    self->dispatch_action(static_cast<int>(action));
}

void TileEditCallout::row_pressed(lv_event_t* /*e*/) {
    if (auto* self = ContextMenu::active_as<TileEditCallout>()) {
        self->lockout_.on_press(lv_tick_get());
    }
}

void TileEditCallout::row_press_lost(lv_event_t* /*e*/) {
    if (auto* self = ContextMenu::active_as<TileEditCallout>()) {
        self->lockout_.on_press_lost();
    }
}

} // namespace helix::ui
