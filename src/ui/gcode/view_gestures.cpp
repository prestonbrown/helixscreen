// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "view_gestures.h"

#if LV_USE_GESTURE_RECOGNITION
#include "lvgl/src/indev/lv_indev_gesture_private.h" // info->delta_x/y, center: no public getter
#include "lvgl/src/indev/lv_indev_private.h" // gesture_data[]: lv_indev_get_gesture_recognizer is static in LVGL
#include "touch_calibration_wrapper.h"
#endif

#include <cmath>

namespace helix::ui {

namespace {
// Normal per-frame pinch ratios sit near 0.85-1.15; anything outside this
// open interval is a recognizer restart, not finger motion. The bounds apply
// from the second Recognized frame on: the first catches up the spread made
// before recognition, which a fast pinch can push past any bound.
constexpr float kMinFrameZoom = 0.7f;
constexpr float kMaxFrameZoom = 1.4f;
} // namespace

TwoFingerStep two_finger_step(const TwoFingerSample& s, TwoFingerState& st) {
    TwoFingerStep out;
    switch (s.phase) {
    case TwoFingerSample::Phase::Ended:
        st = {};
        out.ended = true;
        return out;
    case TwoFingerSample::Phase::Ongoing:
        // Every gesture passes through ONGOING, so totals from a gesture whose
        // ENDED was never delivered cannot leak into this one.
        st = {};
        out.active = true;
        return out;
    case TwoFingerSample::Phase::Recognized:
        break;
    }

    out.active = true;
    out.pan_dx = s.delta_x - st.last_dx;
    out.pan_dy = s.delta_y - st.last_dy;
    st.last_dx = s.delta_x;
    st.last_dy = s.delta_y;

    if (s.kind == TwoFingerSample::Kind::Pinch && s.scale > 0.0f) {
        const float ratio = s.scale / st.last_scale;
        if (!st.recognized || (ratio > kMinFrameZoom && ratio < kMaxFrameZoom)) {
            out.zoom = ratio;
        }
        st.last_scale = s.scale;
    }
    st.recognized = true;

    out.anchor_x = s.start_x + static_cast<int>(std::lround(s.delta_x));
    out.anchor_y = s.start_y + static_cast<int>(std::lround(s.delta_y));
    return out;
}

#if LV_USE_GESTURE_RECOGNITION
namespace {
// LVGL keeps its own event-to-recognizer lookup static inside lv_indev_gesture.c;
// gesture events carry the emitting indev as their param.
lv_indev_gesture_recognizer_t* gesture_recognizer(lv_event_t* e, lv_indev_gesture_type_t type) {
    if (!e) {
        return nullptr;
    }
    auto* indev = static_cast<lv_indev_t*>(lv_event_get_param(e));
    if (!indev || !indev->gesture_data[type]) {
        return nullptr;
    }
    return static_cast<lv_indev_gesture_recognizer_t*>(indev->gesture_data[type]);
}
} // namespace

std::optional<TwoFingerSample> read_two_finger_sample(lv_event_t* e) {
    lv_indev_gesture_type_t type = lv_event_get_gesture_type(e);
    if (type != LV_INDEV_GESTURE_PINCH && type != LV_INDEV_GESTURE_TWO_FINGERS_SWIPE) {
        // Nothing recognized yet: two fingers down shows as the pinch recognizer's ONGOING.
        type = LV_INDEV_GESTURE_PINCH;
    }
    lv_indev_gesture_recognizer_t* r = gesture_recognizer(e, type);
    if (!r || !r->info) {
        return std::nullopt;
    }

    TwoFingerSample s{};
    switch (r->state) {
    case LV_INDEV_GESTURE_STATE_ONGOING:
        s.phase = TwoFingerSample::Phase::Ongoing;
        break;
    case LV_INDEV_GESTURE_STATE_RECOGNIZED:
        s.phase = TwoFingerSample::Phase::Recognized;
        break;
    case LV_INDEV_GESTURE_STATE_ENDED:
    case LV_INDEV_GESTURE_STATE_CANCELED:
        s.phase = TwoFingerSample::Phase::Ended;
        break;
    default:
        return std::nullopt;
    }

    const bool pinch = type == LV_INDEV_GESTURE_PINCH;
    s.kind = pinch ? TwoFingerSample::Kind::Pinch : TwoFingerSample::Kind::Pan;
    s.scale = pinch ? r->scale : 1.0f;

    // LVGL tracks the fingers in the driver's frame, ahead of the calibration and
    // rotation the single pointer goes through. Map where the gesture started and
    // where it is now, so pan and anchor land where the fingers are on screen.
    auto* indev = static_cast<lv_indev_t*>(lv_event_get_param(e));
    const lv_point_t start_driver = r->info->center;
    const lv_point_t now_driver{
        start_driver.x + static_cast<int32_t>(std::lround(r->info->delta_x)),
        start_driver.y + static_cast<int32_t>(std::lround(r->info->delta_y))};
    const lv_point_t start = helix::map_touch_point_to_screen(indev, start_driver);
    const lv_point_t now = helix::map_touch_point_to_screen(indev, now_driver);
    s.start_x = start.x;
    s.start_y = start.y;
    s.delta_x = static_cast<float>(now.x - start.x);
    s.delta_y = static_cast<float>(now.y - start.y);
    return s;
}
#endif

} // namespace helix::ui
