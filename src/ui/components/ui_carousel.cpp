// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_carousel.h"

#include "ui_utils.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace {

/// @p index wrapped into [0, count) when @p wrap, clamped into it otherwise.
/// @p count must be positive.
int wrap_or_clamp(int index, int count, bool wrap) {
    if (wrap) {
        return ((index % count) + count) % count;
    }
    return std::clamp(index, 0, count - 1);
}

/// Whether a carousel of @p page_count pages swipes under @p policy.
bool swipe_enabled(helix::ui::CarouselSwipe policy, int page_count) {
    switch (policy) {
    case helix::ui::CarouselSwipe::Disabled:
        return false;
    case helix::ui::CarouselSwipe::Auto:
        break;
    }
    return page_count > 1;
}

/// Pages the carousel shows: real_page_count when set, the tile count otherwise.
int page_count(const CarouselState& state) {
    return (state.real_page_count >= 0) ? state.real_page_count
                                        : static_cast<int>(state.real_tiles.size());
}

/// Tiles a swipe or a goto can reach: every tile, or only the pages while the
/// tiles past real_page_count are out of reach.
int reachable_tile_count(const CarouselState& state) {
    const int tiles = static_cast<int>(state.real_tiles.size());
    if (state.trailing_tiles_reachable || state.real_page_count < 0) {
        return tiles;
    }
    return std::min(state.real_page_count, tiles);
}

/**
 * @brief Write the carousel's input flags for its page count
 *
 * The swipe (the scroll container's SCROLLABLE flag and scroll direction, from
 * the swipe policy); CLICKABLE and EVENT_BUBBLE on the scroll container and
 * every tile, where a single page passes input through and more pages capture
 * it unless bubble_events is set; and HIDDEN on every tile out of reach. LVGL
 * lays out, scrolls to, snaps to and hit-tests only shown children, so a hidden
 * tile leaves the swipe no room and takes no press. Touches neither the
 * indicator row nor its dots.
 */
void apply_input_flags(CarouselState* state) {
    const int count = page_count(*state);
    const bool multi_page = count > 1;
    const bool bubble = !multi_page || state->bubble_events;

    const int reachable = reachable_tile_count(*state);
    if (state->scroll_container) {
        // A reachable tile past the last page is somewhere to swipe to, so the
        // strip scrolls for it as for a page.
        const bool swipe = swipe_enabled(state->swipe, std::max(count, reachable));
        lv_obj_update_flag(state->scroll_container, LV_OBJ_FLAG_SCROLLABLE, swipe);
        lv_obj_set_scroll_dir(state->scroll_container, swipe ? LV_DIR_HOR : LV_DIR_NONE);
        lv_obj_update_flag(state->scroll_container, LV_OBJ_FLAG_CLICKABLE, multi_page);
        lv_obj_update_flag(state->scroll_container, LV_OBJ_FLAG_EVENT_BUBBLE, bubble);
    }
    for (size_t i = 0; i < state->real_tiles.size(); ++i) {
        lv_obj_t* tile = state->real_tiles[i];
        lv_obj_update_flag(tile, LV_OBJ_FLAG_CLICKABLE, multi_page);
        lv_obj_update_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE, bubble);
        // Written only on a change: showing or hiding redraws the tile, and the
        // swipe policy rewrites these flags on every edit gesture.
        const bool hide = static_cast<int>(i) >= reachable;
        if (lv_obj_has_flag(tile, LV_OBJ_FLAG_HIDDEN) != hide) {
            lv_obj_update_flag(tile, LV_OBJ_FLAG_HIDDEN, hide);
        }
    }
}

/**
 * @brief Update indicator dot styles without recreating them
 *
 * Sets the active dot to accent color with full opacity and
 * inactive dots to text_muted with reduced opacity.
 */
void update_indicators(CarouselState* state) {
    if (!state || !state->indicator_row) {
        return;
    }

    uint32_t dot_count = lv_obj_get_child_count(state->indicator_row);
    for (uint32_t i = 0; i < dot_count; i++) {
        lv_obj_t* dot = lv_obj_get_child(state->indicator_row, static_cast<int32_t>(i));
        if (!dot) {
            continue;
        }

        lv_obj_set_style_bg_color(dot, theme_manager_get_color("text"), LV_PART_MAIN);
        if (static_cast<int>(i) == state->current_page) {
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_opa(dot, LV_OPA_40, LV_PART_MAIN);
        }
    }
}

/**
 * @brief SCROLL_END event handler — detects page changes from swipe gestures
 *
 * Calculates the current page from the scroll offset and updates state.
 */
