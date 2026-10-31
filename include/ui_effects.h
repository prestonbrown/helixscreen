// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace helix::ui {

/**
 * @brief Create a ripple effect animation at the specified position
 *
 * Creates a circular ripple that expands and fades out, providing visual
 * feedback for touch events. The ripple uses the primary color and respects
 * the user's animation settings (disabled if animations are off).
 *
 * The ripple is automatically deleted when the animation completes.
 *
 * @param parent Parent container for the ripple (touch position is relative to this)
 * @param x X coordinate relative to parent (touch point)
 * @param y Y coordinate relative to parent (touch point)
 * @param start_size Initial diameter in pixels (default: 20)
 * @param end_size Final diameter in pixels (default: 120)
 * @param duration_ms Animation duration in milliseconds (default: 400)
 */
void create_ripple(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, int start_size = 20,
                   int end_size = 120, int32_t duration_ms = 400);

/**
 * @brief Draw a touch marker: an expanding ripple plus a lingering center dot
 *
 * Same visual feedback as create_ripple(), but additionally leaves a solid dot
 * at the touch point that holds fully visible for a beat, then fades out and
 * self-deletes. The dot outlives the ripple so a press stays readable long
 * enough to judge where it landed — the self-diagnosis aid for touch
 * calibration accuracy (prestonbrown/helixscreen#1082).
 *
 * Both the ripple and the dot are centered on the SAME (x, y) from this single
 * call, so they can never drift into separate codepaths or positions. Like
 * create_ripple(), everything is self-cleaning — no caller bookkeeping.
 *
 * @param parent Parent container (touch position is relative to this; use
 *               lv_layer_top() for screen-absolute coordinates)
 * @param x X coordinate relative to parent (touch point)
 * @param y Y coordinate relative to parent (touch point)
 */
void create_touch_marker(lv_obj_t* parent, lv_coord_t x, lv_coord_t y);

/**
 * @brief Create a fullscreen backdrop for modals and overlays
 *
 * Creates a fullscreen object that covers the parent with a semi-transparent
 * black background. Used by Modal and BusyOverlay for dimming content behind
 * dialogs and blocking input to underlying UI.
 *
 * The backdrop is configured with:
 * - 100% width and height, centered alignment
 * - Black background with specified opacity
 * - No border, radius, or padding
 * - Clickable flag set (to capture/block input)
 * - Scrollable flag removed
 *
 * @param parent Parent object (typically lv_screen_active() or lv_layer_top())
 * @param opacity Background opacity (0-255, default 180 = ~70%)
 * @return Newly created backdrop object, or nullptr on failure
 */
lv_obj_t* create_fullscreen_backdrop(lv_obj_t* parent, lv_opa_t opacity = 180);

/**
 * @brief Register a screen object that stays in front of every backdrop
 *
 * The navbar's buttons (the E-stop, the spools-on-the-bed button):
 * they have to stay tappable above any overlay or keypad backdrop. A modal
 * covers them (set_chrome_covered()).
 *
 * @param raise How to bring @p obj to the front when the chrome is raised;
 *        empty moves it to the foreground. A navbar button uses it to stay
 *        under a keyboard that covers its slot.
 */
void add_always_on_top(lv_obj_t* obj, std::function<void()> raise = {});

/// Unregister @p obj; unknown objects are ignored.
void remove_always_on_top(lv_obj_t* obj);

/**
 * @brief While covered, always-on-top objects stay under whatever is raised
 *
 * ModalStack covers the chrome while any modal is open: a modal blocks the
 * navbar, E-stop included, so a tap meant for the backdrop cannot land on it.
 * Uncovering raises the chrome back to the front.
 */
void set_chrome_covered(bool covered);

/// True while a modal holds the chrome under its backdrop.
bool chrome_covered();

/**
 * @brief Hides every always-on-top object while a screen snapshot is taken,
 *        so a dimmed copy is not baked into the image
 */
class ScopedHideChrome {
  public:
    ScopedHideChrome();
    ~ScopedHideChrome();
    ScopedHideChrome(const ScopedHideChrome&) = delete;
    ScopedHideChrome& operator=(const ScopedHideChrome&) = delete;

  private:
    std::vector<WeakWidget> hidden_;
};

/**
 * @brief Bring @p obj to the front of its parent, keeping the always-on-top
 *        objects in front of it
 *
 * Every screen-level backdrop goes up through this, so none of them can bury
 * the E-stop. An object that must cover the E-stop (the lock screen, which
 * lives on lv_layer_top) does not need it.
 */
void bring_to_front(lv_obj_t* obj);

/// Whether @p child is screen chrome that sweeps over the screen's children
/// (stale-overlay hiding, the keyboard's page shift) must leave alone.
bool is_screen_chrome(const lv_obj_t* child);

/**
 * @brief Flash an object with a brief opacity pulse for touch feedback
 *
 * Animates the object's opacity down and back up to provide visual confirmation
 * of a touch event. Respects animation settings (no-op if animations disabled)
 * unless force=true.
 *
 * @param obj Object to flash (typically a crosshair or icon)
 * @param duration_ms Total pulse duration in milliseconds (default: 200)
 * @param force If true, bypass animation-enabled check (for essential feedback like calibration)
 */
void flash_object(lv_obj_t* obj, int32_t duration_ms = 200, bool force = false);

/**
 * @brief Recursively remove an object tree from the default focus group
 *
 * Prevents LVGL from auto-focusing the next element when focusable children
 * (buttons, textareas, etc.) are deleted, which triggers scroll-on-focus.
 * Safe to call on objects not in any group (no-op).
 *
 * Called automatically by helix::ui::safe_delete() - manual use only needed
 * when removing objects from the group without deleting them.
 *
 * @param obj Root object of the tree to defocus
 */
void defocus_tree(lv_obj_t* obj);

/// helix::full_style_effects_allowed() for the running tier, read from the
/// "platform_tier" subject; true until that subject is registered.
bool full_style_effects_active();

} // namespace helix::ui
