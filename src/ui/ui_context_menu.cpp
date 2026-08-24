// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_context_menu.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_utils.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

// ============================================================================
// Construction / Destruction
// ============================================================================

ContextMenu::ContextMenu() {
    spdlog::trace("[ContextMenu] Constructed");
}

ContextMenu::~ContextMenu() {
    hide();
    spdlog::trace("[ContextMenu] Destroyed");
}

// The delete hook carries `this` as its user_data, so a move has to re-point it at
// the new owner. Leaving it on the moved-from object is a use-after-free waiting for
// the backdrop to be destroyed.
void ContextMenu::adopt_from(ContextMenu& other) {
    menu_ = other.menu_;
    parent_ = other.parent_;
    item_index_ = other.item_index_;
    click_point_ = other.click_point_;
    anchor_mode_ = other.anchor_mode_;
    anchor_align_ = other.anchor_align_;
    anchor_widget_ = other.anchor_widget_;
    action_callback_ = std::move(other.action_callback_);

    if (menu_ && lv_obj_is_valid(menu_)) {
        lv_obj_remove_event_cb_with_user_data(menu_, on_menu_deleted, &other);
        lv_obj_add_event_cb(menu_, on_menu_deleted, LV_EVENT_DELETE, this);
    }
    if (s_active_ == &other) {
        s_active_ = this;
    }

    other.menu_ = nullptr;
    other.parent_ = nullptr;
    other.anchor_widget_ = nullptr;
    other.item_index_ = -1;
}

ContextMenu::ContextMenu(ContextMenu&& other) noexcept {
    adopt_from(other);
}

ContextMenu& ContextMenu::operator=(ContextMenu&& other) noexcept {
    if (this != &other) {
        hide();
        adopt_from(other);
    }
    return *this;
}

// ============================================================================
// Active-menu registry and the XML callbacks that route through it
// ============================================================================

ContextMenu* ContextMenu::active() {
    return s_active_;
}

void ContextMenu::on_menu_deleted(lv_event_t* e) {
    auto* self = static_cast<ContextMenu*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    // Only the backdrop we still believe in may clear us. Dismissal is deferred, so
    // a menu raised in the meantime has already claimed menu_ and s_active_ — and
    // the outgoing backdrop's delete must not knock the incoming one out.
    if (lv_event_get_current_target_obj(e) != self->menu_) {
        return;
    }
    self->menu_ = nullptr;
    if (s_active_ == self) {
        s_active_ = nullptr;
    }
    spdlog::trace("[ContextMenu] Backdrop deleted — cleared active menu");
}

void ContextMenu::install_delete_hook() {
    if (menu_) {
        lv_obj_add_event_cb(menu_, on_menu_deleted, LV_EVENT_DELETE, this);
    }
}

void ContextMenu::uninstall_delete_hook() {
    if (menu_ && lv_obj_is_valid(menu_)) {
        lv_obj_remove_event_cb_with_user_data(menu_, on_menu_deleted, this);
    }
}