void carousel_scroll_end_cb(lv_event_t* e) {
    lv_obj_t* scroll = lv_event_get_target_obj(e);
    if (!scroll) {
        return;
    }

    // The carousel container is the parent of the scroll container
    lv_obj_t* container = lv_obj_get_parent(scroll);
    if (!container) {
        return;
    }

    CarouselState* state = static_cast<CarouselState*>(lv_obj_get_user_data(container));
    if (!state || state->magic != CarouselState::kMagic) {
        return;
    }

    // A goto sets the page itself. Starting its scroll deletes any running
    // scroll animation, and LVGL sends that animation's SCROLL_END right then,
    // with the offset wherever the animation had reached.
    if (state->goto_scrolling) {
        return;
    }

    int32_t container_w = lv_obj_get_content_width(scroll);
    if (container_w <= 0) {
        return;
    }

    // Nearest page: a scroll can settle a few px either side of a boundary.
    int32_t scroll_x = lv_obj_get_scroll_x(scroll);
    int page = static_cast<int>((scroll_x + container_w / 2) / container_w);

    const int count = reachable_tile_count(*state);
    if (count > 0) {
        page = wrap_or_clamp(page, count, state->wrap);
    }

    if (page != state->current_page) {
        state->current_page = page;
        if (state->page_subject) {
            lv_subject_set_int(state->page_subject, page);
        }
        update_indicators(state);
        spdlog::trace("[ui_carousel] Scroll ended on page {}/{}", page, count);
    }
}

/// Whether a pointer is swiping @p scroll, from its drag through the snap that ends it.
bool swiped_by_pointer(lv_obj_t* scroll) {
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_scroll_obj(indev) == scroll) {
            return true;
        }
    }
    return false;
}

/**
 * @brief SCROLL_BEGIN event handler: only a swipe or a goto moves the page
 *
 * Any other animated scroll of the scroll container is aimed at the current
 * page. Focus is the one that matters: LVGL scrolls every scrollable ancestor
 * of an object that gains focus to show it, animated, and focus moves on by
 * itself when the focused object is hidden or deleted. A control anywhere in
 * the carousel that takes focus, whenever it was created, leaves the page where
 * it is, while scrollables inside a page still bring it into view.
 */
void carousel_scroll_begin_cb(lv_event_t* e) {
    lv_obj_t* scroll = lv_event_get_current_target_obj(e);
    // Scrolls inside the pages bubble up to here; only the page strip's own counts.
    if (lv_event_get_target_obj(e) != scroll) {
        return;
    }
    // A scroll that is not animated has no animation to aim: a swipe's drag, or
    // an offset set outright.
    auto* anim = static_cast<lv_anim_t*>(lv_event_get_param(e));
    if (!anim) {
        return;
    }
    CarouselState* state = ui_carousel_get_state(lv_obj_get_parent(scroll));
    if (!state || state->goto_scrolling || swiped_by_pointer(scroll)) {
        return;
    }
    // A scroll animation runs over the negated scroll offset.
    const int32_t page_x = state->current_page * lv_obj_get_content_width(scroll);
    lv_anim_set_values(anim, anim->start_value, -page_x);
}

/**
 * @brief Auto-advance timer callback — advances to the next page
 *
 * Skips advancement if the user is currently touching the carousel.
 * Relies on goto_page wrap logic for looping behavior.
 */
void auto_advance_cb(lv_timer_t* timer) {
    lv_obj_t* carousel = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    if (!carousel) {
        return;
    }

    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state || state->user_touching) {
        return;
    }

    ui_carousel_goto_page(carousel, state->current_page + 1, true);
}

/**
 * @brief Touch press handler — pauses auto-advance while user is interacting
 */
void carousel_press_cb(lv_event_t* e) {
    lv_obj_t* scroll = lv_event_get_target_obj(e);
    if (!scroll) {
        return;
    }

    lv_obj_t* container = lv_obj_get_parent(scroll);
    if (!container) {
        return;
    }

    CarouselState* state = static_cast<CarouselState*>(lv_obj_get_user_data(container));
    if (!state || state->magic != CarouselState::kMagic) {
        return;
    }

    state->user_touching = true;
    if (state->auto_timer) {
        lv_timer_pause(state->auto_timer);
    }
}

/**
 * @brief Touch release handler — resumes auto-advance after user stops interacting
 */
