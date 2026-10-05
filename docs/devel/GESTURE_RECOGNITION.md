# Gesture Recognition: Pinch Zoom and Two-Finger Pan

How the app turns LVGL 9.5's multi-touch gesture recognizers into pinch zoom and two-finger pan on the 3D G-code preview and the 3D bed mesh. The shared math lives in `include/view_gestures.h` + `src/ui/view_gestures.cpp`; each view maps a `TwoFingerStep` onto its own camera or projection.

## Architecture

LVGL's gesture system uses independent recognizers (PINCH, ROTATE, TWO_FINGERS_SWIPE) that all process the same raw touch data. **Only one recognizer can be RECOGNIZED at a time** - when one wins, all others are reset. The first recognizer to reach RECOGNIZED owns the touch until a finger lifts; there is no handover between recognizers within one touch.

### Recognizer Priority (enum order)

```
LV_INDEV_GESTURE_NONE = 0
LV_INDEV_GESTURE_PINCH = 1           ← checked first
LV_INDEV_GESTURE_SWIPE = 2
LV_INDEV_GESTURE_ROTATE = 3          ← checked third
LV_INDEV_GESTURE_TWO_FINGERS_SWIPE = 4
LV_INDEV_GESTURE_SCROLL = 5
```

PINCH (index 1) is iterated **before** ROTATE (index 3) and before TWO_FINGERS_SWIPE (index 4) in `lv_indev_gesture_recognizers_update()`. If PINCH reaches RECOGNIZED first, the others are reset: a spreading motion that also translates is a pinch, and pans while it zooms. But if PINCH stays in ONGOING and ROTATE reaches RECOGNIZED first, **PINCH gets reset** (scale goes back to 1.0).

## The PINCH vs ROTATE Race

### Default Thresholds

| Recognizer | Threshold | Default | What it means |
|-----------|-----------|---------|---------------|
| PINCH up | `lv_indev_set_pinch_up_threshold()` | **1.5** | Scale must exceed 150% |
| PINCH down | `lv_indev_set_pinch_down_threshold()` | **0.75** | Scale must go below 75% |
| ROTATE | `lv_indev_set_rotation_rad_threshold()` | **0.2 rad** (~11.5°) | Very small angular change |

**Problem**: Any natural two-finger movement has both scale AND rotation components. The rotation threshold (0.2 radians) is trivially easy to hit, while the pinch threshold (1.5x/0.75x) requires significant finger spread. **ROTATE almost always wins the race.**

### What Happens When ROTATE Wins

1. ROTATE reaches RECOGNIZED → all other recognizers reset
2. PINCH's cumulative scale resets to 1.0
3. Next frame: PINCH is back to ONGOING, recomputing from scratch
4. If PINCH reaches RECOGNIZED, its scale starts from 1.0 again
5. Frame-to-frame delta computation sees a huge discontinuity → **visual jump**

## Thresholds: Where They Are Set

The thresholds are configured in one place, `DisplayBackend::configure_touch_gestures` (`src/api/display_backend.cpp#configure_touch_gestures`), called on every evdev touch pointer in both display backends: DRM (`src/api/display_backend_drm.cpp`, including the device-override and evdev fallback paths) and fbdev (`src/api/display_backend_fbdev.cpp`).

```cpp
lv_indev_set_pinch_up_threshold(indev, 1.15f);
lv_indev_set_pinch_down_threshold(indev, 0.85f);
lv_indev_set_rotation_rad_threshold(indev, 3.14f);   // ~180°, effectively disabled
```

Rotation is raised to effectively disable it (the app has no rotate gesture), so PINCH always wins the race and its cumulative scale is never reset mid-gesture. The pinch thresholds sit much closer to 1.0 than LVGL's defaults, so a small spread or close already recognizes.

**Assertion limits**: LVGL requires `pinch_up > 1.0f` and `pinch_down < 1.0f`; passing exactly 1.0 aborts in debug builds.

## Reading a Gesture: `read_two_finger_sample`

Pan comes from the recognizer's `info->delta_x`/`delta_y`, the cumulative translation of the two-finger centre since the gesture began. PINCH updates these fields too, which is what lets a pinch pan the view while it zooms.

These fields are private to LVGL, and the public API does not expose them: the two-finger swipe surface is a distance plus a 4-way direction (`lv_event_get_two_fingers_swipe_distance()` / `lv_event_get_two_fingers_swipe_dir()`), not a per-axis vector, and `lv_indev_get_gesture_center_point()` returns only the *starting* centre. So `src/ui/view_gestures.cpp#read_two_finger_sample` is the app's only reader of the private gesture struct: it includes `lv_indev_gesture_private.h` and `lv_indev_private.h`, gets the emitting indev from `lv_event_get_param()` and the recognizer from `indev->gesture_data[type]` (LVGL's own `lv_indev_get_gesture_recognizer` is `static`), and returns the state, cumulative deltas, cumulative scale and starting centre as a plain `TwoFingerSample`.

The recognizers track the fingers after lv_evdev's own range and axis-swap scaling and nothing else, the same point the single pointer starts from before HelixScreen's read chain turns it. So the reader maps where the gesture started and where it is now through `src/api/touch_calibration_wrapper.cpp#map_touch_point_to_screen`, which replays that chain: the active touch calibration, the pointer frame hook's scanout-plane turn (`include/pointer_frame_hook.h#map_panel_point`), then LVGL's `lv_display_rotate_point`. The sample's start and delta are the mapped points, so pan follows the fingers and the anchor sits under them on rotated and calibrated installs alike. Scale is taken as reported: rotation leaves it unchanged and a calibration stays close to uniform. Anything that is not a pinch or two-finger-swipe state yields `nullopt`; two fingers down before recognition surfaces as the pinch recognizer's ONGOING.

