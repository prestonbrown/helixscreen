// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The gcode viewer's touch input: press, drag-to-rotate, release-as-tap, the
// long-press timer, two-finger pan/zoom, and the object pick they share. A tap
// or long press names the object under the finger through the registered
// callbacks; the exclude-object UI is the consumer.

#if HELIX_HAS_GCODE_VIEWER

#include "ui_gcode_viewer.h"

#include "app_constants.h"
#include "gcode_viewer_state.h"
#include "view_gestures.h"

#include <spdlog/spdlog.h>

#include <glm/glm.hpp>
#include <optional>
#include <string>

using namespace helix;
using namespace helix::gcode_viewer;

constexpr uint32_t DRAG_THROTTLE_MIN_FRAME_MS = 33; // ~30fps throttle during drag
constexpr int CLICK_DISTANCE_THRESHOLD = 10;        // Pixels: distinguish click from drag

// Helper: Check if viewer has any G-code data (full file or streaming)
static bool has_gcode_data(const gcode_viewer_state_t* st) {
    return st->gcode_file || (st->streaming_controller_ && st->streaming_controller_->is_open());
}

static const char* ui_gcode_viewer_pick_object(lv_obj_t* obj, int x, int y);

// Long-press threshold in milliseconds. Deliberately longer than the app-wide
// gesture timeout (AppConstants::Input::LONG_PRESS_MS, 500ms): a hold here fires
// EXCLUDE_OBJECT and cancels printing the object under the finger, so the gesture
// is tuned to demand a deliberate hold and resist accidental cancels.
constexpr uint32_t LONG_PRESS_THRESHOLD_MS = 1000;

/**
 * @brief Timer callback for long-press detection
 *
 * Fires after LONG_PRESS_THRESHOLD_MS if user hasn't moved the finger.
 * Picks the object under the initial press position and invokes the long-press callback.
 */
static void long_press_timer_cb(lv_timer_t* timer) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !has_gcode_data(st))
        return;

    // Timer fired - this is a long-press
    st->long_press_fired = true;

    // Delete the timer (one-shot)
    lv_timer_delete(timer);
    st->long_press_timer_ = nullptr;

    // Pick object at the original press position
    const char* picked = ui_gcode_viewer_pick_object(obj, st->drag_start.x, st->drag_start.y);

    if (picked && picked[0] != '\0') {
        st->long_press_object_name = picked;

        // Highlight the object to provide visual feedback
        st->selected_objects.clear();
        st->selected_objects.insert(picked);
        ui_gcode_viewer_set_highlighted_objects(obj, st->selected_objects);

        spdlog::info("[GCode Viewer] Long-press on object '{}'", picked);

        // Invoke long-press callback
        if (st->object_long_press_callback) {
            st->object_long_press_callback(obj, picked, st->object_long_press_user_data);
        }
    } else {
        st->long_press_object_name.clear();
        spdlog::debug("[GCode Viewer] Long-press at ({}, {}) - no object found", st->drag_start.x,
                      st->drag_start.y);

        // Invoke callback with empty string to indicate long-press on empty space
        if (st->object_long_press_callback) {
            st->object_long_press_callback(obj, "", st->object_long_press_user_data);
        }
    }
}

/**
 * @brief Touch press callback - start drag gesture and long-press timer
 */
static void gcode_viewer_press_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Fresh interaction (all fingers were up): clear per-sequence gesture state.
    // The two-finger latch clears here and nowhere else, so it holds rotate, tap
    // and long-press off until every finger has lifted, even when both fingers
    // lift in one input poll and no ENDED event is delivered.
    if (!st->is_dragging) {
        st->gesture_moved = false;
#if LV_USE_GESTURE_RECOGNITION
        st->two_finger = {};
        st->two_finger_occurred = false;
#endif
    }

    st->is_dragging = true;
    st->drag_start = point;
    st->last_drag_pos = point;
    st->long_press_fired = false;
    st->long_press_object_name.clear();

    spdlog::trace("[GCode Viewer] PRESSED at ({}, {}), is_dragging={}", point.x, point.y,
                  st->is_dragging);

    // Enter interaction mode for reduced resolution during drag
#ifdef ENABLE_3D_RENDERER
    if (st->renderer_) {
        st->renderer_->set_interaction_mode(true);
    }
#endif

    // Start long-press timer if callback is registered
    if (st->object_long_press_callback && has_gcode_data(st)) {
        // Cancel any existing timer
        if (st->long_press_timer_) {
            lv_timer_delete(st->long_press_timer_);
            st->long_press_timer_ = nullptr;
        }
        // Start new timer for long-press detection
        st->long_press_timer_ = lv_timer_create(long_press_timer_cb, LONG_PRESS_THRESHOLD_MS, obj);
        if (st->long_press_timer_) {
            lv_timer_set_repeat_count(st->long_press_timer_, 1); // One-shot timer
        }
    }

    spdlog::trace("[GCode Viewer] Press at ({}, {})", point.x, point.y);
}