void carousel_release_cb(lv_event_t* e) {
    lv_obj_t* scroll = lv_event_get_target_obj(e);
    if (!scroll) {
        return;
    }

    lv_obj_t* container = lv_obj_get_parent(scroll);
    if (!container) {
        return;
    }

    CarouselState* state = static_cast<CarouselState*>(lv_obj_get_user_data(container));
    if (!state || state->magic != CarouselState::kMagic) {
        return;
    }

    state->user_touching = false;
    if (state->auto_timer) {
        lv_timer_reset(state->auto_timer);
        lv_timer_resume(state->auto_timer);
    }
}

/**
 * @brief DELETE event handler — cleans up CarouselState and auto-scroll timer
 */
void carousel_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    CarouselState* state = static_cast<CarouselState*>(lv_obj_get_user_data(obj));
    if (!state || state->magic != CarouselState::kMagic) {
        return;
    }

    if (state->auto_timer) {
        lv_timer_delete(state->auto_timer);
        state->auto_timer = nullptr;
    }

    spdlog::trace("[ui_carousel] Deleting carousel state ({} tiles)", state->real_tiles.size());
    delete state;
    lv_obj_set_user_data(obj, nullptr);
}

/**
 * @brief XML apply callback for <ui_carousel> widget
 *
 * Parses custom attributes: wrap, auto_scroll_ms, show_indicators, current_page_subject
 * Delegates standard attributes to lv_xml_obj_apply.
 *
 * @param state XML parser state
 * @param attrs XML attributes
 */
void ui_carousel_apply(lv_xml_parser_state_t* state, const char** attrs) {
    // Apply standard object properties first (size, position, style, etc.)
    lv_xml_obj_apply(state, attrs);

    void* item = lv_xml_state_get_item(state);
    lv_obj_t* container = static_cast<lv_obj_t*>(item);
    CarouselState* cstate = ui_carousel_get_state(container);
    if (!cstate) {
        return;
    }

    // Parse wrap attribute (default: true)
    const char* wrap_str = lv_xml_get_value_of(attrs, "wrap");
    if (wrap_str) {
        cstate->wrap = (strcmp(wrap_str, "true") == 0 || strcmp(wrap_str, "1") == 0);
    }

    // Parse auto_scroll_ms attribute (default: 0 = disabled)
    const char* auto_str = lv_xml_get_value_of(attrs, "auto_scroll_ms");
    if (auto_str) {
        cstate->auto_scroll_ms = atoi(auto_str);
    }

    // Parse show_indicators attribute (default: true)
    const char* ind_str = lv_xml_get_value_of(attrs, "show_indicators");
    if (ind_str) {
        cstate->show_indicators = (strcmp(ind_str, "true") == 0 || strcmp(ind_str, "1") == 0);
    }

    // Hide indicator row if indicators are disabled
    if (!cstate->show_indicators && cstate->indicator_row) {
        lv_obj_add_flag(cstate->indicator_row, LV_OBJ_FLAG_HIDDEN);
    }

    // Parse current_page_subject for subject binding
    const char* subject_name = lv_xml_get_value_of(attrs, "current_page_subject");
    if (subject_name && subject_name[0] != '\0') {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, subject_name);
        if (subject) {
            cstate->page_subject = subject;
            spdlog::trace("[ui_carousel] Bound to page subject '{}'", subject_name);
        } else {
            spdlog::warn("[ui_carousel] Subject '{}' not found", subject_name);
        }
    }

    // Start auto-advance timer if configured
    if (cstate->auto_scroll_ms > 0) {
        ui_carousel_start_auto_advance(container);
    }

    spdlog::trace("[ui_carousel] Applied: wrap={} auto_scroll={}ms indicators={}", cstate->wrap,
                  cstate->auto_scroll_ms, cstate->show_indicators);
}

} // namespace

/**
 * @brief Core carousel creation logic shared by XML factory and C++ API
 *
 * Creates a vertical container with:
 * - Horizontal scroll container with snap-to-start behavior (for pages)
 * - Indicator row at the bottom (for page dots)
 *
 * @param parent Parent LVGL object
 * @return Created carousel container object
 */
