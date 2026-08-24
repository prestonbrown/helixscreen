# Seven new home-panel widgets

Date: 2026-08-12
Branch: `feature/home-widgets`, based on `fix/grid-cell-metrics`
Status: approved design, not yet implemented

## Goal

Surface seven families of printer data that are already live in subjects but have
never reached the home screen, and consolidate the duplicated logic those widgets
would otherwise have to copy.

The widgets, in build order:

| # | Widget | Kind | Batch |
|---|--------|------|-------|
| 1 | Speed / flow override | binding | 1 |
| 2 | Live Z-offset (baby-stepping) | binding | 1 |
| 3 | Filament dryer | binding | 1 |
| 4 | Host health + sparklines | canvas (reuse) | 2 |
| 5 | Toolhead position / homing | canvas (new draw) | 2 |
| 6 | Bed mesh heightmap | canvas (reuse) | 2 |
| 7 | Exclude-objects mini-map | canvas (reuse + mini mode) | 3 |

Plus one bug fix carried along: pre-print phase is invisible in the print-status
widget's *detailed* layout.

## Why this branch

`fix/grid-cell-metrics` reworked home-grid sizing to square cells with half-cell
tracks. Authoring seven widgets against `main`'s contract would produce seven rows
that all need rewriting at merge. The contract deltas that matter:

- **Spans are in grid tracks; a track is half a cell** (`GridLayout::TRACKS_PER_CELL == 2`).
  Contract documented at `src/ui/panel_widget_registry.cpp:55-64`. The field comments in
  `include/panel_widget_registry.h:29-34` still read as cells and are stale.
- **A widget without `supports_half_col` / `supports_half_row` must span an even
  number of tracks on that axis** - default, min, and max alike. Enforced by
  `tests/unit/test_registry_span_bands.cpp:128`.
- **`effective_min_colspan() <= 8` and `effective_min_rowspan() <= 12`**, the
  272x480 micro-portrait grid. Exceeding it means the widget is disabled at boot
  with a `TooLargeForGrid` toast. Enforced at `test_registry_span_bands.cpp:110`.
- **`on_size_changed(colspan, rowspan, width_px, height_px)` now receives tracks**
  in the first two arguments (`src/ui/panel_widget_manager.cpp:891-894`). Branch on
  `width_px` / `height_px` against the bands in `include/panel_widget_size.h`
  (`W_NORMAL=135`, `W_WIDE=205`, `H_TALL=131`, `H_TALLER=197`), never on span counts.

None of the seven widgets sets a half-cell flag, so every authored span below is even.

## Batch 0: DRY refactors

These land first because each is a duplication the new widgets would otherwise
become the next copy of. Each is independently reviewable and independently
revertible.

### D1. One authority for speed / flow sends

There is no `SpeedFlowController` analogous to `TemperatureController`. Two call
sites format their own gcode with disagreeing clamps:

- `src/ui/ui_print_tune_overlay.cpp:405-435` - speed `[50,200]` via `M220 S<n>`,
  flow `[75,125]` via `M221 S<n>`. Live and reachable.
- `src/ui/ui_panel_controls.cpp:1555-1650` - speed `[10,200]`, flow `[50,150]`.
  **Dead code**: the callbacks are registered at `:272-275` but no XML references
  `on_controls_speed_up` / `on_controls_flow_*`. Its flow handler tracks a
  function-local `static int current_flow = 100` (`:1603`, `:1633`) instead of
  reading `flow_factor`, and `update_flow_display()` (`:1546-1552`) hardcodes 100%.