// Movement threshold to cancel long-press (same as click threshold)
constexpr int LONG_PRESS_MOVE_THRESHOLD = 10;

/**
 * @brief Touch pressing callback - handle drag for camera rotation
 *
 * Also cancels long-press timer if user moves beyond threshold.
 */
static void gcode_viewer_pressing_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !st->is_dragging)
        return;

    // In 2D mode, no camera rotation - skip drag handling entirely.
    // This also prevents mouse micro-jitter from cancelling the long-press timer.
    if (st->is_using_2d_mode())
        return;

#if LV_USE_GESTURE_RECOGNITION
    // No rotation from the first two-finger frame until every finger lifts: a finger
    // left down after a pinch would otherwise jump the camera from a stale position.
    if (st->two_finger_occurred) {
        return;
    }
#endif

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Check if movement exceeds threshold - cancel long-press timer
    int total_dx = abs(point.x - st->drag_start.x);
    int total_dy = abs(point.y - st->drag_start.y);

    // Latch "moved" once movement exceeds the tap threshold anywhere in the
    // sequence. Unlike the net displacement checked at release, this catches
    // rotate-and-return motions that settle back near the start point and must
    // NOT be treated as an object tap.
    if (total_dx >= CLICK_DISTANCE_THRESHOLD || total_dy >= CLICK_DISTANCE_THRESHOLD) {
        st->gesture_moved = true;
    }

    if ((total_dx >= LONG_PRESS_MOVE_THRESHOLD || total_dy >= LONG_PRESS_MOVE_THRESHOLD) &&
        st->long_press_timer_) {
        // User started dragging - cancel long-press
        lv_timer_delete(st->long_press_timer_);
        st->long_press_timer_ = nullptr;
        spdlog::trace("[GCode Viewer] Long-press cancelled due to movement");
    }

    // Calculate delta from last position
    int dx = point.x - st->last_drag_pos.x;
    int dy = point.y - st->last_drag_pos.y;

    if (dx != 0 || dy != 0) {
        // Convert pixel movement to rotation angles (~0.5 degrees per pixel)
        // Azimuth: drag right = orbit right
        // Elevation: drag up = tilt up (screen Y is inverted, so positive dy = down)
        float delta_azimuth = dx * helix::ui::kRotateDegreesPerPixel;
        float delta_elevation = dy * helix::ui::kRotateDegreesPerPixel;

        st->camera_->rotate(delta_azimuth, delta_elevation);

        // Throttled invalidation - limit to ~30fps during drag to reduce CPU load
        // Final frame is always rendered on RELEASED event
        static uint32_t last_invalidate_ms = 0;
        uint32_t now_ms = lv_tick_get();
        if (now_ms - last_invalidate_ms >= DRAG_THROTTLE_MIN_FRAME_MS) {
            lv_obj_invalidate(obj);
            last_invalidate_ms = now_ms;
        }

        st->last_drag_pos = point;

        spdlog::trace("[GCode Viewer] Drag ({}, {}) -> rotate({:.1f}, {:.1f})", dx, dy,
                      delta_azimuth, delta_elevation);
    }
}

/**
 * @brief Touch release callback - handle click vs drag gesture
 *
 * Skips tap handling if long-press already fired (user held for 500ms+).
 */
static void gcode_viewer_release_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    // Cancel long-press timer if still pending
    if (st->long_press_timer_) {
        lv_timer_delete(st->long_press_timer_);
        st->long_press_timer_ = nullptr;
    }

    // Get release position
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        st->is_dragging = false;
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Calculate total drag distance from initial press
    int dx = abs(point.x - st->drag_start.x);
    int dy = abs(point.y - st->drag_start.y);

    const int CLICK_THRESHOLD = CLICK_DISTANCE_THRESHOLD;

    // Skip tap handling if long-press already fired
    if (st->long_press_fired) {
        spdlog::trace("[GCode Viewer] Release after long-press - skipping tap handling");
        st->is_dragging = false;
        st->long_press_fired = false;
        return;
    }

    bool two_finger = false;
#if LV_USE_GESTURE_RECOGNITION
    // A two-finger gesture that ends as a one-finger lift lands as a low-movement
    // release; it must not read as an object tap.
    two_finger = st->two_finger_occurred;
