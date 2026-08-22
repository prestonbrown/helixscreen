# Home Panel Grid Sizing — Unified Model

**Date:** 2026-08-04
**Status:** Design approved, pending implementation plan
**Issues:** closes #1126; supersedes the portrait workarounds from #1215 / #1216

---

## Problem

`GridLayout::get_dimensions()` runs three unrelated sizing models selected by
`LayoutType`, and they disagree about how large a grid cell may be.

| Path | Model | Constants |
|------|-------|-----------|
| `STANDARD` / `MICRO` / `TINY` | Static per-breakpoint **count** table | `GRID_DIMS` |
| `ULTRAWIDE` | Columns from width, rows from the table | `TARGET_CELL_W_PX = 160` |
| `PORTRAIT` family | Both axes from **size** targets | `TARGET_CELL_W_PX = 160`, `TARGET_CELL_H_PX = 120` |

Three consequences.

**1. Portrait micro has no usable grid.** At 272x480 the formula yields
`cols = clamp(272/160, 2, 16) = 2` and `rows = clamp(480/120, 3, 16) = 4` — 8 cells.
The two portrait anchors in `default_layout.json` (`printer_image` 2x2 at row 0,
`print_status` 2x2 at row 2) consume all 8. Every other `default_enabled` widget
hits `PlacementFailure::GridFull`, is disabled by
`panel_widget_manager.cpp:365-380`, and the `enabled:false` is **persisted** to
`/printers/<id>/panel_widgets/home`. Rotating back to landscape does not restore
it, because `build_defaults()` only runs when no saved config exists.

**2. Cell aspect ratio swings 47% across tiers.** A fixed-count table forced onto
screens of differing aspect ratios must absorb the difference in the cell:

| panel | grid | cell | aspect |
|---|---|---|---|
| 480x272 | 6x4 | 80 x 68 | 1.18 |
| 480x320 | 6x4 | 80 x 80 | 1.00 |
| 480x400 | 6x4 | 80 x 100 | 0.80 |
| 800x480 | 6x4 | 133 x 120 | 1.11 |
| 1024x600 | 8x5 | 128 x 120 | 1.07 |
| 1024x768 | 8x5 | 128 x 154 | 0.83 |

A widget authored to look right at 1.18 is a different physical shape at 0.80.

**3. `colspan` is not a physical unit, but 13 widgets treat it as one.** They
branch on raw span to choose compact vs. wide layouts, so the same authored span
produces a different decision on every panel. `MIN_PORTRAIT_COLS = 2` exists
solely to stop this misfiring (see the rationale comment at its declaration in
`include/grid_layout.h`), which is why portrait cannot simply be given more
columns.

These are one problem. Sizing, aspect, and span semantics cannot be fixed
independently.

---

## Goals

1. One sizing expression for every orientation and layout type.
2. Cell aspect ratio ~1.0 on every shipping panel, both orientations.
3. A widget is authored once and never re-tuned for a panel's shape or density.
4. No widget is disabled for lack of room on any shipping panel.
5. Land #1126 (half-cell resolution) in the same pass.

### Non-goals

- Panel-level portrait XML overrides (`ui_xml/portrait/`, `ui_xml/micro_portrait/`).
  Out of scope; this is the C++ grid only.
- Widget swapping on drag (edit mode still rejects occupied drop targets).
- Multi-page home layout changes.

---

## Design

### 1. Sizing model

`GridLayout::get_dimensions()` becomes one expression with no branches:

```cpp
// Target cell edge in px, per breakpoint tier. Cells are square, so colspan and
// rowspan are the same physical unit and a widget is authored once.
static constexpr int GRID_CELL[] = {34, 40, 40, 60, 60, 72};

GridDimensions GridLayout::get_dimensions(UiBreakpoint bp) {
    const int n = GRID_CELL[clamp_bp(bp)];
    auto& lm = LayoutManager::instance();
    return {std::clamp(lm.width()  / n, MIN_TRACKS, MAX_TRACKS),
            std::clamp(lm.height() / n, MIN_TRACKS, MAX_TRACKS)};
}
```

Values are already halved for #1126, so these are the shipped numbers.

**Deleted:** the `LayoutType` switch, the `ULTRAWIDE` case, the
`PORTRAIT`/`TINY_PORTRAIT`/`MICRO_PORTRAIT` case, `GRID_DIMS`,
`TARGET_CELL_W_PX`, `TARGET_CELL_H_PX`, `MIN_PORTRAIT_COLS`, `MIN_DYNAMIC_COLS`,
`MIN_DYNAMIC_ROWS`, `MAX_DYNAMIC_COLS`, `MAX_DYNAMIC_ROWS`.