**Change:** add `helix::tune::set_speed_percent(api, int)` and
`set_flow_percent(api, int)` carrying the single agreed clamp - **speed `[50,200]`,
flow `[75,125]`** (PrintTuneOverlay's, the range that actually ships). Convert
PrintTuneOverlay to call it. **Delete** the dead ControlsPanel handlers, their
registrations, and `update_flow_display()` rather than leave a stale trap for the
next reader.

Subjects are plain int percent, 100 = 100% (`src/printer/printer_motion_state.cpp:43-44`);
Klipper's `extrude_factor` is renamed `flow_factor` on the way in (`:168-180`).

### D2. One authority for Z-offset baby-steps

`z_offset_utils.h` already shares formatting, `apply_and_save`, and the restart
latch. The *adjust* path is not shared - clamping, micron rounding,
`pending_z_offset_delta` bookkeeping, the optimistic subject write, and the
homed-axes `MOVE=1` decision all live inline in
`PrintTuneOverlay::handle_z_offset_changed` (`src/ui/ui_print_tune_overlay.cpp:458-524`).

**Change:** extract `helix::zoffset::adjust(api, printer_state, delta_mm)` into
`z_offset_utils`, carrying the `+/-2.0 mm` clamp
(`include/ui_print_tune_overlay.h:229-230`), micron rounding, pending-delta
accumulation, optimistic write, and the `MOVE=1` gate that fires only when x/y/z
are all homed (`:507-512`). PrintTuneOverlay and the new widget both call it.

**Also fix, while here:** `pending_z_offset_delta` is never reset in production.
`PrinterMotionState::clear_pending_z_offset_delta` (`src/printer/printer_motion_state.cpp:230`)
has zero non-test callers - not on save, not on print end, not on disconnect. The
delta banner in ControlsPanel therefore accumulates across the whole session.
Reset it in `apply_and_save` on success and on print completion.

**Also:** persist the step size. `Z_STEP_AMOUNTS[] = {0.05, 0.025, 0.01, 0.005}`
with default index 2 lives in RAM only (`include/ui_print_tune_overlay.h:227-233`)
and resets to 0.01 mm every launch. Move it to a `Config` key so the overlay and
the new widget share one persisted step.

### D3. Lift the bed coordinate mapper

`ExcludeObjectMapView::CoordMapper` (`include/ui_exclude_object_map_view.h:33-50`,
impl `ui_exclude_object_map_view.cpp:27-45`) is aspect-preserving mm->px with
centering, Y-flip, and center-origin support for delta/Voron beds. It has no LVGL
dependency and is exactly what the toolhead tile needs.

**Change:** move it to its own `include/bed_coord_mapper.h` as
`helix::BedCoordMapper`. `ExcludeObjectMapView` and the toolhead widget both use it.

### D4. One bed-dimension resolver

The "what size is the bed" fallback chain is written twice today and the toolhead
widget would be the third:

- `src/ui/ui_panel_print_status.cpp:1431-1441` - `build_volume()`, else `235x235`
- `src/ui/ui_exclude_object_map_view.cpp:105-106` - same fallback

**Change:** add `helix::bed_dimensions(api)` returning `{w_mm, h_mm, origin}` with
the documented chain: `api->hardware().build_volume()`
(`include/printer_detector.h:47-53`, populated from Klipper
`configfile.settings.stepper_x/y/z` at `src/api/moonraker_api_controls.cpp:796-849`)
-> `PrinterState::get_axis_bounds()` (`include/printer_motion_state.h:18-25`, the
lower-latency kinematic envelope) -> `235x235`. Convert both existing call sites.

Do **not** read `build_volume_range` from `assets/config/printer_database.json` -
that field is a detection heuristic (`src/printer/printer_detector.cpp:346`), not an
authoritative bed size.

### D5. Do not add a fourth homing derivation

`homed_axes` is the raw Klipper string (`printer_motion_state.cpp:126-132`). It is
already decoded three ways: ControlsPanel owns `x_homed`/`y_homed`/`xy_homed`/
`z_homed`/`all_homed` (`src/ui/ui_panel_controls.cpp:164-168`, `:207-245`),
MotionPanel makes prefixed copies to avoid collision (`src/ui/ui_panel_motion.cpp:178-180`),
and `helix::toolhead_is_homed()` exists in `include/toolhead_homing.h`.

**Change:** the toolhead widget derives per-axis homing through
`toolhead_homing.h`, extended with a per-axis accessor if one is missing. It does
**not** bind ControlsPanel's subjects (another panel's `SubjectManager` owns them)
and does **not** add a fourth private copy.

### D6. Give the dryer text subjects a writer, or remove them

`dryer_target_temp_text`, `dryer_time_text`, and `dryer_current_temp_text` are
declared (`include/ams_state.h:1492-1504`, init `src/printer/ams_state.cpp:340-358`)
and **never written**. `src/printer/ams_state.cpp:2080-2081` says formatting "is
handled by observers in `AmsDryerCard::setup()`"; `AmsDryerCard` no longer exists
anywhere in the tree. No XML binds them. Binding them yields permanent empty strings.

**Change:** write them in `sync_dryer_from_backend()`
(`src/printer/ams_state.cpp:2038-2093`), reusing the formatting that the maintained
`env_ind_drying_text_[unit]` path already uses (`:1740-1751`). Delete the stale
comment and the dangling doc reference at `include/ui_ams_sidebar.h:40`.

**Also fix the mirror unit.** The scalar `dryer_*` subjects mirror
`dryer_mirror_unit_`, which is set to whichever unit's detail overlay the user last
opened (`src/ui/ui_ams_environment_overlay.cpp:198`). A home tile bound to them
would silently follow the user's browsing history. Change the mirror to track **the
actively-drying unit, falling back to unit 0** when nothing is drying.

### D7. Documentation (coordinate before doing)

`docs/devel/ARCHITECTURE.md:452-540` documents 8 widgets against 37 shipped, and its
`register_widget_factory` example shows a zero-arg lambda while the real
`WidgetFactory` takes `const std::string& instance_id`
(`include/panel_widget_registry.h:17`). The wrong signature is actively misleading
to anyone adding a widget, so fix that example.

`docs/devel/LAYOUT_SYSTEM.md:462-500` "Widget span authoring" is still written in
cell units and says "`tips` is the honest example: authored 4 wide, minimum 2" when
it is now 8/4 in tracks. **That section is squarely Task 13's scope on this branch.**
Flag it to the branch owner rather than rewriting it here.

## Batch 1: binding widgets

### 1. Speed / flow override (`speed_flow`)

Two rows, speed and flow, each with a value readout and `-`/`+` steppers at 5%.
Long-press either row resets that axis to 100%. Reads `speed_factor` / `flow_factor`
directly; writes through `helix::tune` (D1). No hardware gate.

Spans (tracks): default `4x2`, min `4x2`, max `8x4`. The steppers need horizontal
room, so the minimum is two cells wide rather than one.

### 2. Live Z-offset (`z_offset`)

Large current-offset readout (`gcode_z_offset`, microns, formatted via the existing
`z_offset_utils::format_offset_compact`), `-`/`+` at the persisted step, and a Save
button. Save is gated exactly as the Controls card is today
(`ui_xml/controls_panel.xml:98-103`): visible only when `z_offset_can_save`, hidden
when the offset is zero, disabled during `print_active`. Adjust goes through
`helix::zoffset::adjust` (D2); Save through the existing
`helix::zoffset::apply_and_save`. Tapping the readout opens the tune overlay for
step selection.