#endif

    // If movement was minimal, treat as click and try to pick object.
    // gesture_moved guards against rotate-and-return motions whose net
    // displacement is small but which clearly manipulated the view.
    if (!two_finger && !st->gesture_moved && dx < CLICK_THRESHOLD && dy < CLICK_THRESHOLD &&
        has_gcode_data(st)) {
        spdlog::debug("[GCode Viewer] Click detected at ({}, {})", point.x, point.y);
        const char* picked = ui_gcode_viewer_pick_object(obj, point.x, point.y);

        if (picked && picked[0] != '\0') {
            // Object clicked - toggle selection (single-select)
            std::string picked_name(picked);

            if (st->selected_objects.count(picked_name) > 0) {
                // Already selected - deselect
                st->selected_objects.clear();
                spdlog::info("[GCode Viewer] Deselected object '{}'", picked_name);
            } else {
                // Select this object (replacing any previous selection)
                st->selected_objects.clear();
                st->selected_objects.insert(picked_name);
                spdlog::info("[GCode Viewer] Selected object '{}'", picked_name);
            }

            // Update highlighting to show all selected objects
            ui_gcode_viewer_set_highlighted_objects(obj, st->selected_objects);

            // Invoke tap callback if registered (for exclude object UI)
            if (st->object_tap_callback) {
                st->object_tap_callback(obj, picked, st->object_tap_user_data);
            }
        } else {
            spdlog::debug("[GCode Viewer] Click at ({}, {}) - no object found (G-code may lack "
                          "EXCLUDE_OBJECT metadata)",
                          point.x, point.y);
            // Still invoke callback with empty string to indicate click on empty space
            if (st->object_tap_callback) {
                st->object_tap_callback(obj, "", st->object_tap_user_data);
            }
        }
        // Note: If no object picked, keep current selection (per user requirements)
    }

    st->is_dragging = false;

    // Exit interaction mode to restore full resolution for final frame
#ifdef ENABLE_3D_RENDERER
    if (st->renderer_) {
        st->renderer_->set_interaction_mode(false);
    }
#endif

    // Always render final frame on release to ensure camera settles at correct position
    // (throttling during drag may have skipped the last frame)
    lv_obj_invalidate(obj);

    spdlog::trace("[GCode Viewer] Release at ({}, {}), drag=({}, {})", point.x, point.y, dx, dy);
}

#if LV_USE_GESTURE_RECOGNITION
/**
 * @brief Two-finger pan and pinch zoom
 *
 * Motion is 3D-only; the two-finger latch that holds off tap and long-press
 * applies in both modes. A pinch zooms about the fingers and pans with them
 * in the same frame; a two-finger swipe only pans. Whichever LVGL recognizes
 * first owns the touch until a finger lifts.
 */
static void gcode_viewer_gesture_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    const auto sample = helix::ui::read_two_finger_sample(e);
    if (!sample)
        return;

    const helix::ui::TwoFingerStep step = helix::ui::two_finger_step(*sample, st->two_finger);
    if (!step.active)
        return;

    if (!st->two_finger_occurred) {
        st->two_finger_occurred = true;
        if (st->long_press_timer_) {
            lv_timer_delete(st->long_press_timer_);
            st->long_press_timer_ = nullptr;
        }
    }

    // Pan and zoom move the 3D camera only; the latch above applies in both modes.
    if (st->is_using_2d_mode())
        return;

    if (step.pan_dx == 0.0f && step.pan_dy == 0.0f && step.zoom == 1.0f)
        return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    st->camera_->pan_pixels(step.pan_dx, step.pan_dy);
    st->camera_->zoom_at(step.zoom, static_cast<float>(step.anchor_x - coords.x1),
                         static_cast<float>(step.anchor_y - coords.y1));
    lv_obj_invalidate(obj);
}
#endif

static const char* ui_gcode_viewer_pick_object(lv_obj_t* obj, int x, int y) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !has_gcode_data(st))
        return nullptr;

    // Convert screen coordinates to widget-local coordinates
    lv_area_t widget_coords;
    lv_obj_get_coords(obj, &widget_coords);
    int local_x = x - widget_coords.x1;
    int local_y = y - widget_coords.y1;

    spdlog::debug("[GCode Viewer] pick_object screen=({}, {}), widget_pos=({}, {}), local=({}, {})",
                  x, y, widget_coords.x1, widget_coords.y1, local_x, local_y);

    std::optional<std::string> result;

    // Use 2D renderer's pick_object_at in 2D mode
    if (st->is_using_2d_mode() && st->layer_renderer_2d_) {
        result = st->layer_renderer_2d_->pick_object_at(local_x, local_y);
    }
#ifdef ENABLE_3D_RENDERER
    else if (st->renderer_ && st->gcode_file) {
        // 3D renderer path (requires full gcode file)
        result =
            st->renderer_->pick_object(glm::vec2(local_x, local_y), *st->gcode_file, *st->camera_);
    }
#endif

    if (result) {
        // Store in static buffer (safe for single-threaded LVGL)
        static std::string picked_name;
        picked_name = *result;
        return picked_name.c_str();
    }

    return nullptr;
}

void helix::gcode_viewer::install_input_handlers(lv_obj_t* obj) {
    lv_obj_add_event_cb(obj, gcode_viewer_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_pressing_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_release_cb, LV_EVENT_RELEASED, nullptr);
#if LV_USE_GESTURE_RECOGNITION
    lv_obj_add_event_cb(obj, gcode_viewer_gesture_cb, LV_EVENT_GESTURE, nullptr);
#endif
}

#endif // HELIX_HAS_GCODE_VIEWER
