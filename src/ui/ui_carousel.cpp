// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_carousel.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace {

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
    if (!state || state->magic != CarouselState::MAGIC) {
        return;
    }

    int32_t container_w = lv_obj_get_content_width(scroll);
    if (container_w <= 0) {
        return;
    }

    int32_t scroll_x = lv_obj_get_scroll_x(scroll);
    int page = static_cast<int>(scroll_x / container_w);

    int count = static_cast<int>(state->real_tiles.size());
    if (count > 0) {
        // Apply same wrap/clamp logic as goto_page
        if (state->wrap) {
            page = ((page % count) + count) % count;
        } else {
            if (page < 0) {
                page = 0;
            }
            if (page >= count) {
                page = count - 1;
            }
        }
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
    if (!state || state->magic != CarouselState::MAGIC) {
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
    if (!state || state->magic != CarouselState::MAGIC) {
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
    if (!state || state->magic != CarouselState::MAGIC) {
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
 * @brief XML create callback for <ui_carousel> widget
 *
 * Creates a vertical container with:
 * - Horizontal scroll container with snap-to-start behavior (for pages)
 * - Indicator row at the bottom (for page dots)
 *
 * @param state XML parser state
 * @param attrs XML attributes
 * @return Created carousel container object
 */
void* ui_carousel_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));

    // Outer container: column layout holding scroll area + indicators
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(container, 4, LV_PART_MAIN);
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
    lv_obj_set_scroll_dir(scroll, LV_DIR_HOR);
    lv_obj_set_style_border_width(scroll, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);

    // Indicator row: centered dots at bottom
    lv_obj_t* indicator_row = lv_obj_create(container);
    lv_obj_set_size(indicator_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(indicator_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(indicator_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(indicator_row, 6, LV_PART_MAIN);
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

    // Register touch handlers for auto-advance pause/resume
    lv_obj_add_event_cb(scroll, carousel_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(scroll, carousel_release_cb, LV_EVENT_RELEASED, nullptr);

    spdlog::trace("[ui_carousel] Created carousel widget");
    return container;
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

void ui_carousel_init() {
    lv_xml_register_widget("ui_carousel", ui_carousel_create, ui_carousel_apply);
    spdlog::trace("[ui_carousel] Registered carousel widget");
}

CarouselState* ui_carousel_get_state(lv_obj_t* obj) {
    if (!obj) {
        return nullptr;
    }
    CarouselState* state = static_cast<CarouselState*>(lv_obj_get_user_data(obj));
    if (!state || state->magic != CarouselState::MAGIC) {
        return nullptr;
    }
    return state;
}

void ui_carousel_goto_page(lv_obj_t* carousel, int page, bool animate) {
    CarouselState* state = ui_carousel_get_state(carousel);
    if (!state || !state->scroll_container) {
        return;
    }

    int count = static_cast<int>(state->real_tiles.size());
    if (count == 0) {
        return;
    }

    // Handle out-of-range pages: wrap or clamp
    if (state->wrap) {
        page = ((page % count) + count) % count;
    } else {
        if (page < 0) {
            page = 0;
        }
        if (page >= count) {
            page = count - 1;
        }
    }

    // Calculate scroll position based on page width
    int32_t container_w = lv_obj_get_content_width(state->scroll_container);
    int32_t scroll_x = page * container_w;

    lv_obj_scroll_to_x(state->scroll_container, scroll_x, animate ? LV_ANIM_ON : LV_ANIM_OFF);
    state->current_page = page;

    // Update page subject if bound
    if (state->page_subject) {
        lv_subject_set_int(state->page_subject, page);
    }

    // Update indicator dot styles
    update_indicators(state);

    spdlog::trace("[ui_carousel] Navigated to page {}/{}", page, count);
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
    if (!state || !state->indicator_row) {
        return;
    }

    // Clear existing dots
    lv_obj_clean(state->indicator_row);

    int count = static_cast<int>(state->real_tiles.size());

    // Single page: hide indicators, disable swiping, allow click passthrough
    if (count <= 1) {
        lv_obj_add_flag(state->indicator_row, LV_OBJ_FLAG_HIDDEN);
        if (state->scroll_container) {
            lv_obj_set_scroll_dir(state->scroll_container, LV_DIR_NONE);
            lv_obj_remove_flag(state->scroll_container, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(state->scroll_container, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        for (auto* tile : state->real_tiles) {
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        spdlog::trace("[ui_carousel] Single page — indicators hidden, scroll disabled");
        return;
    }

    // Multiple pages: show indicators, enable horizontal scrolling, capture clicks
    if (state->show_indicators) {
        lv_obj_remove_flag(state->indicator_row, LV_OBJ_FLAG_HIDDEN);
    }
    if (state->scroll_container) {
        lv_obj_set_scroll_dir(state->scroll_container, LV_DIR_HOR);
        lv_obj_add_flag(state->scroll_container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(state->scroll_container, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    for (auto* tile : state->real_tiles) {
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
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
