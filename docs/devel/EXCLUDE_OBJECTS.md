# Exclude Objects (Developer Guide)

How the Exclude Objects feature works, from Klipper integration to the UI interaction flow, per-object thumbnail rendering, and slicer configuration.

---

## Overview

Exclude Objects lets users stop printing individual objects mid-print without aborting the entire job. This is useful when one object detaches from the bed, has a defect, or was added to the plate just for testing.

HelixScreen provides two ways to exclude objects:

1. **Long-press** on an object in the G-code viewer (direct interaction)
2. **Print Objects side list** (tap the Objects button on the print status panel)

Both methods feed through the same confirmation flow: modal dialog, 5-second undo window, then the `EXCLUDE_OBJECT` G-code command is sent to Klipper via Moonraker.

---

## Architecture

```
Slicer (EXCLUDE_OBJECT_DEFINE/START/END in G-code)
  |
  v
Klipper [exclude_object] module
  |
  +-- exclude_object status --> Moonraker WebSocket notify_status_update
  |                                |
  |                                v
  |                          MoonrakerClient (subscription: "exclude_object")
  |                                |
  |                                v
  |                          PrinterState::update_from_status()
  |                                |
  |                                v
  |                          PrinterExcludedObjectsState
  |                            - excluded_objects_ (set)
  |                            - defined_objects_ (vector)
  |                            - current_object_ (string)
  |                            - version subjects for observers
  |                                |
  |                                v
  +-- EXCLUDE_OBJECT NAME=... <-- PrintExcludeObjectManager
       (sent via MoonrakerAPI)        |
                                      +-- ExcludeObjectModal (confirmation)
                                      +-- Toast with undo button
                                      +-- GCode viewer visual update

UI entry points:
  - Long-press on gcode viewer --> PrintExcludeObjectManager
  - Print Objects side list --> ExcludeObjectSideList
```

### Key Files

| File | Role |
|------|------|
| `include/printer_excluded_objects_state.h` | State management: excluded/defined/current objects, version subjects |
| `src/printer/printer_excluded_objects_state.cpp` | Version-based change notification for LVGL observers |
| `include/ui_print_exclude_object_manager.h` | Orchestrates the exclusion flow: long-press, modal, undo, API call |
| `src/ui/ui_print_exclude_object_manager.cpp` | Implementation of the exclusion state machine |
| `include/ui_exclude_object_modal.h` | Confirmation dialog ("Exclude Object?") |
| `ui_xml/exclude_object_modal.xml` | XML layout for the confirmation modal |
| `include/ui_exclude_object_side_list.h` | Side list of all objects with status indicators |
| `src/ui/ui_exclude_object_side_list.cpp` | List population, thumbnail integration, tap-to-exclude |
| `include/ui_exclude_object_map_view.h` | Object map view with 3D selection brackets |
| `src/ui/ui_exclude_object_map_view.cpp` | Map rendering and hit-testing |
| `include/gcode_renderer.h` | 2D renderer with excluded object visual style |
| `include/gcode_object_thumbnail_renderer.h` | Per-object toolpath thumbnail renderer |
| `src/api/moonraker_motion_api.cpp` | `MoonrakerAPI::exclude_object()` with input validation |
| `src/api/moonraker_client_mock.cpp` | Mock mode: EXCLUDE_OBJECT handling and status dispatch |

---

## Klipper EXCLUDE_OBJECT Integration

### Prerequisites

1. **Klipper configuration**: The `[exclude_object]` section must be present in `printer.cfg`:

   ```ini
   [exclude_object]
   ```