## Per-Frame Conversion: `two_finger_step`

LVGL reports cumulative totals since the gesture began; views want this frame's pan and zoom. `src/ui/view_gestures.cpp#two_finger_step` (pure, declared in `include/view_gestures.h`) converts:

- **Pan**: this frame's cumulative `delta_x/y` minus the last totals applied.
- **Zoom**: the ratio of this frame's cumulative scale to the last one (PINCH only).
- **First-frame catch-up**: the first RECOGNIZED frame applies everything done before recognition, for pan and zoom alike - a fast pinch can spread past any frame bound before the recognizer fires.
- **Frame filter**: from the second RECOGNIZED frame on, a zoom ratio outside the open interval (0.7, 1.4), or a non-positive scale, yields zoom 1.0 for that frame - such a jump is a recognizer restart, not finger motion - while pan still applies.
- **Resets**: ONGOING and ENDED both clear the running totals. Every gesture passes through ONGOING, so totals from a gesture whose ENDED was never delivered cannot leak into the next one.

The anchor for zooming is the *live* two-finger centre (starting centre plus this gesture's translation), so content under the fingers stays under them.

## Per-View Mapping

Both views apply pan first, then zoom about the live anchor.

- **G-code preview** (`src/ui/gcode_viewer_input.cpp#gcode_viewer_gesture_cb`): `GCodeCamera::pan_pixels()` then `GCodeCamera::zoom_at()`. A pinch zooms about the fingers and pans with them in the same frame; a two-finger swipe only pans. Motion is 3D-only; the two-finger latch below applies in 2D and 3D alike. After the gesture, RELEASED still leaves interaction mode, so the preview returns to full resolution.
- **Bed mesh** (`src/ui/ui_bed_mesh.cpp#bed_mesh_gesture_cb` → `src/rendering/bed_mesh_renderer.cpp#bed_mesh_renderer_apply_two_finger`): `bed_mesh_projection_pan()` then `src/rendering/bed_mesh_projection.cpp#bed_mesh_projection_zoom_at`. Zoom and pan apply in screen space after the perspective divide. Zoom is clamped to `BED_MESH_ZOOM_MIN`..`BED_MESH_ZOOM_MAX` (1.0-8.0); at 1.0 two-finger pan is a deliberate no-op, and a zoom that reaches 1.0 resets pan to zero - pinching fully out is the reset back to the fitted view. `bed_mesh_renderer_set_bounds` and `bed_mesh_renderer_set_render_mode` also reset zoom and pan (via `bed_mesh_projection_reset_zoom`), bounds first because the auto-fit that follows projects through the zoomed function. In async mode the gesture update takes the render mutex; the fast solid-colour fill is active for the whole press.

## The Two-Finger Latch

From the first two-finger frame until every finger lifts, each 3D widget holds off its one-finger interactions: drag rotation, tap selection and the long-press timer (the G-code viewer's exclude-object long-press included). A finger left down after a pinch would otherwise rotate the camera from a stale position, and the one-finger lift that ends a two-finger gesture would otherwise read as an object tap. The latch clears on the next PRESSED (a fresh interaction, all fingers up); the bed mesh clears it on RELEASED as well.

## Gesture State Flow

```
NONE → ONGOING → RECOGNIZED (repeats each frame) → ENDED
                                                  → CANCELED
```

- **ONGOING**: Two fingers detected, recognizer is tracking but threshold not yet met
- **RECOGNIZED**: Threshold exceeded, gesture data available, repeats each frame
- **ENDED**: Fingers lifted cleanly
- **CANCELED**: Finger lifted before threshold was met

## Configuration Requirements

```c
// lv_conf.h
#define LV_USE_GESTURE_RECOGNITION 1  // enables multi-touch gesture system
#define LV_USE_FLOAT 1                // REQUIRED for gesture math
```

## Driver Requirements

- **evdev**: Supports multi-touch (processes ABS_MT_SLOT, ABS_MT_POSITION_X/Y, ABS_MT_TRACKING_ID)
- **libinput**: Does NOT support multi-touch in LVGL - explicitly states "We don't support multitouch"
- **wayland**: Supports multi-touch via wl_touch protocol

For DRM backend with touchscreens, **use evdev driver** (`lv_evdev_create`), not libinput.

## Testing

Desktop SDL is mouse-only, so gesture behaviour cannot be exercised in the simulator or the unit suite; it is verified on capacitive hardware (the resistive panels listed in `docs/user/guide/getting-started.md` report a single finger and never produce two-finger events). `two_finger_step` itself is pure and unit-tested.

## Sources

- [LVGL 9.5 Gestures Documentation](https://docs.lvgl.io/master/main-modules/indev/gestures.html)
- [LVGL GitHub Issue #6640 - GESTURE bug](https://github.com/lvgl/lvgl/issues/6640)
- [LVGL Forum - How does LVGL support multi touch](https://forum.lvgl.io/t/how-does-lvgl-support-multi-touch/13078)
- Official example: `examples/others/gestures/lv_example_gestures.c`
- Source: `lib/lvgl/src/indev/lv_indev_gesture.c` (recognizer logic)
- Source: `lib/lvgl/src/drivers/evdev/lv_evdev.c` (MT event processing)
