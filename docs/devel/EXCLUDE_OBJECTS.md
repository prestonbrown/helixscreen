# Exclude Objects (Developer Guide)

How the Exclude Objects feature works, from Klipper integration to the UI interaction flow and slicer configuration.

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
| `src/ui/ui_exclude_object_side_list.cpp` | List population, tap-to-exclude |
| `include/ui_exclude_object_map_view.h` | Object map view with 3D selection brackets |
| `src/ui/ui_exclude_object_map_view.cpp` | Map rendering and hit-testing |
| `include/ui_exclude_object_badges.h` | `compute_object_badges()`: each object's number, colour, excluded flag and anchor, plus the shared badge styling and draw call |
| `src/ui/ui_exclude_object_badges.cpp` | Badge decision (pure) and the disc drawn on the map and the 2D/3D render |
| `src/api/moonraker_api_controls.cpp` | `MoonrakerAPI::exclude_object()` with input validation |
| `src/api/moonraker_client_mock.cpp` | Mock mode: EXCLUDE_OBJECT handling and status dispatch |
| `include/ui_exclude_mode_controller.h` | `ExcludeModeController`: exclude mode over a preview (list, map, render badges), shared by print status and print details |
| `src/ui/ui_exclude_mode_controller.cpp` | Opens and closes the list and map over the host's card, routes every tap to the host |
| `include/pre_start_exclude.h` | Pre-start pick rules (availability, upper-case name matching, every-object check) and the after-start send |
| `src/ui/pre_start_exclude.cpp` | `with_pre_start_exclusions()` / `send_pre_start_exclusions()`: EXCLUDE_OBJECT for each pick once Moonraker confirms the start |
| `ui_xml/components/exclude_objects_button.xml` | The skip button in the preview's top-left corner, shared by print status and details |

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

1. **Initiation**: User long-presses an object in the G-code viewer (1000ms threshold — `LONG_PRESS_THRESHOLD_MS` in `src/ui/gcode_viewer_input.cpp#LONG_PRESS_THRESHOLD_MS`, deliberately double the app-wide 500ms gesture timeout because the gesture cancels printing the object) or taps an object in the Print Objects side list.

2. **Guard checks**: Empty names, already-excluded objects, and pending exclusions are rejected.

3. **Confirmation modal**: `ExcludeObjectModal` shows "Exclude Object?" with the object name and Exclude/Cancel buttons.

4. **Visual preview**: On confirm, the object is immediately shown as excluded in the G-code viewer (greyed out with red diagonal stripes) before the API call is made.

5. **Undo window**: A 5-second timer starts. A toast with an "Undo" action button is shown. If the user taps Undo, the timer is cancelled and the visual state is reverted.

6. **API call**: When the timer expires, `MoonrakerAPI::exclude_object()` sends `EXCLUDE_OBJECT NAME=<object_name>` to Klipper via Moonraker's `gcode.script` RPC method.

7. **Success**: The object is added to the confirmed `excluded_objects_` set.

8. **Error**: If the API call fails, the visual state is reverted and an error notification is shown.

### Multi-client sync

Objects excluded by other clients (Mainsail, Fluidd, KlipperScreen) are automatically synced via the `excluded_objects_version_` observer. When Klipper reports new exclusions in `notify_status_update`, `PrinterExcludedObjectsState::set_excluded_objects()` updates the set and notifies observers, which triggers `PrintExcludeObjectManager::on_excluded_objects_changed()` to merge the external exclusions into the local set and update the viewer.

---

## Print Objects Side List

`ExcludeObjectSideList` provides a scrollable list of all defined objects in the current print, shown alongside `ExcludeObjectMapView` (the object map with 3D selection brackets). Both are owned by an `ExcludeModeController` (`include/ui_exclude_mode_controller.h`) in either host, print status or print details. Each row shows:

- **Numbered chip**: the object's number and colour from `compute_object_badges()` (below), so it matches the badge on the map and the render
- **Object name**
- **Status** ("Printing now", "Excluded", or blank); excluded rows are dimmed