`z_offset_can_save` is 0 only for `ZOffsetCalibrationStrategy::FIRMWARE_MANAGED`
(`src/printer/printer_state.cpp:1015-1019`), so no separate hardware gate is needed.

Spans (tracks): default `4x2`, min `4x2`, max `8x4`.

### 3. Filament dryer (`dryer`)

Progress arc plus target temp and remaining time. **`dryer_progress_pct` is -1 when
not drying** (`include/ams_types.h:1533-1543`, and the subject default is also -1 at
`src/printer/ams_state.cpp:345`) - the arc must special-case negative rather than
render it as 0. Idle state shows "Not drying" with the arc empty.

Tapping opens the existing `AmsEnvironmentOverlay`
(`src/ui/ui_ams_environment_overlay.cpp:762-769`), which already owns presets,
temp/duration entry, start/stop, and clamping to
`DryerInfo::min_temp_c/max_temp_c/max_duration_min`. The widget does not implement
its own start/stop.

Hardware gate: `dryer_supported` (`src/printer/ams_state.cpp:340`). Prefer it over
`dryer_info_visible`, which is a display-intent flag that happens to carry the same
value today.

**Backend coverage is narrow.** `get_dryer_info` is implemented by ACE
(`ams_backend_ace.cpp:581-647`), QIDI Box (`ams_backend_qidi.cpp:1464-1560`),
Happy Hare (`ams_backend_happy_hare.cpp:2889-2960`), and Mock
(`ams_backend_mock.cpp:1318-1470`). CFS, AFC, Snapmaker, AD5X/IFS, and Toolchanger
inherit `DryerInfo{.supported=false}` (`include/ams_backend.h:1421-1423`) and will
not show the tile. **The AFC/BoxTurtle rig cannot exercise this widget** - verify
against `HELIX_MOCK_DRYER` (`docs/devel/FILAMENT_MANAGEMENT.md:3130`).

Spans (tracks): default `2x2`, min `2x2`, max `6x4`.

## Batch 2: canvas widgets

### 4. Host health (`host_health`)

Reuses `<helix_sparkline>` wholesale - it is already an XML-registered widget
(`src/ui/helix_sparkline.cpp:141-145`, registered from
`src/application/application.cpp:1696`) drawing on `LV_EVENT_DRAW_MAIN_END` with no
canvas buffer, auto-scaling to the window, taking its color from
`lv_obj_get_style_line_color` so `bind_style` can drive thresholds.

History is real: a 60-sample ring at 1 Hz (`include/performance_state.h:22`,
`:118-127`), read via `PerformanceState::read_history(name)`
(`src/system/performance_state.cpp:113-126`), populated for `host_cpu_pct`,
`host_mem_pct_used`, and per-MCU load. **There is no ring for CPU temperature** -
add one `push_history("host_cpu_temp_c", ...)` line in `apply_sample`
(`performance_state.cpp:135-150`); the ring map is keyed by string so no struct
change is needed.

Throttle state is a raw Pi bitmask, already decoded into `perf_host_throttle_text`
by `MoonrakerPerformanceSource::format_throttle_text`
(`src/system/moonraker_performance_source.cpp:286-317`). Bind the text; do not
re-decode. Hide the row with
`<bind_flag_if_eq subject="perf_host_throttle_state" flag="hidden" ref_value="0"/>`,
the idiom already used at `ui_xml/performance_overlay.xml:20`.

Progressive disclosure, keyed off pixel bands in `on_size_changed`:

| Condition | Shows |
|---|---|
| below `W_NORMAL` | CPU% + sparkline |
| `width_px >= W_NORMAL` | adds memory % + sparkline |
| `height_px >= H_TALL` | adds CPU temp + sparkline, and the throttle row |

Tapping opens the existing Performance overlay (`src/ui/ui_overlay_performance.cpp`).

**Lifetime trap:** the `perf_*` subjects are torn down and recreated on reconnect
and printer switch. Every observer must pass
`PerformanceState::instance().subjects_lifetime()`, as `helix_sparkline.cpp:36-42`
and `ui_overlay_performance.cpp:37-52` do.

Hardware gate: `perf_available`.

Spans (tracks): default `4x2`, min `2x2`, max `8x6`.

### 5. Toolhead position / homing (`toolhead`)

The only genuinely new drawing in this design. A top-down plate rectangle with a
crosshair at the current X/Y, a Z bar down one side, and unhomed axes drawn in the
error color.

Pattern: `LV_EVENT_DRAW_MAIN_END` on a plain `lv_obj`, following
`src/ui/helix_sparkline.cpp:22-121` - impl pointer passed as *per-callback*
`user_data` and retrieved with `lv_event_get_user_data`, `LV_EVENT_DELETE` callback
deleting the impl, drawing with `lv_draw_*` into `lv_event_get_layer(e)` in absolute
coords. Not an `lv_canvas`; no buffer to own or tear down.

**Use `DRAW_MAIN_END` or `DRAW_POST`, never `LV_EVENT_DRAW_TASK_ADDED`.** In LVGL
9.5 `DRAW_TASK_ADDED` fires *after* those two, so `lv_draw_*` calls from it silently
draw nothing - this broke chart gradient fills that worked pre-9.5. The constraint is
already noted in `src/ui/helix_sparkline.cpp:30-32`.

