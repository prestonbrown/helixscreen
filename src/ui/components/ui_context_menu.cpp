// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_context_menu.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_utils.h"

#include "helix/ui/text_metrics.h"
#include "layout_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

namespace helix::ui {

// ============================================================================
// Construction / Destruction
// ============================================================================

ContextMenu::ContextMenu() {
    spdlog::trace("[ContextMenu] Constructed");
}

ContextMenu::~ContextMenu() {
    hide();
    if (s_active_ == this) {
        s_active_ = nullptr;
    }
    spdlog::trace("[ContextMenu] Destroyed");
}

void ContextMenu::adopt_from(ContextMenu& other) {
    menu_ = other.menu_.get();
    parent_ = other.parent_;
    item_index_ = other.item_index_;
    click_point_ = other.click_point_;
    has_click_point_ = other.has_click_point_;
    anchor_mode_ = other.anchor_mode_;
    anchor_align_ = other.anchor_align_;
    anchor_widget_ = other.anchor_widget_.get();
    action_callback_ = std::move(other.action_callback_);

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
    // The backdrop can be destroyed without going through hide(): a screen swap takes
    // its parent with it. menu_ reads nullptr from then on, so that menu is not active.
    return s_active_ && s_active_->menu_ ? s_active_ : nullptr;
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

bool ContextMenu::show_around_widget(lv_obj_t* parent, int item_index, lv_obj_t* anchor) {
    return show_impl(parent, item_index, anchor, AnchorMode::AroundAnchor, AnchorAlign::Center);
}

bool ContextMenu::show_impl(lv_obj_t* parent, int item_index, lv_obj_t* anchor, AnchorMode mode,
                            AnchorAlign align) {
    // Hide any existing menu first. A click point set for this show survives the
    // hide, which clears it so a later show cannot inherit it.
    const bool had_click_point = has_click_point_;
    hide();
    has_click_point_ = had_click_point;

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

    // A card fitted to its rows can only be sized once on_created() has built them.
    if (menu_card) {
        apply_fit_card_width(menu_card);
    }

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
    // Use deferred delete since we may be called during event processing
    helix::ui::safe_delete_deferred(menu_);
    item_index_ = -1;
    anchor_widget_ = nullptr;
    has_click_point_ = false;
    spdlog::debug("[ContextMenu] hide()");
}

int32_t ContextMenu::compute_card_width(const CardWidth& policy, int32_t screen_w,
                                        int32_t content_w, int32_t margin, bool portrait) {
    if (!policy.fit_content) {
        return std::clamp(screen_w * policy.pct / 100, static_cast<int32_t>(policy.min),
                          static_cast<int32_t>(policy.max));
    }
    const int pct = (portrait && policy.portrait_pct > 0) ? policy.portrait_pct : policy.pct;
    int32_t ceiling = std::min(screen_w * pct / 100, screen_w - (margin * 2));
    if (policy.max > 0) {
        ceiling = std::min(ceiling, static_cast<int32_t>(policy.max));
    }
    return std::max(std::min(std::max(content_w, static_cast<int32_t>(policy.min)), ceiling),
                    static_cast<int32_t>(0));
}

void ContextMenu::apply_card_width(lv_obj_t* menu_card) {
    const CardWidth policy = card_width();
    if (!policy.is_set() || policy.fit_content) {
        return;
    }
    lv_obj_t* backdrop = lv_obj_get_parent(menu_card);
    if (!backdrop) {
        return;
    }
    const int32_t screen_w = lv_obj_get_width(backdrop);
    const int32_t width = compute_card_width(policy, screen_w, 0, 0, false);
    lv_obj_set_width(menu_card, width);
    spdlog::trace("[ContextMenu] Card width {}px ({}% of {} clamped to [{},{}])", width, policy.pct,
                  screen_w, policy.min, policy.max);
}

void ContextMenu::apply_fit_card_width(lv_obj_t* menu_card) {
    const CardWidth policy = card_width();
    if (!policy.is_set() || !policy.fit_content) {
        return;
    }
    lv_obj_t* backdrop = lv_obj_get_parent(menu_card);
    if (!backdrop) {
        return;
    }
    const int32_t screen_w = lv_obj_get_width(backdrop);
    // A measured-exact fit can still render a pixel or two over (text_metrics.h),
    // so the rows get a comfort margin before the card is sized to them.
    const int32_t content_w = content_width() + theme_manager_get_spacing("space_xs") +
                              lv_obj_get_style_space_left(menu_card, LV_PART_MAIN) +
                              lv_obj_get_style_space_right(menu_card, LV_PART_MAIN);
    const bool portrait = helix::app_layout_is_portrait(screen_w, lv_obj_get_height(backdrop));
    const int32_t width = compute_card_width(policy, screen_w, content_w,
                                             theme_manager_get_spacing("space_md"), portrait);
    // DECLARATIVE_OK: measured layout — the width of the rows' own text
    lv_obj_set_width(menu_card, width);
    spdlog::trace("[ContextMenu] Card fitted to {}px (content {}px, {}% of {}, min {})", width,
                  content_w, policy.pct, screen_w, policy.min);
}

int32_t ContextMenu::row_width_for_text(lv_obj_t* row, lv_obj_t* label, const char* text) {
    if (!row || !label) {
        return 0;
    }
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    int32_t width = lv_obj_get_style_space_left(row, LV_PART_MAIN) +
                    lv_obj_get_style_space_right(row, LV_PART_MAIN);

    // The label may sit in a text column inside the row: that column is the
    // row's child to measure, from the text plus every padding in between.
    lv_obj_t* holder = label;
    int32_t text_inset = lv_obj_get_style_space_left(label, LV_PART_MAIN) +
                         lv_obj_get_style_space_right(label, LV_PART_MAIN);
    while (holder && lv_obj_get_parent(holder) != row) {
        holder = lv_obj_get_parent(holder);
        if (holder) {
            text_inset += lv_obj_get_style_space_left(holder, LV_PART_MAIN) +
                          lv_obj_get_style_space_right(holder, LV_PART_MAIN);
        }
    }

    int32_t visible = 0;
    const uint32_t child_cnt = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(row, i);
        if (!child || lv_obj_is_hidden(child)) {
            continue;
        }
        visible++;
        if (child == holder) {
            width += helix::ui::text_width(text, font, letter_space) + text_inset;
        } else {
            width += lv_obj_get_width(child);
        }
    }
    if (visible > 1) {
        width += lv_obj_get_style_pad_column(row, LV_PART_MAIN) * (visible - 1);
    }
    return width;
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
    dispatch_action(kActionCancelled);
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
        if (lv_obj_is_hidden(child))
            continue;
        // Only rows that can actually be tapped — labels, hints and separators keep
        // their declared sizing.
        if (!lv_obj_is_clickable(child))
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
    lv_obj_t* columns = lv_obj_find_by_name(menu_card, kColumnsName);
    if (columns) {
        uint32_t col_cnt = lv_obj_get_child_count(columns);
        // Stacked, the groups are rows of one list: each takes the list's width, so
        // the narrower group's rows still reach the card's far edge.
        const int32_t columns_w = lv_obj_get_content_width(columns);
        if (lv_obj_get_style_flex_flow(columns, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN &&
            columns_w > 0) {
            for (uint32_t i = 0; i < col_cnt; i++) {
                lv_obj_t* col = lv_obj_get_child(columns, i);
                if (col && !lv_obj_is_hidden(col))
                    lv_obj_set_width(col, columns_w);
            }
            lv_obj_update_layout(menu_card);
        }
        for (uint32_t i = 0; i < col_cnt; i++) {
            lv_obj_t* col = lv_obj_get_child(columns, i);
            if (col && !lv_obj_is_hidden(col))
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
    lv_obj_t* columns = lv_obj_find_by_name(menu_card, kColumnsName);
    if (columns) {
        tidy_column_groups(columns);
    }

    lv_obj_update_layout(menu_card);
    const int32_t stacked_h = lv_obj_get_height(menu_card);
    if (stacked_h <= available_h) {
        return; // Stacked layout fits; leave it as one list.
    }

    if (columns) {
        lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
        lv_obj_update_layout(menu_card);
        spdlog::debug("[ContextMenu] Card {}px tall exceeded {}px — switched to side-by-side "
                      "columns ({}x{}px)",
                      stacked_h, available_h, lv_obj_get_width(menu_card),
                      lv_obj_get_height(menu_card));

        const int32_t available_w = lv_obj_get_width(backdrop) - (margin * 2);
        if (decide_column_flow(stacked_h, lv_obj_get_width(menu_card), available_w, available_h) ==
            ColumnFlow::RowWrap) {
            // A wrapping row needs a width to wrap against: the card's own, at
            // the most the screen allows.
            const int32_t inner_w = available_w -
                                    lv_obj_get_style_space_left(menu_card, LV_PART_MAIN) -
                                    lv_obj_get_style_space_right(menu_card, LV_PART_MAIN);
            lv_obj_set_width(columns, inner_w);
            lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_update_layout(menu_card);
            // A group wrapped below the first line has the card's whole width to
            // itself, so it takes it rather than ellipsize its rows in a column
            // sized for sitting beside the others.
            int32_t first_line_y = -1;
            for (uint32_t i = 0; i < lv_obj_get_child_count(columns); i++) {
                lv_obj_t* col = lv_obj_get_child(columns, static_cast<int32_t>(i));
                if (!col || lv_obj_is_hidden(col))
                    continue;
                if (first_line_y < 0) {
                    first_line_y = lv_obj_get_y(col);
                } else if (lv_obj_get_y(col) > first_line_y) {
                    lv_obj_set_width(col, inner_w);
                }
            }
            lv_obj_update_layout(menu_card);
            spdlog::debug("[ContextMenu] Columns wider than {}px — wrapped ({}x{}px)", available_w,
                          lv_obj_get_width(menu_card), lv_obj_get_height(menu_card));
        }
    }

    // Even side by side the card can outgrow a very short screen, so cap it and let
    // the remainder scroll rather than fall off the edge.
    if (lv_obj_get_height(menu_card) > available_h) {
        lv_obj_set_style_max_height(menu_card, available_h, LV_PART_MAIN);
        lv_obj_set_scrollable(menu_card, true);
        lv_obj_set_scroll_dir(menu_card, LV_DIR_VER);
        lv_obj_update_layout(menu_card);
        spdlog::debug("[ContextMenu] Card still over budget — capped at {}px, scrollable",
                      available_h);
    }
}

ContextMenu::ColumnFlow ContextMenu::decide_column_flow(int32_t stacked_h, int32_t row_w,
                                                        int32_t avail_w, int32_t avail_h) {
    if (stacked_h <= avail_h) {
        return ColumnFlow::Stacked;
    }
    return row_w <= avail_w ? ColumnFlow::Row : ColumnFlow::RowWrap;
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
        // cannot quietly keep an empty column alive.
        bool has_action = false;
        uint32_t row_cnt = lv_obj_get_child_count(col);
        for (uint32_t r = 0; r < row_cnt && !has_action; r++) {
            lv_obj_t* row = lv_obj_get_child(col, r);
            if (row && !lv_obj_is_hidden(row) && lv_obj_is_clickable(row) &&
                lv_obj_get_height(row) >= kMinTappableH) {
                has_action = true;
            }
        }

        if (has_action) {
            lv_obj_set_hidden(col, false);
            last_visible = col;
            visible_cnt++;
        } else {
            lv_obj_set_hidden(col, true);
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
        if (lv_obj_t* heading = lv_obj_find_by_name(col, kColumnHeadingName)) {
            if (visible_cnt <= 1 && col == last_visible) {
                // DECLARATIVE_OK: measured layout — visible sibling group count
                lv_obj_set_hidden(heading, true);
            } else {
                // DECLARATIVE_OK: measured layout — visible sibling group count
                lv_obj_set_hidden(heading, false);
            }
        }
    }
}

// ============================================================================
// Positioning
// ============================================================================

lv_point_t ContextMenu::compute_card_pos(lv_point_t card, lv_area_t anchor, lv_point_t bounds,
                                         int32_t margin, int32_t gap, AnchorMode mode,
                                         AnchorAlign align, AnchorSide* side) {
    int32_t x = 0;
    int32_t y = 0;
    AnchorSide intended = AnchorSide::Over;

    if (mode == AnchorMode::AroundAnchor) {
        // Anchor coordinates are inclusive, so a card gap px clear of the anchor
        // starts one past its far edge.
        const int32_t anchor_w = anchor.x2 - anchor.x1 + 1;
        const int32_t anchor_h = anchor.y2 - anchor.y1 + 1;
        const int32_t centre_x = anchor.x1 + anchor_w / 2 - card.x / 2;
        const int32_t centre_y = anchor.y1 + anchor_h / 2 - card.y / 2;
        const bool fits_across = card.x <= bounds.x - 2 * margin;
        const bool fits_down = card.y <= bounds.y - 2 * margin;
        const auto fits_y = [&](int32_t top) {
            return fits_across && top >= margin && top + card.y <= bounds.y - margin;
        };
        const auto fits_x = [&](int32_t left) {
            return fits_down && left >= margin && left + card.x <= bounds.x - margin;
        };
        const int32_t below_y = anchor.y2 + 1 + gap;
        const int32_t above_y = anchor.y1 - gap - card.y;
        const int32_t right_x = anchor.x2 + 1 + gap;
        const int32_t left_x = anchor.x1 - gap - card.x;
        if (fits_y(below_y)) {
            intended = AnchorSide::Below;
            x = centre_x;
            y = below_y;
        } else if (fits_y(above_y)) {
            intended = AnchorSide::Above;
            x = centre_x;
            y = above_y;
        } else if (fits_x(right_x)) {
            intended = AnchorSide::Right;
            x = right_x;
            y = anchor.y1;
        } else if (fits_x(left_x)) {
            intended = AnchorSide::Left;
            x = left_x;
            y = anchor.y1;
        } else {
            x = centre_x;
            y = centre_y;
        }
    } else if (mode == AnchorMode::BelowAnchor) {
        intended = AnchorSide::Below;
        x = (align == AnchorAlign::Center) ? (anchor.x1 + anchor.x2) / 2 - card.x / 2 : anchor.x1;
        y = anchor.y2 + gap;
        // Flip above the anchor rather than covering it. Only worth doing if there is
        // actually more room up there — on a card taller than the screen both sides
        // overflow, and the clamp below is what saves it either way.
        if (y + card.y > bounds.y - margin) {
            intended = AnchorSide::Above;
            y = anchor.y1 - card.y - gap;
        }
    } else {
        // Hang off the click point and mirror about it, so the pointer stays at a
        // corner of the card instead of the card jumping to the other side of the row.
        // The card covers the point, so it gets no caret: the point is on a canvas
        // that draws many items, and no side of it is clear of the item tapped.
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

    // A click-point card hangs off its point with no gap for a caret, so even one
    // the clamp slid off the point reports Over.
    if (side) {
        *side = mode == AnchorMode::ClickPoint
                    ? AnchorSide::Over
                    : side_of_anchor({x, y, x + card.x - 1, y + card.y - 1}, anchor, intended);
    }
    return {x, y};
}

ContextMenu::AnchorSide ContextMenu::side_of_anchor(lv_area_t card, lv_area_t anchor,
                                                    AnchorSide prefer) {
    const auto clears = [&](AnchorSide s) {
        switch (s) {
        case AnchorSide::Below:
            return card.y1 > anchor.y2;
        case AnchorSide::Above:
            return card.y2 < anchor.y1;
        case AnchorSide::Right:
            return card.x1 > anchor.x2;
        case AnchorSide::Left:
            return card.x2 < anchor.x1;
        case AnchorSide::Over:
            break;
        }
        return false;
    };
    if (clears(prefer)) {
        return prefer;
    }
    for (AnchorSide s :
         {AnchorSide::Below, AnchorSide::Above, AnchorSide::Right, AnchorSide::Left}) {
        if (clears(s)) {
            return s;
        }
    }
    return AnchorSide::Over;
}

ContextMenu::CaretPlacement ContextMenu::compute_caret(lv_area_t card, lv_area_t anchor,
                                                       AnchorSide side, int32_t radius,
                                                       int32_t caret_h) {
    if (side == AnchorSide::Over || caret_h <= 0) {
        return {};
    }
    const bool horizontal_edge = side == AnchorSide::Below || side == AnchorSide::Above;
    const int32_t edge_start = horizontal_edge ? card.x1 : card.y1;
    const int32_t edge_len = horizontal_edge ? card.x2 - card.x1 + 1 : card.y2 - card.y1 + 1;
    const int32_t target = horizontal_edge ? (anchor.x1 + anchor.x2) / 2 - edge_start
                                           : (anchor.y1 + anchor.y2) / 2 - edge_start;

    // The caret's base is 2 * caret_h wide; its half-width plus the corner radius
    // is how close to either end of the edge its centre may come.
    const int32_t inset = std::max(radius, static_cast<int32_t>(0)) + caret_h;
    const int32_t lo = inset;
    const int32_t hi = edge_len - 1 - inset;
    const int32_t offset = lo <= hi ? std::clamp(target, lo, hi) : edge_len / 2;
    return {side, offset};
}

namespace {

/// A caret's geometry, owned by the card it is drawn on.
struct CaretDraw {
    ContextMenu::CaretPlacement caret;
    int32_t caret_h = 0;
};

/// A point @p along the caret's edge from the card's start, @p out px outward
/// from the card's outer boundary (negative goes into the card).
lv_point_precise_t caret_point(const lv_area_t& card, ContextMenu::AnchorSide side, float along,
                               float out) {
    using AnchorSide = ContextMenu::AnchorSide;
    float x = 0.0f;
    float y = 0.0f;
    switch (side) {
    case AnchorSide::Below: // top edge, pointing up
        x = static_cast<float>(card.x1) + along;
        y = static_cast<float>(card.y1) - out;
        break;
    case AnchorSide::Above: // bottom edge, pointing down
        x = static_cast<float>(card.x1) + along;
        y = static_cast<float>(card.y2 + 1) + out;
        break;
    case AnchorSide::Right: // left edge, pointing left
        x = static_cast<float>(card.x1) - out;
        y = static_cast<float>(card.y1) + along;
        break;
    case AnchorSide::Left: // right edge, pointing right
    case AnchorSide::Over:
        x = static_cast<float>(card.x2 + 1) + out;
        y = static_cast<float>(card.y1) + along;
        break;
    }
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}

void draw_caret_triangle(lv_layer_t* layer, const lv_area_t& card, ContextMenu::AnchorSide side,
                         float centre, float tip, float base, lv_color_t color, lv_opa_t opa) {
    // The sides run at 45 degrees, so the half-width at the base is its
    // distance from the tip.
    const float half = tip - base;
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    dsc.p[0] = caret_point(card, side, centre, tip);
    dsc.p[1] = caret_point(card, side, centre - half, base);
    dsc.p[2] = caret_point(card, side, centre + half, base);
    lv_draw_triangle(layer, &dsc);
}

/// The card edge a caret on @p side sits on, as a border side.
lv_border_side_t caret_edge_border_side(ContextMenu::AnchorSide side) {
    using AnchorSide = ContextMenu::AnchorSide;
    switch (side) {
    case AnchorSide::Below:
        return LV_BORDER_SIDE_TOP;
    case AnchorSide::Above:
        return LV_BORDER_SIDE_BOTTOM;
    case AnchorSide::Right:
        return LV_BORDER_SIDE_LEFT;
    case AnchorSide::Left:
        return LV_BORDER_SIDE_RIGHT;
    case AnchorSide::Over:
        break;
    }
    return LV_BORDER_SIDE_NONE;
}

void caret_ext_draw_size_cb(lv_event_t* e) {
    auto* draw = static_cast<CaretDraw*>(lv_event_get_user_data(e));
    auto* card = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!draw || !card) {
        return;
    }
    // One extra px for the anti-aliased tip.
    lv_event_set_ext_draw_size(e, draw->caret_h + 1);
}

// A border on the edge the caret sits on runs on along the caret's two sides: a
// border-coloured triangle, then a fill-coloured one inset by the border width
// whose base reaches into the card past its border, so no border line crosses
// the caret's mouth.
void caret_draw_cb(lv_event_t* e) {
    auto* draw = static_cast<CaretDraw*>(lv_event_get_user_data(e));
    auto* card = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!draw || !card || !layer || !draw->caret.visible()) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(card, &area);
    const auto side = draw->caret.side;
    const float centre = static_cast<float>(draw->caret.offset) + 0.5f;
    const float h = static_cast<float>(draw->caret_h);

    const int32_t border_w = lv_obj_get_style_border_width(card, LV_PART_MAIN);
    const lv_opa_t border_opa = lv_obj_get_style_border_opa(card, LV_PART_MAIN);
    const bool has_border =
        border_w > 0 && border_opa > LV_OPA_MIN &&
        (lv_obj_get_style_border_side(card, LV_PART_MAIN) & caret_edge_border_side(side)) != 0;
    float inner_tip = h;
    if (has_border) {
        draw_caret_triangle(layer, area, side, centre, h, 0.0f,
                            lv_obj_get_style_border_color(card, LV_PART_MAIN), border_opa);
        // A 45-degree side of perpendicular width w moves the tip in by w * sqrt(2).
        inner_tip = h - static_cast<float>(border_w) * 1.41421356f;
    }
    if (inner_tip <= 0.0f) {
        return;
    }
    // An opaque fill reaches past the edge's border into the card to cover it; a
    // translucent one would blend twice where it overlaps, so it stops at the edge.
    const lv_opa_t bg_opa = lv_obj_get_style_bg_opa(card, LV_PART_MAIN);
    const float base =
        bg_opa >= LV_OPA_COVER ? -static_cast<float>(has_border ? border_w : 0) - 1.0f : 0.0f;
    draw_caret_triangle(layer, area, side, centre, inner_tip, base,
                        lv_obj_get_style_bg_color(card, LV_PART_MAIN), bg_opa);
}

void caret_delete_cb(lv_event_t* e) {
    delete static_cast<CaretDraw*>(lv_event_get_user_data(e));
}

} // namespace

void ContextMenu::attach_caret(lv_obj_t* menu_card, CaretPlacement caret, int32_t caret_h) {
    if (!menu_card || !caret.visible() || caret_h <= 0) {
        return;
    }
    auto draw = std::make_unique<CaretDraw>(CaretDraw{caret, caret_h});
    CaretDraw* raw = draw.release();
    // DECLARATIVE_OK: draw hook, the caret is painted from measured placement.
    lv_obj_add_event_cb(menu_card, caret_ext_draw_size_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, raw);
    lv_obj_add_event_cb(menu_card, caret_draw_cb, LV_EVENT_DRAW_MAIN_END, raw);
    lv_obj_add_event_cb(menu_card, caret_delete_cb, LV_EVENT_DELETE, raw);
    lv_obj_refresh_ext_draw_size(menu_card);
    lv_obj_invalidate(menu_card);
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
    if (mode != AnchorMode::ClickPoint && anchor_widget_) {
        lv_obj_get_coords(anchor_widget_, &anchor);
    } else {
        // No live anchor rect to hang off: fall back to the click point, or to the
        // middle of the screen when this show was given none.
        mode = AnchorMode::ClickPoint;
        const lv_point_t at = has_click_point_
                                  ? click_point_
                                  : lv_point_t{(backdrop_area.x1 + backdrop_area.x2) / 2,
                                               (backdrop_area.y1 + backdrop_area.y2) / 2};
        anchor = {at.x, at.y, at.x, at.y};
    }
    anchor.x1 -= backdrop_area.x1;
    anchor.x2 -= backdrop_area.x1;
    anchor.y1 -= backdrop_area.y1;
    anchor.y2 -= backdrop_area.y1;

    const lv_point_t card = {lv_obj_get_width(menu_card), lv_obj_get_height(menu_card)};
    const lv_point_t bounds = {lv_obj_get_width(backdrop), lv_obj_get_height(backdrop)};
    // The gap leaves room for the caret, so its tip stops short of the anchor.
    const int32_t caret_h = theme_manager_get_spacing("menu_caret");
    AnchorSide side = AnchorSide::Below;
    const lv_point_t pos = compute_card_pos(
        card, anchor, bounds, theme_manager_get_spacing("space_md"),
        theme_manager_get_spacing("space_xs") + caret_h, mode, anchor_align_, &side);

    lv_obj_set_pos(menu_card, pos.x, pos.y);

    // A click-point card reports Over, so it gets none.
    const lv_area_t placed{pos.x, pos.y, pos.x + card.x - 1, pos.y + card.y - 1};
    attach_caret(menu_card,
                 compute_caret(placed, anchor, side,
                               lv_obj_get_style_radius(menu_card, LV_PART_MAIN), caret_h),
                 caret_h);

    static constexpr const char* kSideNames[] = {"below", "above", "right", "left", "over"};
    spdlog::debug("[ContextMenu] {} anchor({},{})-({},{}) card {}x{} in {}x{} -> ({},{}) {}",
                  mode == AnchorMode::ClickPoint    ? "click"
                  : mode == AnchorMode::BelowAnchor ? "below"
                                                    : "around",
                  anchor.x1, anchor.y1, anchor.x2, anchor.y2, card.x, card.y, bounds.x, bounds.y,
                  pos.x, pos.y, kSideNames[static_cast<int>(side)]);
}

} // namespace helix::ui
