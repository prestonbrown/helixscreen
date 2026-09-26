# Motion panel enhancements

Design for seven additions to the Motion: XYZ panel, plus one prerequisite fix to
`header_bar`.

## Motivation

Two user reports converge on the same panel.

A Discord user asked whether manual moves can be given exact coordinates, and why
HelixScreen jogs Z at 10 mm/s when their web UI does 25 mm/s. Both answers are in
`src/ui/ui_panel_motion.cpp#MotionPanel::send_jog_move`: the feedrates are `constexpr`
(6000 mm/min XY, 600 mm/min Z) and nothing reads a setting. The panel is jog-only, so
the G-code console is the only way to name a coordinate.

Issue #865 asks for adjustable step distances and alternative motion layouts.

Investigation also turned up a real gap: **Z jogs are not bounds-clamped.** X and Y go
through `helix::clamp_jog_delta` in `MotionPanel::on_jog`, but
`MotionPanel::on_motion_z_button` dispatches straight through. Klipper refuses the
out-of-range move, so this is not a crash, but each refusal fires an error toast. Adding
hold-to-repeat on top of an unclamped axis turns that into a toast flood.

## Scope

| # | Item |
|---|---|
| 0 | **Prerequisite**: `header_bar` action button slot 2 reaches parity with slot 1 |
| 1 | Jog speeds (XY, Z) become settings |
| 2 | Jog step distances become settings (six values) |
| 3 | Motors off available from the Motion panel |
| 4 | Hold-to-repeat jog, with Z clamping as a prerequisite |
| 5 | Absolute coordinate entry via the existing keypad |
| 6 | Position presets: Center, Front Left, Back Right, Park |
| 7 | Bed map tab with tap and drag to position |

### Non-goals

- Multiple selectable jog layouts. #865's mockups are answered by items 1, 2 and 7
  without shipping two layouts to maintain.
- Endstop and probe status display. Diagnostics, belongs on its own screen.
- Extruder jog. The filament panel owns it correctly.
- Refactoring the existing `on_machine_limits_clicked` duplication. Noted, not this
  change's job.
- Folding `header_save_z_offset` into the action button slots.

## Architecture

### The panel becomes tabbed on the left, unchanged on the right

The left column's jog pad slot gains a three-tab strip. The right column (Z buttons,
QGL/Z-Tilt) is untouched and stays visible on every tab, so Z is always reachable.

| Tab | Holds | Precision tier |
|---|---|---|
| **Jog** | Today's circular pad and Fine/Coarse/Turbo, unchanged | Relative nudges |
| **Move** | Tappable X/Y/Z fields (5), presets (6), motors off (3) | Exact numbers |
| **Bed** | Bed map (7) | Coarse spatial |

Each tab is a precision tier. That is the rationale for keeping the bed map deliberately
imprecise: anyone who wants an exact coordinate is one tab away.

The strip reuses `ui_xml/components/zone_tab.xml`, which already provides pill styling,
an active subject and index dispatch. It currently hardcodes
`callback="on_zone_tab_clicked"`; that becomes a prop so Motion and the AMS environment
overlay share one component.

### Settings gains a Motion section

New `setting_action_row` in `ui_xml/settings_printing_overlay.xml`, new
`src/ui/ui_settings_motion.cpp` following the row-descriptor table in
`src/ui/ui_settings_machine_limits.cpp`. Holds items 1 and 2. Machine Limits keeps
extrude speed and the velocity/acceleration limits it already owns.

### Shared plumbing

Three changes to existing code serve several items each.

- **`JogCoalescer` gains a merge policy.** `Accumulate` sums pending deltas (today's
  behaviour, correct for jog). `Replace` keeps only the latest (correct for absolute
  targets). Pending state is one kind or the other: dispatching a target discards a
  pending delta and vice versa, so a bed tap can never sum with a queued jog into a
  position nobody asked for.
- **`helix::clamp_jog_delta` gets applied to Z.** It is already generic; Z simply never
  called it.