Note no panel widget currently draws custom graphics - they all compose XML. The
proven route is TempGraphWidget's: the widget owns a C++ component instantiated in
`attach()` (`src/ui/panel_widgets/temp_graph_widget.cpp:69-113`).

- **Coordinates:** `helix::BedCoordMapper` (D3).
- **Bed size:** `helix::bed_dimensions(api)` (D4), re-resolved when
  `api->get_build_volume_version_subject()` bumps
  (`include/i_moonraker_api.h:305`), observed the way
  `src/ui/ui_panel_bed_mesh.cpp:917-930` does it.
- **Homing:** through `toolhead_homing.h` (D5).
- **Units:** `position_x/y/z` are centimillimetres, mm x 100
  (`printer_motion_state.cpp:79-92`). `gcode_z_offset` is microns - different scale,
  do not mix.
- **Redraw:** positions fire ~4-10 Hz during a print with no client-side rate
  limiting, and three separate observers would each invalidate. All observers funnel
  into one `CoalescedTimer{0}` member calling `schedule_once([this]{ lv_obj_invalidate(obj_); })`.
  `schedule()` is the trailing-edge debounce and would starve under a continuous
  producer; `schedule_once` is leading-edge and fires at a bounded rate
  (`src/ui/ui_coalesced_timer.cpp:53-62`). Never `lv_async_call` from a draw path -
  it mallocs and creates a one-shot timer per call, which is the bug commit
  `0f145b14c` fixed. Do not `memset` any struct holding a `CoalescedTimer`.

Spans (tracks): default `4x4`, min `2x2`, max `8x8`.

### 6. Bed mesh heightmap (`bed_mesh_tile`)

No new rendering. `<bed_mesh>` is already an XML-registered widget
(`src/ui/ui_bed_mesh.cpp:606-609`) that renders on `LV_EVENT_DRAW_POST` and has a 2D
heatmap fast path (`BedMeshRenderMode`, `include/bed_mesh_renderer.h:16-20`). At tile
size: force `Force2D`, leave async mode **off** (the render thread plus a 600x400
blit buffer is far too heavy for a dashboard cell), skip the touch-drag handlers
(`ui_bed_mesh.cpp:571-578`).

Feed it through the public C API - `ui_bed_mesh_set_data(canvas, mesh, rows, cols)`
and `ui_bed_mesh_set_bounds(...)` - copying the panel's feed path
(`src/ui/ui_panel_bed_mesh.cpp:846-878`, `:938-953`).

**Data trap:** do not bind `bed_mesh_available`. That subject and its siblings are
declared globally (`src/ui/ui_panel_bed_mesh.cpp:159-228`, run at boot from
`subject_initializer.cpp:456-458`) but only *written* from the panel's create path
(`:288`, `:291`), so a home tile reads a stale 0 until the user opens the Bed Mesh
panel. Read `api->advanced().has_bed_mesh()` / `get_active_bed_mesh()`
(`include/moonraker_advanced_api.h:122`, `:152`) instead - that cache is warm from
connect because `bed_mesh` rides the union subscription
(`src/api/moonraker_discovery_sequence.cpp:1172-1175`).

Do not re-call `ui_bed_mesh_set_data` on every notification; gate on profile name or
matrix change. The renderer has its own FPS governance, so do not wrap it in an
additional timer.

Tapping opens the Bed Mesh panel. Hardware gate: `printer_has_bed_mesh`.

Spans (tracks): default `4x4`, min `4x4`, max `12x8`. A heatmap below two cells
square is unreadable, so the minimum is deliberately larger than the others.

## Batch 3: exclude-objects mini-map (`exclude_map`)

**Read-only tile.** It renders first-layer outlines with excluded objects shown as
excluded; tapping anywhere opens the existing full map view in print status, where
the destructive action stays behind its existing guards. Those guards are good -
a confirmation modal ("Stop printing X? This cannot be undone after 5 seconds")
plus a 5-second undo window with a toast action button, and the RPC only fires when
the undo timer expires (`src/ui/ui_print_exclude_object_manager.cpp:135-243`,
`:290-340`). At tile size the 28 px minimum touch rects
(`ui_exclude_object_map_view.h:MIN_TOUCH_TARGET_PX`) inflate and overlap, making
per-object hit testing ambiguous - which is exactly the wrong property for an
irreversible action.

The renderer is genuinely panel-independent: `create()` takes a parent, a
`PrinterExcludedObjectsState*`, bed dimensions, and a `PrintExcludeObjectManager*`
(`include/ui_exclude_object_map_view.h:26-79`). Polygon data is global state -
parsed from the `exclude_object` subscription
(`src/api/moonraker_discovery_sequence.cpp:1178`) into
`PrinterExcludedObjectsState::object_geometry_`
(`include/printer_excluded_objects_state.h:36-43`, `:228`).

Four things must change for it to work as a tile:

1. **`static ExcludeObjectMapView* g_active_map_view`** (`ui_exclude_object_map_view.cpp:21`)
   makes the class single-instance; with a tile and the print-status view both live,
   the close button routes to whichever was created last. Replace the global with
   per-instance routing.
2. **No resize path.** `create()` reads `lv_obj_get_width/height(plate_area_)`
   immediately after `lv_obj_update_layout` (`:218-238`) to build the mapper and the
   canvas buffer, and there is no `LV_EVENT_SIZE_CHANGED` handler anywhere in the
   class. A tile whose real size arrives later via `on_size_changed` would build a
   0x0 mapper (scale 0, every rect collapsed to the 28 px floor) and no canvas. Add
   a rebuild-on-resize path.