2. **Slicer support**: The slicer must emit `EXCLUDE_OBJECT_DEFINE`, `EXCLUDE_OBJECT_START`, and `EXCLUDE_OBJECT_END` comments in the G-code. See the [Slicer Configuration](#slicer-configuration) section below.

3. **Moonraker subscription**: HelixScreen subscribes to `exclude_object` status updates via Moonraker's WebSocket API. This happens automatically in `MoonrakerClient::subscribe_to_status_updates()`.

### G-code Commands

Klipper's `exclude_object` module uses these G-code commands:

| Command | Source | Purpose |
|---------|--------|---------|
| `EXCLUDE_OBJECT_DEFINE NAME=... CENTER=... POLYGON=...` | Slicer | Defines an object with bounding polygon |
| `EXCLUDE_OBJECT_START NAME=...` | Slicer | Marks the start of an object's layer segment |
| `EXCLUDE_OBJECT_END NAME=...` | Slicer | Marks the end of an object's layer segment |
| `EXCLUDE_OBJECT NAME=...` | HelixScreen / Client | Excludes an object from printing |

### Moonraker Status Object

Klipper reports `exclude_object` status with three fields:

```json
{
  "exclude_object": {
    "objects": [
      {"name": "Part_1"},
      {"name": "Part_2"}
    ],
    "excluded_objects": ["Part_1"],
    "current_object": "Part_2"
  }
}
```

HelixScreen processes this in `PrinterState::update_from_status()` which delegates to `PrinterExcludedObjectsState`:

- `objects` array --> `set_defined_objects()` (all objects in the print)
- `excluded_objects` array --> `set_excluded_objects()` (objects already excluded)
- `current_object` --> `set_current_object()` (object currently being printed)

### State Notification Pattern

Since LVGL subjects do not natively support set types, `PrinterExcludedObjectsState` uses a **version-based notification pattern**:

1. `excluded_objects_version_` is an integer subject initialized to 0
2. When the excluded set changes, the version is incremented by 1
3. Observers watch the version subject and call `get_excluded_objects()` when notified
4. `set_excluded_objects()` compares the new set with the current set and only increments the version if the contents actually changed

The same pattern is used for `defined_objects_version_`.

### Security: Input Validation

`MoonrakerAPI::exclude_object()` validates object names before sending G-code to prevent injection attacks. The `is_safe_identifier()` check rejects:

- Newlines (`\n`, `\r`) -- could inject additional G-code commands
- Semicolons (`;`) -- G-code comment/command separator
- Control characters, null bytes
- Shell metacharacters (`&`, `|`, `` ` ``, `$`)
- Any character not in the strict allowlist: alphanumeric characters, underscores, and spaces only. All other characters are rejected.

Valid object names: `Part_1`, `Benchy_3DBenchy_copy_2`, `My Part 1`

---

## Exclusion Flow (State Machine)

The `PrintExcludeObjectManager` implements a state machine with three states:

```
                   long-press / list tap
                         |
                         v
    IDLE ------------> PENDING (modal shown)
     ^                   |
     |        cancel     |   confirm
     +-------------------+      |
     |                          v
     |                   TIMER_ACTIVE (5s undo window)
     |                     |              |
     |         undo        |   timer      |
     +-----------+---------+   expires    |
                                |         |
                                v         |
                          API CALL --------+
                            |
                       success / error
                            |
                            v
                         IDLE (object in excluded set on success)
```

### Step-by-step

1. **Initiation**: User long-presses an object in the G-code viewer (1000ms threshold — `LONG_PRESS_THRESHOLD_MS` in `src/ui/ui_gcode_viewer.cpp:825`, deliberately double the app-wide 500ms gesture timeout because the gesture cancels printing the object) or taps an object in the Print Objects side list.

2. **Guard checks**: Empty names, already-excluded objects, and pending exclusions are rejected.

3. **Confirmation modal**: `ExcludeObjectModal` shows "Exclude Object?" with the object name and Exclude/Cancel buttons.

4. **Visual preview**: On confirm, the object is immediately shown as excluded in the G-code viewer (red/orange at reduced opacity) before the API call is made.

5. **Undo window**: A 5-second timer starts. A toast with an "Undo" action button is shown. If the user taps Undo, the timer is cancelled and the visual state is reverted.

6. **API call**: When the timer expires, `MoonrakerAPI::exclude_object()` sends `EXCLUDE_OBJECT NAME=<object_name>` to Klipper via Moonraker's `gcode.script` RPC method.

7. **Success**: The object is added to the confirmed `excluded_objects_` set.

8. **Error**: If the API call fails, the visual state is reverted and an error notification is shown.

### Multi-client sync

Objects excluded by other clients (Mainsail, Fluidd, KlipperScreen) are automatically synced via the `excluded_objects_version_` observer. When Klipper reports new exclusions in `notify_status_update`, `PrinterExcludedObjectsState::set_excluded_objects()` updates the set and notifies observers, which triggers `PrintExcludeObjectManager::on_excluded_objects_changed()` to merge the external exclusions into the local set and update the viewer.

---

## Print Objects Side List

`ExcludeObjectSideList` provides a scrollable list of all defined objects in the current print, shown alongside `ExcludeObjectMapView` (the object map with 3D selection brackets). Both are owned by `PrintStatusPanel`. Each row shows:

- **Thumbnail** (40x40 per-object toolpath rendering, if available)
- **Status dot** (green = idle/printing, red = excluded)
- **Object name**
- **Status text** ("Printing", "Excluded", or blank)

### Behavior

- Tapping a non-excluded row triggers the same `PrintExcludeObjectManager::request_exclude()` confirmation flow as a long-press
- Excluded rows are non-clickable and displayed at reduced opacity
- The list auto-refreshes via observers on both `excluded_objects_version_` and `defined_objects_version_`
- The list is accessed from PrintStatusPanel via `on_objects_clicked()` event callback

### XML Layout

The side list is built from `ui_xml/components/exclude_object_side_list.xml`, the map from `ui_xml/components/exclude_object_map.xml`. Rows are populated dynamically in C++ because the object list is not known at compile time (this is an allowed exception to the "no `lv_obj_add_event_cb()`" rule noted in the code).

---

## Per-Object GCode Toolpath Thumbnails

`GCodeObjectThumbnailRenderer` generates small ARGB8888 thumbnails of each object's toolpath for display in the side list.

### Rendering Pipeline

1. **Bounding box extraction**: Each object's AABB is read from `ParsedGCodeFile::objects`
2. **Projection setup**: FRONT isometric projection is configured per-object using `gcode_projection.h`
3. **Single-pass rendering**: All layers and segments are iterated once. Each extrusion segment is dispatched to its object's pixel buffer based on `segment.object_name`. Travel moves are skipped.
4. **Bresenham line drawing**: Lines are drawn directly to raw `uint8_t[]` pixel buffers using integer Bresenham's algorithm. No LVGL calls from the background thread.
5. **Depth shading**: RGB channels are darkened based on Z-height for visual depth. Alpha is preserved at full opacity.
6. **UI thread delivery**: Results are marshaled to the UI thread via `ui_queue_update()` and converted to `lv_draw_buf_t` for use in `lv_image` widgets.

### Thread Safety

The renderer runs in a background thread. Thread safety is maintained by:

- Background thread only reads `ParsedGCodeFile` (immutable during print)
- Raw pixel buffers use `std::make_unique` (no LVGL allocations from background thread)
- Results are delivered to the UI thread via `ui_queue_update()`
- Cancellation via `std::atomic<bool>` flag checked between layers
- Cancelling waits for the background thread to join before proceeding

### Thumbnail Sizing

Default is 40x40 pixels (`kThumbnailSize` in the overlay source). Non-square sizes are supported. Each thumbnail is `width * height * 4` bytes (ARGB8888, no row padding).

---

## 2D Mode / Streaming Mode Support

### 2D Layer Renderer

The 2D layer renderer (`GCodeLayerRenderer`) is what the Auto render mode lands on for builds without the GLES 3D renderer (and what `HELIX_GCODE_MODE=2D` / `--render-2d` force everywhere). It supports the full exclude objects feature:

- **Object picking**: `pick_object_at()` is a two-stage hit test (`src/rendering/gcode_layer_renderer.cpp:1363`). Stage 1 projects each object's 3D bounding box — accumulated over the object's whole toolpath, clamped to the drawn Z range (only layers up to the current one are on screen), and inflated by the pick threshold — and returns that object immediately when it is the only candidate whose footprint covers the touch point; no segment is touched. Objects that have not started printing yet, and support objects while supports are hidden, are skipped at this stage. Only when footprints overlap does stage 2 run: it walks layers downward from the current layer, finds the closest segment to the touch point (`PICK_THRESHOLD_PX`, 15px), and the first layer that produces a hit wins — so a tap over a stack picks what is visually on top.
- **Excluded object rendering**: Excluded objects are drawn in orange-red (`0xFF6B35`) at reduced opacity (`LV_OPA_60`) with 1px line width.
- **Selection brackets**: Highlighted objects show corner bracket wireframes around their 3D bounding box (20% of shortest edge, capped at 5mm). 8 corners x 3 axes = 24 bracket lines per object.
- **Interaction model**: a press-and-hold excludes the object under the finger; a tap (release with minimal movement, no pinch, long-press not already fired) toggles single-selection, which is what draws the corner brackets. The skip-objects icon (`btn_objects` on the print status panel, visible when `exclude_objects_available` is set) opens the map + side-list panel instead.
- **Long-press detection**: In 2D mode, mouse/touch micro-jitter during pressing events is ignored (the `pressing` callback returns early in 2D mode), which prevents accidental cancellation of the long-press timer.

### Streaming Mode

In streaming mode (`GCodeStreamingController`), the layer renderer operates on per-layer segment data fetched on demand rather than the full parsed file. Streaming has no object list — `set_streaming_controller()` clears the full-file pointer (`src/rendering/gcode_layer_renderer.cpp:152`) — so stage 1 of picking is skipped and the unfiltered downward segment walk runs against whatever the stream has cached. That walk is cache-only: `try_get_layer_segments()` returns cached layers and skips uncached ones, never seeking and parsing on a tap — a hit-test must not freeze the UI to load geometry (`gcode_layer_renderer.cpp:1513`; a mid-pick load froze the UI for seconds on a 2-core board).

Note: Per-object thumbnails require `ParsedGCodeFile` segment data. In streaming mode, `ui_gcode_viewer_get_parsed_file()` returns `nullptr`, so thumbnails are not available and the side list displays rows without thumbnail images.

---

## GCode Parser Object Detection

The G-code parser (`GCodeParser`) processes `EXCLUDE_OBJECT_*` commands during parsing:

| Command | Parser Action |
|---------|---------------|
| `EXCLUDE_OBJECT_DEFINE NAME=... CENTER=... POLYGON=...` | Creates a `GCodeObject` entry in `ParsedGCodeFile::objects` with bounding polygon |
| `EXCLUDE_OBJECT_START NAME=...` | Sets `current_object_` so subsequent segments are tagged with this object name |
| `EXCLUDE_OBJECT_END NAME=...` | Clears `current_object_` (segments after this point are untagged) |

Each `ToolpathSegment` carries an `object_name` field. Segments without an object name (skirt, brim, purge line) are not pickable and are skipped by the thumbnail renderer.

Wipe tower segments are tagged with the special name `__WIPE_TOWER__`.

---

## Mock Mode

The `MoonrakerClientMock` fully simulates the `exclude_object` feature for testing:

1. **Object discovery**: When a print starts, the mock scans the G-code file for `EXCLUDE_OBJECT_DEFINE` lines and populates the defined objects list.

2. **Status dispatch**: After discovering objects, the mock dispatches an `exclude_object` status update with the defined objects, empty excluded list, and null current object.

3. **Exclude command handling**: When `EXCLUDE_OBJECT NAME=...` is received via `execute_gcode()`, the mock:
   - Parses the NAME parameter (supports both bare names and quoted names)
   - Adds the object to its internal excluded set
   - Dispatches a status update with the updated excluded list

4. **Periodic status**: During simulated printing, the mock includes `exclude_object` state in its periodic status updates, including the current object being "printed".

### Testing with Mock Mode

```bash
# Run with mock printer and the test G-code file that has 3 objects:
./build/bin/helix-screen --test -vv

# The test G-code (assets/test_gcodes/exclude_object_test.gcode) defines:
#   - Cone_id_0_copy_0
#   - Cube_id_1_copy_0
#   - Cylinder_id_2_copy_0
```

Start a mock print, then long-press objects in the G-code viewer or open the Print Objects side list to test the exclusion flow.

---

## Slicer Configuration

For the exclude objects feature to work, your slicer must output `EXCLUDE_OBJECT` metadata in the G-code. Most modern slicers support this.

> **Slicer support tiers:** HelixScreen is developed and tested primarily against **OrcaSlicer 2.3.2+** — that's the recommended slicer. Manufacturer slicers (Creality Print, FlashForge Orca, Bambu Studio, etc., most of which are OrcaSlicer/PrusaSlicer forks) and **PrusaSlicer / SuperSlicer** are also supported. **Cura is not a target** — we don't test against it or build features for it, but we also don't deliberately break it; its output generally works with the extra setup noted below.

### OrcaSlicer (recommended)

**Printer Settings > General > Firmware**:
- Set **G-code flavor** to `Klipper`

**Print Settings > Output options**:
- Enable **Label objects** (checkbox, listed as **Exclude object** in some versions)

This causes the slicer to emit `EXCLUDE_OBJECT_DEFINE`, `EXCLUDE_OBJECT_START`, and `EXCLUDE_OBJECT_END` commands. OrcaSlicer 2.3.2+ also round-trips filament presets with HelixScreen's filament slots — see `FILAMENT_MANAGEMENT.md`.

### PrusaSlicer / BambuStudio

**Printer Settings > General > Firmware**:
- Set **G-code flavor** to `Klipper`

**Print Settings > Output options**:
- Enable **Label objects** (checkbox)

Emits the same `EXCLUDE_OBJECT_*` commands as OrcaSlicer.

### Cura (not a tested target)

Cura isn't a slicer we test against, but its output works if you enable object labeling. Install the **Exclude Objects for Klipper** post-processing plugin:

1. **Extensions > Post Processing > Modify G-Code**
2. Add **Exclude Objects for Klipper**
3. Slice normally

Alternatively, recent Cura versions (5.x+) have built-in support:
- **Special Modes > Enable Object Labeling**: Checked

### SuperSlicer

**Printer Settings > General**:
- Set **G-code flavor** to `Klipper`

**Print Settings > Output options > Output file**:
- Enable **Label objects**

### IdeaMaker

IdeaMaker does not natively support `EXCLUDE_OBJECT`. Use the `preprocess_cancellation` post-processing script from the [Klipper documentation](https://www.klipper3d.org/Exclude_Object.html) to add the required metadata.

### Verifying Slicer Output

Open the sliced G-code file in a text editor and search for `EXCLUDE_OBJECT_DEFINE`. You should see lines like:

```gcode
EXCLUDE_OBJECT_DEFINE NAME=Part_1 CENTER=100.0,100.0 POLYGON=[[80,80],[120,80],[120,120],[80,120],[80,80]]
EXCLUDE_OBJECT_DEFINE NAME=Part_2 CENTER=150.0,100.0 POLYGON=[[130,80],[170,80],[170,120],[130,120],[130,80]]
```

And throughout the file, segments wrapped in START/END markers:

```gcode
EXCLUDE_OBJECT_START NAME=Part_1
G1 X100 Y100 E1.0
G1 X120 Y100 E1.2
...
EXCLUDE_OBJECT_END NAME=Part_1
```

---

## Tests

Tests are run with:

```bash
./build/bin/helix-tests "[exclude_object]"        # Exclusion state machine tests
./build/bin/helix-tests "[excluded_objects]"       # PrinterExcludedObjectsState tests
./build/bin/helix-tests "[object-thumbnail]"       # Per-object thumbnail renderer tests
./build/bin/helix-tests "[security][injection]"    # G-code injection prevention tests
./build/bin/helix-tests "[mock][print]"            # Mock client exclude_object tests
```

### Test Files

| File | Tag | What it Tests |
|------|-----|---------------|
| `tests/unit/test_exclude_object_char.cpp` | `[exclude_object]` | Exclusion state machine: long-press, confirm, cancel, undo, timer, API, sync |
| `tests/unit/test_excluded_objects_char.cpp` | `[excluded_objects]` | `PrinterExcludedObjectsState`: version subjects, set change detection, observer notification |
| `tests/unit/test_moonraker_api_exclude_object.cpp` | `[security]`, `[mock]` | Input validation, injection prevention, mock client integration |
| `tests/unit/test_gcode_object_thumbnail_renderer.cpp` | `[object-thumbnail]` | Thumbnail rendering: empty/single/multi object, sizing, cancellation, edge cases |

### Test G-code

`assets/test_gcodes/exclude_object_test.gcode` contains three objects (Cone, Cube, Cylinder) with `EXCLUDE_OBJECT_DEFINE` headers and `START`/`END` markers throughout. Used by mock mode for testing.

---

## Developer Guide

### Adding Exclude Objects to a New Panel

If building a panel that needs exclude object support:

```cpp
#include "ui_print_exclude_object_manager.h"

// In panel construction:
exclude_manager_ = std::make_unique<helix::ui::PrintExcludeObjectManager>(
    api, printer_state, gcode_viewer_widget);
exclude_manager_->init();

// When viewer widget is recreated:
exclude_manager_->set_gcode_viewer(new_viewer_widget);

// When API pointer changes:
exclude_manager_->set_api(new_api);

// Cleanup:
exclude_manager_->deinit();
exclude_manager_.reset();
```

### Observing Excluded Objects State

```cpp
// Watch for changes using version-based pattern:
excluded_observer_ = ObserverGuard(
    printer_state.get_excluded_objects_version_subject(),
    [](lv_observer_t* obs, lv_subject_t*) {
        auto* self = static_cast<MyPanel*>(lv_observer_get_user_data(obs));
        const auto& excluded = self->printer_state_.get_excluded_objects();
        // Update UI based on new excluded set
    },
    this);
```

### Thread Safety

- `PrinterExcludedObjectsState::set_excluded_objects()` is called from the main thread (inside `PrinterState::update_from_status()` which holds `state_mutex_`)
- `set_excluded_objects()` calls `lv_subject_set_int()` which must happen on the LVGL thread
- `PrintExcludeObjectManager` uses `ui_queue_update()` to marshal API error callbacks to the main thread
- The `alive_` shared pointer guard prevents use-after-free when async callbacks fire after manager destruction

### Visual States in the GCode Viewer

| State | Visual Treatment |
|-------|-----------------|
| Normal | Default filament color, standard line width |
| Highlighted (selected) | Brightened color, corner bracket wireframe around bounding box |
| Excluded | Orange-red (`0xFF6B35`), 1px line width, 60% opacity |
| Pending exclusion | Same as excluded (visual preview before API call) |