- **`helix::BedCoordMapper` gains `px_to_mm()`**, the inverse of the `mm_to_px()` it
  already has.

## Item designs

### 0. `header_bar` action button parity (prerequisite)

Slot 1 takes icon, icon size, icon colour, radius, width, height, pad and three reactive
bindings. Slot 2 takes text, background colour and a callback, and hardcodes
`style_min_width="90"`, which shapes it as a text button. An icon-only button there would
render as a 90px-wide circle on a 22px-tall micro header.

Slot 2 gains slot 1's prop set, and `min_width` becomes a prop. **Every new prop defaults
to today's behaviour**, so all existing consumers render identically.

Blast radius, complete:

| File | Role |
|---|---|
| `ui_xml/header_bar.xml` | The change |
| `ui_xml/micro/header_bar.xml` | Must move in lockstep; `check_variant_parity.py` gates it |
| `ui_xml/overlay_panel.xml` | Pass-through wrapper, forwards the new props |
| `ui_xml/theme_preview_overlay.xml` + micro variant | Consumer: "Edit", secondary |
| `ui_xml/spoolman_panel.xml` | Consumer: "+ Add", primary |

Own commit, sequenced first, because item 5's settings shortcut depends on it.

### 1. Jog speeds

Two sliders in Settings → Motion: **Jog speed XY** and **Jog speed Z**.

- Stored in mm/min (what the motion API takes), displayed in mm/s (what Extrude Speed
  already shows and what web UIs use). Convert at the boundary only.
- Defaults are today's constants: 6000 and 600 mm/min. No existing printer changes
  behaviour on upgrade.
- Slider maximum comes from the same source as `is_safe_feedrate`, which
  `src/api/moonraker_api_controls.cpp` refreshes from the printer's reported
  `max_velocity`. The UI therefore cannot offer a speed the machine will reject.
- Storage mirrors `SettingsManager::get_extrude_speed` / `set_extrude_speed`, subjects
  included.

`MotionPanel::send_jog_move` reads these instead of its `constexpr` values.

### 2. Jog step distances

Six values: inner and outer for each of Fine, Coarse and Turbo. Tappable rows into the
existing keypad, plus reset-to-defaults.

`src/ui/ui_panel_motion.cpp#get_jog_mode_distances` is already the single source feeding
the pad, the Z buttons and the mode labels. It reads settings instead of a static table;
no call site changes.

Defaults are today's values (0.1/1, 1/10, 10/50).

**Ordering:** the keypad already takes min/max per field, so inner's maximum is the
current outer and outer's minimum is the current inner. This prevents a pad whose inner
ring travels further than its outer ring.

### Settings shortcut (serves 1 and 2)

A ghost cog in the Motion header, left of the E-stop, using the slot-2 icon support from
item 0. It pushes the Motion settings overlay onto the navigation stack, so `go_back()`
returns to the Motion panel rather than dropping the user into the settings tree.

The overlay gets **one** opener function shared by Motion, `SettingsPanel` and
`PrintingSettingsOverlay`, rather than a third copy of the pattern those two already
duplicate (`on_machine_limits_clicked` is registered separately by each).

The cog binds to `ui_breakpoint` and appears from the small tier up. On micro and tiny
the header caps buttons at 22-26px and the title already flex-grows, so the space is
better spent on the title; those tiers reach it through Settings as today.

### 3. Motors off

`src/ui/ui_panel_controls.cpp#ControlsPanel::handle_motors_confirm` already implements
this correctly: confirmation dialog, then `M84`, then success or error toasts. That is a
rule with three outcomes needed in two places, so it is extracted and both panels call
it. Same wording, same confirmation, no second dialect.

Lives on the Move tab.

### 4. Hold-to-repeat, and the Z clamp

**The clamp lands first**, because hold-to-repeat is what makes its absence dangerous.

`MotionPanel::on_motion_z_button` gains the same `clamp_jog_delta` treatment `on_jog`
gives X and Y, gated on `AxisBounds::has_z` and Z being homed.