3. **`ignore_layout="true"` with a 100%/100% root** (`ui_xml/components/exclude_object_map.xml:6-8`)
   assumes it covers its parent. The mini mode must take a normal layout slot.
4. **A mini render mode**: outlines only - no key bar (`style_max_height="48"`), no
   dims label, no close button, no 22x22 number badges. Those are fixed-size and
   dominate a small tile.

Availability: bind `defined_objects_count`, which is state-owned and global
(`src/printer/printer_excluded_objects_state.cpp:32`, written at `:72`) and
currently bound by no XML. Do **not** use `exclude_objects_available` or
`exclude_map_active` - both are `PrintStatusPanel`-owned
(`src/ui/ui_panel_print_status.cpp:142`, `:475`), and `exclude_map_active` exists
purely to hide that panel's own thumbnail.

Spans (tracks): default `4x4`, min `4x4`, max `8x6`.

## Carried fix: pre-print in the detailed layout

`ui_xml/components/panel_widget_print_status.xml:162-175` binds `print_start_phase`,
`print_start_progress`, `print_start_message`, and `print_start_time_left` in a
`print_card_preparing_info` block, and `PrintStartCollector` drives those subjects
app-globally (`src/print/print_start_collector.cpp`, owned by `MoonrakerManager`),
so they populate whether or not any panel exists.

But that block lives in `print_card_layout`, hidden unless `print_status_view == 3`
(`:152`). `update_view_subject()` (`src/ui/panel_widgets/print_status_widget.cpp:637-646`)
selects view `4` for the detailed non-compact layout, and
`ui_xml/components/print_status_detailed_active.xml` contains **no** `print_start_*`
bindings at all. During the ~95 s preparing window the detailed card therefore shows
an arc at ~0% and `"0h 00m / 0h 00m"` with no phase indication.

**Change:** add the preparing block to `print_status_detailed_active.xml`, bound to
the same subjects and hidden on `print_start_phase == 0` (IDLE). Phases are
enumerated at `include/printer_state.h:156-168`.

## Per-widget authoring checklist

For each of the seven, in this order:

1. `src/ui/panel_widgets/<name>_widget.{h,cpp}` - subclass `PanelWidget`, implement
   `attach()`, `detach()`, `id()`. Branch on `width_px`/`height_px`, never spans.
   Instances are recycled across rebuilds, so any imperative apply must also run
   from `attach()`, not only from `on_size_changed()`.
2. Forward-declare `register_<name>_widget()` in the block at
   `src/ui/panel_widget_registry.cpp:15-49` and call it in
   `init_widget_registrations()` (`:165-199`). There is no static self-registration.
3. Registry row in `s_widget_defs` (`:66-108`), spans in tracks, all even.
4. `ui_xml/components/panel_widget_<id>.xml` - the component name must be exactly
   `panel_widget_<id>` unless `get_component_name()` is overridden.
5. Register it in `src/xml_registration.cpp:495-545`. Skipping this fails the
   content-fit sweep's `unbuildable == 0` assertion, not just runtime.
6. Add an 8-entry row to `expected` in `tests/unit/test_registry_span_bands.cpp:173-211`,
   keyed by the widget id. Non-optional - `REQUIRE` at `:226` fails a missing key.
   Practical method: add the row with placeholder bands, run, read the `INFO`
   output, pin the real values.
7. Run `./build/bin/helix-tests "[content_fits]"`. `RegistryWidgetHarness` sweeps the
   registry automatically - nothing to register. If the widget clips at its own
   minimum, fix it; only add a `kKnownClipping` entry
   (`tests/unit/test_widget_content_fits.cpp:135-185`) with a written reason if the
   clipping is legitimate.
8. Seed layouts are optional. An absent span now falls back to the registry
   (`src/system/panel_widget_config.cpp:99-111`). **Avoid editing
   `assets/config/default_layout.json` and the preset anchor tables** - three layout
   design questions about those are parked awaiting Preston on this branch.
9. **Regenerate derived artifacts.** These gates fail *after* the widget looks
   finished, in shards unrelated to the tag being iterated on, so a green
   `[content_fits]` run proves nothing about them:
   - `make regen-tokens` - **required for every new or edited `ui_xml` file.** Seven
     new components change the design-token set and stale
     `src/generated/theme_token_table.cpp`; the failure surfaces as
     `REQUIRE(table == scanned)` full of `{?}` placeholders under `[theme][tokens]`,
     roughly shard 83.
   - `make translation-sync` then `make translations` for any new user-facing string,
     staging the YAMLs **and** the regenerated `ui_xml/translations/*.xml`. Wrap new
     strings in `lv_tr("...")` in C++ or `label_tag="<literal>"` in XML. Every one of
     these seven widgets introduces labels, so this applies to all of them.
   - `make regen-fonts` only if a widget introduces a new icon codepoint in
     `include/ui_icon_codepoints.h`.
   - `make regen-xml-schema` is **not** needed here. The XML linter schema tracks
     widgets registered through `lv_xml_register_widget`; all seven of these are
     `PanelWidget` subclasses composing existing components, not new XML widgets.
     (It *would* be needed if the toolhead draw surface were promoted to a
     registered widget rather than kept private to its panel widget.)