**Added:** `MIN_TRACKS = 4`, `MAX_TRACKS = 64`.

`LayoutManager` stops being a grid-sizing input entirely; it keeps its
XML-variant-chain job.

#### Resulting grids

| panel | today | after | actual cell | aspect |
|---|---|---|---|---|
| 480x272 micro | 6x4 | 14x8 | 34.3 x 34.0 | 1.01 |
| 272x480 micro | 2x4 | 8x14 | 34.0 x 34.3 | 0.99 |
| 480x320 tiny | 6x4 | 12x8 | 40.0 x 40.0 | 1.00 |
| 320x480 tiny | 2x4 | 8x12 | 40.0 x 40.0 | 1.00 |
| 480x400 small | 6x4 | 12x10 | 40.0 x 40.0 | 1.00 |
| 800x480 medium | 6x4 | 13x8 | 61.5 x 60.0 | 1.03 |
| 480x800 portrait | 3x6 | 8x13 | 60.0 x 61.5 | 0.98 |
| 1024x600 large | 8x5 | 17x10 | 60.2 x 60.0 | 1.00 |
| 1280x720 xlarge | 8x5 | 17x10 | 75.3 x 72.0 | 1.05 |
| 1920x440 ultrawide | 12x4 | 48x11 | 40.0 x 40.0 | 1.00 |
| 320x1480 ultratall | 2x12 | 8x37 | 40.0 x 40.0 | 1.00 |

Aspect lands in [0.98, 1.05] everywhere, against today's [0.80, 1.18]. Rotating
any panel transposes its grid exactly. Half-cells improve squareness, because
finer granularity means integer truncation costs proportionally less.

#### On `MAX_TRACKS = 64`

1024x600 already wants 17 columns against today's cap of 16, and 1920x440 wants
48. Any cap below the demand stretches cells and breaks the aspect invariant,
which is the property this refactor exists to buy.

The #1215 rationale for capping at 16 was descriptor memory and per-track layout
cost. That reasoning conflated tracks with cells: a 48x11 grid is 59 `int32`
entries in the LVGL grid descriptor (`cols+1` plus `rows+1`), not 528 objects.

`MIN_TRACKS = 4` is a degenerate-display guard. No shipping panel reaches it —
the narrowest is 272px at 34px cells, giving 8.

### 2. Edit mode

#### 2a. Resize hit bands must scale (blocker)

`grid_edit_mode.cpp:50-51` defines `EDGE_HIT_INWARD = 18` and
`EDGE_HIT_MARGIN = 18`: a fixed 36px resize band per edge, 18px inside the
widget. At 480x272 today a 1x1 widget is 80x68px, leaving a ~44x32 drag core.

A half-width widget at the new resolution is 34px across. Two 18px inward bands
leave **-2px**: `detect_resize_edge()` returns non-`None` for every pixel and the
widget can never be dragged, only resized.

Derive the band from the widget, per axis:

```cpp
int inward_x = std::min(EDGE_HIT_INWARD, (area.x2 - area.x1) / 3);
int inward_y = std::min(EDGE_HIT_INWARD, (area.y2 - area.y1) / 3);
```

Per-axis because a 1x2 half-cell widget is 34x68 and the axes need different
answers.

#### 2b. Half-step visual indication

The visible lattice shows **the grid the current selection can actually snap
to**:

- Nothing selected, or a full-cell-only widget selected → major dots at
  full-cell boundaries. Visually identical to today.
- A widget declaring `supports_half_col` / `supports_half_row` → minor dots fade
  in on the supported axes at half-cell boundaries, smaller and fainter
  (`DOT_SIZE` 4 → 3, `LV_OPA_30` → `LV_OPA_15`).

If a dot is visible, it is a legal drop target. That is the whole affordance.

This also bounds the object cost. The lattice is one `lv_obj_t` per intersection
(`grid_edit_mode.cpp:2186-2208`). At 14x8 an unconditional lattice is 15x9 = 135
objects, up from today's 35. Major-only is 8x5 = 40 — roughly today's cost — and
the extra 95 exist only while a half-capable widget is selected.