> **Ordering detail, easy to get backwards.** `bed_moves_` inverts the sign before
> dispatch, and `AxisBounds` is in G-code space. The clamp must run **after** the
> inversion. Clamped on the wrong side, bed-moves printers get mirrored limits, which
> fails silently in the middle of the range and is worse than no clamp.

The `x_edge_warned_` / `y_edge_warned_` latches fold into one per-axis form as part of
this change, rather than becoming a third hand-written copy.

**Repeat mechanism:** an `lv_timer` owned by the panel, started on press and killed on
release. Not LVGL's built-in `LONG_PRESSED_REPEAT`, whose repeat interval lives on the
input device rather than the widget, so tuning it for the jog pad would retune every
long-press in the application.

Timing: roughly 400 ms before the first repeat, so an ordinary tap is never a repeat,
then a tick every 150 ms.

**The coalescer sets the real cadence.** Each tick goes through `JogCoalescer` with the
`Accumulate` policy, so ticks arriving while a move is in flight sum rather than queue.
Holding produces one move covering everything accumulated since the last acknowledgement,
then the next. The head moves near-continuously and Klipper's queue never fills with
hundreds of small moves.

**Stop conditions**, all of which kill the timer:

- Release, or press-lost (finger slides off the zone)
- Panel deactivate or navigate-away
- Printer disconnect, or klippy not ready
- **Bounds reached.** When the clamp returns approximately zero the repeat stops and the
  latch fires exactly one "blocked at bed edge". It does not tick into a wall generating
  one error per tick. The latch clears on release and on direction change.

Applies to the jog pad and the four Z buttons.

### 5. Coordinate entry

Three rows on the Move tab showing live position, bound to the `motion_pos_*` subjects
the position card already uses. Tap opens `ui_keypad_show()` seeded with the current
value.

- `allow_decimal` true. `allow_negative` from whether that axis's minimum is below zero,
  which is what makes it correct on deltas rather than assuming a zero origin.
- Minimum and maximum straight from `AxisBounds`.
- On confirm, an absolute move at the configured jog speed for that axis.

**No `returning_from_keypad_` guard.** `src/ui/ui_overlay_retraction_settings.cpp` needs
one because its fields hold a pending value a re-sync would clobber. These fields hold
nothing: they are live readouts, and the typed number leaves immediately as a move. The
field tracking the head back to the requested position is correct behaviour.

Unhomed goes through the `ensure_homed_then` helper used by
`src/ui/ui_panel_belt_tension.cpp` and the AMS backends, so asking for a position on a
cold printer offers to home rather than surfacing a Klipper error.

### 6. Position presets

Center, Front Left and Back Right are derived from `AxisBounds`.
`include/belt_gating.h#park_x_center` already derives a bounds-centred X and handles the
degenerate-bounds case, so the resolver extends it rather than deriving centres a second
time.

**Park is not a coordinate. It is a macro slot.**

A printer that has its own park macro knows where park belongs on that machine better
than any bounds arithmetic does: clear of the purge bucket, clear of the chamber camera,
Z lifted and filament retracted first. Guessing front-centre throws that away.

`StandardMacros` already solves exactly this: eleven semantic slots with auto-detection
from name patterns, a `HELIX_*` fallback, a user override in the macro settings overlay,
and graceful handling of an empty slot. `CleanNozzle` is the precedent, a slot whose
macro also moves the toolhead somewhere machine-specific and does work there.

So Park adds a twelfth slot, `ParkToolhead`, and its name table goes in
`include/macro_patterns.h` beside `clean_nozzle()`. That file exists specifically so
`PrinterDiscovery`'s object scan and the `StandardMacros` slot resolver read one list and
cannot drift; its own header documents the bug that happened when clean-nozzle names were
maintained in two places.

Resolution order for the Park preset:

1. The printer's own park macro, if the `ParkToolhead` slot resolves.
2. Otherwise a bounds-derived move to front-centre, the same as the other three presets.