void ContextMenu::backdrop_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ContextMenu] backdrop_cb");
    (void)e;
    if (ContextMenu* self = active()) {
        self->on_backdrop_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ContextMenu::close_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ContextMenu] close_cb");
    (void)e;
    if (ContextMenu* self = active()) {
        self->on_close_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ContextMenu::register_shared_callbacks() {
    ::register_xml_callbacks({
        {"context_menu_backdrop_cb", ContextMenu::backdrop_cb},
        {"context_menu_close_cb", ContextMenu::close_cb},
    });
}

// ============================================================================
// Public API
// ============================================================================

void ContextMenu::set_action_callback(ActionCallback callback) {
    action_callback_ = std::move(callback);
}

bool ContextMenu::show_near_widget(lv_obj_t* parent, int item_index, lv_obj_t* near_widget) {
    return show_impl(parent, item_index, near_widget, AnchorMode::ClickPoint, AnchorAlign::Center);
}

bool ContextMenu::show_below_widget(lv_obj_t* parent, int item_index, lv_obj_t* anchor,
                                    AnchorAlign align) {
    return show_impl(parent, item_index, anchor, AnchorMode::BelowAnchor, align);
}

bool ContextMenu::show_impl(lv_obj_t* parent, int item_index, lv_obj_t* anchor, AnchorMode mode,
                            AnchorAlign align) {
    // Hide any existing menu first
    hide();

    // ...including one belonging to a different instance. Two widgets of the same
    // kind on one page would otherwise stack two backdrops, and only the newer one
    // would answer the shared callbacks, leaving the older stranded on screen.
    if (ContextMenu* other = s_active_) {
        other->hide();
    }

    if (!parent || !anchor) {
        spdlog::warn("[ContextMenu] Cannot show - missing parent or widget");
        return false;
    }

    // Store state
    parent_ = parent;
    item_index_ = item_index;
    anchor_mode_ = mode;
    anchor_align_ = align;
    anchor_widget_ = anchor;

    // Create context menu from XML
    menu_ = static_cast<lv_obj_t*>(lv_xml_create(parent, xml_component_name(), nullptr));
    if (!menu_) {
        spdlog::error("[ContextMenu] Failed to create menu from XML: {}", xml_component_name());
        return false;
    }

    s_active_ = this;
    install_delete_hook();

    // The backdrop is width/height 100% and nothing has resolved that yet. Everything
    // below reads 0 without this: the width policy collapses to its minimum, and any
    // measuring a subclass does in on_created() sizes against nothing.
    lv_obj_update_layout(menu_);

    lv_obj_t* menu_card = lv_obj_find_by_name(menu_, menu_card_name());

    // Width first, and before on_created(): a card whose rows are width="100%" cannot
    // resolve them against a width="content" parent, so the rows the subclass is about
    // to add would each collapse to their own text.
    if (menu_card) {
        apply_card_width(menu_card);
    }

    // Let subclass configure the menu
    on_created(menu_);

    // Fit the card to the screen, widen its action rows, then position it. Order
    // matters: the column layout decides the row widths, and both decide the height
    // the positioner has to place.
    if (menu_card) {
        fit_card_to_screen(menu_card);
        stretch_rows_to_card(menu_card);
        position_card(menu_card);
    } else {
        spdlog::warn("[ContextMenu] '{}' has no card named '{}' — left unpositioned",
                     xml_component_name(), menu_card_name());
    }

    spdlog::debug("[ContextMenu] Shown '{}' for item {}", xml_component_name(), item_index);
    return true;
}

void ContextMenu::hide() {
    if (!menu_)
        return;

    if (s_active_ == this) {
        s_active_ = nullptr;
    }
    // The hook has done its job; leaving it armed would let this backdrop's deferred
    // delete clear a menu raised before the delete lands.
    uninstall_delete_hook();

    // Use deferred delete since we may be called during event processing
    helix::ui::safe_delete_deferred(menu_);
    item_index_ = -1;
    anchor_widget_ = nullptr;
    spdlog::debug("[ContextMenu] hide()");
}

void ContextMenu::apply_card_width(lv_obj_t* menu_card) {
    const CardWidth policy = card_width();
    if (!policy.is_set()) {
        return;
    }
    lv_obj_t* backdrop = lv_obj_get_parent(menu_card);
    if (!backdrop) {
        return;
    }
    const int32_t screen_w = lv_obj_get_width(backdrop);
    const int32_t width = std::clamp(screen_w * policy.pct / 100, static_cast<int32_t>(policy.min),
                                     static_cast<int32_t>(policy.max));
    lv_obj_set_width(menu_card, width);
    spdlog::trace("[ContextMenu] Card width {}px ({}% of {} clamped to [{},{}])", width, policy.pct,
                  screen_w, policy.min, policy.max);
}

// ============================================================================
// Protected Helpers
// ============================================================================

lv_obj_t* ContextMenu::card() const {
    return menu_ ? lv_obj_find_by_name(menu_, menu_card_name()) : nullptr;
}

int32_t ContextMenu::screen_height_pct(int pct) const {
    if (!menu_) {
        return 0;
    }
    lv_obj_t* screen = lv_obj_get_screen(menu_);
    return screen ? lv_obj_get_height(screen) * pct / 100 : 0;
}

void ContextMenu::on_backdrop_clicked() {
    dispatch_action(ACTION_CANCELLED);
}

void ContextMenu::on_close_clicked() {
    on_backdrop_clicked();
}

void ContextMenu::dispatch_action(int action) {
    int item = item_index_;
    ActionCallback callback_copy = action_callback_;
    spdlog::debug("[ContextMenu] Dispatch action {} for item {}", action, item);

    hide();

    if (callback_copy) {
        callback_copy(action, item);
    }
}

// ============================================================================
// Row sizing
// ============================================================================

// DECLARATIVE_OK: measured layout. A menu card sized `width="content"` cannot stretch
// its rows declaratively — LVGL flex has no cross-axis stretch, and giving a row
// `width="100%"` drops it out of the parent's content-width calculation entirely
// (`w_ignore_size`, lv_obj_pos.c), collapsing the card to its widest non-percentage
// child. So measure the container once, then widen every action row to that
// measurement: the whole row becomes the hit target instead of just the text inside it.
void ContextMenu::stretch_rows_in(lv_obj_t* container) {
    int32_t content_w = lv_obj_get_content_width(container);
    if (content_w <= 0)
        return;

    uint32_t child_cnt = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(container, i);
        if (!child)
            continue;
        // Hidden rows do not contribute to content_w, so forcing a width on one could
        // clip it if it is revealed later. Leave them at their natural size.
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
            continue;
        // Only rows that can actually be tapped — labels, hints and separators keep
        // their declared sizing.
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE))
            continue;
        // Percentage rows already track the container width.
        if (LV_COORD_IS_PCT(lv_obj_get_style_width(child, LV_PART_MAIN)))
            continue;

        lv_obj_set_width(child, content_w);
    }
}