A `DRAW_POST` overlay would be cheaper and is an approved structural exception,
but it is a larger change than this needs. Revisit if the object churn measures
badly on AD5M-class hardware.

#### 2c. Snap granularity

`round_to_grid_cell(px, content_origin, content_size, ncells)`
(`grid_edit_mode.cpp:890-894`) already takes the cell count as a parameter. A
full-cell-only widget passes `ncells / 2` and multiplies the result by 2. Ten
call sites gain one argument: `:1259`, `:1260`, `:1595`, `:1597`, `:1599`,
`:1601`, `:1699`, `:1702`, `:1705`, `:1708`.

`compute_resize_result`'s `std::max(new_colspan, 1)` (`:912`, `:927`) and
`handle_resize_move`'s pixel floor (`:1569-1571`) become granularity-aware.

#### 2d. Cell metrics helper (precondition)

Every cell-geometry site computes `cell_w = cw / ncols`, ignoring the grid
gutters. `panel_widget_manager.cpp:633-634` sets `pad_column` / `pad_row` to
`space_xs` and the tracks are `LV_GRID_FR(1)`, so edit mode's math drifts
progressively across the row: 5 gaps of error at the right edge at 6 columns, 13
at 14 columns, where the rightmost dot column sits visibly off its track.

```cpp
cell_w = (cw - (ncols - 1) * gap) / ncols;   // line c at c * (cell_w + gap)
```

That block is duplicated at nine sites (`:987`, `:1154`, `:1243`, `:1540`,
`:1684`, `:1836`, `:2007`, `:2058`, `:2144`), some integer and some float. It
collapses into one `cell_metrics()` helper. **Do this first** — fixing the gutter
bug nine times independently is how it comes back.

#### 2e. Dead code to reclaim

`create_drag_ghost()` (`:2002`) is never called; `handle_drag_start:1179`
explains why. `DRAG_SHADOW_OPA` / `DRAG_SHADOW_WIDTH` / `DRAG_SHADOW_OFS`
(`:46-48`) are unreferenced.

### 3. Widgets threshold on pixels

Thirteen widgets stop reading `colspan`/`rowspan`.
`panel_widget_manager.cpp:860` already passes real pixels:

```cpp
slot.instance->on_size_changed(p.colspan, p.rowspan, cell_w * p.colspan, cell_h * p.rowspan);
```

#### Threshold derivation

Measured, not estimated. Every tier was driven live and its grid geometry read
back (`.superpowers/sdd/2026-08-05-grid-metrics-followups/span-pixel-table.md`):

| tier | span1 | span2 | span3 | row1 | row2 |
|---|---|---|---|---|---|
| Micro 480x272 | 70 | 142 | 214 | 64.5 | 131 |
| Tiny 480x320 | 68 | 138 | 208 | 76.5 | 155 |
| Small 480x400 | 65.7 | 135 | 205 | 94 | 192 |
| Medium 800x480 | 114 | 233 | 352 | 113 | 230 |
| Large 1024x600 | 108 | 221 | 335 | 141.5 | 289 |
| XLarge 1280x720 | 134 | 276 | 418 | 169 | 346 |
| Micro portrait 272x480 | 131 | 264 | — | 97 | 196 |
| Portrait 480x800 | 152 | 309 | 466 | 107 | 218 |

**A single threshold cannot reproduce today's behavior, because span and width
are not monotonically related across tiers.** Portrait's span-1 is 152px, wider
than Small's span-2 at 135px — portrait has 3 columns of 152px where small
landscape has 6 of 66px. On the row axis the overlap is larger: XLarge's row-1
(169px) exceeds both Micro's row-2 (131px) and Tiny's (155px).

So the choice is which way to break the tie:

- **Low thresholds** (just under each measured minimum): nothing that renders a
  wide or tall layout today loses it. Some widgets that have the physical room
  but were denied it by a unitless span count gain it.
- **High thresholds** (above the largest span-1): nothing gains, but
  Micro/Tiny/Small lose their wide layouts and Micro/Tiny lose their tall ones —
  regressions on precisely the cramped panels this work exists to serve.

**Low wins.** The migration's premise is that physical size should decide
layout; where span and size disagree, size is the one that was right.