One component arranges these by orientation through `ui_is_portrait`. In landscape, where
the list is a narrow column, the status sits on its own line under the full-width name. In
portrait, where the list spans the screen, it sits in a slot right of the name. Both status
strings are always present but invisible inside that area, so it keeps its height when blank
and, in portrait, is as wide as the longer string in the active language. A row's height
therefore never changes as the printing object moves.

### Behavior

- Tapping a non-excluded row triggers the same `PrintExcludeObjectManager::request_exclude()` confirmation flow as a long-press
- Excluded rows are non-clickable and displayed at reduced opacity
- The list auto-refreshes via observers on both `excluded_objects_version_` and `defined_objects_version_`
- The list is accessed from PrintStatusPanel via `on_objects_clicked()` event callback

### Object numbers on the map and the render

The chip number and colour identify an object everywhere it appears. All of them key on
the object's **defined index**, its position in
`PrinterExcludedObjectsState::get_defined_objects()`, through one decision:
`helix::ui::compute_object_badges()` (`include/ui_exclude_object_badges.h`). It returns one
`ObjectBadge` per defined object with the number text, the palette index, the excluded and
current flags, and a world anchor. It is pure: callers only map the anchor to their own
screen and draw.

- **Side list** (`ExcludeObjectSideList`): each row's chip number and colour, and its idle /
  printing / excluded state.
- **Thumbnail map** (`ExcludeObjectMapView`): a numbered disc in each object's rect. The map
  has no legend of its own: it is always shown beside the side list, which names every
  object, so the plate takes the whole card. An object with no bounding box gets no rect
  but keeps its number, so later objects do not renumber.
- **2D/3D render**: while the side list is open,
  `src/ui/ui_exclude_mode_controller.cpp#refresh_render_badges` pushes the badges to the
  viewer (`ui_gcode_viewer_set_object_badges()`), and again on every
  `defined_objects_version` / `excluded_objects_version` bump of the host's state, through
  the controller's own version observers (the current object bumps the latter). Closing
  the list pushes an empty set. The viewer draws them in its `LV_EVENT_DRAW_POST`
  pass after the renderer (`src/ui/ui_gcode_viewer.cpp#draw_object_badges`),
  projecting each anchor through the transform of the image on screen, so they follow pan,
  zoom and rotation. In 2D that is `GCodeLayerRenderer::project_to_screen()`: every 2D
  transform change invalidates both caches, so what is drawn always uses the live transform.
  In 3D it is `GCodeGLESRenderer::project_to_shown_image()`, which uses the MVP latched when
  the last finished frame was blitted. The 3D renderer keeps showing that frame during VBO
  upload, render deferral and refinement, and the badges stay on it rather than running
  ahead to the live camera. Setting badges equal to the current ones does nothing; a change
  only invalidates the widget, and the renderers repaint from their caches.
- **Parsed file arriving later**: each host calls
  `ExcludeModeController::refresh_render_badges()` when its viewer finishes a parse, so a
  file that finishes loading after the list opened gives its objects parsed anchors and a
  top Z.

Anchor priority: Klipper `CENTER`, the parsed file's `CENTER`, Klipper's bbox centre, the
parsed toolpath bbox centre. Parsed objects are looked up by name; `ParsedGCodeFile::objects`
is a name-sorted map and never decides a number. The anchor's Z is the top of what is on
screen: the lower of the object's parsed top and the current layer, so a badge rides the
print while the object is growing. Streaming 2D has no parsed objects, so it uses Klipper's
geometry and the current layer's Z.

The badge of the object printing now carries a `success`-coloured outline, `space_xxs` wide.
The viewer resolves badge colours and styling once per badge list, and again on the next draw
after the theme or size class changes. A side-list chip takes its number colour from the same
`object_badge_text_color()` as the badges.
Excluded objects' badges are drawn at `LV_OPA_30`, the same fade the map applies to an
excluded rect (`object_badge_opa()`). Anchors that project outside the widget are skipped.
Badges can overlap when objects sit close together; nothing spreads them apart.

A tap inside a drawn (non-excluded) badge picks that badge's object before the renderer's
own picker runs, checking badges in reverse paint order so the one on top wins (`src/ui/gcode_viewer_input.cpp#ui_gcode_viewer_pick_object`), since a badge
can sit over empty space, e.g. the hole of a ring. Badges are drawn, not widgets, so they
never take input themselves.