> **The name table needs a pass over real configs, not a guess from me.** `PARK` and
> `PARK_TOOLHEAD` are conventional; `TOOLHEAD_PARK` and `PARKING` appear in the wild.
> Underscore-prefixed forms such as `_PARK` are deliberately excluded: a leading
> underscore is the author marking a macro internal, and invoking one is machine motion
> nobody sanctioned. Because a mis-detected park macro moves the toolhead, the existing
> per-slot user override in the macro settings overlay is the mitigation, and it is
> already built.

Park pairs with #1690 (manual nozzle cleaning), which reaches for the same slot, so this
is written once with two consumers.

All four route through `ensure_homed_then`.

**This exposes a gap.** `MoonrakerMotionAPI::move_to_position` is single-axis.
`src/ui/ui_panel_belt_tension.cpp` works around it by chaining, moving Y and then moving X
from the success callback, which traces an L and costs two round trips. That is wrong for
a preset and flatly wrong for item 7, where tapping a point must produce one diagonal
move.

So `MoonrakerMotionAPI::generate_absolute_move_gcode` widens to take optional X, Y and Z
instead of one axis and one position, mirroring what `generate_relative_move_gcode`
already does for the relative case. The single-axis entry point stays as a thin wrapper,
so belt tension and `src/ui/ui_overlay_qr_scanner.cpp` are unaffected.

### 7. Bed map

**Rendering.** No canvas. The plate is a styled `lv_obj` with a border; the toolhead is a
small child repositioned on position updates. `ExcludeObjectMapView` needs a canvas
because it draws arbitrary object polygons; this draws a rectangle and a dot. Avoiding
the canvas also avoids an ARGB draw buffer, which matters on two-core boards.

The bed map gets its own XML component as a sibling of `exclude_object_map`. **No
print-path code is edited.**

**Bed shape.** `BedCoordMapper` already handles delta's centre origin, but the outline
must also be right: a delta bed is circular, and a corner tap addresses somewhere the
toolhead cannot reach. When `PrinterDetector` reports delta kinematics the plate renders
circular and taps outside the radius clamp to the edge.

**Interaction.**

- **Tap** commits immediately.
- **Drag** updates the coordinate readout live with the target under the finger, and
  commits one move on release. No offset marker: dragging is coarse positioning, and
  anyone wanting precision is one tab away.
- Repeated taps coalesce under the `Replace` policy. Tap three times during an in-flight
  move and the third target is what goes out when the acknowledgement lands.
- A tap discards any pending jog delta.

**Open for measurement, not for design.** Whether drag should also stream moves while the
finger is down (the head chasing the finger) is a feel question, gated behind
`HELIX_BED_MAP_DRAG=commit|live`, defaulting to `commit`. With the `Replace` policy a
streamed drag is safe by construction: one move in flight, latest target pending, at most
one move outliving the release. The cost is that each queued move decelerates to zero at
its target, so the head hops rather than glides.

**This must be evaluated on real hardware.** The difference is entirely in how real
acceleration and deceleration feel; a simulated toolhead has neither and the mock will
give a misleading answer. Once decided, the winner is hardcoded and both the switch and
the losing path are deleted. This is a question to answer once, not a preference to
maintain.

**Gates.**

- **Unhomed:** the plate greys and the toolhead marker is *hidden*, not drawn at a guessed
  position. A marker at 0,0 on an unhomed printer is a lie that gets believed. A Home
  button takes its place.
- **Printing:** the Bed tab is disabled outright. This needs its own gate: the panel's
  existing `nav_buttons_enabled` covers connection and klippy-ready, not whether a print
  is running.
- **Z clearance:** a tap moves XY only, at the current Z. Below a configured clearance
  height the move **lifts to clearance first**, and the readout says so before the user
  commits ("Z 0.2, will lift to 5mm"). Unrequested Z motion is acceptable only because it
  is announced before the tap, not after. The clearance value is a setting with a sane
  default, not a magic number.

## DRY ledger

What this work reuses rather than rewrites.