Registry-wide tests a new row enters automatically: `test_grid_layout.cpp:420`
(min<=max, default in range), `test_panel_widget_config.cpp` sweeps,
`test_default_layout.cpp:600` (every non-multi-instance def appears exactly once in
the default grid), `test_panel_widget_manager.cpp:189` (every `hardware_gate_subject`
must be XML-registered - relevant for `dryer_supported`, `perf_available`,
`printer_has_bed_mesh`).

## Sequencing

All seven widgets touch the same three shared files - `panel_widget_registry.cpp`,
`xml_registration.cpp`, and the band table in `test_registry_span_bands.cpp`.
**Implement them serially in this one worktree.** Parallel agents would collide on
every one of those files, and the registry table in particular is a single
contiguous array where concurrent edits produce silent merge damage.

Batch 0 lands first and is verified before any widget is written, because five of
the seven depend on one of its extractions.

## Testing

- **Gates per batch:** `./build/bin/helix-tests "[span_bands]" "[content_fits]"
  "[widget_def]" "[default_layout]" "[panel_widget_config]" "[panel_widget_manager]"
  "[grid_edit]" "[card_merge]"`.
- **Full suite:** `make test-run` (sharded). Never pipe it through `tail` - that
  reports the filter's exit code and red reads as green. Redirect to a file and grep.
- **Unit tests for Batch 0**, written before the extractions: clamp behavior at both
  ends for `helix::tune`; `helix::zoffset::adjust` rounding, clamping, pending-delta
  accumulation, and the `MOVE=1` gate flipping with `homed_axes`;
  `helix::bed_dimensions` fallback chain with each source absent in turn;
  `BedCoordMapper` Y-flip and center-origin math.
- **Per-widget size tests** using the typed `PanelWidgetHarness<W>`
  (`tests/test_helpers/panel_widget_size_harness.h:115-147`) for any widget with
  progressive disclosure - `host_health` at minimum, since its content changes across
  three pixel bands.
- **Mutation-verify one new test per batch**, not all of them; each relink is ~90 s.
- **Runtime verification** with `--test` and a pinned socket and config dir, driven
  by `helix-screen ctl` with `SDL_VIDEODRIVER=dummy`. The dryer widget needs
  `HELIX_MOCK_DRYER`. Widgets needing an active print use `--sim-speed 4-10`.

## Out of scope

- The calibration-status tile (originally item 8) - dropped.
- Half-cell support for any new widget. No new widget sets `supports_half_col` or
  `supports_half_row`; `test_grid_layout.cpp:438-470` pins the current set of five
  and adding a sixth is a separate decision.
- The three parked layout design questions and the known-red
  `test_grid_edit_drag_path.cpp:285-286`, both owned by the square-cell work.
- Task 12 (AMS mini-status) and the `LAYOUT_SYSTEM.md` span-authoring rewrite
  (Task 13), both open on `fix/grid-cell-metrics`.

---

# Batch 0 outcome and inputs to later batches

Batch 0 (the DRY refactors) is complete: 13 commits on `feature/home-widgets`,
branched from `fix/grid-cell-metrics`. Plan:
`docs/devel/plans/2026-08-12-home-widgets-batch0-dry.md`.

## What Batch 0 delivered

| Consolidation | Result |
|---|---|
| `helix::BedCoordMapper` (mm→px, Y-flip, center-origin) | one implementation tree-wide |
| `helix::bed_dimensions()` (build_volume → axis_bounds → 235x235) | one fallback chain |
| `helix::tune::set_speed_percent/set_flow_percent` | one M220/M221 formatter, one clamp |
| `helix::zoffset::adjust()` | one baby-step path; ±2.0mm clamp has one definition |
| `kZStepAmountsMm` / persisted step index | one step table, now persisted per printer |
| `helix::axis_is_homed()` | **added, but NOT yet consumed — see below** |

Bug fixed along the way: `pending_z_offset_delta` was never cleared in production,
so the "unsaved Z-offset" banner lied for the rest of the session after a save. Now
cleared on both success paths (`FIRMWARE_MANAGED` and `APPLY`→`SAVE_CONFIG`) across
all three call sites.

## Must be resolved before or during Batch 1

1. **`test_widget_content_fits` leaks a thread.** The "every home widget renders its
   content at its authored minimum" sweep prints
   `[ISOLATION-LEAK] ... leaked 1 thread(s): 8 -> 9 (likely an unjoined
   hv::EventLoopThread → later UAF crash)`, which is why an unfiltered
   single-process `helix-tests` run SIGSEGVs. Pre-existing on
   `fix/grid-cell-metrics`, not caused by Batch 0. **Own this before Batch 1**: all
   seven new widgets enter that same sweep, and the leak becomes far harder to
   attribute once they are stacked on top of it. Use sharded `make test-run` until
   it is fixed.
2. **`SAVE_CONFIG` mock defect.** `src/api/moonraker_client_mock.cpp:2011-2015`
   returns 1 where sibling handlers (`BED_MESH_CALIBRATE`, `PID_CALIBRATE`) return 0
   with the comment "Success - results come asynchronously via gcode_response";
   `moonraker_client_mock_print.cpp` treats any nonzero as an RPC failure. Effect: no
   test can drive `apply_and_save` to success through the real
   `Z_OFFSET_APPLY_PROBE`→`SAVE_CONFIG` chain, so the primary Z-offset save path is
   untestable and Task 6's fix is verified only on the firmware-managed path. A grep
   of all 30 `SAVE_CONFIG` hits in `tests/` found **zero assertions** depending on
   the current failure behaviour, so the blast radius appears to be nil — but it is
   shared test infrastructure, so it needs its own change plus one sharded run.