The disc's diameter is one line of `font_small`, so it scales with the breakpoint like the
rest of the UI.

### XML Layout

The side list is built from `ui_xml/components/exclude_object_side_list.xml`, the map from `ui_xml/components/exclude_object_map.xml`. Rows are populated dynamically in C++ because the object list is not known at compile time (this is an allowed exception to the "no `lv_obj_add_event_cb()`" rule noted in the code). Rows are rebuilt only when the defined object set changes. Exclusions and the printing object restyle the existing rows in place through one int subject per row (`exclude_row_state_<i>`: 0 idle, 1 printing, 2 excluded) that `exclude_object_row.xml` binds to, so the list keeps its scroll position while the printing object changes.

---

## Choosing objects before a print starts

Print details lets the user pick objects to skip before tapping Print. There is no
second model for this: details owns a private `PrinterExcludedObjectsState`
(`init_subjects(false)`, so nothing registers globally) and its excluded set *is* the picks.

- **Objects.** `src/ui/ui_print_select_detail_view.cpp#refresh_exclude_objects`
  fills the defined objects from the file scan's `ScanResult::objects` (the first
  `PRINTER_STOP_SCAN_BYTES` of the file), merged with any the viewer's full parse found
  (`src/ui/pre_start_exclude.cpp#merge_defined_objects`).
- **Taps.** Exclude mode is the same `ExcludeModeController` print status uses, opened with
  `ExcludeTapMode::Toggle`: a tap picks or un-picks an object, with no confirmation modal.
  A name the printer cannot be sent is refused at the tap with a toast
  (`src/ui/ui_print_select_detail_view.cpp#toggle_exclude_pick`).
- **The button** shows when `src/ui/pre_start_exclude.cpp#pre_start_exclude_available` says
  so: the printer has `[exclude_object]`, the file is G-code (not 3MF), and it defines at
  least two objects.
- **Capture.** `src/ui/ui_panel_print_select.cpp#start_print` reads the
  picks with the file at the Print tap, so a USB copy that lands after another file opened
  still sends the tapped file's picks. `PrintStartController::set_exclude_picks` carries
  them to the next start, which consumes them; a reprint never reads them.
- **Send.** Klipper resets `exclude_object` as a print starts (`virtual_sdcard:reset_file`),
  so nothing can be sent ahead of the start. `src/ui/pre_start_exclude.cpp#"with_pre_start_exclusions(std::function"`
  wraps the callback Moonraker's start confirmation fires and sends `EXCLUDE_OBJECT` for each
  pick after it. Every start path from details goes through it: the direct start, the
  plugin-modified start, the pre-start wait and the remap rewrite
  (`src/ui/ui_panel_print_select.cpp#apply_remap`).
- **Refusals.** Picking every object refuses the start before anything heats
  (`src/ui/ui_panel_print_select.cpp#refuse_start_with_every_object_picked`).
  A tap that queues the file queues it without them; once the add succeeds the picks are
  dropped with the toast "Object picks apply only to prints started now". A refused add
  keeps them.
- **Lifetime.** The start hides details with the picks held, so a start that fails comes
  back to the same file with them intact. They clear once Moonraker confirms the start, or
  when the user leaves the file (back to the list, or another file). The hold is one-shot:
  during a long upload or plugin-modify window, reopening the same file before Moonraker
  confirms spends it, so the picks already sent with that start stay on details afterwards.
  The busy overlay over that window mostly prevents the reopen.
- **Exclude mode closes, picks stay.** Details closes exclude mode whenever it deactivates
  (any reason), when its viewer is cleared (memory pressure frees the parse the map and
  badges drew from), and when it is shown for another file. The map copies the outlines it
  draws at open, so nothing it holds outlives the viewer's parse.
- **Offered picks only.** While the skip button is hidden (e.g. after a switch to a printer
  without `[exclude_object]`), `src/ui/ui_print_select_detail_view.cpp#exclude_picks` returns
  none, so hidden picks are never sent or refused on. Both hosts read "has
  `[exclude_object]`" from one place, `src/ui/pre_start_exclude.cpp#printer_has_exclude_object`
  (the `printer_has_exclude_object` capability subject), and observe that subject, so a
  printer switch with details open hides or re-offers the picks at once.