```cpp
// include/panel_widget_size.h
namespace helix::widget_size {
// Physical size bands, set just below the smallest measured extent at which each
// span predicate fires today. A widget picks its layout from the pixels it
// occupies, so one authored span reads correctly on every panel and orientation.
inline constexpr int W_NORMAL = 134; // was colspan >= 2  (min measured 135)
inline constexpr int W_WIDE   = 204; // was colspan >= 3  (min measured 205)
inline constexpr int H_TALL   = 130; // was rowspan >= 2  (min measured 131)
}
```

**Behavior deltas this accepts**, all in the expanding direction:

| panel | span | old | new |
|---|---|---|---|
| Portrait 480x800 | colspan 1 = 152px | compact | wide |
| Large 1024x600 | rowspan 1 = 141.5px | short | tall |
| XLarge 1280x720 | rowspan 1 = 169px | short | tall |

Each needs a visual check on that panel during the migration.

**Superseded:** an earlier draft of this section set 160/240/136 by assuming
80px cells at micro. Real micro cells are 70px. Those values would have removed
the wide layout from Micro, Tiny and Small — the opposite of the intent.

#### Migration table

| widget | file:line | today | becomes |
|---|---|---|---|
| `active_spool` | `active_spool_widget.cpp:135` | `colspan >= 2` | `w >= W_NORMAL` |
| `camera` | `camera_widget.cpp:258` | `colspan <= 1 && rowspan <= 1` | `w < W_NORMAL && h < H_TALL` |
| `clock` | `clock_widget.cpp:180-186` | 4-way on spans | 4-way on `w`/`h` |
| `fan_stack` | `fan_stack_widget.cpp:237` | `colspan >= 2 \|\| rowspan >= 2` | `w >= W_NORMAL \|\| h >= H_TALL` |
| `favorite_macro` | `favorite_macro_widget.cpp:219-220` | `rowspan >= 2`, `colspan >= 2` | `h >= H_TALL`, `w >= W_NORMAL` |
| `humidity` | `humidity_widget.cpp:101-102` | `colspan >= 2`, `rowspan >= 2` | `w >= W_NORMAL`, `h >= H_TALL` |
| `job_queue` | `job_queue_widget.cpp:165-169` | 3-way on spans | 3-way on `w`/`h` |
| `print_stats` | `print_stats_widget.cpp:201-207` | 4-way on spans | 4-way on `w`/`h` |
| `print_status` | `print_status_widget.cpp:455,461,464,477` | 4 predicates incl. `colspan == 2 && rowspan >= 2` | `w >= W_NORMAL && w < W_WIDE && h >= H_TALL` etc.; mirror at `:1713` |
| `temp_graph` | `temp_graph_widget.cpp:179,184,194` | 3 span predicates | `w`/`h` equivalents |
| `tips` | `tips_widget.cpp:165` | `colspan <= 2` | `w < W_WIDE` |
| `tool_switcher` | `tool_switcher_widget.cpp:104` | `colspan == 1 && rowspan == 1` | `w < W_NORMAL && h < H_TALL` |
| `width_sensor` | `width_sensor_widget.cpp:85-86` | `colspan >= 2`, `rowspan >= 2` | `w >= W_NORMAL`, `h >= H_TALL` |

**No change:** `temp_stack` (rowspan-only tier selection, immune to a column
change), `clog_detection` (ignores all four parameters, pure geometry re-fit).

**Already correct:** `nozzle_temps` (`nozzle_temps_widget.cpp:286`) measures
pixels and feeds `decide_nozzle_layout(avail_px)`. It is the pattern the others
adopt. Its one span read at `:436` (long vs. short labels) migrates too.

**Also updated:** `panel_widget_manager.cpp:870` passes raw colspan to
`ui_ams_mini_status_set_width(ams_child, cell_w * p.colspan, p.colspan)`.

#### Verification

Every migrated widget needs a visual check on 480x272 and 272x480 via
`helix-screen ctl`. Expect one or two thresholds to move; the table above is a
starting point, not a finished answer.

Widgets whose transition does real work, and therefore need the closest
attention: `camera` (starts/stops the MJPEG stream), `tool_switcher` and
`job_queue` (full rebuilds), `print_status` (four predicates plus persisted
`is_column_` state and a recycle-guard test).

### 4. Registry and default data

#### 4a. `PanelWidgetDef`

New fields `supports_half_col` / `supports_half_row` (default false).
Half-capable per #1126: `lock`, `shutdown`, `firmware_restart`, `led_controls`,
`clock`. Explicitly not: `camera`, `temp_graph`, `print_status`, `job_queue`,
`ams`, `tips`.

