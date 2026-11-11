// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

#include <vector>

/**
 * @file ui_carousel.h
 * @brief General-purpose carousel widget using horizontal scroll-snap
 *
 * Provides a <ui_carousel> XML widget with:
 * - Horizontal scrollable container with snap-to-page behavior
 * - Page indicator dots
 * - Optional auto-scroll timer
 * - Optional wrap-around
 * - Subject binding for current page
 * - A page that only a swipe or a goto moves: a control that takes focus never
 *   scrolls the carousel to its page
 *
 * Usage in XML:
 *   <ui_carousel wrap="true" auto_scroll_ms="5000" show_indicators="true">
 *     <lv_obj>Page 1 content</lv_obj>
 *     <lv_obj>Page 2 content</lv_obj>
 *   </ui_carousel>
 */

namespace helix::ui {

/**
 * @brief When a carousel's pages respond to a horizontal swipe
 */
enum class CarouselSwipe {
    Auto,     ///< Swipe when the carousel has more than one page
    Disabled, ///< Never swipe
};

} // namespace helix::ui

struct CarouselState {
    static constexpr uint32_t kMagic = 0x43415231; // "CAR1"
    uint32_t magic{kMagic};
    lv_obj_t* scroll_container = nullptr;
    lv_obj_t* indicator_row = nullptr;
    std::vector<lv_obj_t*> real_tiles;
    lv_subject_t* page_subject = nullptr;
    lv_timer_t* auto_timer = nullptr;
    int current_page = 0;
    int auto_scroll_ms = 0;
    int real_page_count = -1; ///< When >= 0, limits indicator dots and page clamping
    bool wrap = true;
    bool show_indicators = true;
    bool user_touching = false;
    bool bubble_events = false; ///< Pages' events continue to the carousel's parent
    helix::ui::CarouselSwipe swipe = helix::ui::CarouselSwipe::Auto; ///< Applied at every rebuild
    bool goto_scrolling = false; ///< A goto is starting its scroll and sets the page itself
    bool trailing_tiles_reachable = true; ///< Tiles past real_page_count can be reached
};

/**
 * @brief Initialize the ui_carousel custom widget
 *
 * Registers the <ui_carousel> XML widget with LVGL's XML parser.
 * Must be called after lv_xml_init() and before any XML using this widget.
 */
void ui_carousel_init();

/**
 * @brief Get the CarouselState for a carousel object
 * @param obj The carousel container object
 * @return Pointer to CarouselState, or nullptr if obj is not a carousel
 */
CarouselState* ui_carousel_get_state(lv_obj_t* obj);

/**
 * @brief Navigate to a specific page
 *
 * Wraps or clamps @p page to the page count (real_page_count when set), then
 * navigates as helix::ui::carousel_goto_tile does.
 *
 * @param carousel The carousel container object
 * @param page Zero-based page index
 * @param animate Whether to animate the transition
 */
void ui_carousel_goto_page(lv_obj_t* carousel, int page, bool animate = true);

/**
 * @brief Get the currently visible page index
 * @param carousel The carousel container object
 * @return Zero-based page index, or 0 if not a valid carousel
 */
int ui_carousel_get_current_page(lv_obj_t* carousel);

/**
 * @brief Get the number of tiles (real_page_count does not change it)
 * @param carousel The carousel container object
 * @return Number of tiles, or 0 if not a valid carousel
 */
int ui_carousel_get_page_count(lv_obj_t* carousel);

/**
 * @brief Add a child item as a new page in the carousel
 * @param carousel The carousel container object
 * @param item The widget to add as a page (will be reparented into a tile)
 */
void ui_carousel_add_item(lv_obj_t* carousel, lv_obj_t* item);

/**
 * @brief Apply the input flags for the current page count, then rebuild the indicator dots
 *
 * Writes the scroll container's swipe (LV_OBJ_FLAG_SCROLLABLE and scroll
 * direction, from the swipe policy), LV_OBJ_FLAG_CLICKABLE and
 * LV_OBJ_FLAG_EVENT_BUBBLE on the scroll container and every tile, and
 * LV_OBJ_FLAG_HIDDEN on the tiles out of reach (see
 * helix::ui::carousel_set_trailing_tiles_reachable), then recreates the dots.
 * The flags are written even when the carousel has no indicator row.
 *
 * @param carousel The carousel container object
 */