static lv_obj_t* carousel_create_core(lv_obj_t* parent) {
    // Outer container: column layout holding scroll area + indicators
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(container, 0, LV_PART_MAIN);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);

    // Scroll container: horizontal, full width, grows to fill available space
    lv_obj_t* scroll = lv_obj_create(container);
    lv_obj_set_width(scroll, LV_PCT(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scroll, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scroll, 0, LV_PART_MAIN);
    lv_obj_set_scroll_snap_x(scroll, LV_SCROLL_SNAP_START);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_style_border_width(scroll, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);

    // Indicator row: floating, overlaps content at bottom center (takes zero layout space)
    lv_obj_t* indicator_row = lv_obj_create(container);
    lv_obj_set_size(indicator_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(indicator_row, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(indicator_row, LV_ALIGN_BOTTOM_MID, 0, -theme_manager_get_spacing("space_xxs"));
    lv_obj_set_flex_flow(indicator_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(indicator_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(indicator_row, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);
    lv_obj_remove_flag(indicator_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(indicator_row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator_row, LV_OPA_TRANSP, LV_PART_MAIN);

    // Allocate and store carousel state
    CarouselState* cstate = new CarouselState();
    cstate->scroll_container = scroll;
    cstate->indicator_row = indicator_row;
    lv_obj_set_user_data(container, cstate);

    // Register delete handler for cleanup
    lv_obj_add_event_cb(container, carousel_delete_cb, LV_EVENT_DELETE, nullptr);

    // Register scroll-end handler for page tracking from swipe gestures
    lv_obj_add_event_cb(scroll, carousel_scroll_end_cb, LV_EVENT_SCROLL_END, nullptr);

    // Only a swipe or a goto moves the page; focus never does
    lv_obj_add_event_cb(scroll, carousel_scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, nullptr);

    // Register touch handlers for auto-advance pause/resume
    lv_obj_add_event_cb(scroll, carousel_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(scroll, carousel_release_cb, LV_EVENT_RELEASED, nullptr);

    // Swipe and input flags for the empty carousel
    apply_input_flags(cstate);

    spdlog::trace("[ui_carousel] Created carousel widget");
    return container;
}

static void* ui_carousel_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    return carousel_create_core(parent);
}

void ui_carousel_init() {
    lv_xml_register_widget("ui_carousel", ui_carousel_create, ui_carousel_apply);
    spdlog::trace("[ui_carousel] Registered carousel widget");
}

CarouselState* ui_carousel_get_state(lv_obj_t* obj) {
    if (!obj) {
        return nullptr;
    }
    CarouselState* state = static_cast<CarouselState*>(lv_obj_get_user_data(obj));
    if (!state || state->magic != CarouselState::kMagic) {
        return nullptr;
    }
    return state;
}

void ui_carousel_goto_page(lv_obj_t* carousel, int page, bool animate) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state || !state->scroll_container) {
        return;
    }

    const int count = page_count(*state);
    if (count == 0) {
        return;
    }

    helix::ui::carousel_goto_tile(carousel, wrap_or_clamp(page, count, state->wrap), animate);
}

int ui_carousel_get_current_page(lv_obj_t* carousel) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return 0;
    }
    return state->current_page;
}

int ui_carousel_get_page_count(lv_obj_t* carousel) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return 0;
    }
    return static_cast<int>(state->real_tiles.size());
}

void ui_carousel_add_item(lv_obj_t* carousel, lv_obj_t* item) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state || !state->scroll_container || !item) {
        return;
    }

    // Create a tile container inside the scroll area
    lv_obj_t* tile = lv_obj_create(state->scroll_container);
    lv_obj_set_size(tile, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_SNAPPABLE);

    // Reparent the item into the tile
    lv_obj_set_parent(item, tile);

    // Track the tile
    state->real_tiles.push_back(tile);

    // Rebuild indicator dots to match new page count
    ui_carousel_rebuild_indicators(carousel);

    spdlog::trace("[ui_carousel] Added item, page count now {}", state->real_tiles.size());
}

void ui_carousel_rebuild_indicators(lv_obj_t* carousel) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return;
    }

    apply_input_flags(state);
    if (!state->indicator_row) {
        return;
    }

    // Clear existing dots
    helix::ui::safe_clean_children(state->indicator_row);

    const int count = page_count(*state);

    // Single page: no indicators
    if (count <= 1) {
        lv_obj_add_flag(state->indicator_row, LV_OBJ_FLAG_HIDDEN);
        spdlog::trace("[ui_carousel] Single page — indicators hidden");
        return;
    }

    if (state->show_indicators) {
        lv_obj_remove_flag(state->indicator_row, LV_OBJ_FLAG_HIDDEN);
    }

    // Create one dot per real page
    for (int i = 0; i < count; i++) {
        lv_obj_t* dot = lv_obj_create(state->indicator_row);
        // Strip LVGL's default theme styles so only our explicit styles apply
        lv_obj_remove_style(dot, nullptr, LV_PART_MAIN);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Apply active/inactive styles
    update_indicators(state);

    spdlog::trace("[ui_carousel] Rebuilt indicators ({} dots)", count);
}