| Need | Existing | Change |
|---|---|---|
| mm to px, top-down bed | `helix::BedCoordMapper` (Y-flip, delta origins, unit tested) | Add `px_to_mm()` |
| Pill tab strip | `ui_xml/components/zone_tab.xml` | Callback becomes a prop |
| Numeric entry | `include/ui_component_keypad.h` | None; call it |
| In-flight back-pressure | `include/jog_coalescer.h` | Merge policy parameter |
| Bounds clamping | `helix::clamp_jog_delta` | None; Z calls it |
| Edge-warn latch | `x_edge_warned_` / `y_edge_warned_` | Fold two copies into one per-axis form |
| Jog step distances | `get_jog_mode_distances()` | Point at settings |
| Absolute moves | `MoonrakerMotionAPI::move_to_position` | Widen to multi-axis, keep wrapper |
| Feedrate validation | `is_safe_feedrate` | None |
| Homing precondition | `ensure_homed_then` | None |
| Motors off | `ControlsPanel::handle_motors_confirm` | Extract, two consumers |
| Park position | Shared with #1690 | One resolver |
| Settings row pattern | `src/ui/ui_settings_machine_limits.cpp` | Pattern to follow |
| Plate view precedent | `ExcludeObjectMapView` | Untouched |

## Testing

Pure logic, unit tested without the framework:

- `px_to_mm()` round-trips against `mm_to_px()` across rectangular and centre-origin
  (delta) beds, including viewport aspect ratios that letterbox on each axis.
- `JogCoalescer` under both merge policies: `Accumulate` sums, `Replace` keeps the latest,
  and switching input kind discards the other's pending state.
- Z clamping, specifically **on a bed-moves printer**, proving the clamp is applied after
  the sign inversion and the limits are not mirrored.
- Edge-latch behaviour: one warning per approach, cleared on release and on direction
  change.
- `generate_absolute_move_gcode` multi-axis output, including single-axis calls through
  the wrapper producing byte-identical G-code to today.
- Preset coordinate derivation from `AxisBounds`, including centre-origin beds.
- Step-distance min/max coupling: inner cannot exceed outer.

Panel-level:

- Hold-to-repeat stops on every listed stop condition, with bounds-reached asserted
  explicitly, since that is the toast-flood case.
- Bed tab gates: unhomed hides the marker, printing disables the tab.
- Z clearance lift is announced before commit and performed on commit.

Gates: `check_variant_parity.py` proves `micro/header_bar.xml` kept pace with item 0.
Per `tests/CLAUDE.md`, `make mutate-diff` on the logic hunks, with the surviving mutation
named in each commit body. A green suite is not evidence.

## Risks

- **Z clamp applied on the wrong side of the `bed_moves_` inversion.** Silent, mid-range,
  and only visible on bed-moves printers. Covered by a dedicated test rather than by
  inspection.
- **Item 0 regressing four existing consumers.** Mitigated by defaulting every new prop to
  current behaviour and by the parity gate.
- **Tab strip costing vertical space on micro and tiny tiers.** The panel already carries
  measured per-tier size floors (`motion_card_min_*`); the strip needs the same treatment
  and re-measurement with `ctl geom`.
- **Bed map on non-rectangular, non-delta kinematics.** Unknown bed shapes fall back to
  the rectangular `AxisBounds` outline, which is the current assumption everywhere else.

## Sequencing

1. **Item 0**, `header_bar` parity. Own commit, no motion dependency, unblocks the cog.
2. **Item 4's clamp half**, Z bounds clamping and the latch fold. Standalone bug fix,
   shippable alone.
3. **Items 1 and 2**, Settings → Motion. Closes the Discord thread and half of #865.
4. **Shared plumbing**: coalescer merge policy, multi-axis absolute move, `px_to_mm()`.
5. **Item 4's repeat half**, now that Z is clamped.
6. **Tab strip**, plus items 3, 5 and 6 on the Move tab.
7. **Item 7**, bed map.

Steps 1 through 3 are independently shippable and each closes a real user report.