void ui_carousel_rebuild_indicators(lv_obj_t* carousel);

/**
 * @brief Start auto-advancing the carousel on a timer
 *
 * Uses auto_scroll_ms from CarouselState for the interval.
 * Stops any existing timer first. No-op if auto_scroll_ms <= 0.
 *
 * @param carousel The carousel container object
 */
void ui_carousel_start_auto_advance(lv_obj_t* carousel);

/**
 * @brief Stop the auto-advance timer
 * @param carousel The carousel container object
 */
void ui_carousel_stop_auto_advance(lv_obj_t* carousel);

/**
 * @brief Create a carousel programmatically (not via XML)
 *
 * Creates the same structure as the XML factory: outer container with
 * scroll container and indicator row. Use this from C++ code when
 * creating carousels without XML.
 *
 * @param parent Parent LVGL object
 * @return The carousel container object
 */
lv_obj_t* ui_carousel_create_obj(lv_obj_t* parent);

/**
 * @brief Set the real page count for indicator display
 *
 * When set to >= 0, indicator dots and page clamping use this count
 * instead of real_tiles.size(). Useful when the carousel manages
 * virtual pages that don't map 1:1 to tiles.
 *
 * @param carousel The carousel container object
 * @param count Number of real pages, or -1 to use tile count
 */
void ui_carousel_set_real_page_count(lv_obj_t* carousel, int count);

/**
 * @brief Remove an item (tile) from the carousel by index
 *
 * Deletes the tile's LVGL object and removes it from real_tiles.
 * Adjusts current_page if it was pointing at or past the removed item.
 * Rebuilds indicators after removal.
 *
 * @param carousel The carousel container object
 * @param index Zero-based index of the tile to remove
 */
void ui_carousel_remove_item(lv_obj_t* carousel, int index);

namespace helix::ui {

/**
 * @brief Route the pages' input events on to the carousel's parent
 *
 * Sets LV_OBJ_FLAG_EVENT_BUBBLE on the carousel, its scroll container and
 * every tile, and keeps it through later add_item, remove_item and
 * set_real_page_count calls. For a parent that handles its pages' input
 * itself. The default (false) lets a multi-page carousel's tiles keep input
 * to themselves; a single-page carousel always passes it through. Writes
 * input flags only; the indicator dots are untouched.
 *
 * @param carousel The carousel container object
 * @param bubble Whether page events should reach the carousel's parent
 */
void carousel_set_bubble_events(lv_obj_t* carousel, bool bubble);

/**
 * @brief Set when the carousel's pages respond to a horizontal swipe
 *
 * Stores the policy and writes the scroll container's LV_OBJ_FLAG_SCROLLABLE
 * and scroll direction from it; add_item, remove_item and set_real_page_count
 * apply it again at each page count. Writes input flags only, leaving the
 * indicator dots untouched, so it is safe inside input dispatch. Which
 * objects capture input still follows the page count.
 *
 * @param carousel The carousel container object
 * @param policy Auto (the default) swipes when there is more than one page
 */
void carousel_set_swipe(lv_obj_t* carousel, CarouselSwipe policy);

/**
 * @brief Navigate to a tile, including tiles past real_page_count while they are reachable
 *
 * Wraps or clamps @p tile to the tiles within reach, scrolls to it, and sets
 * the current page, the page subject and the indicator dots to it. The
 * SCROLL_END of a scroll animation this navigation replaces leaves the page
 * alone.
 *
 * @param carousel The carousel container object
 * @param tile Zero-based tile index
 * @param animate Whether to animate the transition
 */
void carousel_goto_tile(lv_obj_t* carousel, int tile, bool animate = true);

/**
 * @brief Set whether the tiles past real_page_count can be reached
 *
 * Out of reach, those tiles are hidden, so nothing reaches them: a swipe ends
 * at the last page, carousel_goto_tile() stops there, and nothing on them takes
 * a press. Writes flags only, creating, deleting and scrolling nothing, so it is
 * safe inside input dispatch and a slide under way runs on; the carousel keeps
 * the setting through later page-count changes. The carousel does not move
 * itself: a caller takes it off those tiles before they go out of reach.
 * Reachable by default.
 *
 * @param carousel The carousel container object
 * @param reachable Whether the tiles past real_page_count can be reached
 */
void carousel_set_trailing_tiles_reachable(lv_obj_t* carousel, bool reachable);

} // namespace helix::ui