void ContextMenu::stretch_rows_to_card(lv_obj_t* menu_card) {
    lv_obj_update_layout(menu_card);

    // Rows inside a column group must span their own column, not the whole card —
    // stretching them to the card width would make each one as wide as both
    // columns together and blow the row flow apart.
    lv_obj_t* columns = lv_obj_find_by_name(menu_card, COLUMNS_NAME);
    if (columns) {
        uint32_t col_cnt = lv_obj_get_child_count(columns);
        for (uint32_t i = 0; i < col_cnt; i++) {
            lv_obj_t* col = lv_obj_get_child(columns, i);
            if (col && !lv_obj_has_flag(col, LV_OBJ_FLAG_HIDDEN))
                stretch_rows_in(col);
        }
    }

    stretch_rows_in(menu_card);
}

// ============================================================================
// Fitting the card to the screen
// ============================================================================

// DECLARATIVE_OK: measured layout. Whether the stacked card overflows depends on how
// many rows the backend left visible, which is only known after on_created() has run
// and the layout has been measured — there is no subject to bind a structural
// conditional to at build time. The widget tree is identical in both layouts, so this
// only flips one container's flex flow; nothing is created or destroyed.
void ContextMenu::fit_card_to_screen(lv_obj_t* menu_card) {
    lv_obj_t* backdrop = lv_obj_get_parent(menu_card);
    if (!backdrop)
        return;

    lv_obj_update_layout(menu_card);

    // Leave a margin top and bottom so the card reads as a menu floating over the
    // screen rather than a panel that happens to fill it.
    const int32_t margin = theme_manager_get_spacing("space_md");
    const int32_t available_h = lv_obj_get_height(backdrop) - (margin * 2);
    if (available_h <= 0)
        return;

    // A column group whose actions are all hidden would otherwise render as a
    // heading and a rule with nothing under them (external-spool mode hides every
    // lane action, for one).
    lv_obj_t* columns = lv_obj_find_by_name(menu_card, COLUMNS_NAME);
    if (columns) {
        tidy_column_groups(columns);
    }

    lv_obj_update_layout(menu_card);
    if (lv_obj_get_height(menu_card) <= available_h) {
        return; // Stacked layout fits; leave it as one list.
    }

    if (columns) {
        lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
        lv_obj_update_layout(menu_card);
        spdlog::debug("[ContextMenu] Card exceeded {}px — switched to side-by-side columns ({}px)",
                      available_h, lv_obj_get_height(menu_card));
    }

    // Even side by side the card can outgrow a very short screen, so cap it and let
    // the remainder scroll rather than fall off the edge.
    if (lv_obj_get_height(menu_card) > available_h) {
        lv_obj_set_style_max_height(menu_card, available_h, LV_PART_MAIN);
        lv_obj_add_flag(menu_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(menu_card, LV_DIR_VER);
        lv_obj_update_layout(menu_card);
        spdlog::debug("[ContextMenu] Card still over budget — capped at {}px, scrollable",
                      available_h);
    }
}

void ContextMenu::tidy_column_groups(lv_obj_t* columns) {
    lv_obj_t* last_visible = nullptr;
    uint32_t visible_cnt = 0;

    uint32_t col_cnt = lv_obj_get_child_count(columns);
    for (uint32_t i = 0; i < col_cnt; i++) {
        lv_obj_t* col = lv_obj_get_child(columns, i);
        if (!col)
            continue;

        // A column earns its heading only if something tappable survived on_created().
        // Height matters as well as the flag: a bare lv_obj is CLICKABLE by default in
        // LVGL, so the 1px rule under the heading otherwise counts as an action and
        // keeps an empty column alive. The rules are also marked clickable="false" —
        // this is the belt to that pair of braces, so an unmarked divider added later
        // cannot quietly resurrect the bug.
        bool has_action = false;
        uint32_t row_cnt = lv_obj_get_child_count(col);
        for (uint32_t r = 0; r < row_cnt && !has_action; r++) {
            lv_obj_t* row = lv_obj_get_child(col, r);
            if (row && !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) &&
                lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE) &&
                lv_obj_get_height(row) >= MIN_TAPPABLE_H) {
                has_action = true;
            }
        }

        if (has_action) {
            lv_obj_remove_flag(col, LV_OBJ_FLAG_HIDDEN);
            last_visible = col;
            visible_cnt++;
        } else {
            lv_obj_add_flag(col, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // A group heading only earns its space when there is another group to tell it
    // apart from. Alone, it restates the card header one line below it — the
    // external-spool menu read "External Spool" and then "Spool".
    for (uint32_t i = 0; i < col_cnt; i++) {
        lv_obj_t* col = lv_obj_get_child(columns, i);
        if (!col)
            continue;
        // The trigger is how many sibling groups survived on_created(), counted above
        // from live widget state. Binding it would mean every menu publishing a
        // visible-group-count subject purely to feed a generic base-class rule.
        if (lv_obj_t* heading = lv_obj_find_by_name(col, COLUMN_HEADING_NAME)) {
            if (visible_cnt <= 1 && col == last_visible) {
                // DECLARATIVE_OK: measured layout — visible sibling group count
                lv_obj_add_flag(heading, LV_OBJ_FLAG_HIDDEN);
            } else {
                // DECLARATIVE_OK: measured layout — visible sibling group count
                lv_obj_remove_flag(heading, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ============================================================================
// Positioning
// ============================================================================

lv_point_t ContextMenu::compute_card_pos(lv_point_t card, lv_area_t anchor, lv_point_t bounds,
                                         int32_t margin, int32_t gap, AnchorMode mode,
                                         AnchorAlign align) {
    int32_t x = 0;
    int32_t y = 0;

    if (mode == AnchorMode::BelowAnchor) {
        x = (align == AnchorAlign::Center) ? (anchor.x1 + anchor.x2) / 2 - card.x / 2 : anchor.x1;
        y = anchor.y2 + gap;
        // Flip above the anchor rather than covering it. Only worth doing if there is
        // actually more room up there — on a card taller than the screen both sides
        // overflow, and the clamp below is what saves it either way.
        if (y + card.y > bounds.y - margin) {
            y = anchor.y1 - card.y - gap;
        }
    } else {
        // Hang off the click point and mirror about it, so the pointer stays at a
        // corner of the card instead of the card jumping to the other side of the row.
        x = anchor.x1 - margin;
        y = anchor.y1 - margin;
        if (x + card.x > bounds.x - margin) {
            x = anchor.x1 - card.x + margin;
        }
    }

    // High edge first, low edge LAST, on both axes. A card larger than the backdrop
    // gets pushed to a negative origin by the high-edge clamp, and a negative origin
    // clips the header and the first row off the top — the failure #1212 was about.
    // Clamping the low edge afterwards turns that into an overflowing tail instead,
    // which is at least scrollable.
    if (x + card.x > bounds.x - margin) {
        x = bounds.x - card.x - margin;
    }
    if (y + card.y > bounds.y - margin) {
        y = bounds.y - card.y - margin;
    }
    if (x < margin) {
        x = margin;
    }
    if (y < margin) {
        y = margin;
    }

    return {x, y};
}

void ContextMenu::position_card(lv_obj_t* menu_card) {
    // Update layout to get accurate dimensions
    lv_obj_update_layout(menu_card);

    lv_obj_t* backdrop = lv_obj_get_parent(menu_card);
    if (!backdrop) {
        return;
    }

    // The anchor arrives in display coordinates; everything below is backdrop-local.
    lv_area_t backdrop_area;
    lv_obj_get_coords(backdrop, &backdrop_area);

    AnchorMode mode = anchor_mode_;
    lv_area_t anchor{};
    if (mode == AnchorMode::BelowAnchor && anchor_widget_ && lv_obj_is_valid(anchor_widget_)) {
        lv_obj_get_coords(anchor_widget_, &anchor);
    } else {
        // No live anchor rect to hang off — fall back to the click point.
        mode = AnchorMode::ClickPoint;
        anchor = {click_point_.x, click_point_.y, click_point_.x, click_point_.y};
    }
    anchor.x1 -= backdrop_area.x1;
    anchor.x2 -= backdrop_area.x1;
    anchor.y1 -= backdrop_area.y1;
    anchor.y2 -= backdrop_area.y1;

    const lv_point_t card = {lv_obj_get_width(menu_card), lv_obj_get_height(menu_card)};
    const lv_point_t bounds = {lv_obj_get_width(backdrop), lv_obj_get_height(backdrop)};
    const lv_point_t pos =
        compute_card_pos(card, anchor, bounds, theme_manager_get_spacing("space_md"),
                         theme_manager_get_spacing("space_xs"), mode, anchor_align_);

    lv_obj_set_pos(menu_card, pos.x, pos.y);

    spdlog::debug("[ContextMenu] {} anchor({},{})-({},{}) card {}x{} in {}x{} -> ({},{})",
                  mode == AnchorMode::BelowAnchor ? "below" : "click", anchor.x1, anchor.y1,
                  anchor.x2, anchor.y2, card.x, card.y, bounds.x, bounds.y, pos.x, pos.y);
}

} // namespace helix::ui
