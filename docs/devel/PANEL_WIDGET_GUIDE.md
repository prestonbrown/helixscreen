# Panel Widgets (Developer Guide)

How home-dashboard widgets are built, and the reference pattern for making
one size itself responsively. The nozzle-temps widget is the exemplar to copy:
`src/ui/panel_widgets/nozzle_temps_widget.{h,cpp}`,
`src/ui/panel_widgets/nozzle_layout.h`, and its three XML components. Seven widgets
are built on the shared size ladder (`speed_flow`, `z_offset`, `dryer`, `host_health`,
`toolhead`, `bed_mesh_tile`, `exclude_map`); `speed_flow` is the smallest worked example:
`src/ui/panel_widgets/speed_flow_widget.{h,cpp}`,
`src/ui/panel_widgets/speed_flow_layout.h`, and its two XML components. The ladder,
the helpers around it and the checklist every one of them satisfies are in
[Shared helpers: the size ladder, content chrome, tile ownership and value status](#shared-helpers-the-size-ladder-content-chrome-tile-ownership-and-value-status)
and [The sized-widget family](#the-sized-widget-family). The eighteen
centred-icon tiles run the same pattern one instance deeper; see
[The tile instance of the pattern](#the-tile-instance-of-the-pattern) below, and the
printer image's live callouts are a third instance
([The printer image callouts instance](#the-printer-image-callouts-instance)).

**Related**: `LAYOUT_SYSTEM.md` (the home grid that sizes tiles),
`LVGL9_XML_GUIDE.md` (bindings), `ARCHITECTURE.md` (subjects).

---

## The pieces of a widget

| Piece | Where | Notes |
|-------|-------|-------|
| Registry def | `src/ui/panel_widget_registry.cpp` | id, display name, icon, category, default/enabled, colspan/rowspan and their min/max, whether it may span half cells, `scope` |
| Widget class | `src/ui/panel_widgets/<name>_widget.{h,cpp}` | extends `PanelWidget`; implements `attach()` / `detach()` / `on_size_changed()` |
| XML component | `ui_xml/components/panel_widget_<name>.xml` | the entire appearance |
| Row/child components | `ui_xml/components/<name>_*.xml` | repeated fragments the widget creates per item |
| Registration | `register_<name>_widget()` | factory + `register_widget_subjects()` (see below) |
| Placement | `assets/config/panel_widgets/<preset>/home.json` | per-printer-preset seeds; users rearrange at runtime |

Choose `scope` (`include/panel_widget_registry.h#WidgetScope`) for every new def. The default,
`Printer`, is right for anything that describes the printer or its host: temperatures, fans,
macros, LEDs, cameras, Moonraker perf data. Use `Screen` only when the tile describes the
device it is drawn on (its own network link, clock, lock, tips, notifications, host power).
A screen-scoped widget is left out of the home layout the printer's own screen shares, and
the receiving screen keeps its own default for it (see "Layouts shared by the printer's own
screen" in `LAYOUT_SYSTEM.md`).

Not every definition is compiled in. A Lua plugin registers its widgets at load
time as runtime definitions (`register_runtime_widget_def`,
`include/panel_widget_registry.h`): they land in the catalog's Plugins category,
are never enabled by default, and their ids carry the plugin's `<id>__` prefix.
The saved layout keeps ids it has no definition for
(`PanelWidgetConfig::parse_widget_array` in `src/system/panel_widget_config.cpp`),
so a plugin widget's placement survives a disabled or absent plugin and returns
when the definition is registered again. The registry bumps a generation counter
on every runtime register and unregister (`runtime_widget_generation`), because a
plugin reload re-registers the same ids while the factories now build widgets
bound to a different runtime.

## The reference pattern: measure, decide, publish, bind

A widget that sizes itself responsively is four layers with exactly one
responsibility each. `NozzleTempsWidget` (#1613) is the working instance.

**1. A pure decision function, no LVGL.** All sizing logic lives in a
header-only function next to the widget (`nozzle_layout.h`) taking measured
pixel inputs and returning a small verdict struct. It is unit-testable
without a display, and the tests' fixtures are the widths the widget really
measured, so the boundaries they pin are the live ones. Split the decision
the way the questions actually split — for nozzle-temps, the font tier is a
HEIGHT question (a stack that overflows the tile overflows it at any width)
and the label rung a WIDTH question (a row is one line tall whatever it
says, so label richness never costs height). When two axes both matter,
model both explicitly rather than folding one into constants.

**2. Measurement in the widget, one composer for render and measure.**
`on_size_changed()` measures text in the fonts the rows actually render, and
the strings it measures come from the same composer that `update_row_display`
renders — measurement and rendering cannot drift apart, or the ladder decides
on widths the row does not draw. Measure the values the rows are ACTUALLY
showing, not worst-case glyphs: an "888°" budget excludes normal-font rungs
from tiles where every real value fits.

**3. Publish the verdict as subjects.** One int subject per axis
(`nozzle_row_label_mode`, `nozzle_row_columns`, `nozzle_row_compact`),
registered via `register_widget_subjects("<id>", fn)` so they exist before
any XML that binds them is parsed — the parser permanently skips a binding
whose subject does not exist at parse time. `on_size_changed()` becomes
measure-and-publish with zero widget calls. Enum values ARE the subject ints
(`NozzleLabelMode`), so XML `ref_value`s cannot drift from C++.

**4. Bind every appearance in XML.** Row widths, container flow, label
visibility, fonts — all `bind_flag_if_*` / `bind_style_if_*` off the
subjects. A row created by a late rebuild reads the current values at
creation and agrees with its siblings with no seeding pass. Prefer one
threshold bind (`bind_flag_if_lt subject=... ref_value="2"`) over enumerated
equality pairs; it reads as the predicate it is. A style the binds apply lives in
the widget's own `<styles>` block when one file uses it; when several widgets bind
the same look, it lives once in `ui_xml/styles.xml` and every file binds it by
dotted name (`bind_style_if_eq name="styles.<name>" ...`).

### Degradation design

Let content degrade before identity does: the nozzle-temps value drops its
target half (one tap away in a detail view) before the row drops its tool
number, and the number before the icon. Budget each rung for what it
ACTUALLY draws at that rung — the target-hiding rungs measure the
current-only value. Re-decide when the value's width class changes (a
target setting or clearing), so a tile sized under idle values never draws
the wide value in the narrow rung.

### Shared helpers: the size ladder, content chrome, tile ownership and value status

Four small headers carry the parts of the pattern that more than one widget needs.
`SpeedFlowWidget` is built on all four.

**`src/ui/panel_widgets/size_rung.h`** is the size ladder and its one chooser.
`helix::SizeRung` names the rungs a widget can draw at: `Glance` (0.5x0.5), `Strip`
(1x0.5), `Column` (0.5x1), `Cell` (1x1), `Wide` (2x1), `Full` (2x2 and larger) and
`Large` (3x3 and larger). The enum values are the subject ints, so the XML `ref_value`s
cannot drift from C++. A widget fills a `RungNeeds`, one `RungBox` per rung it offers
(`rung_box(w, h)` marks a rung offered; one left default is never chosen), with the
minimum content box that rung draws in for the state being decided.
`helix::choose_size_rung(content_w, content_h, needs)` returns the largest offered rung
whose box fits on both axes, in the order Large, Full, Wide, Cell, then Strip or Column
(a box that holds both takes the one matching its aspect), then Glance.
`helix::holds_any_rung()` says whether the box holds any offered rung at all, which is
what `fits_at()` reports; it is monotonic because every comparison is `>=`. The nominal
sizes are names only: a box between rungs takes the largest rung it holds, and the
boxes themselves come from measurement. A widget may offer different rungs in different
states: the bed mesh tile with no mesh loaded offers Glance, Cell, Wide and Full only,
and the state is part of what `needs` was measured for.

**`src/ui/panel_widgets/content_chrome.h`** measures the box the content really draws in.
`helix::measure_content_chrome(root, reserve_px)` walks the single-child chain from the
widget's root down to the first container that holds more than one child and sums the
padding of every container on the way, plus the flex gap of that branching container as its
`BranchGap` says. `Visible` (the default) charges the gap when more than one child is
visible: flex leaves hidden children out, so a part that is hidden is not charged the gap
beside it. Visibility is the last verdict's, though, so a ladder whose parts change by rung
decides on padding alone (`content_padding_or_estimate()`, `BranchGap::None`) and counts
each rung's gaps in its needs, or charges the gap whatever is visible (`BranchGap::Always`,
speed/flow); otherwise a box near a boundary decides one rung from below and the other from
above. TileSizing takes `None`: `decide_tile_layout()` prices the gap before every part a
candidate draws, and a part outside the glyph/value/label stack (bypass's spool row) is
counted as `TileSizing::Content::detail_font` while it is drawn. `PanelWidgetHarnessBase::unsettled_boxes()` finds those
boxes in a test. The
widget subtracts the result from the span's pixel size and decides on what is left, so a
theme or breakpoint that changes the tile's padding moves the boundaries with it. The
tree does not exist yet when the load path asks `fits_at()`, so each widget's `decide()`
falls back to the root's padding from `theme_manager_get_spacing()` until `root()` is
set (`src/ui/panel_widgets/speed_flow_widget.cpp#decide`).

**`src/ui/panel_widgets/live_tile_owner.h`** says which instance of a single-instance
widget publishes. These widgets keep their subjects at module level, so only the instance
bound to the tile on screen may write them. A second instance exists briefly: a
`fits_at()` probe is never attached and so never claims, and a rebuild's incoming and
outgoing instances can overlap.
`helix::LiveTileOwner<W>` is a stack: `claim()` in `attach()`, `release()` in `detach()`.
When the releasing instance was the owner and another is still attached, `release()`
returns that instance and the caller has it republish, so the tile beneath a transient one
does not go dead (`src/ui/panel_widgets/speed_flow_widget.cpp#detach`). A method that
publishes opens with `if (!s_tile.is_owner(this)) return;`, and `on_size_changed()` stores
its verdict first and publishes only when it owns.

**`include/value_status.h`** is the one classification for a reading a widget colours.
`helix::ValueStatus` is `Normal = 0`, `Warn = 1`, `Danger = 2`, the same ints as the
`ams_env_ind_<n>_humidity_status` subjects. Each function in `helix::status` returns a
classification and never an appearance: the caller publishes the int and its XML maps
it with `bind_style_if_eq` to whatever that widget draws.

| Function | Reads | Result |
|----------|-------|--------|
| `status::override_status(pct)` | a speed or flow override | Warn whenever it is off 100% |
| `status::z_offset_status(save_available)` | whether an unsaved offset exists | Warn while there is one to save |
| `status::mesh_flatness_status(range_mm)` | a bed mesh's Z range | Normal below 0.15 mm, Warn below 0.30, Danger above |
| `status::maintenance_status(used_fraction)` | a maintenance meter's used share of its interval | Normal below 0.75, Warn from there, Danger once due |
| `status::host_health_status(HostHealthInput)` | host CPU, CPU temperature, Pi throttle bits, busiest MCU load, MCU retransmits, gcodes disk free | the worst status, with the `HealthCause` that set it |

A metric the printer does not report stays at its `-1` default in `HostHealthInput` and is
never judged. When metrics tie at the worst status, the one earlier in `HealthCause` wins,
so the cause does not flicker.

How speed/flow uses them (`src/ui/panel_widgets/speed_flow_widget.cpp`):

- **Decide.** `helix::decide_speed_flow_layout()`
  (`src/ui/panel_widgets/speed_flow_layout.h#decide_speed_flow_layout`) calls
  `choose_size_rung()` and derives the widget's flags from the rung and the state: the
  measured mm/s and mm³/s readings show beside or under the percentages from `Cell` up
  while printing, the -/+ steppers show at `Full` while printing, and below `Cell` the
  "physical units" setting puts the measured reading in the percentage's place, only
  where it fits the rung the percentages already chose.
- **Measure.** `SpeedFlowWidget::decide` measures each rung's box with the same
  composer the labels render (`compose_texts`), against the content box that
  `helix::measure_content_chrome()` leaves after the tree's padding. Unlike nozzle-temps
  it budgets the widest readings the labels can show (the percentages' clamp maximums and
  fixed ceilings for the measured readings), so the rung holds still while the numbers
  move; it re-decides when the print starts or ends or the units setting flips, because
  those change what each rung draws.
- **Publish.** Module-level subjects registered through `register_widget_subjects()`,
  written only by the `LiveTileOwner`'s owner.
- **Refuse.** `fits_at()` accepts a size only when the `Glance` content fits both idle
  and printing; the units setting never refuses a size.
- **Bind.** `speed_flow_part.xml` binds every part's visibility, flow and font off
  `speed_flow_rung`, `speed_flow_show_measured` and `speed_flow_show_steppers`, and
  the value colour off `speed_flow_speed_status` / `speed_flow_flow_status`.

### The sized-widget family

Each widget below is the same four layers over its own header pair. The decision header
is pure and has a boundary test; the widget measures, decides and publishes.

| Widget | Decision header | Gate subject | What it draws, by rung |
|--------|-----------------|--------------|------------------------|
| `speed_flow` | `speed_flow_layout.h` | - | override percentages; measured mm/s and mm³/s from Cell while printing; -/+ steppers at Full while printing |
| `z_offset` | `z_offset_layout.h` | - | the live offset (two decimals at Glance, three above); Save from Wide; baby-step buttons at Full; the tool name at Full on a tool changer |
| `dryer` | `dryer_layout.h`, `dryer_model.h`, `include/dryer_summary.h` | `home_dryer_available` | a time-left readout with a progress ring where progress is known; one row per source at Full |
| `host_health` | `host_health_layout.h` | `perf_available` | a status chip with the reading that set it; sparklines and MCU rows by rung; the busiest MCU in the memory row's place while printing at Wide |
| `toolhead` | `toolhead_layout.h`, `toolhead_plate.h` | - | homed letters (Z height while printing) at Glance and Strip; axis rows at Column and Cell; a top-down plate with crosshair, Z bar, 50 mm grid and the bed-occupancy outlines from Wide, interactive while homed and idle (tap then Move here, or drag); Home All at Full while idle with an axis unhomed; three configurable preset slots and a lift line at Full when `presets_extra_h` fits (a 2x3); the Move here and Cleared chips go icon-only when the plate is narrower than the labelled chip (`chip_icon_only`) |
| `bed_mesh_tile` | `bed_mesh_tile_layout.h` | `printer_has_bed_mesh` | a grid glyph and Z range at Glance; the heatmap from Strip; profile details at Wide; colour scale, footer, Calibrate and Load profile from Full; a static 3D render at Large on a full-tier board |
| `exclude_map` | `exclude_map_layout.h` | `printer_has_exclude_object` | counts of skipped and remaining objects; the mini exclude-object map from Column up; the object printing now at Wide and above; a "Skip objects..." button at Full |
| `job_queue` | `job_queue_tile_layout.h`, `include/job_queue_tile_state.h` | `printer_has_job_queue` | the next job and the queued count; Start next from Strip (icon only at Strip), with the thumbnail and estimate from Wide, and up to three following jobs at Full; "Starts automatically" only while a print runs under `automatic_transition` |
| `print_stats` | `print_stats_tile_layout.h`, `include/print_activity.h` | - | this week's hours on one line at Strip; hours stacked over "this week" at Cell; seven day bars beside the hours at Wide; bars with weekday initials under them at Full; a lifetime line (prints, hours, filament in meters) where it fits whole |
| `maintenance` | `maintenance_tile_layout.h`, `maintenance_model.h`, `include/maintenance_meters.h` | - | the ring of the meter closest to due with its hours left at Glance; its name and hours on one line at Strip; the ring over the name at Column and Cell; the ring beside a row per meter at Wide; a title, a row and a bar per meter and the total print time at Full. The ring is a square whose side the widget publishes (`maintenance_ring_d`), because an arc draws its circle from its top-left corner |

All ten offer half-cell spans: their registry rows in `src/ui/panel_widget_registry.cpp`
set `supports_half_col` and `supports_half_row` and a one-track minimum, which is the
combination `PanelWidgetDef` requires for a minimum below a whole cell (the flag on that
axis AND a `fits_at()` that can refuse). A Glance tile is therefore a legal 0.5x0.5
placement, and `fits_at()` is what stops a drag from shrinking below the content.

The shared decisions around them:

- **Verdict structs carry `fits`.** `fits = holds_any_rung(...)` is what `fits_at()` reads.
  A widget whose content differs by state (idle and printing, mesh loaded or not) asks
  `decide()` for every state and refuses a size unless the Glance content fits in all of
  them (`src/ui/panel_widgets/dryer_widget.cpp#fits_at`).
- **Full is a size class as well as a fit.** `host_health` and `exclude_map` carry a
  `full_min_h` in their input and raise the Full box's height to it, because a 2x1 tile
  can hold the Full content but the rung is for 2x2 and up
  (`src/ui/panel_widgets/host_health_layout.h#decide_host_health_layout`,
  `src/ui/panel_widgets/exclude_map_layout.h#decide_exclude_map_layout`).
- **Re-decide when what is measured changes.** Each widget stores the state its verdict was
  decided for (`decided_printing_`, `decided_loaded_`, `decided_mcu_count_`, ...) and calls
  `relayout_for_granted_size()` when the live state differs, so a print starting never
  leaves a tile sized for idle content.
- **Aggregation lives in a pure header, rendering in the widget.** The dryer widget reads
  `helix::DryerSummary` from `helix::dryer_model::summary()`
  (`src/ui/panel_widgets/dryer_model.h`), and `summarize_dryers()` in
  `include/dryer_summary.h` is the only place box, chamber and bed drying are folded
  together. See [FILAMENT_ENVIRONMENT_ZONES.md](FILAMENT_ENVIRONMENT_ZONES.md#the-dryer-summary).
- **State that outlives the tile lives in a model.** The maintenance meters accrue print
  time whether or not their tile is on a page, so `helix::maintenance_model`
  (`src/ui/panel_widgets/maintenance_model.h`) owns the state, its subjects and its
  persistence, and is brought up from the widget's `register_widget_subjects()` hook once
  per session. The rules (accrual, the due latch, intervals, the persisted form, the
  plugin-store migration) are pure, in `include/maintenance_meters.h`; the tile only
  measures and publishes its rung.
- **A control the user drags is a drag surface.** The toolhead plate calls
  `helix::ui::mark_drag_surface()` (`include/ui_drag_surface.h`) in `attach()`, so a press
  on it neither swipes the home page nor enters edit mode
  ([HOME_EDIT_MODE.md](HOME_EDIT_MODE.md#entering)). Its `clickable` flag is then bound to
  `toolhead_plate_interactive`: an inert plate takes no press, so the tap reaches the tile
  (opens Motion) and a hold there enters edit mode as anywhere else. `attach()` notifies
  the subject after marking, because marking sets `clickable`.
- **A widget that moves the toolhead shares the Bed tab's rules, not a copy of them.** The
  toolhead plate maps touches with `bed_map_target()`, gates on `plate_moves_allowed()` and
  lifts with `current_plate_lift()` (`include/bed_plate_context.h`), streams drags through
  `helix::BedPlateGesture` (`include/bed_plate_gesture.h`) and sends through
  `helix::ToolheadMoveSender` (`include/toolhead_move_sender.h`), and draws with
  `include/bed_plate_draw.h`, exactly as `MotionPanel`'s Bed tab does. Its preset slots
  (`include/toolhead_preset_slots.h`, configured by `ToolheadPresetConfigModal` from
  `on_edit_configure()`) run a macro through `run_home_macro()` (`include/home_macro_run.h`),
  the Macro Button widget's path.

### Rules every sized widget follows

These are the checks a new widget in this family has to pass. The ones with a gate say so.

- **Design each ladder against measured boxes.** The tile sizes the family's decision
  functions are tested at, from `ctl geom` on a live grid (the content box is smaller by the
  chrome): at 800x480, 55x54 (0.5x0.5), 114x54 (1x0.5), 55x113 (0.5x1) and 114x113 (1x1);
  at 1024x600 the half cells are smaller, 51x53, 108x53 and 51x112, so every half-cell rung
  has to fit those; at 480x272 a 1x1 is 70x64. The micro and tiny tiers decline half-cell
  spans, so there every ladder starts at 1x1. Where a boundary is a size class rather than
  a content fit (how many rows a tall tile holds), decide it from the per-tier bands in
  `include/panel_widget_size.h` (`w_normal()`, `w_wide()`, `h_tall()`, `h_taller()`), not
  a new constant.
- **Too small for a control means read-only.** A rung without room for a control draws
  values only, and a tap anywhere opens the widget's overlay or panel. Controls (steppers,
  Save, Home All, Calibrate) appear only at the rungs the family table names.
- **Above the top rung, add spacing, not content.** A box larger than the widget's highest
  rung spends the extra on spacing unless the widget offers a rung for it (`Large` on the
  bed mesh tile).
- **Re-decide on `print_active`.** It is 1 while printing or paused. Where idle and printing
  content differ, a change re-runs the decision, because what is measured changed.

- **Centre vertically with `flex_track_place`.** Inside a row flow, `flex_cross_place="center"`
  centres children within their track, and the track is only as tall as its tallest child,
  so it sits at the top of a taller box. Add `flex_track_place="center"` to the container
  (`ui_xml/components/panel_widget_speed_flow.xml`, `sf_body_row`;
  `ui_xml/components/panel_widget_z_offset.xml`, `z_offset_top`).
- **A `<style>` that sets `flex_flow` or `flex_main_place` also sets `layout="flex"`.** The
  style path, unlike the inline attribute, does not switch the layout on, and the children
  pile onto the first one. `lint_flex_styles_layout` in `scripts/code_lint.sh` covers
  `panel_widget_*.xml` plus `speed_flow_part.xml`, `chamber_dryer_row.xml` and
  `host_health_mcu_row.xml`; a new component file that carries such styles is added to that
  list.
- **Stop redrawing while the home panel is covered.** `HomePanel` fans `on_deactivate()` and
  `on_activate()` out to the active page's widgets when the panel is covered or revealed and
  when the page changes (`src/ui/ui_panel_home.cpp#on_deactivating`). A widget
  that draws a canvas or refreshes on a timer keeps a `panel_hidden_` flag, skips every
  update while it is set, and applies whatever changed once in `on_activate()`:
  `ToolheadWidget` cancels its 250 ms `CoalescedTimer` and forgets the drawn plate key
  (`src/ui/panel_widgets/toolhead_widget.cpp#on_deactivate`), and `BedMeshTileWidget` skips
  live and mesh updates until the panel returns
  (`src/ui/panel_widgets/bed_mesh_tile_widget.cpp#on_activate`).
- **Re-compose translated text on a language switch.** XML re-translates what it binds
  through `translation_tag`; text C++ formatted with `lv_tr()` stays in the old language,
  and the rung was chosen on widths measured in it. A widget that composes such text holds
  `ui::observe_language_change()` (`include/observe_language.h`) from `attach()`, resets it
  in `detach()`, and re-decides in the handler
  (`src/ui/panel_widgets/dryer_widget.cpp#attach`). A state singleton that owns translated
  text, such as `BedMeshState`, exposes `retranslate()` for the same purpose.
- **Classify every control against the toolhead-safety census.** A control that commands
  the toolhead or starts a print carries `moves_machine="true"` (see
  [LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md#moves_machine)). `tests/unit/test_job_holds_machine.cpp`
  walks every XML file that declares a control and fails unless the file is a row of
  `kGuardedFiles` (with the guard count) or `kNoMachineControlFiles`. The family's rows:
  `panel_widget_z_offset.xml` has two guards (Save and the steppers),
  `panel_widget_toolhead.xml` six (the plate, Move here, Home All and the three preset
  slots), `panel_widget_bed_mesh_tile.xml` one
  (Calibrate); `panel_widget_speed_flow.xml`, `speed_flow_part.xml`,
  `panel_widget_host_health.xml` and `panel_widget_dryer.xml` command no motion and are in
  `kNoMachineControlFiles`. The send layer is the
  guarantee; the disabled state is the courtesy.
- **Give a gate subject an owner row.** A widget with a `hardware_gate_subject` needs that
  name in `gate_subject_lifetime()` (`src/ui/panel_widget_manager.cpp#gate_subject_lifetime`),
  which hands the gate observer the death token of the object that registers the
  subject. `tests/unit/test_gate_subject_lifetime.cpp` has one `owners()` row per gate name,
  proves behaviourally that the token flips when the owner tears down, and fails a registry
  def whose gate has no row. `home_dryer_available` is owned by `dryer_model`, which the
  dryer widget initialises from its `register_widget_subjects()` hook so the gate exists
  before the manager resolves gates.
- **Put module-level subjects and text buffers in PSRAM on the ESP32.** Every `lv_subject_t`
  and `char` buffer at namespace scope in these widgets is declared
  `HELIX_PSRAM_BSS` (`include/helix_psram_attr.h`), as are the state singletons they read
  (`src/printer/bed_mesh_state.cpp#instance`, `src/system/performance_state.cpp#instance`).
  The macro is empty off the firmware. It is for statics first touched after boot and never
  reached by DMA or an ISR, which describes all of them. Without it, a handful of widgets
  moves `.dram0.bss` past its ceiling and WiFi stops associating; see
  [ESP32_PORT.md](ESP32_PORT.md#changing-shared-code).
- **Register the widget's source with the firmware manifest.** A new `.cpp` is a line in
  `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (or the excluded list with a
  reason), or the commit gate fails.

### The tile instance of the pattern

The eighteen centred-icon tiles run the same four layers one instance deeper: a tile has
no content of its own to arrange, so the rung its glyph draws at IS the layout
(prestonbrown/helixscreen#1559). Pure decision: `helix::decide_tile_layout()`
(`src/ui/panel_widgets/tile_layout.h`) takes measured content widths per icon rung and
returns a `TileVerdict` - the rung, the direction (icon above or beside the value), label
visibility, whether the target half is drawn, and `fits`. Content is surrendered in a
fixed order (the target half first, then the label), and within each step the largest
rung that fits wins, so a tile grows its glyph rather than its text.

- **`helix::TileSizing`** (`src/ui/panel_widgets/tile_sizing.h`) is the measure/decide/
  publish layer, one per widget INSTANCE - `fan`, `thermistor` and `power_device` are
  multi-instance, and a type-global subject would make the second instance overwrite the
  first's verdict. It measures the WORST-CASE value strings and publishes four subjects:
  icon rung, label, direction, show-target. Budget the string the tile can ACTUALLY
  render, including any part a sibling label draws: `temp_display` puts the unit in its
  own label, so a heater budgeted as "888 / 888" is narrower than the row it draws, and a
  sensor reading is seven glyphs ("110.0°C"), not three.
- **The verdict is computed against the content box, not the tile.** `TileSizing` measures
  the padding and flex gap of every container between the tile's outer edge and its glyph
  (`set_content_root()`), because that chrome is a theme value that varies by widget and
  by tier. Estimating it picks a rung that spills the tile's own container.
- **Constructor registration is load-bearing.** A tile's subjects are per-instance, so
  they register in the widget's CONSTRUCTOR, never from `attach()`. The manager creates
  the widget, then calls `lv_xml_create()` on its component, and only then `attach()`;
  the parser permanently and silently drops a binding whose subject does not exist at
  parse time, so subjects registered from `attach()` never bind and the tile renders at
  its default appearance forever. The nozzle-temps spelling satisfies the same constraint
  with `register_widget_subjects()`, which runs at registration, before any XML is parsed.
- **Per-instance subject names reach XML through `xml_attrs()`.**
  `PanelWidget::xml_attrs()` returns the flat key/value list `lv_xml_create()` takes; the
  manager passes it when creating the component, and the component's `tile_icon_subject`
  prop carries the instance's rung subject name. Whatever supplies the attrs is
  constructed with the widget, for the same parse-time reason.
- **`PanelWidget::fits_at()` is the refusal path.** Defaults to true; a tile overrides
  it with `TileSizing::fits()`. It must be MONOTONIC (fits at a size means fits at every
  larger size), because the clamp walks outward assuming the first accepting size is the
  nearest one. Edit mode's resize clamp and the load path both consult it through
  `helix::grow_span_to_fit()` (`include/grid_layout.h`), so the rule lives once.
- **`helix::TiledPanelWidget`** (`src/ui/panel_widgets/tiled_panel_widget.h`) owns the
  TileSizing and the four hooks that reach it (`on_size_changed`, `fits_at`, `xml_attrs`,
  `tile_sizing`). A tile with behaviour derives from it and passes its TileSizing
  arguments to the base initializer; a tile with its own fit rule overrides `fits_at()`.
- **`helix::TileWidget`** (`src/ui/panel_widgets/tile_widget.h`) is a `TiledPanelWidget`
  for a tile that has no widget class of its own: `notifications`, `firmware_restart`,
  `humidity`, `width_sensor` and `lock` (rows of `kPureXmlTiles` in `tile_widget.cpp`).
  Every other centred-icon tile owns one on its own class, including the three heaters,
  which share `HeaterTempWidget`, and `power_device` and `filament`, whose classes sit
  outside `src/ui/panel_widgets/`. It is registered LAST and skips any id a class
  already took, because the last factory registration wins and a sizing-only shell
  would otherwise replace real behaviour. A tile that grows real behaviour stops using
  it and derives `TiledPanelWidget` directly.
- **The root's `user_data` is the base's.** `attach_tile()` binds the tile root (and
  `root()` returns it) before `attach()`, `detach_tile()` clears it after `detach()`, on
  every reuse, so a widget never sets or clears it. Anything that hands a widget a tree
  calls those two, not `attach()`/`detach()` directly.

On the XML side, every part of a tile binds its face with
`<bind_tile_rung ladder="icon|value|label" subject="$tile_icon_subject"/>`
(`include/ui_tile_rung.h`). The ladders there are the one table TileSizing measures in
and the binding draws in, and each rung names a TOKEN (`#icon_font_*`, `font_*`), never a
literal face, because a literal face a platform did not link renders tofu.
The icon's top rung, xxl, is a size rather than a token (`#tile_icon_xxl_size`, twice the
tier's xl): it draws in the largest MDI face the build links at or below that size and scales
it up to reach it, at most 2x (`ui::TileFace`), so a tile grows past a cell and a half on every
platform. Which boards link the larger faces is decided in `mk/fonts.mk`.
`styles.tile_column` / `styles.tile_row` in `ui_xml/styles.xml` carry the direction. The seven single-icon action tiles share
`ui_xml/components/home_action_tile.xml`, whose `tile_icon_subject` prop installs the
per-instance rung binding (empty installs none). The Controls panel's calibration cells and
Motors Off use the same component; its props are listed in
[LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md#home_action_tile).

### The printer image callouts instance

`PrinterImageWidget` (`src/ui/panel_widgets/printer_image_widget.{h,cpp}`) runs the pattern
over live chips on the home printer picture (the print status panel's printer tile drives the same engine through `CalloutPresenter`, see [the print status panel](architecture/08-panels-navigation.md#the-print-status-panel)): temperature chips for the nozzle, bed and
chamber, the part fan's speed, and a light chip (prestonbrown/helixscreen#1397). The pure
decision is `helix::compute_callout_layout()` (`include/callout_layout.h`),
which returns a `CalloutMode`, the image rect, and each chip's rect and leader line.

- **The mode ladder.** Image only (one cell on both axes, or an image too short for three
  chip heights); both sides (each side band of the centred image fits a chip column, each
  chip on the side nearer its point); one side (the image moves to the near edge and every
  chip stacks in the far band); pinned (tagged image, no band: chips sit on their points,
  nozzle + fan merge into one toolhead chip, and chips that would overlap slide apart);
  docked (untagged image: chips in the free band, else along the bottom edge, never a
  line). A tile taller than the image's aspect runs the same ladder with bands above and
  below. When neither line mode fits at the contain-fit size, the image may shrink by up
  just enough for one side (up to `kMaxImageShrinkPctOneSide`, 40%), then for both (up to `kMaxImageShrinkPctBothSides`, 20%), before pinned.
- **The budget decides the mode; the active chips get positions.** `CalloutLayoutInput`
  carries both: `budget` is every chip this printer can ever show at its widest text,
  `active` is what shows now. Fitting against the budget is what keeps the image still
  as chips come and go. A capability (`printer_has_led`, `Capability::HasChamber`: a
  chamber sensor or heater) changes the budget, so its observer relayouts even when no
  chip text changes.
- **Two expendable flags: one fixed per phase, one live.** `CalloutPresenter::Chip`
  carries `budget_expendable` (may this chip's room be shed in this phase; the print
  status tile sets it from `phase_budget_expendable()` in `include/print_status_chips.h`)
  and `expendable` (the live reading is an expected value). The pass that picks the mode
  reads `budget_expendable` for both `budget` and `active`, because the engine sheds both
  vectors by one rule, and a chip placed without budgeted room overlaps; so the mode
  cannot change inside a phase. The pinned second pass, whose mode is already fixed,
  reads the live flag. Last, a chip the phase shed whose live reading turns unusual (a
  spinning aux fan) is docked by `dock_free()` into room the line mode left free, and
  stays hidden when there is none: no reading is worth an overlap.
- **Chips are measured with the composer they render.** `apply_callout_layout()` measures
  in the chips' own fonts, takes padding and border from the live chip's style, and
  `around_text()` is the single expression for everything in a chip except its text. It
  sizes the chip and bounds the label's `max_width`, so a chip clamped narrower than its
  text ends in dots instead of spilling. A chip is one text line (`chip_h`) unless it
  carries its own height: `CalloutChipIn::h`, 0 for one line. A multi-line chip must set
  it, from `CalloutChipMetrics::chip_h_for()` and, for its pinned form,
  `compact_callout_chip_h(..., lines)`, because every stack, band slot, dock and pinned
  spot is sized from it; its width is its widest line.
- **Pinned chips are compact.** A pinned chip sits on the picture, so it drops its icon
  and unit letter and sets its text in `font_xs` (`printer_callout_mode` bindings in the
  XML). The text and the chip's width and height come from
  `src/ui/panel_widgets/callout_chip.h`, which the image tagger's review uses too, so its
  preview is the home chip. The budget pass still measures full chips to pick the mode; a
  second pass without the budget places the active chips at their compact size. The light
  chip keeps its xs icon, which at some breakpoints is taller than a `font_xs` line, so
  while it shows every chip slot takes the icon's height. The merged toolhead chip exists
  only in pinned mode, so it is text alone.
- **Geometry is set only from the deferred timer.** Observers publish subjects and call
  `schedule_callout_layout()`; the one-shot timer measures and places. Nothing forces a
  layout pass during a grid rebuild (#983, #1025). Unchanged coordinates are not
  rewritten, since every style write invalidates and temperatures relayout every tick.
- **One side and the exact-size image cache.** `helix::ui::ExactSizeImage`
  (`src/ui/panel_widgets/exact_size_image.h`) owns it for both surfaces that draw the
  printer picture, the home widget and the print status tile: no redraw scales the
  full-resolution decode. A moved image declares a pixel rect whose
  coords only follow at the next layout pass, and the cache check runs from timers that
  can fire first, so the cache reads the DECLARED size
  (`src/ui/panel_widgets/exact_size_image.cpp#declared_image_size`), never the coords.
  A generated copy whose size no longer matches the declared rect is dropped and the check
  rescheduled. An exact copy is shown 1:1: `show_exact_copy()` sets the inner align to
  CENTER, then `LV_SCALE_NONE`, then the source, because LVGL keeps the scale CONTAIN
  computed until the align changes and refuses a scale while CONTAIN is still set.
- **Pinned chips slide apart.** After the pinned chips are placed on their points (and
  after the toolhead merge and docking), `callout_detail::slide_apart()`
  (`include/callout_layout.h#slide_apart`) groups pinned chips whose x-ranges
  intersect into columns and spreads each column vertically with the same `spread_1d()`
  the side bands use, between the top edge and the docked row. A chip moves only within
  its column, only as far as `spread_1d()` needs, and never horizontally; a lone chip
  moves only to clear the docked row. A column too tall to stack there gives up its
  lowest-priority chip (light, fan, chamber, then bed; never the nozzle or toolhead) to
  the docked row, and the layout runs again until every column fits.
- **Leader lines are elbows.** Each line is three points: the tagged point, an elbow, and
  the middle of the chip's inner edge. From the point it runs at 45 degrees toward the
  chip's height, then straight into the chip: level into a side-band chip, vertical into a
  chip above or below the image. When 45 degrees would leave less than `min_line` of
  straight run, the diagonal steepens to keep that stub, and it never runs back past the
  point. `callout_detail::leader_elbow()`
  (`include/callout_layout.h#leader_elbow`) is the rule;
  `stack()` records the elbow in `CalloutChipOut::line_xm` / `line_ym`.
- **Leader lines keep their points alive.** `<leader_line>` (`include/ui_leader_line.h`)
  is a bare `lv_line`, and `lv_line` keeps the pointer it is given, so each line's three
  points live in the widget (`callout_line_pts_`, one set per `CalloutKind` that can draw
  a line) for as long as the line exists.
- **A chip's shown int has three values.** `callout_<kind>_shown` is 0 hidden, 1 active,
  2 residual: a heater that is off but still above 50C. XML hides a chip on `eq 0`, and
  `activity_chip` takes the chip's shown subject as `shown_subject` and binds a
  `#text_subtle` style on its label for 2, so a residual chip's text is greyed with no C++
  styling (the chip text is already `#text_muted`).
- **Where the points come from.** Each shipped image's nozzle, bed edge, part fan, chamber
  and light are hand-tagged, normalized over the source PNG, in
  `assets/images/printers/regions.json`; `assets/images/printers/README.md` documents the
  format and `tools/printer-regions-tagger.html` is the tagging tool. `[regions]` fails,
  naming the image, when a PNG no longer matches its recorded size.
- **Users tag their own.** Tag parts in the printer image picker opens
  `PrinterImageTaggerOverlay` (`src/ui/ui_overlay_printer_image_tagger.cpp`), which walks
  `helix::ImageTagSession` through the same six prompts on the image the widget displays and
  saves to `<config dir>/printer_image_regions.json` through `save_user_image_regions()`.
  Both files are keyed by `printer_image_region_key()`, which gives a custom photo
  `custom:<name>`. `lookup_image_regions()` prefers a user entry, but only when its `size`
  matches the displayed image's natural size, so tags made on another screen tier, or on
  shipped art that has since been re-cut, fall back to the shipped entry or to docked chips.
  The size cannot tell one custom photo from another of the same aspect, so
  `PrinterImageManager::import_image()` and `delete_custom_image()` clear that image's tags.
  A save or reset changes memory only after the file is written. A user file that does not
  parse is moved aside to `printer_image_regions.json.bad` on load, so tagging carries on;
  one that cannot be read, or moved, is never written over. Save and Reset tags bump
  `PrinterImageManager::notify_image_changed()`, which relayouts the widget.

### The tool switcher instance

`ToolSwitcherWidget` (`src/ui/panel_widgets/tool_switcher_widget.{h,cpp}`) runs the
pattern with touch size as the constraint instead of text width
(prestonbrown/helixscreen#1784). The pure decision is `helix::decide_tool_switcher_layout()`
(`src/ui/panel_widgets/tool_switcher_layout.h`): from the content box (the tile less
`#space_md` each side), the head count, the `#touch_target_min` floor (48 px), the 2 px
cell gap, the body and small line heights and the action cells the backend offers
(`ToolSwitcherActions`: Park, the feeder), it returns a `ToolSwitcherLayout` whose mode is
the first that fits: a segment per head plus every action cell (`RailWithActions`), a segment per head (`Rail`), the mounted head's face beside Park
(`Pair`), or the face alone (`Single`). Every cell it hands back clears the floor on both
axes, and `fits_at()` refuses a tile whose content box cannot hold one face, so no size
draws a target a finger cannot hit. The widget publishes the verdict as
`tool_switcher_mode` (the `ToolSwitcherMode` ints), `_cols`, `_rows`, `_cell_w`,
`_cell_h`, `_rail_h`, `_detail` and `_two_line`, plus per-head pools (`tool_switcher_seg_<i>_*`)
and the face's text (`tool_switcher_face_label`, `_sub`, `_color`, `_has_color`), and the
XML (`ui_xml/components/panel_widget_tool_switcher.xml`) sizes segments with
`bind_style_prop` off `_cell_w`/`_cell_h`. The face sizes itself off `tool_switcher_face_w`
and `tool_switcher_face_h`: its cell along a `Pair`'s long axis, the whole view across it.
Every cell carries its filament colour as one band (`ui_xml/components/tool_switcher_band.xml`),
sized per surface by the pure `tool_switcher_band()`: the lower part of the cell with the
material name on it where one small line fits, else a plain bar. Each head's look follows
`tool_switcher_seg_<i>_state` (`tool_switcher_cell_state()`): idle, mounted, or faulted while
the changer reports an error. The picker (`ToolSwitcherWidget::ToolPicker`,
`ui_xml/tool_switcher_picker.xml`) is the fallback for whatever the tile does not show: a grid
of the rail's own segments sized by `fit_tool_picker_grid()` (three columns up to six heads,
four beyond, never scrolling up to eight), with Park and the feeder in a footer under it. It
opens from the face or from the mounted segment. A printer with more heads than the segment
pools hold (`ToolSwitcherWidget::kPoolSize`) is decided as unrailable and gets the face and
the picker.

### Engine contracts this pattern relies on

- A `<style>` carrying `flex_flow` must ALSO carry `layout="flex"` — the
  inline attribute path sets both, the style path does not, and a container
  without an active layout piles every child onto the first while
  `lv_obj_get_style_flex_flow` still reports the flow. Test layouts by
  asserting `LV_STYLE_LAYOUT` and child positions, not the flow value.
- Inline `style_*` attributes write LOCAL styles that outrank any bound
  style (declarative rule 6) — including `style_pad_all`, which shadows a
  per-state bound `pad_top`. Split the inline all-sides form when any side
  is bound.
- A measured-exact fit renders as an overlap: measurement and rendered width
  disagree by a few pixels, so keep a comfort margin on text-budgeted rungs
  and pin one-line labels (`long_mode="dots"` on a `text_*` label with a width
  bound, which holds it to one line) when the stack model budgets one line.
- Bindable fonts on semantic `text_*` widgets work because the semantic font
  rides a shared ADDED style (#1614). An `<icon>`'s face and `temp_display`'s four labels
  ride the same mechanism (`helix::ui::apply_font_style`,
  `include/helix/ui/shared_font_style.h`), which is what the tile rung binds rely on.
  Do not reintroduce local font writes.

## Testing the pattern

- Pure decision tests on measured-live fixtures, every boundary exact
  (`[nozzle][layout]`).
- Widget tests asserting the BOUND OUTCOME: which label carries HIDDEN, the
  resolved width/flow/layout, distinct cell positions — not cached decisions
  (`[widget_size][nozzle_temps]`). Drive value-shape transitions through the
  real subjects to pin the re-decide.
- The shared helpers have their own boundary tests (`[size_rung]`,
  `[value_status]`); the speed/flow instance pins its decision in
  `[speed_flow][layout]` and its bound outcome in `[widget_size][speed_flow]`. The
  other family members follow the same two-tag shape: `[z_offset][layout]`,
  `[dryer][layout]`, `[host_health][layout]`, `[toolhead][layout]`,
  `[bed_mesh_tile][layout]` for the decision, and `[widget_size][<id>]` for the bound
  outcome. `[dryer][summary]` pins the aggregation in `include/dryer_summary.h`.
- `test_widget_content_fits.cpp` sweeps every widget at every shipping
  geometry at its authored minimum; its baseline entries are the honest
  record of which tiles sit below a widget's floor.