3. **No unit test file exists for `PrintTuneOverlay`.** `[print_tune]` matches
   nothing. The new `[z_offset]` tests cover the extracted core, which is the part
   the widget consumes, but the overlay itself is uncovered.

## Carried to Batch 2 — finish the `homed_axes` consolidation

`axis_is_homed()` and `toolhead_is_homed()` exist and are tested, but
`axis_is_homed()` has **no production callers yet** and five sites still open-code
the decode:

- `src/ui/ui_panel_controls.cpp:206-208` — publishes `x_homed`/`y_homed`/`xy_homed`/`z_homed`/`all_homed`
- `src/ui/ui_panel_motion.cpp:465-467` — publishes prefixed `motion_*` copies
- `src/ui/ui_panel_motion.cpp:686-687` — gates jog soft-stop
- `src/ui/ui_overlay_qr_scanner.cpp:266` — gates a bed-lowering move
- `src/print/print_start_collector.cpp:467-468` — full XYZ, equivalent to `toolhead_is_homed()`

The first two own **published subjects that XML binds**, so converting them means
changing how the subjects are computed, not deleting them. This wants its own task
with its own tests, not a drive-by during widget work.

## Smaller items worth folding into the batch that touches them

- `ui_print_tune_overlay.cpp` computes the Z-offset indicator's micron value with a
  truncating `static_cast<int>(mm * 1000.0)` instead of `std::lround`, so it can drop
  a micron on floating-point edges. Pre-existing; the Batch 1 Z-offset widget will
  copy this line, so fix it there.
- `ControlsPanel::speed_override_subject_` / `update_speed_display()` are now
  write-only — no `ui_xml/` file binds `controls_speed_pct`. Batch 0 removed the flow
  twin for exactly this reason but kept the speed one because `update_speed_display()`
  still writes it from two live call sites.
- `docs/devel/ARCHITECTURE.md`'s "HomePanel Integration" sample still shows a
  single-page `populate_widgets()` while the real one loops over `populate_page()`.
- `tests/unit/test_print_controls_char.cpp:167-181` re-encodes the M220/M221 format
  and the 50-200 / 75-125 ranges in local mirror helpers, so it stays green if
  `helix::tune`'s constants change.
- `docs/devel/LAYOUT_SYSTEM.md` § "Widget span authoring" is still written in whole
  cells rather than half-cell tracks. Owned by Task 13 on `fix/grid-cell-metrics` —
  coordinate rather than editing it.

---

# Documentation and screenshots (missing from the original scope)

The seven-widget scope covered code, XML, and tests but **not docs or screenshots**.
Both are required. None of what follows is written down anywhere in the repo today —
it is all convention-by-example, inferred from what shipped widgets did. Writing the
convention down is itself a task (see the last item).

## User docs — all seven go in `docs/user/guide/home-panel.md`

The established convention is **one guide page per navigable panel, never per widget**.
Every home widget that has ever shipped is a section inside `home-panel.md`. Do not
create new guide pages; if you did, `docs/CLAUDE.md` requires updating that file,
`docs/README.md`, and `docs/user/CLAUDE.md`'s index.

Per widget, that file needs:

1. **A row in the right `## Available Widgets` category table** (`:205-297`) —
   columns are `Widget | Description | Default | Min | Max | Resizable | Hardware Required`.
2. **A row in `## Widget Interactions`** (`:328-370`) — `Widget | Tap Action`, or
   `— (display only)`.
3. **A row in `### Hardware-Gated Widgets`** (`:280-301`) only if the widget sets a
   `hardware_gate_subject` — that is dryer (`dryer_supported`), bed mesh
   (`printer_has_bed_mesh`), and host health (`perf_available`).
4. **An optional `##` deep-dive section** with its own screenshot. Precedent is that a
   widget earns one when it has non-obvious state or its own config UI — see
   `## Clog Detection Widget` (`:408`) as the template. Likely candidates: speed/flow
   tuner, Z-offset, dryer, exclude-objects map.

> **Unit trap.** `home-panel.md`'s size columns are in **whole cells** with an explicit
> note at `:207` ("2x1 means 2 columns wide and 1 row tall"), while `s_widget_defs` now
> stores **half-cell tracks**. Divide the registry values by 2 when writing these rows,
> or the new rows will silently contradict every existing one. The square-cell plan
> already flagged that the existing columns need doubling — coordinate rather than
> fixing it twice.

**Cross-link bidirectionally** to the existing feature docs rather than duplicating
them. The pattern already exists twice (`docs/user/guide/sensors.md:78` and
`docs/user/guide/settings/led-settings.md:5`). Targets:

| Widget | Existing coverage to link |
|---|---|
| Speed / flow | `guide/printing.md:145` § Print Tune Overlay |
| Z-offset | `guide/printing.md:169` § Z-Offset / Baby Steps (also `calibration.md:125`) |
| Dryer | `guide/filament.md:364` § Filament Drying |
| Host health | `guide/settings/system.md` § Performance |
| Bed mesh | `guide/calibration.md:9` § Bed Mesh |
| Exclude objects | `guide/printing.md:196` § Exclude Object |
| Toolhead position | none — the only genuinely new ground; nearest is `guide/motion.md` |

Style rules bind here (`docs/user/CLAUDE.md`): end-user voice, **never** reference
source files or class names, exact UI paths, and *"screenshots are better than
descriptions"*.

## Screenshots