- **Failures.** A failed send is one error toast naming the objects, unless the print ended
  first; a TIMEOUT is advisory, since the command may still run
  (`src/ui/pre_start_exclude.cpp#send_pre_start_exclusions`).

---

## 2D Mode / Streaming Mode Support

### 2D Layer Renderer

The 2D layer renderer (`GCodeLayerRenderer`) is what the Auto render mode lands on for builds without the GLES 3D renderer (and what `HELIX_GCODE_MODE=2D` / `--render-2d` force everywhere). It supports the full exclude objects feature:

- **Object picking**: `pick_object_at()` is a two-stage hit test (`src/rendering/gcode_layer_renderer.cpp#pick_object_at`). Stage 1 projects each object's 3D bounding box — accumulated over the object's whole toolpath, clamped to the drawn Z range (only layers up to the current one are on screen), and inflated by the pick threshold — and returns that object immediately when it is the only candidate whose footprint covers the touch point; no segment is touched. Objects that have not started printing yet, and support objects while supports are hidden, are skipped at this stage. Only when footprints overlap does stage 2 run: it walks layers downward from the current layer, finds the closest segment to the touch point (`PICK_THRESHOLD_PX`, 15px), and the first layer that produces a hit wins — so a tap over a stack picks what is visually on top.
- **Excluded object rendering**: Excluded objects keep their shading but drain to grey, with red (`gcode_selection_excluded`) 45-degree stripes over them. See the state table below.
- **Selection brackets**: Highlighted objects show corner bracket wireframes around their 3D bounding box (20% of shortest edge, capped at 5mm). 8 corners x 3 axes = 24 bracket lines per object.
- **Interaction model**: a press-and-hold excludes the object under the finger; a tap (release with minimal movement, no two-finger gesture, long-press not already fired) toggles single-selection, which is what draws the corner brackets. On the 3D preview, a pinch zooms about the fingers and pans with them and a two-finger drag pans; in 2D and 3D alike, rotate, tap and long-press stay off from the first two-finger frame until every finger lifts. The skip-objects icon (`btn_objects` on the print status panel, visible when `exclude_objects_available` is set) opens the map + side-list panel instead.
- **Long-press detection**: In 2D mode, mouse/touch micro-jitter during pressing events is ignored (the `pressing` callback returns early in 2D mode), which prevents accidental cancellation of the long-press timer.

### Streaming Mode

In streaming mode (`GCodeStreamingController`), the layer renderer operates on per-layer segment data fetched on demand rather than the full parsed file. Streaming has no object list — `set_streaming_controller()` clears the full-file pointer (`src/rendering/gcode_layer_renderer.cpp#set_streaming_controller`) — so stage 1 of picking is skipped and the unfiltered downward segment walk runs against whatever the stream has cached. That walk is cache-only: `try_get_layer_segments()` returns cached layers and skips uncached ones, never seeking and parsing on a tap — a hit-test must not freeze the UI to load geometry (`src/rendering/gcode_layer_renderer.cpp#pick_object_at`; a mid-pick load froze the UI for seconds on a 2-core board).

---

## GCode Parser Object Detection

The G-code parser (`GCodeParser`) processes `EXCLUDE_OBJECT_*` commands during parsing:

| Command | Parser Action |
|---------|---------------|
| `EXCLUDE_OBJECT_DEFINE NAME=... CENTER=... POLYGON=...` | Creates a `GCodeObject` entry in `ParsedGCodeFile::objects` with bounding polygon |
| `EXCLUDE_OBJECT_START NAME=...` | Sets `current_object_` so subsequent segments are tagged with this object name |
| `EXCLUDE_OBJECT_END NAME=...` | Clears `current_object_` (segments after this point are untagged) |

Each `ToolpathSegment` carries an `object_name` field. Segments without an object name (skirt, brim, purge line) are not pickable.

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

To check picks made before the start, leave `HELIX_MOCK_EXCLUDE_OBJECTS` unset: it replaces
the file's object names at print start, so the picks would name objects the mock no longer
has.