Spans re-authored, starting from double today's values, then adjusted where a
widget opts into half-width.

#### 4b. `default_layout.json`

Anchors re-authored in the new units, both variants.

Two scarcity workarounds are **deleted**, because the scarcity they worked around
is gone:

- The forced portrait `tips` suppression (`panel_widget_config.cpp:722-731`).
  `tips` is authored 4 columns with `min_colspan` 2 and could not fit a 2-column
  grid. Portrait micro now has 8.
- The `bed_temperature` gate `is_large || !ams_present`
  (`panel_widget_config.cpp:735-746`), which trades bed temperature against AMS
  for want of cells.

Both are special cases that exist only because the grid was too small. Removing
them is part of the DRY goal, not scope creep.

#### 4c. Settings migration (config_version 21 → 22)

Key is `/printers/<id>/panel_widgets/home`. One key — no orientation or
breakpoint suffix — so a layout saved in any geometry is reused in every other.

**A migration is mandatory.** Spans are stored as raw cell counts, so without one
they stay numerically identical while the cells shrink. At 480x272 a saved 2x2
widget goes from 160x136px to 68x68px. Nothing is disabled — the grid is three
times larger and everything fits — but every widget on every existing install
renders at roughly 40% size, clustered top-left with most of the screen empty.

##### Use the existing version chain

`settings.json` already carries `config_version` (`include/config.h:60`,
`CURRENT_CONFIG_VERSION = 21`) with a `migrate_vN_to_vN+1` chain dispatched by
`run_versioned_migrations()` (`src/system/config.cpp:1292`). Chain head is
`migrate_v20_to_v21` at `:1217`.

This migration is `migrate_v21_to_v22`, `CURRENT_CONFIG_VERSION` bumps to 22, one
`if (version < 22)` line in the dispatch. **No new per-blob `schema` key** — that
would be a second version mechanism next to a working one.

##### The migration unplaces; it does not rescale

`run_versioned_migrations()` runs at config load (`config.cpp:1660`).
`m_screen_width` is not final until `application.cpp:1293`, can change again at
`:1536` after rotation, and `layout_mgr.init()` is at `:1559`. No resolution is
stored in `settings.json`. **At migration time there is no screen size**, so the
migration cannot compute a rescale factor even if we wanted one.

That forces the DRY answer, which is also the correct one: saved `col`, `row`,
`colspan`, `rowspan` are expressed in cells of a grid that no longer exists.
There is nothing faithful to convert them into — a 2-column portrait layout
stretched into 8 columns is an artifact of the broken grid, not the user's
layout. So clear them and let the placement engine seat everything on whatever
grid the panel actually has.

```cpp
static void migrate_v21_to_v22(json& config) {
    // The home grid moved to square cells at half-cell resolution, so saved
    // col/row/span are in units that no longer exist. Clear them and let the
    // placement engine seat everything on whatever grid the panel actually has.
    // Read intent BEFORE blanking: a disabled entry holding real coordinates is
    // a user trash-removal; one at -1 was auto-disabled or never added.
    for (auto& printer : config["printers"]) {
        for (auto& entry : /* each page's widgets under panel_widgets/home */) {
            bool user_removed = !entry["enabled"] && entry["col"] >= 0;
            if (!user_removed && !entry["enabled"]) entry.erase("enabled");
            entry["col"] = -1;
            entry["row"] = -1;
            entry.erase("colspan");
            entry.erase("rowspan");
        }
    }
}
```

Nothing new is introduced. `-1` is already the unplaced sentinel
(`panel_widget_config.h:19-22`, `has_grid_position()` at `:31-33`); absent spans
already fall back to registry defaults in `parse_widget_array()`
(`panel_widget_config.cpp:95-107`); and the two-pass placement engine already
handles exactly this case for every never-positioned widget on every boot. This
reuses the live path rather than building a parallel one.

Iterating `config["printers"]` also covers **every** printer profile. Doing this
inside `PanelWidgetConfig` would have touched only the active one — the shipped
`config/settings.json` already carries `default` and `printer-2`.

A useful consequence: because no step depends on the old grid, any future grid
change reuses this same move. There is no per-version geometry table to maintain.

**Preserved:** the widget set, deliberate hides, and array order (which drives
placement order). **Lost:** hand-arranged coordinates.

##### Rejected: rescale by a frozen V1 grid table