void ui_carousel_start_auto_advance(lv_obj_t* carousel) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return;
    }

    // Stop any existing timer first
    if (state->auto_timer) {
        lv_timer_delete(state->auto_timer);
        state->auto_timer = nullptr;
    }

    if (state->auto_scroll_ms <= 0) {
        return;
    }

    state->auto_timer =
        lv_timer_create(auto_advance_cb, static_cast<uint32_t>(state->auto_scroll_ms), carousel);
    spdlog::trace("[ui_carousel] Started auto-advance timer ({}ms)", state->auto_scroll_ms);
}

void ui_carousel_stop_auto_advance(lv_obj_t* carousel) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return;
    }

    if (state->auto_timer) {
        lv_timer_delete(state->auto_timer);
        state->auto_timer = nullptr;
        spdlog::trace("[ui_carousel] Stopped auto-advance timer");
    }
}

lv_obj_t* ui_carousel_create_obj(lv_obj_t* parent) {
    if (!parent) {
        return nullptr;
    }
    return carousel_create_core(parent);
}

void ui_carousel_set_real_page_count(lv_obj_t* carousel, int count) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return;
    }

    state->real_page_count = count;
    ui_carousel_rebuild_indicators(carousel);
    spdlog::trace("[ui_carousel] Set real_page_count to {}", count);
}

void ui_carousel_remove_item(lv_obj_t* carousel, int index) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return;
    }

    if (index < 0 || index >= static_cast<int>(state->real_tiles.size())) {
        spdlog::warn("[ui_carousel] remove_item: index {} out of range (size={})", index,
                     state->real_tiles.size());
        return;
    }

    // Delete the tile LVGL object (and its children)
    lv_obj_t* tile = state->real_tiles[static_cast<size_t>(index)];
    lv_obj_delete(tile);

    // Remove from tracking vector
    state->real_tiles.erase(state->real_tiles.begin() + index);

    // Adjust current_page if it was pointing at or past the removed item
    int new_count = static_cast<int>(state->real_tiles.size());
    if (new_count == 0) {
        state->current_page = 0;
    } else if (state->current_page >= new_count) {
        state->current_page = new_count - 1;
    }

    // Update page subject if changed
    if (state->page_subject) {
        lv_subject_set_int(state->page_subject, state->current_page);
    }

    // Rebuild indicators
    ui_carousel_rebuild_indicators(carousel);

    spdlog::trace("[ui_carousel] Removed item at index {}, page count now {}", index, new_count);
}

namespace helix::ui {

void carousel_set_bubble_events(lv_obj_t* carousel, bool bubble) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return;
    }
    state->bubble_events = bubble;
    lv_obj_update_flag(carousel, LV_OBJ_FLAG_EVENT_BUBBLE, bubble);
    // The scroll container and tiles take the setting through their input flags.
    apply_input_flags(state);
}

void carousel_set_swipe(lv_obj_t* carousel, CarouselSwipe policy) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state) {
        return;
    }
    state->swipe = policy;
    // Flags only: edit gestures change the policy inside input dispatch.
    apply_input_flags(state);
}

void carousel_goto_tile(lv_obj_t* carousel, int tile, bool animate) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state || !state->scroll_container) {
        return;
    }

    const int count = reachable_tile_count(*state);
    if (count == 0) {
        return;
    }
    tile = wrap_or_clamp(tile, count, state->wrap);

    const int32_t container_w = lv_obj_get_content_width(state->scroll_container);
    // The SCROLL_END this scroll provokes carries no page: see carousel_scroll_end_cb.
    state->goto_scrolling = true;
    lv_obj_scroll_to_x(state->scroll_container, tile * container_w,
                       animate ? LV_ANIM_ON : LV_ANIM_OFF);
    state->goto_scrolling = false;
    state->current_page = tile;

    if (state->page_subject) {
        lv_subject_set_int(state->page_subject, tile);
    }
    update_indicators(state);

    spdlog::trace("[ui_carousel] Navigated to tile {}/{}", tile, count);
}

void carousel_set_trailing_tiles_reachable(lv_obj_t* carousel, bool reachable) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state || state->trailing_tiles_reachable == reachable) {
        return;
    }
    state->trailing_tiles_reachable = reachable;
    // Flags only. A slide under way runs on: LVGL readjusts no scroll offset
    // along an axis that snaps.
    apply_input_flags(state);
}

} // namespace helix::ui