```bash
unset HELIX_MOCK_EXCLUDE_OBJECTS
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test --sim-speed 6 -vv --render-2d \
  --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
C="./build/bin/helix-screen ctl -s $HELIX_SOCK"
$C navigate print-select
$C ls                                   # the card labelled exclude_object_test.gcode
$C click <that card's path>
$C click btn_detail_objects
$C ls rows_container                    # Cone_id_0_copy_0, Cube_id_1_copy_0, Cylinder_id_2_copy_0
$C click <row 1's path>
$C click <row 3's path>
$C text objects_pick_count              # "2"
$C click close_btn
$C click print_button
# once Printing:
$C text objects_count_label             # "1/3"
grep -n "PreStartExclude" /tmp/helix-$TREE.log
```

The mock log shows `EXCLUDE_OBJECT: 'Cone_id_0_copy_0'` and `'Cylinder_id_2_copy_0'` after
the `printer.print.start` line, and no `Could not skip` line.

To see the render badges on a real three-object plate, print that file directly. The 3D
renderer needs a GL context, which `SDL_VIDEODRIVER=dummy` lacks (the viewer falls back to
2D); `offscreen` provides one without opening a window:

```bash
HELIX_MOCK_AUTO_PRINT=1 SDL_VIDEODRIVER=offscreen ./build/bin/helix-screen --test \
  --sim-speed 6 -vv --render-3d --gcode-file assets/test_gcodes/exclude_object_test.gcode
```

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
./build/bin/helix-tests "[security][injection]"    # G-code injection prevention tests
./build/bin/helix-tests "[mock][print]"            # Mock client exclude_object tests
```

### Test Files

| File | Tag | What it Tests |
|------|-----|---------------|
| `tests/unit/test_print_exclude_object_manager.cpp` | `[exclude_object]` | Exclusion state machine driven through the real `PrintExcludeObjectManager`: confirm, cancel, undo, timer, API, sync |
| `tests/unit/test_exclude_object_long_press_gate.cpp` | `[exclude_object]` | Long-press gate: pending object, timer arming, clear |
| `tests/unit/test_excluded_objects_char.cpp` | `[excluded_objects]` | `PrinterExcludedObjectsState`: version subjects, set change detection, observer notification |
| `tests/unit/test_moonraker_api_exclude_object.cpp` | `[security]`, `[mock]` | Input validation, injection prevention, mock client integration |
| `tests/unit/test_exclude_object_badges.cpp` | `[exclude_badges]` | Badge numbering by defined order, flags, anchor fallback chain, map badge numbering with a bbox-less object; the viewer's draw pass (drawn-top Z, off-screen skip, no stale pick targets), pick precedence (badge over geometry, top badge wins, excluded not pickable), equal badges not invalidating, exclusion dropping selection, the 3D shown-image transform |
| `tests/unit/test_print_status_exclude_badges.cpp` | `[exclude_badges]` | Panel lifecycle: badges appear with the side list, match its chips, follow version bumps, clear on close |
| `tests/unit/test_exclude_object_side_list.cpp` | `[exclude_side_list]` | Rows restyle in place, keep scroll and height as the printing object moves |
| `tests/unit/test_exclude_mode_controller.cpp` | `[exclude_mode]` | The shared exclude mode: list and map over the host's card, taps reaching the host |
| `tests/unit/test_pre_start_exclude.cpp` | `[pre_start_exclude]` | Pick rules (availability, name matching, every-object check, merge) and the after-start send |
| `tests/unit/test_print_select_pre_start_exclude.cpp` | `[pre_start_exclude][start]` | Picks captured at the Print tap, sent only after the start is confirmed on the direct and remap paths, held across a failed start, refused when every object is picked, dropped by a queued start |

### Test G-code

`assets/test_gcodes/exclude_object_test.gcode` contains three objects (Cone, Cube, Cylinder) with `EXCLUDE_OBJECT_DEFINE` headers and `START`/`END` markers throughout. Used by mock mode for testing.

---

## Developer Guide

### Adding Exclude Objects to a New Panel

A panel that shows exclude mode over its preview owns an `ExcludeModeController` and hands
it the widgets it covers, the state it reads, and what a tap does:

```cpp
#include "ui_exclude_mode_controller.h"