Considered and dropped. It required either resurrecting the deleted sizing model
inside the migration (`GRID_DIMS`, the `LayoutType` switch,
`TARGET_CELL_W_PX/H_PX`, `MIN_PORTRAIT_COLS`) or freezing its output as a
resolution lookup table — a second copy of grid knowledge, maintained forever,
for coordinates that could not be faithfully converted anyway. It also could not
run in the version chain at all, since the resolution is unknown there.

##### `enabled`: preserve user intent, clear engine damage

User intent is recoverable from the data. Only three sites write `col = -1` at
runtime (`panel_widget_manager.cpp:372`, `panel_widget_config.cpp:727`,
`panel_widget_config.cpp:788`) and none is reachable from a user gesture, while
the trash button (`grid_edit_mode.cpp:717-725`) sets `enabled = false` and
**leaves the coordinates intact**.

| state on disk | means | migration |
|---|---|---|
| `enabled:false, col >= 0` | user pressed trash | keep disabled |
| `enabled:false, col == -1` | engine auto-disable, or never added | take registry `default_enabled` |
| `enabled:true` | in use | keep enabled |

This clears the #1216 damage without discarding deliberate hides. A blanket
`enabled = registry default_enabled` was considered and rejected: it throws away
every deliberate add and hide while carefully preserving geometry, which is
internally inconsistent.

Two caveats:

- **`bed_temperature` false positive.** It is an anchor with real coordinates,
  and `panel_widget_config.cpp:733-746` disables it conditionally without
  clearing them, so a fresh install with AMS present looks identical to a user
  removal. §4b deletes that gate, so this resolves itself — but the gate must be
  removed *before* or *with* the migration, not after.
- **Multi-instance widgets leave no marker.** `delete_entry()`
  (`panel_widget_config.cpp:439-450`) erases the entry outright for ids
  containing `:`, so a trashed `fan_stack:2` is simply absent. Absent entries are
  re-appended at `-1,-1` with `default_enabled` by `parse_widget_array()`
  (`:119-128`), which is the existing behaviour and unchanged here.

The `home_widgets` → `panel_widgets.home` legacy migration runs first and is
unaffected.

### 5. Testing

#### Rewritten

These assert properties of the deleted model and cannot be satisfied:

| test | pins |
|---|---|
| `test_grid_layout.cpp:21-67` | static table 6x4 / 8x5 |
| `test_grid_layout.cpp:481` | ultrawide 1920x440 → 12x4 |
| `test_grid_layout.cpp:514` | all portrait dynamic cases |
| `test_grid_layout.cpp:576` | `portrait_row_px == 123`, `cols*rows == 24` |
| `test_grid_layout.cpp:595` | micro 480x272 stays 6x4 |
| `test_default_layout.cpp:979-982` | the `max_cols` mirror map |
| `test_default_layout.cpp:988` | `id != "tips"` for portrait anchors |
| `test_grid_edit_mode.cpp:1404` | the exact 18+18 hit band, 16 assertions |

#### New invariants

1. **Cell aspect** within [0.95, 1.05] for every entry in a table of real
   devices, in both orientations.
2. **Rotation transposes exactly**: `dims(w,h) == transpose(dims(h,w))` for the
   full device table.
3. **No widget disabled for want of room** on any shipping panel, either
   orientation, from a fresh default config. This is the regression test #1216
   never got.
4. **Migration preserves the widget set**: a v21 config with a mix of enabled,
   user-trashed (`enabled:false, col >= 0`) and auto-disabled
   (`enabled:false, col == -1`) entries produces a v22 config with the same
   widgets on, deliberate hides still off, and every `col`/`row` at `-1`.
5. **Migration covers every printer profile**, not just the active one — a
   fixture with two printers must migrate both.
6. **Resize band is a fraction of the widget**, not a constant — replaces the
   pinned-pixel test.
7. **Snap granularity is honored**: a full-cell-only widget never lands on an odd
   boundary.

#### Preserved

The `[minfirst][1216]` and `[grow][1216]` suites (`test_grid_layout.cpp:690-885`)
test the placement algorithm, which is unchanged. `test_panel_widget_portrait_span.cpp`
keeps its central assertion — a span reduced only to survive the current
orientation must not be persisted (`panel_widget_manager.cpp:570-577`) — with
updated numbers.

Per-widget size-branch tests (`test_active_spool_widget_wide.cpp`,
`test_print_status_widget_recycle.cpp`,
`test_print_status_widget_layout_gate.cpp`, `test_panel_widget_temp_graph.cpp`)
change from driving spans to driving pixels.