- **Convention:** 800x480, dark mode, `helixscreen` theme. Every committed doc
  screenshot is that size. Not documented anywhere — inferred from
  `scripts/screenshot-all.sh`, which hardcodes `--dark` and the theme.
- **Committed to** `docs/images/user/<name>.png` (58 tracked files), referenced from
  guide pages as `![Alt](../../images/user/foo.png)`.
- **There is no token or recipe concept for an individual widget.** Recipes in
  `scripts/screenshot-recipes.sh` address panels and overlays; a widget is just part of
  the home screen, and there is no flag to seed which widgets are on the grid. So each
  widget is illustrated by one of three routes, all convention-by-example:
  1. **The overlay it opens** — preferred where applicable, because it is reproducible:
     add a `"<token>  navigate home; click <node>"` line to `SCREENSHOT_RECIPES`, add
     `"output-name:token"` to `PANELS` in `screenshot-all.sh`, then
     `HELIX_THEME=helixscreen ./scripts/screenshot.sh helix-screen <name> <token> --dark --test`
     and move `/tmp/ui-screenshot-<name>.png` to `docs/images/user/`.
  2. **The whole home screen** — only works if the widget is in the default grid.
  3. **A hand-crop** of that shot. Precedent: `home-carousel-modes.png` (237x237),
     `home-job-queue.png` (306x168), `home-widget-trash.png` (239x262), all committed in
     `ef268139b`, none scripted. No tooling exists for the crop step.
- **No gate anywhere.** No CI job regenerates, verifies, or diffs doc screenshots, and
  `scripts/check_doc_refs.py` does not check `.png` or Markdown image links, so a broken
  image reference is invisible. Nothing fails if a screenshot is missing or stale.
- `home-panel.md` uses HTML-comment placeholders for shots not yet taken (e.g. `:7`) —
  a usable way to land docs ahead of images.
- **Golden tests are not a concern:** `home` is deliberately excluded from
  `tests/ui/goldens/` because the mock's simulation thread drifts temperatures. New home
  widgets cannot break the golden suite unless one alters the `bed-mesh` or `zoffset`
  screens.

## Other artefacts that go stale when a widget is added

- **`CHANGELOG.md`** `## [Unreleased] / ### Added` — feeds `scripts/generate-whatsnew.sh`.
- **`docs/user/CONFIGURATION.md:1156-1175`** — a *second* widget-ID enumeration, listing
  14 IDs against 37 shipped. Already rotten; seven more widens it. Either refresh it or
  give it the same treatment `ARCHITECTURE.md` got and replace it with a registry
  pointer. Also `:1153` names which widgets use the `config` key.
- **`README.md:33` and `:102`** — "30+ widgets", twice. 37→44 makes "40+" honest.
- **`docs/user/FAQ.md:284`** — claims "up to 10 widgets" (no such cap) and
  "Settings → Home Widgets" (it is long-press Edit Mode). Both already wrong.
- **`docs/devel/ARCHITECTURE.md:458`** — hardcoded "it currently holds 37 entries", the
  one count left after the table became a pointer. Bump it or drop the number.
- **Translations.** XML labels are obligatory: `make translation-sync` then
  `make translations`, staging both `translations/*.yml` (9 languages) and the
  regenerated `ui_xml/translations/*.xml`. Separately, and pre-existing: the widget
  catalog's `display_name` / `description` / `hardware_gate_hint` go through `lv_tr()`
  at runtime but the extractor only scans XML, so **the entire Widget Catalog is
  English-only today**. The seven will inherit that. Not this work's job to fix, but do
  not perpetuate it silently.
- **`docs/devel/480x320_UI_AUDIT.md`** — sanity-check the seven at the smallest tier.

## Default layout — ruling

`tests/unit/test_default_layout.cpp:600` asserts every non-multi-instance registry def
appears **exactly once** in the default grid, so each new widget row would otherwise
fail that test the moment it lands, and this spec's step 8 says to avoid editing
`assets/config/default_layout.json` while three layout questions sit parked.

**Ruling (Preston, 2026-08-12): a new default layout will be authored once all seven
widgets exist.** So:

- Do **not** re-author `default_layout.json` per widget as Batches 1-3 proceed.
- Expect `test_default_layout.cpp:600` to go red as soon as the first
  non-multi-instance widget row lands, and treat that as **known and accepted**, not a
  regression to chase. Record it in the batch ledger so it is not rediscovered.
- Author the new default layout as the final task of Batch 3, covering all seven at
  once, and land it together with the anchor tables for every breakpoint tier plus the
  per-printer preset seeds (`assets/config/panel_widgets/{cc1,ad5x,ad5x_zmod}/home.json`).
  `test_default_layout.cpp` goes green again there, and the three parked layout
  questions get answered in the same pass rather than piecemeal.
- Every span written into those tables is in **half-cell tracks**, and
  `check_anchor_table()` enforces per-tier column/row budgets and pairwise
  non-overlap — so this is a real task, not a formality.

## Write the convention down

The 9-step authoring checklist in this spec contains zero documentation or screenshot
steps, and nothing in `docs/devel/` states them either. Add a "Documentation and
screenshot obligations" step to the checklist and mirror it into
`docs/devel/ARCHITECTURE.md` § Panel Widget System. If instead you create a new
`docs/devel/*.md`, it **must** be listed in `docs/devel/CLAUDE.md` or
`scripts/check_doc_refs.py --index` fails in `scripts/quality-checks.sh`.