helix::ui::ExcludeModeController exclude_mode_;   // member

helix::ui::ExcludeModeTargets targets;
targets.card = card_widget;                        // what the map covers
targets.columns = columns_row;                     // what the list floats over
targets.gcode_viewer = gcode_viewer_widget;        // render badges in 2D/3D
targets.thumbnail_mode = showing_thumbnail;        // map instead of badges
exclude_mode_.show(targets, &excluded_objects_state, helix::ui::ExcludeTapMode::Toggle,
                   [this](const std::string& name) { on_object_tapped(name); });

exclude_mode_.hide();                              // on close and on deactivate
```

Print status (`src/ui/ui_panel_print_status.cpp#show_exclude_map_view`)
routes taps through `PrintExcludeObjectManager`'s confirm-and-undo flow against the
printer's live state. Print details
(`src/ui/ui_print_select_detail_view.cpp#toggle_exclude_mode`)
toggles picks in its own state.

### Observing Excluded Objects State

```cpp
// Watch for changes using version-based pattern:
excluded_observer_ = ObserverGuard(
    printer_state.get_excluded_objects_version_subject(),
    [](lv_observer_t* obs, lv_subject_t*) {
        auto* self = static_cast<MyPanel*>(lv_observer_get_user_data(obs));
        const auto& excluded = self->printer_state_.excluded_objects_state().get_excluded_objects();
        // Update UI based on new excluded set
    },
    this);
```

### Thread Safety

- `PrinterExcludedObjectsState::set_excluded_objects()` is called from the main thread (inside `PrinterState::update_from_status()`)
- `set_excluded_objects()` calls `lv_subject_set_int()` which must happen on the LVGL thread
- `PrintExcludeObjectManager` uses `ui_queue_update()` to marshal API error callbacks to the main thread
- The `alive_` shared pointer guard prevents use-after-free when async callbacks fire after manager destruction

### Visual States in the GCode Viewer

The colors are XML tokens in `ui_xml/gcode_tokens.xml`
(`gcode_selection_outline`, `gcode_selection_excluded`, `gcode_selection_bracket`),
resolved once per renderer through `selection::palette_from_theme()`. They are
static tokens - a bare name with no `_light`/`_dark` pair - because the cues have
to stay legible against arbitrary filament colors, which do not follow the UI
theme.

| State | Visual Treatment |
|-------|-----------------|
| Normal | Default filament color, standard line width |
| Highlighted (selected) | **Keeps its own filament color**, plus a white silhouette rim tracing the object's contour and the corner-bracket wireframe around its bounding box |
| Excluded | Selection dropped (`ui_gcode_viewer_set_excluded_objects()` removes it from the highlight and the tap toggle). Shading kept, hue drained to grey (`selection::excluded_grey`), red stripes (`gcode_selection_excluded`, `#FF3B30`) every 6px at 45 degrees. Line width is **unchanged**. Same in 2D, the 2D ghost and 3D; the 3D ghost (about 2% opacity) is left as is, and a 3D frame drawn while the camera is moving shows filament color until the still render lands |
| Excluded *and* selected | Grey inside the white rim, without stripes: a pixel carries one alpha tag and the selection tag wins, because seeing what you picked matters more |
| Pending exclusion | Same as excluded (visual preview before API call) |

Two corrections to what this table used to say, both worth knowing if you are
reading older code or notes:

- **Selection does not brighten the object.** It used to claim a "brightened
  color", which described a 1.8x `HIGHLIGHT_BRIGHTNESS` bake in the geometry
  builder that no live caller reached. A selected object deliberately keeps its
  filament color and is marked by the rim instead, as in OrcaSlicer.
- **Exclusion does not change line width.** Excluded objects draw at the same
  stroke width as everything else.

The rim is derived from where the object actually landed on screen, not painted
speculatively: the selected object is drawn once with a tag in its alpha byte,
and `stroke_selection_rim()` writes white where the tagged region ends. It is
`selection::kOutlinePx` (2) screen pixels wide, dropping to
`kOutlineSmallPanelPx` (1) on panels at or below 320px, where 2px per side
swallows a small object whole.