`test_grid_edit_mode.cpp:1290` — "All registered widgets have valid sizing
constraints" — sweeps the whole registry and will catch inconsistencies
introduced by the span re-authoring. Keep it.

---

## Previously rejected — do not re-propose

From the #1216 closing comment:

- **Greedy span-stepping** (step the requested span down until it fits). At
  480x800 it saved `tips` at 3x2 and disabled `fan_stack`, `ams`, and
  `notifications` — three widgets lost instead of one, three stacked toasts.
- **Strict minimum-first**. Bottom-packing minimums fragments the grid into
  1-cell strips that directional growth cannot reassemble; a roomy landscape
  dashboard visibly shrank (`printer_image` 1 row instead of 2, `tips` 2 columns
  instead of 4).

Authored-first with a minimum-first fallback is the only shape that keeps both
properties. This design does not touch it.

From #1215: raising `MAX_DYNAMIC_ROWS` past 16 was rejected on the grounds that
every row is a real LVGL grid track. This design raises it to 64 anyway — see
§1 "On `MAX_TRACKS`" for why that reasoning does not hold at the track level.

---

## Risks

| Risk | Mitigation |
|---|---|
| A pixel threshold reads wrong on a panel not checked | Visual pass on 480x272 and 272x480 per widget; the three constants are shared, so a correction is one edit |
| 135-object lattice churns on AD5M-class hardware | Minor dots exist only while a half-capable widget is selected; `DRAW_POST` overlay as fallback |
| Users lose hand-arranged positions on upgrade | Unavoidable — the coordinates are in units of a deleted grid. The widget set and deliberate hides survive; needs a release note |
| `migrate_v21_to_v22` walks a config shape that varies (missing `printers`, missing `panel_widgets`, pre-multi-page blob) | Defensive traversal; the `home_widgets` legacy shape is handled by the existing earlier migration. Fixtures for each shape |
| Ultrawide 12x4 → 48x11 surprises someone | 1920x440 is alpha per `ROADMAP.md:23`; cells stay 40px square, matching every other panel |
| Deleting the `bed_temperature` gate changes defaults on AMS printers | Covered by the new "no widget disabled for want of room" test plus a default-set assertion per tier |

## Open questions

None blocking. Two to settle during implementation:

1. Whether `W_WIDE = 240` is right for `tips` specifically, whose current
   `colspan <= 2` predicate means 160px at micro and 266px at medium — the
   widest spread of any single predicate.
2. Whether the `clock` widget's four modes still want four distinct bands once
   thresholds are physical, or collapse to three.

---

## Files

**Core**
`include/grid_layout.h`, `src/ui/grid_layout.cpp`,
`include/panel_widget_size.h` (new), `include/panel_widget_registry.h`,
`src/ui/panel_widget_registry.cpp`, `src/system/panel_widget_config.cpp`,
`src/ui/panel_widget_manager.cpp`, `include/grid_edit_mode.h`,
`src/ui/grid_edit_mode.cpp`

**Migration**
`include/config.h` (`CURRENT_CONFIG_VERSION` 21 → 22),
`src/system/config.cpp` (`migrate_v21_to_v22` after the `migrate_v20_to_v21`
head at `:1217`, plus one dispatch line in `run_versioned_migrations()` at
`:1292`)

**Data**
`assets/config/default_layout.json`, `assets/config/panel_widgets/{ad5x,ad5x_zmod,cc1}/home.json`

**Widgets** (13)
`src/ui/panel_widgets/{active_spool,camera,clock,fan_stack,humidity,job_queue,print_stats,print_status,temp_graph,tips,tool_switcher,width_sensor,nozzle_temps}_widget.cpp`,
`src/ui/widgets/favorite_macro_widget.cpp`

**Tests**
`tests/unit/{test_grid_layout,test_default_layout,test_grid_edit_mode,test_panel_widget_portrait_span,test_panel_widget_config,test_panel_widget_manager}.cpp`,
the config-migration suite covering `migrate_v21_to_v22`, plus the per-widget
size-branch tests listed in §5

**Docs**
`docs/devel/LAYOUT_SYSTEM.md` § "Home Widget Grid" (lines 325-485 describe the
deleted model), `docs/user/TROUBLESHOOTING.md:592-629` (the manual-repair entry
becomes obsolete), `docs/user/guide/home-panel.md`
