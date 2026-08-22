# Square-Cell Home Grid Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the home panel's three competing grid-sizing models with one square-cell expression at half-cell resolution, re-author every default layout into the new units, and migrate saved layouts safely.

**Architecture:** `GridLayout::get_dimensions()` becomes `clamp(panel_axis / GRID_CELL[bp], MIN_TRACKS, MAX_TRACKS)` on both axes with no `LayoutType` branch. Grid *tracks* are half-cells: one authored cell is `GridLayout::TRACKS_PER_CELL` (= 2) tracks, so a widget can occupy half a cell on an axis its registry definition marks `supports_half_col` / `supports_half_row`. Edit mode snaps to a per-widget track step derived from those flags. All authored spans (registry, `default_layout.json`, preset seeds) are re-expressed in tracks, and `migrate_v21_to_v22` unplaces saved layouts so the existing two-pass placement engine re-seats them.

**Tech Stack:** C++17, LVGL 9.5, Catch2 (amalgamated), nlohmann/json via `hv/json.hpp`, spdlog, pure Makefile, bats for lint-gate meta-tests.

## Global Constraints

Every task's requirements implicitly include this section.

- spdlog only — never `printf`/`cout`/`LV_LOG_*`.
- SPDX header `// SPDX-License-Identifier: GPL-3.0-or-later` on new files.
- Design tokens, never raw values: `theme_manager_get_color("card_bg")`, `theme_manager_get_spacing("space_md")`, `#space_md` in XML.
- Declarative UI rules: no `lv_obj_add_event_cb`, no imperative visibility, no `lv_label_set_text`, no C++ styling. Measured layout is an explicit permitted exception.
- Widget lookup by `lv_obj_find_by_name()`, never `lv_obj_get_child()` with a fixed index.
- Comments say what the code does and why; never reference a refactor, a session, or "the fix".
- Conventional commits, subject plus ~4 lines.
- Stage files explicitly by path — never `git add -A`/`-u`/`commit -a` (SHARED worktree). Never `--no-verify`.
- `make test` builds tests but does NOT run them; `make -j` builds only the app. Run tags via `./build/bin/helix-tests "[tag]"`; never unfiltered (`[slow]`).
- Never modify anything under `lib/` (symlinks to the main tree).

---

## Owner rulings on the spec-versus-code conflicts

The plan draft surfaced six places where the approved spec and the current code
disagree. These are the decisions; they are binding and the tasks below are
written to match.

**1. Absent spans must fall back to the registry (Task 10). MUST FIX.** The spec
claims `parse_widget_array()` already does this. It does not - it initialises
`colspan`/`rowspan` to `1` and only overwrites when the key is present. Since the
migration erases spans, every widget on every upgraded install would come back one
track wide, silently. **Fix `parse_widget_array` to seed from the registry**, as
Task 10 does. Do not instead have the migration write spans: the registry is the
single source for a widget's authored size, and a coordinate table frozen into a
migration would be a second one.

**2. Row tracks come from the grid (Task 6).** Accepted as planned. A sparse page
leaves the bottom of the grid empty rather than stretching every track to fill the
height. Non-square cells on a sparse dashboard defeat the entire point of the
change.

**3. The measured-aspect invariant stays two-tier (Tasks 5 and 6).** Nominal aspect
is pinned at [0.95, 1.05], measured at [0.80, 1.25]. Test-only, no user impact.

**4. Snap rounds from the track pitch, not a halved track count (Task 3).**
The spec's `ncells / 2` mechanism is exact only for even counts. Rounding from
`pitch = (size + gutter) / ncells` is exact at any count. Keep the plan's version.

**5. `test_grid_edit_mode.cpp:1404` is NOT rewritten (Task 2).** It drives a
200x200 area where `min(18, 200/3) == 18`, so all 16 assertions survive unchanged.
The spec's claim that it needs rewriting predates the per-axis design.

**6. Track counts are floored to a whole number of cells. CHANGED FROM THE SPEC.**
The spec specifies 13 columns for 800x480 and 17 for 1024x600 and 1280x720. Whole-cell
widgets snap to even boundaries, so the final odd track is permanently unreachable -
a dead strip down the right edge of the three most common panels. Flooring each
track count to a multiple of `TRACKS_PER_CELL` removes the strip **and** makes the
cells measurably squarer, because the leftover was being spread across every track:

| tier | spec | aspect | floored | aspect |
|---|---|---|---|---|
| Medium 800x480 | 13x8 | 0.928 | 12x8 | **1.013** |
| Large 1024x600 | 17x10 | 0.897 | 16x10 | **0.960** |
| XLarge 1280x720 | 17x10 | 0.937 | 16x10 | **1.003** |

Tiers already even are unaffected. This is also the principled form: if a track is
half a cell, a grid should be a whole number of cells wide.

**Task 5 must therefore floor both axes**, and every expected-grid table in this
plan changes accordingly - `{800,480,13,8}` becomes `{800,480,12,8}`, and both
`{1024,600,17,10}` and `{1280,720,17,10}` become `16,10`. The 1920x440 case
(48x11) floors its row axis to 10. Re-derive every span and anchor table in Tasks
7, 8 and 9 against the floored counts rather than the spec's odd ones.

---

**7. The square-cell invariant is asserted on MEASURED cells, not nominal. RULING, supersedes the plan's Task 5 test.**
Ruling 6 (floor track counts to whole cells) collides with the nominal
[0.95, 1.05] aspect assertion: flooring shrinks an axis by up to ~6%, and
nominal aspect divides raw panel extent by track count, ignoring the container
padding and inter-track gutters that differ per axis. Measured against the real
content boxes, the two metrics are **anti-correlated**:

| tier | grid | nominal | measured |
|---|---|---|---|
| Micro 480x272 | 14x8 | 1.008 | 0.923 |
| Micro-p 272x480 | 8x14 | 0.992 | 1.189 |
| Tiny 480x320 | 12x8 | 1.000 | 0.886 |
| Small 480x400 | 12x10 | 1.000 | 0.876 |
| Medium 800x480 | 12x8 | **1.111 fails** | 1.013 |
| Portrait 480x800 | 8x12 | **0.900 fails** | 1.062 |
| Large 1024x600 | 16x10 | **1.067 fails** | 0.960 |
| XLarge 1280x720 | 16x10 | **1.111 fails** | 1.003 |

Nominal fails 4 of 8; measured fails 0 of 8. Every tier that fails nominal has a
near-perfect measured cell, and the tiers that pass nominal have the worst real
cells. Nominal is the wrong oracle.

**Therefore:** Task 5 asserts the MEASURED aspect within [0.80, 1.25] against the
tier table's real content boxes and gutters, and does NOT assert nominal aspect at
all. Do not retune `GRID_CELL` and do not relax ruling 6 to satisfy a metric that
does not describe what is on screen. The measured range 0.876-1.189 is a genuine
improvement on the pre-change model's 0.70-1.43.

---

## What could silently break

This codebase's characteristic failure is a lookup or predicate that fails with no crash, no log, and no test. Twelve such sites exist in this change. Each is paired with the assertion that catches it; the assertion is written into the task listed.

| # | Silent failure | Assertion that catches it | Task |
|---|---|---|---|
| 1 | **`parse_widget_array` defaults absent spans to `1`, not the registry span.** `src/system/panel_widget_config.cpp:96-99` sets `int colspan = 1;` and only overwrites when the key exists. §4c's migration *erases* `colspan`/`rowspan`, so every migrated widget would come back one half-cell wide — 27px at micro — with no error. The spec asserts the opposite ("absent spans already fall back to registry defaults"); that is false today. | Unit test: an entry JSON with `id` + `enabled` and **no** span keys parses to `find_widget_def(id)->colspan` / `->rowspan`. | 10 |
| 2 | **Row tracks come from content, not from the grid.** `panel_widget_manager.cpp:585-619` builds `grid_rows = max(max_row_used, cached_rows)`, so the container has as many row tracks as the widgets happen to use. With 8 nominal rows and 5 used, cells are 1.6× taller than wide and the aspect promise silently evaporates. | Test that drives each shipping geometry, builds the default set, and asserts the *measured* `cell_h / cell_w` is within `[0.80, 1.25]`. | 6 |
| 3 | **Legacy flat-array configs lose their widget set.** `panel_widget_config.cpp:222-236`: if no entry `has_grid_position()`, the config is treated as pre-grid and replaced with `build_defaults()`. After v22 unplaces everything, a legacy array config trips this and every deliberate hide is discarded. | Migration test with a v21 fixture whose `panel_widgets/home` is a **flat array**: after init, the widget set and the `enabled:false` hides must survive. | 11 |
| 4 | **Odd track counts break the `ncells / 2` snap trick.** §2c's mechanism is exact only for even `ncells`, because `pitch = (size + gutter) / ncells`; halving the count doubles the pitch exactly only when the count is even. 800x480 gives 13 columns and 1024x600 / 1280x720 give 17 — the pitch is 8% wrong and the error accumulates to a full cell across the row, with no symptom but a misaligned drop. | `round_to_grid_cell` step test at `ncells = 13, step = 2`: snapping a point at track 6.0 must return 6, and at track 7.0 must return 6 or 8, never 7. | 3 |
| 5 | **Uninitialized `LayoutManager` yields a 4x4 grid.** `LayoutManager::width_` defaults to `0` (`include/layout_manager.h:82`) and `init()` runs at `application.cpp:1559`. `clamp(0 / 34, 4, 64)` is `4`, silently, on any path that asks before init. | Test: with `LayoutManagerTestAccess::reset()`, `get_dimensions()` returns exactly `{MIN_TRACKS, MIN_TRACKS}` — pinned, so the degenerate answer is a decision rather than an accident. | 5 |
| 6 | **Re-authored spans land in a different pixel band.** Doubling spans does **not** preserve physical size: micro goes from 6 columns to 14, so an old `colspan 2` (142px, ≥ `W_NORMAL`) becomes `colspan 4` (121px, **below** `W_NORMAL`) and the widget silently renders its compact layout. Verified by arithmetic below. | Table-driven test over every `PanelWidgetDef` × all 8 shipping geometries: compute `grid_track_extent()` for the authored span and assert the resulting band matches a checked-in expected-band table. | 7 |
| 7 | **`temp_stack` class of failure — a widget still reading a span.** One widget was caught reading `rowspan` only by manual audit. `temp_graph_widget.h:88-89` still *stores* `current_colspan_ = 2` / `current_rowspan_ = 2` as defaults, now in units that no longer exist. | Grep gate step in Task 7 verifying no `on_size_changed` body references its `colspan`/`rowspan` parameters outside a `spdlog::` call. | 7 |
| 8 | **AMS mini-status shows more, smaller spools.** `ui_ams_mini_status.cpp:609` computes `visible = (avail_w + gap) / (MIN_SPOOL_W + gap)` with `MIN_SPOOL_W = 60`. Its authored width changes; nothing logs a complaint. | Test asserting the visible spool count at each panel's authored `ams` width against an explicit table. | 12 |
| 9 | **`/ui/cached_grid/<panel>/rows` carries an old-unit row count.** `panel_widget_manager.cpp:602-607` reads it as a floor. A `4` written by the old grid is meaningless against 8 nominal rows. | Migration test asserting the `/ui/cached_grid` node is absent after v22. | 11 |
| 10 | **`default_layout.json` has no `micro` landscape key.** Micro falls back to `tiny` (`panel_widget_config.cpp:603-611`). Old: both were 6 columns, so the fallback was free. New: micro is **14** columns and tiny is **12**, so micro anchors under-fill by two columns with no warning. | Extend the shipped-anchors test to assert `col + colspan == cols` for the widest anchor row at every breakpoint key, micro included. | 8 |
| 11 | **`MIN_PORTRAIT_COLS` removal leaves stale doc/test references.** Deleting a `static constexpr` is compile-checked in C++ but not in Markdown. | `grep -rn "MIN_PORTRAIT_COLS\|TARGET_CELL_W_PX\|TARGET_CELL_H_PX\|MAX_DYNAMIC\|MIN_DYNAMIC\|GRID_DIMS" src include tests docs` must return zero hits. | 13 |
| 12 | **`clamp_span` silently shrinks a user's saved span.** `grid_edit_mode.cpp:861-864` clamps to `effective_min/max_*span()`. A saved span in old units that survives migration would be clamped into the new min/max without a log. §4c erases spans, so this is only reachable if the migration is skipped — which is exactly what a version-gate mistake looks like. | Migration idempotence test: re-initing an already-v22 config must leave every span key absent. | 11 |

---

## Reference data the implementation depends on

Measured content boxes and gutters, from `.superpowers/sdd/2026-08-05-grid-metrics-followups/span-pixel-table.md`. `content_w` / `content_h` are the home grid container's content box and do **not** change with the track count.

| tier | panel | bp | `GRID_CELL` | new cols | new rows | gutter | content_w | content_h | track_w | track_h |
|---|---|---|---|---|---|---|---|---|---|---|
| Micro | 480x272 | 0 | 34 | 14 | 8 | 2 | 430 | 264 | 28.86 | 31.25 |
| Tiny | 480x320 | 1 | 40 | 12 | 8 | 2 | 418 | 312 | 33.00 | 37.25 |
| Small | 480x400 | 2 | 40 | 12 | 10 | 4 | 414 | 388 | 30.83 | 35.20 |
| Medium | 800x480 | 3 | 60 | 13 | 8 | 5 | 710 | 466 | 50.00 | 53.88 |
| Large | 1024x600 | 4 | 60 | 17 | 10 | 6 | 904 | 584 | 47.53 | 53.00 |
| XLarge | 1280x720 | 5 | 72 | 17 | 10 | 8 | 1128 | 700 | 58.82 | 62.80 |
| Micro portrait | 272x480 | 0 | 34 | 8 | 14 | 2 | 264 | 394 | 31.25 | 26.29 |
| Portrait | 480x800 | 3 | 60 | 8 | 13 | 5 | 466 | 664 | 53.88 | 46.46 |

`track = (content - (n-1) * gutter) / n`. `extent(span) = span * track + (span - 1) * gutter`.

**Extent of a doubled span, against the thresholds in `include/panel_widget_size.h`** (`W_NORMAL = 134`, `W_WIDE = 204`, `H_TALL = 130`, `H_TALLER = 197`):

| tier | span 4 (was colspan 2) | span 6 (was colspan 3) | rowspan 4 (was rowspan 2) |
|---|---|---|---|
| Micro | **121.4 — loses `W_NORMAL`** | **183.1 — loses `W_WIDE`** | 131.0 (holds, by 1px) |
| Tiny | 138.0 | 208.0 | 155.0 |
| Small | 135.3 | 205.0 | 152.8 |
| Medium | 215.0 (**gains** `W_WIDE`) | 325.0 | 230.5 (**gains** `H_TALLER`) |
| Large | 208.1 (**gains** `W_WIDE`) | 315.2 | 230.0 (**gains** `H_TALLER`) |
| XLarge | 259.3 (**gains** `W_WIDE`) | 393.0 | 275.2 (**gains** `H_TALLER`) |
| Micro portrait | **131.0 — loses `W_NORMAL`** | 197.5 | **111.2 — loses `H_TALL`** |
| Portrait | 230.5 (**gains** `W_WIDE`) | 348.3 | 200.8 (**gains** `H_TALLER`) |

This is why §4a says "double today's values, **then adjusted**". Task 7 turns this table into an executable assertion.

---

## File Structure

**Modified — core**

| File | Responsibility after this change |
|---|---|
| `include/grid_layout.h` | `GRID_CELL[]`, `MIN_TRACKS`, `MAX_TRACKS`, `TRACKS_PER_CELL`; the old constants are gone |
| `src/ui/grid_layout.cpp` | One branchless `get_dimensions()`; `GRID_DIMS` and the `LayoutType` switch are gone |
| `include/panel_widget_registry.h` | `PanelWidgetDef` gains `supports_half_col` / `supports_half_row` |
| `src/ui/panel_widget_registry.cpp` | Every default span re-expressed in tracks; five widgets marked half-capable |
| `include/grid_edit_mode.h` | `round_to_grid_cell` and `compute_resize_result` gain a `step`; new `snap_step_for()` |
| `src/ui/grid_edit_mode.cpp` | Per-axis hit band; step-aware snapping; two-tier dot lattice |
| `src/ui/panel_widget_manager.cpp` | Row tracks from the grid, not from content |
| `src/system/panel_widget_config.cpp` | Registry-default span fallback; the two scarcity workarounds deleted |
| `include/config.h` | `CURRENT_CONFIG_VERSION` 21 → 22 |
| `src/system/config.cpp` | `migrate_v21_to_v22` + one dispatch line |

**Modified — data**

`assets/config/default_layout.json`, `assets/config/panel_widgets/{ad5x,ad5x_zmod,cc1}/home.json`

**Modified — tests**

`tests/unit/test_grid_layout.cpp`, `test_default_layout.cpp`, `test_grid_edit_mode.cpp`, `test_panel_widget_portrait_span.cpp`, `test_widget_size_ams_mini_status.cpp`

**New — tests**

`tests/unit/test_grid_square_cells.cpp` (aspect + rotation invariants), `tests/unit/test_registry_span_bands.cpp` (span → pixel band table), `tests/unit/test_config_migration_v22.cpp`

**Modified — docs**

`docs/devel/LAYOUT_SYSTEM.md` §"Home Widget Grid" (lines 325-400), `docs/user/guide/home-panel.md` (lines 15-26, 231, 538), `docs/user/TROUBLESHOOTING.md:592-629`

---

## Sequencing and the point of no return

Tasks 1-4 are inert or independent and leave the shipped behaviour identical. **Task 5 is the visible break:** from Task 5 until Task 9 completes, a developer's existing `settings.json` holds coordinates in old units and the dashboard renders small and clustered top-left. Tests stay green throughout. Do not tag a release between Task 5 and Task 9; smoke-test intermediate tasks with a scratch config dir:

```bash
export HELIX_CONFIG_DIR=/tmp/helix-config-grid-cell-metrics && mkdir -p "$HELIX_CONFIG_DIR"
```

**Task 11 is the point of no return for saved user layouts.** Once `CURRENT_CONFIG_VERSION` is 22 and a user's config is stamped, `col`/`row`/`colspan`/`rowspan` are gone and reverting the code does not bring them back. It is deliberately last of the coupled tasks: if it landed before Task 5, an existing install would be unplaced onto the *old* grid, re-seated, its old-unit coordinates written back and stamped v22, and the later grid change would then have no migration left to fix them.

---

### Task 1: Half-cell capability on the widget registry

**Files:**
- Modify: `include/panel_widget_registry.h:20-56`
- Modify: `src/ui/panel_widget_registry.cpp:55-97`
- Test: `tests/unit/test_grid_layout.cpp` (append near the `[widget_def][scalability]` cases at `:389-449`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  struct PanelWidgetDef {
      // ...existing fields through `multi_instance`...
      bool supports_half_col = false;
      bool supports_half_row = false;
      WidgetFactory factory = nullptr;
      SubjectInitFn init_subjects = nullptr;
  };
  ```

**Critical detail:** `s_widget_defs` uses positional aggregate initialization. The new fields go **after** `multi_instance` and **before** `factory`, so the existing rows that pass a trailing `true` for `multi_instance` (`power_device`, `fan_stack`, `fan`, `thermistor`, `temp_graph`, `favorite_macro`) keep compiling unchanged.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_grid_layout.cpp`:

```cpp
TEST_CASE("PanelWidgetDef: half-cell capability is opt-in", "[widget_def][half_cell][1126]") {
    // #1126 grants half-cell resolution to the small single-action widgets only.
    // Anything that renders a chart, an image, a list or a video frame needs a
    // whole cell on both axes, so its flags stay false.
    const std::vector<std::string> half_capable = {"lock", "shutdown", "firmware_restart",
                                                   "led_controls", "clock"};
    const std::vector<std::string> whole_cell_only = {"camera",    "temp_graph", "print_status",
                                                      "job_queue", "ams",        "tips"};

    for (const auto& id : half_capable) {
        INFO("widget " << id);
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        CHECK(def->supports_half_col);
    }
    for (const auto& id : whole_cell_only) {
        INFO("widget " << id);
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        CHECK_FALSE(def->supports_half_col);
        CHECK_FALSE(def->supports_half_row);
    }

    // clock is the only one that can halve on both axes.
    const auto* clock = helix::find_widget_def("clock");
    REQUIRE(clock != nullptr);
    CHECK(clock->supports_half_row);
}

TEST_CASE("PanelWidgetDef: half-cell defaults to off", "[widget_def][half_cell]") {
    helix::PanelWidgetDef def{};
    CHECK_FALSE(def.supports_half_col);
    CHECK_FALSE(def.supports_half_row);
}
```

- [ ] **Step 2: Run the test and watch it fail**

```bash
make test -j && ./build/bin/helix-tests "[half_cell]"
```

Expected: compile error, `'const struct helix::PanelWidgetDef' has no member named 'supports_half_col'`.

- [ ] **Step 3: Add the fields**

In `include/panel_widget_registry.h`, immediately after `bool multi_instance = false;` (line 35):

```cpp
    /// True when this widget may occupy half a cell on that axis (#1126).
    ///
    /// The home grid lays out half-cell tracks, so an authored colspan of 1 is
    /// GridLayout::TRACKS_PER_CELL tracks wide. A widget with the flag set may
    /// also be placed and sized at odd track counts; edit mode snaps everything
    /// else to even boundaries so a whole-cell widget can never straddle two.
    bool supports_half_col = false;
    bool supports_half_row = false;
```

- [ ] **Step 4: Mark the five half-capable widgets**

In `src/ui/panel_widget_registry.cpp`, the rows for `shutdown` (`:59`), `lock` (`:60`), `firmware_restart` (`:63`), `led_controls` (`:66`) and `clock` (`:85`) gain trailing positional values. Each of those rows currently ends at `max_rowspan` with no `multi_instance`, so append `, false, true, false` (multi_instance, supports_half_col, supports_half_row) — except `clock`, which takes `, false, true, true`. Update the two column-legend comments at `:56` and `:88` to read:

```cpp
    //  hint   en  col row min_c min_r max_c max_r  multi  half_c half_r
```

- [ ] **Step 5: Run the test and watch it pass**

```bash
make test -j && ./build/bin/helix-tests "[half_cell]"
```

Expected: `All tests passed`.

- [ ] **Step 6: Prove the whole registry still parses**

```bash
./build/bin/helix-tests "[widget_def]"
```

Expected: PASS — in particular "registry entries have valid scalability constraints" (`test_grid_layout.cpp:433`), which sweeps every row and would catch a positional-initializer slip.

- [ ] **Step 7: Commit**

```bash
git add include/panel_widget_registry.h src/ui/panel_widget_registry.cpp tests/unit/test_grid_layout.cpp
git commit -m "feat(widgets): declare which widgets may occupy half a grid cell

The home grid is moving to half-cell tracks, so each widget definition now
states whether it can live at half-cell resolution on each axis. The small
single-action widgets can; anything rendering a chart, image, list or video
frame needs a whole cell and keeps the default of false."
```

---

### Task 2: Resize hit band scales with the widget

**Files:**
- Modify: `src/ui/grid_edit_mode.cpp:44-46, 871-887`
- Test: `tests/unit/test_grid_edit_mode.cpp` (append after the `detect_resize_edge` cases ending at `:1430`)

**Interfaces:**
- Consumes: nothing.
- Produces: `GridEditMode::detect_resize_edge(int px, int py, const lv_area_t& widget_area) const` — signature unchanged; behaviour now clamps the inward band to one third of the widget's extent on each axis independently.

**Note on the existing pinned test:** `test_grid_edit_mode.cpp:1404` "detect_resize_edge: 18+18 hit zone boundaries" drives a 200x200 area. `min(18, 200/3) == 18`, so all 16 of its assertions still hold. Do **not** delete it; it becomes the large-widget half of the contract. The spec's §5 table lists it as needing a rewrite — that was written before the per-axis form was chosen and is not correct against this implementation.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_grid_edit_mode.cpp`:

```cpp
TEST_CASE("detect_resize_edge: the inward band is a fraction of a narrow widget",
          "[grid_edit][resize][1126]") {
    GridEditMode em;
    // 30px wide, 90px tall — a half-cell-wide widget on a micro panel. Two flat
    // 18px inward bands would overlap and leave no interior at all, so every
    // pixel would report an edge and the widget could never be dragged.
    lv_area_t narrow = {100, 100, 130, 190};

    // Horizontal centre is 10px from each vertical edge: inward_x = 30/3 = 10,
    // so x=115 is outside both the left and the right inward band.
    CHECK(em.detect_resize_edge(115, 145, narrow) == GridEditMode::ResizeEdge::None);

    // The edges themselves still resize.
    CHECK(em.detect_resize_edge(101, 145, narrow) == GridEditMode::ResizeEdge::Left);
    CHECK(em.detect_resize_edge(129, 145, narrow) == GridEditMode::ResizeEdge::Right);
}

TEST_CASE("detect_resize_edge: the two axes clamp independently",
          "[grid_edit][resize][1126]") {
    GridEditMode em;
    // 30 wide x 200 tall. inward_x = 10, inward_y = min(18, 66) = 18.
    lv_area_t tall = {100, 100, 130, 300};

    // 12px below the top edge: inside the 18px vertical band, so Top wins even
    // though the horizontal centre is outside the (narrower) horizontal bands.
    CHECK(em.detect_resize_edge(115, 112, tall) == GridEditMode::ResizeEdge::Top);

    // 40px below the top edge: outside both bands on both axes.
    CHECK(em.detect_resize_edge(115, 140, tall) == GridEditMode::ResizeEdge::None);
}
```

- [ ] **Step 2: Run the test and watch it fail**

```bash
make test -j && ./build/bin/helix-tests "[grid_edit][resize]"
```

Expected: FAIL — `detect_resize_edge(115, 145, narrow)` returns `Left` or `Right`, not `None`.

- [ ] **Step 3: Derive the band from the widget**

Replace `src/ui/grid_edit_mode.cpp:44-46` with:

```cpp
// Resize edge detection. EDGE_HIT_MARGIN is the outward reach, which is always
// the full value — it lands outside the widget, where there is nothing to run
// out of. EDGE_HIT_INWARD is a ceiling: on a widget narrower than three bands
// the inward reach shrinks with it, so an interior big enough to grab and drag
// always survives. A half-cell widget is ~30px across, where two flat 18px
// bands would overlap and every pixel would report an edge.
static constexpr int EDGE_HIT_INWARD = 18;
static constexpr int EDGE_HIT_MARGIN = 18;
```

Then replace the first four `bool near_*` initialisers at `:874-881` with:

```cpp
    const int inward_x = std::min(EDGE_HIT_INWARD, lv_area_get_width(&widget_area) / 3);
    const int inward_y = std::min(EDGE_HIT_INWARD, lv_area_get_height(&widget_area) / 3);

    bool near_right = (px >= widget_area.x2 - inward_x && px <= widget_area.x2 + EDGE_HIT_MARGIN);
    bool near_left = (px >= widget_area.x1 - EDGE_HIT_MARGIN && px <= widget_area.x1 + inward_x);
    bool near_bottom = (py >= widget_area.y2 - inward_y && py <= widget_area.y2 + EDGE_HIT_MARGIN);
    bool near_top = (py >= widget_area.y1 - EDGE_HIT_MARGIN && py <= widget_area.y1 + inward_y);
```

`lv_area_get_width` / `lv_area_get_height` are LVGL's inclusive accessors (`x2 - x1 + 1`), which is the convention already used at `grid_edit_mode.cpp:618`.

- [ ] **Step 4: Run the tests and watch them pass**

```bash
make test -j && ./build/bin/helix-tests "[grid_edit]"
```

Expected: PASS, including the untouched "18+18 hit zone boundaries" case.

- [ ] **Step 5: Confirm the metrics gate is unmoved**

```bash
python3 scripts/check_grid_metrics_single_source.py
```

Expected: `OK: src/ui/grid_edit_mode.cpp has 2 grid-dimension call site(s)`.

- [ ] **Step 6: Commit**

```bash
git add src/ui/grid_edit_mode.cpp tests/unit/test_grid_edit_mode.cpp
git commit -m "fix(grid-edit): scale the resize hit band to the widget

The inward half of the resize band was a flat 18px per edge. On a widget
narrower than 36px the two bands overlap, detect_resize_edge() reports an
edge for every pixel and the widget can only be resized, never dragged. The
inward reach is now capped at a third of the widget's extent, per axis."
```

---

### Task 3: Step-aware snapping

**Files:**
- Modify: `include/grid_layout.h` (add `TRACKS_PER_CELL` near `NUM_BREAKPOINTS` at `:82`)
- Modify: `include/grid_edit_mode.h:109-124`
- Modify: `src/ui/grid_edit_mode.cpp:924-933, 935-985, 1287-1288, 1596-1598, 1622-1628, 1726-1735`
- Test: `tests/unit/test_grid_edit_mode.cpp` (append after the `round_to_grid_cell` cases)

**Interfaces:**
- Consumes: `PanelWidgetDef::supports_half_col` / `supports_half_row` (Task 1).
- Produces:
  ```cpp
  // include/grid_layout.h, inside class GridLayout
  static constexpr int TRACKS_PER_CELL = 1;

  // include/grid_edit_mode.h, public statics
  static int round_to_grid_cell(int px, int content_origin, int content_size, int ncells,
                                int gutter, int step);
  static ResizeResult compute_resize_result(ResizeEdge edge, int orig_col, int orig_row,
                                            int orig_colspan, int orig_rowspan,
                                            int new_edge_cell, int ncells, int step);
  static std::pair<int, int> snap_step_for(const std::string& widget_id);
  ```

`TRACKS_PER_CELL` lands at `1` here and becomes `2` in Task 5, at the same moment the grid's tracks become half-cells. Every snap site reads it, so the two states are both correct and the intermediate build behaves exactly as it does today.

`snap_step_for` returns `{col_step, row_step}` in tracks.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_grid_edit_mode.cpp`:

```cpp
TEST_CASE("round_to_grid_cell: step 2 snaps to even boundaries on an even grid",
          "[grid_edit][resize][half_cell]") {
    // 12 tracks in 600px, no gutter: pitch = 50. Boundaries at 0,50,...,600.
    CHECK(GridEditMode::round_to_grid_cell(100, 0, 600, 12, 0, 2) == 2);
    CHECK(GridEditMode::round_to_grid_cell(140, 0, 600, 12, 0, 2) == 2); // 2.8 -> 2
    CHECK(GridEditMode::round_to_grid_cell(160, 0, 600, 12, 0, 2) == 4); // 3.2 -> 4
    CHECK(GridEditMode::round_to_grid_cell(600, 0, 600, 12, 0, 2) == 12);
    // Step 1 still reaches odd boundaries.
    CHECK(GridEditMode::round_to_grid_cell(150, 0, 600, 12, 0, 1) == 3);
}

TEST_CASE("round_to_grid_cell: step 2 stays exact on an odd track count",
          "[grid_edit][resize][half_cell]") {
    // 13 tracks in 650px, no gutter: pitch = 50 exactly. Halving the track
    // count would give a pitch of 650/6 = 108.3 instead of 100, an 8% error
    // that compounds to a whole cell by the right edge.
    CHECK(GridEditMode::round_to_grid_cell(300, 0, 650, 13, 0, 2) == 6);  // 6.0
    CHECK(GridEditMode::round_to_grid_cell(350, 0, 650, 13, 0, 2) == 6);  // 7.0 -> 6 (ties down)
    CHECK(GridEditMode::round_to_grid_cell(400, 0, 650, 13, 0, 2) == 8);  // 8.0
    // The final odd track is unreachable at step 2 — the clamp is the last
    // even boundary, not the track count.
    CHECK(GridEditMode::round_to_grid_cell(650, 0, 650, 13, 0, 2) == 12);
}

TEST_CASE("round_to_grid_cell: gutters do not shift the stepped boundary",
          "[grid_edit][resize][half_cell]") {
    // pitch = (content_size + gutter) / ncells = (404 + 2) / 14 = 29.0
    CHECK(GridEditMode::round_to_grid_cell(116, 0, 404, 14, 2, 2) == 4); // 4.0
    CHECK(GridEditMode::round_to_grid_cell(174, 0, 404, 14, 2, 2) == 6); // 6.0
}

TEST_CASE("snap_step_for: whole-cell widgets step by a full cell",
          "[grid_edit][resize][half_cell]") {
    auto [wc, wr] = GridEditMode::snap_step_for("camera");
    CHECK(wc == helix::GridLayout::TRACKS_PER_CELL);
    CHECK(wr == helix::GridLayout::TRACKS_PER_CELL);

    auto [hc, hr] = GridEditMode::snap_step_for("shutdown");
    CHECK(hc == 1);
    CHECK(hr == helix::GridLayout::TRACKS_PER_CELL);

    auto [cc, cr] = GridEditMode::snap_step_for("clock");
    CHECK(cc == 1);
    CHECK(cr == 1);

    // An id with no registry entry gets the conservative whole-cell answer.
    auto [uc, ur] = GridEditMode::snap_step_for("not_a_widget");
    CHECK(uc == helix::GridLayout::TRACKS_PER_CELL);
    CHECK(ur == helix::GridLayout::TRACKS_PER_CELL);
}

TEST_CASE("compute_resize_result: a stepped span never lands on an odd count",
          "[grid_edit][resize][half_cell]") {
    // Right edge dragged to track 7 on a widget at col 2, step 2: the span must
    // round to an even number rather than leaving the widget straddling a cell.
    auto r = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Right, 2, 0, 2, 2, 7,
                                                 12, 2);
    CHECK(r.colspan % 2 == 0);
    CHECK(r.colspan >= 2);

    // The floor is one whole cell, not one track.
    auto tiny = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Right, 2, 0, 4, 2, 2,
                                                    12, 2);
    CHECK(tiny.colspan == 2);
}
```

- [ ] **Step 2: Run the tests and watch them fail**

```bash
make test -j && ./build/bin/helix-tests "[half_cell]"
```

Expected: compile error, `no matching function for call to 'round_to_grid_cell'` (six arguments given, five expected).

- [ ] **Step 3: Add `TRACKS_PER_CELL`**

In `include/grid_layout.h`, immediately after `static constexpr int NUM_BREAKPOINTS = 6;` (line 82):

```cpp
    /// Number of grid tracks that make up one authored cell.
    ///
    /// Authored spans — in the registry, in default_layout.json and in a saved
    /// layout — are expressed in cells. The grid lays out tracks. Every site
    /// that converts between the two reads this, so a widget that is not
    /// allowed to occupy half a cell can never be placed straddling one.
    static constexpr int TRACKS_PER_CELL = 1;
```

- [ ] **Step 4: Make snapping step-aware**

Replace `src/ui/grid_edit_mode.cpp:924-933` with:

```cpp
int GridEditMode::round_to_grid_cell(int px, int content_origin, int content_size, int ncells,
                                     int gutter, int step) {
    if (ncells <= 0 || step <= 0) {
        return 0;
    }
    // LVGL distributes LV_GRID_FR(1) tracks as (content - (n-1)*gutter)/n, so
    // the track-to-track pitch reduces exactly to (content + gutter)/n. Working
    // from the pitch keeps the arithmetic exact for any track count; dividing
    // the count instead only doubles the pitch when the count is even, and is
    // 8% wrong on the 13- and 17-column grids.
    const float pitch = (static_cast<float>(content_size) + static_cast<float>(gutter)) /
                        static_cast<float>(ncells);
    if (pitch <= 0.0f) {
        return 0;
    }
    const float tracks = static_cast<float>(px - content_origin) / pitch;
    const int snapped =
        static_cast<int>(std::round(tracks / static_cast<float>(step))) * step;
    return std::clamp(snapped, 0, (ncells / step) * step);
}

std::pair<int, int> GridEditMode::snap_step_for(const std::string& widget_id) {
    const auto* def = find_widget_def(widget_id);
    if (!def) {
        return {GridLayout::TRACKS_PER_CELL, GridLayout::TRACKS_PER_CELL};
    }
    return {def->supports_half_col ? 1 : GridLayout::TRACKS_PER_CELL,
            def->supports_half_row ? 1 : GridLayout::TRACKS_PER_CELL};
}
```

In `compute_resize_result` (`:935-985`) add `int step` as the final parameter, replace the `Right` case's `r.colspan = std::max(new_colspan, 1);` with `r.colspan = std::max((new_colspan / step) * step, step);`, the `Bottom` case's `r.rowspan = std::max(new_rowspan, 1);` with `r.rowspan = std::max((new_rowspan / step) * step, step);`, and in the `Left` / `Top` cases replace `right_edge - 1` with `right_edge - step` and `bottom_edge - 1` with `bottom_edge - step`.

- [ ] **Step 5: Thread the step through all ten call sites**

`grid_edit_mode.cpp:1287-1288` (`handle_drag_move`) — the dragged widget's id is available from `find_config_index_for_widget(selected_)`; look it up before the two calls:

```cpp
    const int drag_cfg = find_config_index_for_widget(selected_);
    const std::string drag_id =
        drag_cfg >= 0
            ? config_->page_entries(static_cast<size_t>(page_index_))[static_cast<size_t>(drag_cfg)]
                  .id
            : std::string{};
    const auto [col_step, row_step] = snap_step_for(drag_id);
    int target_col = round_to_grid_cell(widget_left, content_area.x1, cw, ncols, m.gutter, col_step);
    int target_row = round_to_grid_cell(widget_top, content_area.y1, ch, nrows, m.gutter, row_step);
```

`:1596-1598` (`handle_resize_move` pixel floor) — one whole step, not one track:

```cpp
    // Minimum size in pixels: one snap step, so a whole-cell widget can never
    // be dragged down to a half cell.
    int min_w = static_cast<int>(grid_track_extent(m.cell_w, m.gutter, col_step));
    int min_h = static_cast<int>(grid_track_extent(m.cell_h, m.gutter, row_step));
```

`:1622-1628` and `:1726-1735` — pass `col_step` for the `Left`/`Right` branches and `row_step` for the `Top`/`Bottom` branches; pass the matching step as the new final argument to both `compute_resize_result` calls (`:1631` and `:1740`). In both functions derive `col_step`/`row_step` from `entry.id` — `handle_resize_move` already fetches `entry` at `:1640-1641`; move that fetch above the rounding block.

`:661` and `:1033` (`screen_to_grid_cell`) are catalog-placement paths that produce an *origin* for a not-yet-chosen widget, so they keep stepping at 1 and are unchanged.

- [ ] **Step 6: Run the tests and watch them pass**

```bash
make test -j && ./build/bin/helix-tests "[grid_edit]"
```

Expected: PASS, including the pre-existing `round_to_grid_cell` and `screen_to_grid_cell` cases, which all pass `step = 1` explicitly after this change.

- [ ] **Step 7: Confirm the metrics gate budget is unchanged**

```bash
python3 scripts/check_grid_metrics_single_source.py && bats tests/shell/test_grid_metrics_gate.bats
```

Expected: `OK: src/ui/grid_edit_mode.cpp has 2 grid-dimension call site(s)` and all bats tests pass. Nothing in this task calls `GridLayout::get_cols`/`get_rows`/`get_dimensions`; the step comes from the registry and `TRACKS_PER_CELL`, both outside the gate's pattern. **The `LIMIT = 2` budget does not need raising for any task in this plan.**

- [ ] **Step 8: Commit**

```bash
git add include/grid_layout.h include/grid_edit_mode.h src/ui/grid_edit_mode.cpp tests/unit/test_grid_edit_mode.cpp
git commit -m "feat(grid-edit): snap each widget to the resolution it supports

Drag and resize now round to a per-widget track step taken from the registry:
one track for a widget that may occupy half a cell, a whole cell otherwise.
The rounding works from the track pitch rather than a halved track count, so
it stays exact on the 13- and 17-column grids."
```

---

### Task 4: Two-tier dot lattice

**Files:**
- Modify: `src/ui/grid_edit_mode.cpp:2094-2169` (`create_dots_overlay`), `:2135-2136` (dot constants)
- Modify: `include/grid_edit_mode.h:140-141` (add `refresh_dots_overlay`)
- Modify: `src/ui/grid_edit_mode.cpp` `select_widget()` — call the refresh
- Test: `tests/unit/test_grid_edit_mode.cpp`

**Interfaces:**
- Consumes: `GridEditMode::snap_step_for(const std::string&)` (Task 3).
- Produces: `void GridEditMode::refresh_dots_overlay();` — private; rebuilds the lattice for the current selection.

The lattice shows the grid the current selection can snap to: major dots at whole-cell boundaries always, minor dots at the intervening half-cell boundaries only while a half-capable widget is selected, and only on the axes it supports. If a dot is visible, it is a legal drop target.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "dots overlay: minor dots appear only for a half-capable selection",
                 "[grid_edit][half_cell][dots]") {
    // The lattice is (cols/step + 1) x (rows/step + 1) intersections. With
    // TRACKS_PER_CELL == 2 on a 12x8 track grid a whole-cell selection draws
    // 7x5 = 35 dots and a half-col selection draws 13x5 = 65.
    const int cols = 12;
    const int rows = 8;
    const int cell = helix::GridLayout::TRACKS_PER_CELL;

    CHECK(GridEditMode::dot_count(cols, rows, cell, cell) == (cols / cell + 1) * (rows / cell + 1));
    CHECK(GridEditMode::dot_count(cols, rows, 1, cell) == (cols + 1) * (rows / cell + 1));
    CHECK(GridEditMode::dot_count(cols, rows, 1, 1) == (cols + 1) * (rows + 1));
}
```

Add the pure counting helper to `include/grid_edit_mode.h` alongside the other statics so the object budget is testable without a live container:

```cpp
    /// Number of lattice intersections drawn for a selection with these snap
    /// steps. Public so the object cost can be pinned without a live grid.
    static int dot_count(int ncols, int nrows, int col_step, int row_step);
```

- [ ] **Step 2: Run the test and watch it fail**

```bash
make test -j && ./build/bin/helix-tests "[dots]"
```

Expected: compile error, `'dot_count' is not a member of 'helix::GridEditMode'`.

- [ ] **Step 3: Implement the counting helper**

In `src/ui/grid_edit_mode.cpp`, next to `round_to_grid_cell`:

```cpp
int GridEditMode::dot_count(int ncols, int nrows, int col_step, int row_step) {
    if (ncols <= 0 || nrows <= 0 || col_step <= 0 || row_step <= 0) {
        return 0;
    }
    return (ncols / col_step + 1) * (nrows / row_step + 1);
}
```

- [ ] **Step 4: Draw the two tiers**

Replace the dot loop at `:2135-2169`. The whole-cell lattice is drawn at full weight; the intervening half-cell intersections are drawn smaller and fainter, and only on the axes the selection supports.

```cpp
    constexpr int DOT_SIZE_MAJOR = 4;
    constexpr int DOT_SIZE_MINOR = 3;
    // Use contrast text color so dots are visible on both light and dark backgrounds
    lv_color_t screen_bg = ThemeManager::instance().current_palette().screen_bg;
    lv_color_t dot_color = theme_manager_get_contrast_color(screen_bg);

    const int cell = GridLayout::TRACKS_PER_CELL;
    auto [col_step, row_step] = selected_ ? snap_step_for(selected_widget_id())
                                          : std::pair<int, int>{cell, cell};

    // c/r run 0..ncols/0..nrows inclusive to draw both edges of the lattice.
    // grid_track_origin() only knows track starts (0..n-1); the final boundary
    // is the right/bottom edge of the last track, not a further track start
    // (which would land one gutter past the content edge).
    auto track_x = [&](int c) {
        return static_cast<int>(c < ncols ? grid_track_origin(m.cell_w, m.gutter, c)
                                          : grid_track_origin(m.cell_w, m.gutter,
                                                              std::max(ncols - 1, 0)) +
                                                m.cell_w);
    };
    auto track_y = [&](int r) {
        return static_cast<int>(r < nrows ? grid_track_origin(m.cell_h, m.gutter, r)
                                          : grid_track_origin(m.cell_h, m.gutter,
                                                              std::max(nrows - 1, 0)) +
                                                m.cell_h);
    };

    for (int r = 0; r <= nrows; r += row_step) {
        for (int c = 0; c <= ncols; c += col_step) {
            const bool major = (c % cell == 0) && (r % cell == 0);
            const int size = major ? DOT_SIZE_MAJOR : DOT_SIZE_MINOR;

            lv_obj_t* dot = lv_obj_create(dots_overlay_);
            lv_obj_set_size(dot, size, size);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, dot_color, 0);
            lv_obj_set_style_bg_opa(dot, major ? LV_OPA_30 : LV_OPA_10, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(dot, track_x(c) - size / 2, track_y(r) - size / 2);
        }
    }
```

Add the id accessor used above, next to `find_config_index_for_widget`:

```cpp
std::string GridEditMode::selected_widget_id() const {
    const int idx = find_config_index_for_widget(selected_);
    if (idx < 0 || !config_) {
        return {};
    }
    return config_->page_entries(static_cast<size_t>(page_index_))[static_cast<size_t>(idx)].id;
}
```

- [ ] **Step 5: Rebuild the lattice when the selection changes**

Add to `include/grid_edit_mode.h` beside `create_dots_overlay()`:

```cpp
    /// Redraw the lattice for the current selection. The visible dots are the
    /// boundaries the selected widget can actually snap to, so they change when
    /// the selection does.
    void refresh_dots_overlay();
    std::string selected_widget_id() const;
```

Implement it as `destroy_dots_overlay(); create_dots_overlay();` and call it at the end of `GridEditMode::select_widget()` — after `selected_` is assigned and after `create_selection_chrome()`, so the chrome buttons stay above the overlay (`create_dots_overlay` documents that ordering at `:2118-2119`).

- [ ] **Step 6: Run the tests and watch them pass**

```bash
make test -j && ./build/bin/helix-tests "[grid_edit]"
```

Expected: PASS.

- [ ] **Step 7: Verify the object cost live**

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
make -j && ./build/bin/helix-screen --test -vv -s 480x272 --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
sleep 5
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate home
```

Long-press a widget to enter edit mode, select `camera` (whole-cell) then `shutdown` (half-col), and confirm the minor dot column appears only for `shutdown`. Kill the instance when done.

- [ ] **Step 8: Commit**

```bash
git add include/grid_edit_mode.h src/ui/grid_edit_mode.cpp tests/unit/test_grid_edit_mode.cpp
git commit -m "feat(grid-edit): show the lattice the selection can snap to

Whole-cell boundaries are always drawn; the half-cell boundaries between them
appear smaller and fainter only while a widget that supports them is selected,
and only on the axes it supports. A visible dot is a legal drop target, and
the extra objects exist only for as long as that selection does."
```

---

### Task 5: One square-cell sizing expression

**Files:**
- Modify: `include/grid_layout.h:84-121` (delete), `:82` (`TRACKS_PER_CELL` 1 → 2), add `GRID_CELL`/`MIN_TRACKS`/`MAX_TRACKS`
- Modify: `src/ui/grid_layout.cpp:20-89`
- Rewrite: `tests/unit/test_grid_layout.cpp:21-67, 481-632`
- Create: `tests/unit/test_grid_square_cells.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  static constexpr int TRACKS_PER_CELL = 2;
  static constexpr int MIN_TRACKS = 4;
  static constexpr int MAX_TRACKS = 64;
  static GridDimensions get_dimensions(UiBreakpoint bp);   // signature unchanged
  ```
  Deleted: `TARGET_CELL_W_PX`, `TARGET_CELL_H_PX`, `MIN_DYNAMIC_COLS`, `MAX_DYNAMIC_COLS`, `MIN_DYNAMIC_ROWS`, `MAX_DYNAMIC_ROWS`, `MIN_PORTRAIT_COLS`, `GRID_DIMS`.

**On `MIN_PORTRAIT_COLS`:** verified removable. Its only consumer is `src/ui/grid_layout.cpp:80`, and its stated purpose (`include/grid_layout.h:114-121`) is to stop `colspan >= 2` predicates misfiring in portrait. A grep of every `on_size_changed` override confirms no widget reads a span for a layout decision — `temp_graph_widget.cpp:136-137` stores them and `clock`/`fan_stack`/`nozzle_temps` pass them to `spdlog` only.

**This is the visible-break task.** From here until Task 9, a dev config holds old-unit coordinates.

- [ ] **Step 1: Write the failing invariant tests**

Create `tests/unit/test_grid_square_cells.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

// The properties the square-cell sizing model exists to buy: cells that are
// square on every shipping panel, and a grid that transposes exactly when the
// panel rotates. Both are checked against the nominal grid — panel extent
// divided by track count — which is the whole of what get_dimensions()
// controls. Container padding and inter-track gutters belong to
// PanelWidgetManager and are asserted separately in test_panel_widget_manager.

#include "grid_layout.h"
#include "layout_manager.h"
#include "ui_breakpoint.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::GridDimensions;
using helix::GridLayout;
using helix::LayoutManager;

class LayoutManagerTestAccess {
  public:
    static void reset(helix::LayoutManager& lm) {
        lm.type_ = helix::LayoutType::STANDARD;
        lm.name_ = "standard";
        lm.override_name_.clear();
        lm.initialized_ = false;
        lm.width_ = 0;
        lm.height_ = 0;
    }
};

namespace {

struct Panel {
    const char* name;
    int w;
    int h;
};

// Every geometry HelixScreen ships or is known to run on.
const std::vector<Panel> kShippingPanels = {
    {"micro 480x272", 480, 272},    {"tiny 480x320", 480, 320},
    {"small 480x400", 480, 400},    {"medium 800x480", 800, 480},
    {"large 1024x600", 1024, 600},  {"xlarge 1280x720", 1280, 720},
    {"ultrawide 1920x440", 1920, 440}, {"ultratall 320x1480", 320, 1480},
};

GridDimensions dims_for(int w, int h) {
    auto& lm = LayoutManager::instance();
    LayoutManagerTestAccess::reset(lm);
    lm.init(w, h);
    return GridLayout::get_dimensions(helix::breakpoint_for(std::min(w, h)));
}

} // namespace

TEST_CASE("square cells: nominal cell aspect is within 5% of square", "[grid_layout][square]") {
    for (const auto& p : kShippingPanels) {
        for (bool rotated : {false, true}) {
            const int w = rotated ? p.h : p.w;
            const int h = rotated ? p.w : p.h;
            auto d = dims_for(w, h);
            REQUIRE(d.cols > 0);
            REQUIRE(d.rows > 0);
            const double cell_w = static_cast<double>(w) / d.cols;
            const double cell_h = static_cast<double>(h) / d.rows;
            const double aspect = cell_w / cell_h;
            INFO(p.name << (rotated ? " rotated" : "") << " -> " << d.cols << "x" << d.rows
                        << " cell " << cell_w << "x" << cell_h << " aspect " << aspect);
            CHECK(aspect >= 0.95);
            CHECK(aspect <= 1.05);
        }
    }
    LayoutManagerTestAccess::reset(LayoutManager::instance());
}

TEST_CASE("square cells: rotation transposes the grid exactly", "[grid_layout][square]") {
    for (const auto& p : kShippingPanels) {
        auto landscape = dims_for(p.w, p.h);
        auto portrait = dims_for(p.h, p.w);
        INFO(p.name << ": " << landscape.cols << "x" << landscape.rows << " vs " << portrait.cols
                    << "x" << portrait.rows);
        CHECK(landscape.cols == portrait.rows);
        CHECK(landscape.rows == portrait.cols);
    }
    LayoutManagerTestAccess::reset(LayoutManager::instance());
}

TEST_CASE("square cells: the shipped grids match the design table", "[grid_layout][square]") {
    struct Expected {
        int w, h, cols, rows;
    };
    const std::vector<Expected> table = {
        {480, 272, 14, 8},   {272, 480, 8, 14},   {480, 320, 12, 8},  {320, 480, 8, 12},
        {480, 400, 12, 10},  {800, 480, 12, 8},   {480, 800, 8, 13},  {1024, 600, 16, 10},
        {1280, 720, 16, 10}, {1920, 440, 48, 10}, {320, 1480, 8, 37},
    };
    for (const auto& e : table) {
        auto d = dims_for(e.w, e.h);
        INFO(e.w << "x" << e.h);
        CHECK(d.cols == e.cols);
        CHECK(d.rows == e.rows);
    }
    LayoutManagerTestAccess::reset(LayoutManager::instance());
}

TEST_CASE("square cells: no shipping panel reaches either track clamp",
          "[grid_layout][square]") {
    // MIN_TRACKS is a degenerate-display guard and MAX_TRACKS is a memory
    // ceiling. A shipping panel landing on either means the cell size no longer
    // controls the grid and the aspect invariant above is being met by accident.
    for (const auto& p : kShippingPanels) {
        for (bool rotated : {false, true}) {
            auto d = dims_for(rotated ? p.h : p.w, rotated ? p.w : p.h);
            INFO(p.name << (rotated ? " rotated" : ""));
            CHECK(d.cols > GridLayout::MIN_TRACKS);
            CHECK(d.rows > GridLayout::MIN_TRACKS);
            CHECK(d.cols < GridLayout::MAX_TRACKS);
            CHECK(d.rows < GridLayout::MAX_TRACKS);
        }
    }
    LayoutManagerTestAccess::reset(LayoutManager::instance());
}

TEST_CASE("square cells: an unsized LayoutManager falls to the track floor",
          "[grid_layout][square]") {
    // LayoutManager::width_ is 0 until Application initialises it. Any consumer
    // asking before then gets the floor, deliberately and identically on both
    // axes, rather than a plausible-looking grid derived from nothing.
    LayoutManagerTestAccess::reset(LayoutManager::instance());
    auto d = GridLayout::get_dimensions(helix::UiBreakpoint::Medium);
    CHECK(d.cols == GridLayout::MIN_TRACKS);
    CHECK(d.rows == GridLayout::MIN_TRACKS);
}
```

- [ ] **Step 2: Run the tests and watch them fail**

```bash
make test -j && ./build/bin/helix-tests "[square]"
```

Expected: FAIL — "the shipped grids match the design table" reports `6 != 14` for 480x272.

- [ ] **Step 3: Replace the constants**

In `include/grid_layout.h`, delete lines 84-121 entirely (`TARGET_CELL_W_PX` through `MIN_PORTRAIT_COLS`) and replace with:

```cpp
    /// Target track edge in px, per breakpoint tier, indexed by UiBreakpoint.
    ///
    /// A track is half a cell, so a widget's authored colspan and rowspan are
    /// the same physical unit and it is authored once for every panel and
    /// orientation. Dividing each screen axis by the same number is what makes
    /// the cell square: a rotated panel transposes its grid exactly.
    static constexpr int GRID_CELL[NUM_BREAKPOINTS] = {34, 40, 40, 60, 60, 72};

    /// Degenerate-display guard. No shipping panel reaches it — the narrowest
    /// is 272px against a 34px track, which gives 8.
    static constexpr int MIN_TRACKS = 4;

    /// Ceiling on track count. 1024x600 wants 17 columns and 1920x440 wants 48,
    /// so any lower cap stretches the track and breaks the square-cell
    /// invariant. The cost is descriptor entries, not objects: a 48x11 grid is
    /// 59 int32 values (cols+1 plus rows+1) in the LVGL grid descriptor.
    static constexpr int MAX_TRACKS = 64;
```

Change `TRACKS_PER_CELL` (added in Task 3) from `1` to `2`. Its comment is already correct at either value.

- [ ] **Step 4: Replace the sizing expression**

In `src/ui/grid_layout.cpp`, delete `GRID_DIMS` (`:20-33`) and replace `get_dimensions` (`:49-89`) with:

```cpp
GridDimensions GridLayout::get_dimensions(UiBreakpoint bp) {
    const int track = GRID_CELL[static_cast<size_t>(clamp_bp(bp))];
    auto& lm = LayoutManager::instance();
    // Floored to a whole number of cells: a track is half a cell, so an odd
    // track count leaves a final half-cell that no whole-cell widget can ever
    // occupy. Dropping it also spreads less leftover across the remaining
    // tracks, which is what keeps the cell square.
    auto tracks = [track](int extent) {
        const int n = std::clamp(extent / track, MIN_TRACKS, MAX_TRACKS);
        return n - (n % GridLayout::TRACKS_PER_CELL);
    };
    return {tracks(lm.width()), tracks(lm.height())};
}
```

Remove the now-unused `#include "layout_manager.h"`? No — it is still needed. Remove `#include <array>` if `GRID_DIMS` was its only user.

- [ ] **Step 5: Rewrite the tests that pin the deleted model**

Delete `tests/unit/test_grid_layout.cpp:21-58` (the six per-breakpoint `6x4`/`8x5` cases) and `:481-632` (the `ULTRAWIDE`, `PORTRAIT`, row-height, landscape-untouched, `STANDARD`-table and uninitialized-table cases) — all six properties now live in `test_grid_square_cells.cpp`. Keep `:59-72` (out-of-range clamping) and everything from `:74` onward that tests descriptors, placement, collision, `find_available*`, `grow_*` and `failure_text`, all of which are unaffected.

Update the two remaining descriptor-length cases at `:638-664` to drive `lm.init()` and assert against `GridLayout::get_cols()` / `get_rows()` rather than hardcoded 12 and 13.

- [ ] **Step 6: Run the tests and watch them pass**

```bash
make test -j && ./build/bin/helix-tests "[grid_layout]" && ./build/bin/helix-tests "[square]"
```

Expected: PASS.

- [ ] **Step 7: Prove nothing still references the deleted constants**

```bash
grep -rn "TARGET_CELL_W_PX\|TARGET_CELL_H_PX\|MIN_DYNAMIC\|MAX_DYNAMIC\|MIN_PORTRAIT_COLS\|GRID_DIMS" src include tests
```

Expected: no output.

- [ ] **Step 8: Commit**

```bash
git add include/grid_layout.h src/ui/grid_layout.cpp tests/unit/test_grid_layout.cpp tests/unit/test_grid_square_cells.cpp
git commit -m "feat(grid): size the home grid from one square-cell expression

Both axes now divide the panel by the same per-breakpoint track size, so the
cell is square on every panel and a rotated panel transposes its grid exactly.
The per-layout-type branches, the fixed count table and the separate width and
height targets are gone; tracks are half cells, per #1126. Closes #1126."
```

---

### Task 6: Row tracks come from the grid

**Files:**
- Modify: `src/ui/panel_widget_manager.cpp:585-619, 638-646`
- Test: `tests/unit/test_panel_widget_manager_cell_px.cpp`

**Interfaces:**
- Consumes: `GridLayout::get_rows(UiBreakpoint)` (Task 5).
- Produces: no API change.

**Flagged for adjudication.** The spec's §1 "actual cell" table divides panel height by the *nominal* row count, but `panel_widget_manager.cpp:585-619` builds `grid_rows = max(max_row_used, cached_rows)`. §1's aspect promise cannot hold while the row track count tracks widget content. The spec never mentions `max_row_used`. This task removes it; the alternative is to keep it and accept that a sparse dashboard has non-square cells. Structure this as one revertible commit.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_panel_widget_manager_cell_px.cpp`:

```cpp
TEST_CASE_METHOD(LVGLUITestFixture, "grid rows come from the panel, not from the widget footprint",
                 "[panel_widget_manager][square]") {
    // A dashboard with two widgets in the top two rows must still build the
    // full row count. Sizing the row axis to the content stretches every track
    // to fill the height, so a 1x1 widget on a sparse page is not square.
    auto& lm = helix::LayoutManager::instance();
    LayoutManagerTestAccess::reset(lm);
    lm.init(800, 480);

    lv_obj_t* container = make_grid_container(710, 466);
    populate_with(container, {{"printer_image", 0, 0, 4, 4}});

    const int32_t* rows = lv_obj_get_style_grid_row_dsc_array(container, LV_PART_MAIN);
    CHECK(helix::grid_count_tracks(rows) ==
          helix::GridLayout::get_rows(helix::UiBreakpoint::Medium));

    LayoutManagerTestAccess::reset(lm);
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
make test -j && ./build/bin/helix-tests "[panel_widget_manager][square]"
```

Expected: FAIL — 4 tracks built against 8 expected.

- [ ] **Step 3: Take the row count from the grid**

Replace `src/ui/panel_widget_manager.cpp:585-619` with:

```cpp
    // Row track count comes from the grid, exactly like the column count. The
    // grid's tracks are square by construction, so a page that uses only the
    // top half of them leaves the bottom half empty rather than stretching
    // every widget to fill the height.
    const int cols = GridLayout::get_cols(breakpoint);
    const int grid_rows = GridLayout::get_rows(breakpoint);

    spdlog::debug("[PanelWidgetManager] Grid layout: {}cols x {}rows (bp={}) for '{}'", cols,
                  grid_rows, to_int(breakpoint), panel_id);

    auto& dsc = grid_descriptors_[make_cache_key(panel_id, page_index)];
    dsc.col_dsc = GridLayout::make_col_dsc(breakpoint);
    dsc.row_dsc = GridLayout::make_row_dsc(breakpoint);
```

Delete the now-dead `int cols = GridLayout::get_cols(breakpoint);` at `:638` and the `spdlog::debug` at `:640-641` that it fed (both are folded into the block above).

- [ ] **Step 4: Run the tests and watch them pass**

```bash
make test -j && ./build/bin/helix-tests "[panel_widget_manager]"
```

Expected: PASS.

- [ ] **Step 5: Prove the cached key is dead**

```bash
grep -rn "cached_grid" src include tests
```

Expected: no output. The `/ui/cached_grid/<panel>/rows` setting is now unread; Task 11 removes it from existing configs.

- [ ] **Step 6: Commit**

```bash
git add src/ui/panel_widget_manager.cpp tests/unit/test_panel_widget_manager_cell_px.cpp
git commit -m "fix(home): build every row track the grid declares

The row axis was sized to the widgets' actual footprint, floored by a cached
count, so a page using the top half of the grid stretched every track to fill
the height. With square tracks that makes a 1x1 widget a tall rectangle. Rows
now come from GridLayout exactly as columns do."
```

---

### Task 7: Registry spans in tracks, with a pixel-band assertion

**Files:**
- Modify: `src/ui/panel_widget_registry.cpp:55-97`
- Create: `tests/unit/test_registry_span_bands.cpp`

**Interfaces:**
- Consumes: `GridLayout::TRACKS_PER_CELL`, `GridLayout::get_dimensions`, `helix::grid_cell_metrics`, `helix::grid_track_extent`, `helix::widget_size::{W_NORMAL, W_WIDE, H_TALL, H_TALLER}`.
- Produces: re-authored `s_widget_defs` spans.

**Starting table.** Multiply every span by `TRACKS_PER_CELL` (2), then adjust the five half-capable widgets so they can shrink to one track on the axis they support. Full list, `{colspan, rowspan, min_c, min_r, max_c, max_r}`:

| id | today | becomes |
|---|---|---|
| `printer_image` | 2,2,1,1,4,3 | 4,4,2,2,8,6 |
| `print_status` | 2,2,2,1,4,3 | 4,4,4,2,8,6 |
| `shutdown` | 1,1,1,1,1,1 | 2,2,**1**,2,2,2 |
| `lock` | 1,1,1,1,1,1 | 2,2,**1**,2,2,2 |
| `power_device` | 1,1,1,1,1,1 | 2,2,2,2,2,2 |
| `network` | 1,1,1,1,2,1 | 2,2,2,2,4,2 |
| `firmware_restart` | 1,1,1,1,1,1 | 2,2,**1**,2,2,2 |
| `tool_switcher` | 1,1,1,1,2,2 | 2,2,2,2,4,4 |
| `led` | 1,1,1,1,2,1 | 2,2,2,2,4,2 |
| `led_controls` | 1,1,1,1,1,1 | 2,2,**1**,2,2,2 |
| `fan_stack` | 1,1,1,1,3,2 | 2,2,2,2,6,4 |
| `fan` | 1,1,1,1,2,1 | 2,2,2,2,4,2 |
| `temperature` | 1,1,1,1,2,2 | 2,2,2,2,4,4 |
| `nozzle_temps` | 1,2,1,1,2,3 | 2,4,2,2,4,6 |
| `bed_temperature` | 1,1,1,1,2,2 | 2,2,2,2,4,4 |
| `chamber_temperature` | 1,1,1,1,2,2 | 2,2,2,2,4,4 |
| `temp_stack` | 1,1,1,1,3,2 | 2,2,2,2,6,4 |
| `thermistor` | 1,1,1,1,2,1 | 2,2,2,2,4,2 |
| `temp_graph` | 2,2,1,1,6,4 | 4,4,2,2,12,8 |
| `preheat` | 3,1,2,1,4,1 | 6,2,4,2,8,2 |
| `ams` | 1,1,1,1,4,2 | 2,2,2,2,8,4 |
| `active_spool` | 1,1,1,1,4,2 | 2,2,2,2,8,4 |
| `filament` | 1,1,1,1,2,1 | 2,2,2,2,4,2 |
| `humidity` | 1,1,1,1,2,2 | 2,2,2,2,4,4 |
| `width_sensor` | 1,1,1,1,2,2 | 2,2,2,2,4,4 |
| `favorite_macro` | 1,1,1,1,2,1 | 2,2,2,2,4,2 |
| `macros` | 1,1,1,1,1,1 | 2,2,2,2,2,2 |
| `motion` | 1,1,1,1,1,1 | 2,2,2,2,2,2 |
| `clock` | 2,1,1,1,3,3 | 4,2,**1**,**1**,6,6 |
| `control_buttons` | 2,1,2,1,2,1 | 4,2,4,2,4,2 |
| `job_queue` | 2,2,2,1,4,3 | 4,4,4,2,8,6 |
| `tips` | 4,2,2,1,6,2 | 8,4,4,2,12,4 |
| `clog_detection` | 1,1,1,1,2,2 | 2,2,2,2,4,4 |
| `print_stats` | 2,2,2,1,3,2 | 4,4,4,2,6,4 |
| `gcode_console` | 1,1,1,1,1,1 | 2,2,2,2,2,2 |
| `camera` | 2,2,1,1,4,3 | 4,4,2,2,8,6 |
| `notifications` | 1,1,1,1,2,1 | 2,2,2,2,4,2 |

This table is the **starting point**. Step 4 runs the band test, which will report where a doubled span drops a widget out of the band it renders today; Step 5 raises those spans. From the arithmetic in "Reference data", expect `print_status`, `job_queue`, `print_stats`, `tips` and `camera` to need attention at micro (480x272) and micro portrait (272x480).

- [ ] **Step 1: Write the band test**

Create `tests/unit/test_registry_span_bands.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

// Every widget picks its layout from the pixels it occupies (panel_widget_size.h).
// Its authored span decides those pixels, and the span is authored once for
// every panel. This walks the registry against every shipping geometry and
// reports the band each authored span lands in, so a span change that quietly
// demotes a widget to its compact layout on one panel is a red test rather
// than a visual surprise on that panel only.

#include "grid_layout.h"
#include "layout_manager.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "ui_breakpoint.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

class LayoutManagerTestAccess {
  public:
    static void reset(helix::LayoutManager& lm) {
        lm.type_ = helix::LayoutType::STANDARD;
        lm.name_ = "standard";
        lm.override_name_.clear();
        lm.initialized_ = false;
        lm.width_ = 0;
        lm.height_ = 0;
    }
};

namespace {

// Measured container content boxes and gutters, from a live run of each tier.
// content_w/content_h are the home grid container's content box, which does not
// change with the track count; the gutter is space_xs at that breakpoint.
struct Geometry {
    const char* name;
    int panel_w, panel_h;
    int content_w, content_h;
    int gutter;
};

const std::vector<Geometry> kGeometries = {
    {"micro 480x272", 480, 272, 430, 264, 2},
    {"tiny 480x320", 480, 320, 418, 312, 2},
    {"small 480x400", 480, 400, 414, 388, 4},
    {"medium 800x480", 800, 480, 710, 466, 5},
    {"large 1024x600", 1024, 600, 904, 584, 6},
    {"xlarge 1280x720", 1280, 720, 1128, 700, 8},
    {"micro portrait 272x480", 272, 480, 264, 394, 2},
    {"portrait 480x800", 480, 800, 466, 664, 5},
};

int width_band(float px) {
    if (px >= static_cast<float>(W_WIDE))
        return 2;
    if (px >= static_cast<float>(W_NORMAL))
        return 1;
    return 0;
}

int height_band(float px) {
    if (px >= static_cast<float>(H_TALLER))
        return 2;
    if (px >= static_cast<float>(H_TALL))
        return 1;
    return 0;
}

CellMetrics metrics_for(const Geometry& g) {
    auto& lm = LayoutManager::instance();
    LayoutManagerTestAccess::reset(lm);
    lm.init(g.panel_w, g.panel_h);
    auto d = GridLayout::get_dimensions(breakpoint_for(std::min(g.panel_w, g.panel_h)));
    return grid_cell_metrics(g.content_w, g.content_h, d.cols, d.rows, g.gutter);
}

} // namespace

TEST_CASE("registry spans: no authored span exceeds the narrowest grid",
          "[widget_def][span_bands]") {
    // A widget wider at its declared minimum than the grid is TooLargeForGrid
    // and gets disabled at boot. 272x480 is the narrowest panel: 8 tracks.
    auto& lm = LayoutManager::instance();
    LayoutManagerTestAccess::reset(lm);
    lm.init(272, 480);
    auto d = GridLayout::get_dimensions(UiBreakpoint::Micro);

    for (const auto& def : get_all_widget_defs()) {
        INFO("widget " << def.id << " min " << def.effective_min_colspan() << "x"
                       << def.effective_min_rowspan() << " grid " << d.cols << "x" << d.rows);
        CHECK(def.effective_min_colspan() <= d.cols);
        CHECK(def.effective_min_rowspan() <= d.rows);
    }
    LayoutManagerTestAccess::reset(lm);
}

TEST_CASE("registry spans: authored spans land in the intended pixel band",
          "[widget_def][span_bands]") {
    // Expected width band per widget per geometry: 0 compact, 1 normal, 2 wide.
    // Fill this in from the reported table below, then the test pins it.
    const std::map<std::string, std::vector<int>> expected_width_band = {
        // {"print_status", {1, 1, 1, 2, 2, 2, 1, 2}},
    };

    for (const auto& g : kGeometries) {
        CellMetrics m = metrics_for(g);
        for (const auto& def : get_all_widget_defs()) {
            const float w = grid_track_extent(m.cell_w, m.gutter, def.colspan);
            const float h = grid_track_extent(m.cell_h, m.gutter, def.rowspan);
            // Reported unconditionally so a failing pin prints the whole row.
            WARN(g.name << " " << def.id << " " << def.colspan << "x" << def.rowspan << " -> "
                        << w << "x" << h << "px  wband=" << width_band(w)
                        << " hband=" << height_band(h));
        }
    }

    size_t geo_index = 0;
    for (const auto& g : kGeometries) {
        CellMetrics m = metrics_for(g);
        for (const auto& [id, bands] : expected_width_band) {
            const auto* def = find_widget_def(id);
            REQUIRE(def != nullptr);
            REQUIRE(bands.size() == kGeometries.size());
            const float w = grid_track_extent(m.cell_w, m.gutter, def->colspan);
            INFO(g.name << " " << id << " span " << def->colspan << " -> " << w << "px");
            CHECK(width_band(w) == bands[geo_index]);
        }
        ++geo_index;
    }
    LayoutManagerTestAccess::reset(LayoutManager::instance());
}
```

- [ ] **Step 2: Run it against the un-doubled registry**

```bash
make test -j && ./build/bin/helix-tests "[span_bands]" -s 2>&1 | tee /tmp/bands-before.txt
```

Expected: PASS, with a `WARN` line per widget per geometry. This is the **before** picture. `-s` forces successful assertions and warnings to print.

- [ ] **Step 3: Apply the span table**

Edit `src/ui/panel_widget_registry.cpp:57-96` per the table above. Keep the existing `clang-format off` block and the column legend comments.

- [ ] **Step 4: Re-run and diff the bands**

```bash
./build/bin/helix-tests "[span_bands]" -s 2>&1 | tee /tmp/bands-after.txt
diff /tmp/bands-before.txt /tmp/bands-after.txt
```

Read the diff. Any widget whose `wband` or `hband` **fell** has silently lost a layout on that panel.

- [ ] **Step 5: Raise the spans that dropped a band**

For each widget in the diff whose band fell, raise its `colspan`/`rowspan` by one track at a time and re-run Step 4 until the band is restored on every geometry. From the reference arithmetic, expect micro (14 columns, 28.86px tracks) to need `colspan 5` where `4` gives 121px against `W_NORMAL = 134`, and micro portrait (14 rows, 26.29px tracks) to need `rowspan 5` where `4` gives 111px against `H_TALL = 130`.

- [ ] **Step 6: Pin the final bands**

Populate `expected_width_band` in the test from the final `WARN` output, one row per widget that has any width-band logic — `active_spool`, `camera`, `clock`, `fan_stack`, `favorite_macro`, `humidity`, `job_queue`, `print_stats`, `print_status`, `temp_graph`, `tips`, `tool_switcher`, `width_sensor`, `nozzle_temps`. Delete the `WARN` loop once the map is filled.

- [ ] **Step 7: Prove no widget regrew a span read**

```bash
grep -rn "on_size_changed" src/ui/panel_widgets src/ui/widgets | grep -v "int /\*colspan\*/" | grep -v "\.h:"
```

Every remaining hit must either take `int colspan, int rowspan` and use them only inside a `spdlog::` call, or be a comment. Inspect each by hand: `temp_graph_widget.cpp:135-143`, `clock_widget.cpp:178,233`, `fan_stack_widget.cpp:229,333`, `nozzle_temps_widget.cpp:281,311,401`.

- [ ] **Step 8: Run the full widget-size suite**

```bash
./build/bin/helix-tests "[widget_size]" && ./build/bin/helix-tests "[widget_def]" && ./build/bin/helix-tests "[grid_edit][sizing]"
```

Expected: PASS. `test_grid_edit_mode.cpp:1290` "All registered widgets have valid sizing constraints" sweeps the whole registry and catches a min > max slip; `clamp_span: asymmetric constraints` (`:1304`) hardcodes `tips` at min 2 / max 6 and needs updating to 4 / 12.

- [ ] **Step 9: Commit**

```bash
git add src/ui/panel_widget_registry.cpp tests/unit/test_registry_span_bands.cpp tests/unit/test_grid_edit_mode.cpp
git commit -m "feat(widgets): express default spans in half-cell tracks

Every registry span is re-authored against the half-cell grid, and a new
table-driven test walks the whole registry across all eight shipping
geometries and pins which pixel band each authored span lands in. Doubling a
span is not size-preserving when the track count changes, so the spans that
would have demoted a widget to its compact layout are raised."
```

---

### Task 8: `default_layout.json` in the new units

**Files:**
- Modify: `assets/config/default_layout.json` (whole file)
- Modify: `src/system/panel_widget_config.cpp:652-666, 717-746`
- Modify: `tests/unit/test_default_layout.cpp:692-762, 938-1000`

**Interfaces:**
- Consumes: the grid sizes from Task 5.
- Produces: no API change.

**Anchor table.** Each anchor is scaled by the ratio of new tracks to old tracks for its breakpoint, then adjusted so the anchors tile the row exactly. `micro` gains an explicit landscape key: it used to fall through to `tiny` when both were 6 columns, and is now 14 against tiny's 12.

Landscape `anchors`:

> **STALE - RE-DERIVE.** The placements in this table were computed against the
> spec's odd track counts (medium 13, large/xlarge 17). Owner ruling 6 floors
> those to 12, 16 and 16. Every value below must be re-derived against the
> floored counts before use. Step 1's test asserts `widest_right_edge == budget`
> for each breakpoint, so a stale value fails rather than shipping.

| id | micro (14x8) | tiny (12x8) | small (12x10) | medium (12x8) | large (16x10) | xlarge (16x10) |
|---|---|---|---|---|---|---|
| `printer_image` | 0,0,5,4 | 0,0,4,4 | 0,0,4,4 | 0,0,4,4 | 0,0,6,6 | 0,0,6,6 |
| `print_status` | 0,4,5,4 | 0,4,4,4 | 0,4,4,4 | 0,4,7,4 | 0,6,6,4 | 0,6,6,4 |
| `tips` | 5,0,9,4 | 4,0,4,4 | 4,0,4,4 | 4,0,9,4 | 6,0,11,4 | 6,0,11,4 |
| `temperature` | 5,4,2,2 | 4,4,2,2 | 4,4,2,2 | 7,4,2,2 | 6,4,2,2 | 6,4,2,2 |
| `bed_temperature` | 5,6,2,2 | 4,6,2,2 | 4,6,2,2 | 7,6,2,2 | 6,6,2,2 | 6,6,2,2 |

`variants.portrait` — the grid is now 8 or 10 tracks wide everywhere, so anchors take a fixed 4-track band at the top rather than consuming the whole grid:

| id | micro (8x14) | tiny (8x12) | small (10x12) | medium (8x13) | large (10x17) | xlarge (10x17) | xxlarge (10x17) |
|---|---|---|---|---|---|---|---|
| `printer_image` | 0,0,8,4 | 0,0,8,4 | 0,0,10,4 | 0,0,8,4 | 0,0,10,6 | 0,0,10,6 | 0,0,10,6 |
| `print_status` | 0,4,8,4 | 0,4,8,4 | 0,4,10,4 | 0,4,8,4 | 0,6,10,4 | 0,6,10,4 | 0,6,10,4 |

- [ ] **Step 1: Write the failing tests**

Replace `tests/unit/test_default_layout.cpp:938-961` ("portrait disables tips by default") with:

```cpp
TEST_CASE("default_layout: portrait keeps tips enabled", "[default_layout][portrait]") {
    // tips is authored 8 tracks with a minimum of 4. Every portrait grid is at
    // least 8 tracks wide, so the widget fits at its authored size and the
    // landscape-only special case that used to hide it is gone.
    auto& lm = helix::LayoutManager::instance();
    LayoutManagerTestAccess::reset(lm);
    lm.init(480, 800);

    auto entries = build_defaults_for(helix::UiBreakpoint::Medium);
    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->enabled);
    CHECK(tips->col == -1); // auto-placed, not anchored

    LayoutManagerTestAccess::reset(lm);
}

TEST_CASE("default_layout: bed_temperature is enabled on every tier",
          "[default_layout][1126]") {
    // The AMS-versus-bed-temperature trade existed because the grid ran out of
    // cells. It has three times as many now, so both are on everywhere.
    for (auto bp : {helix::UiBreakpoint::Micro, helix::UiBreakpoint::Small,
                    helix::UiBreakpoint::Medium, helix::UiBreakpoint::Large}) {
        for (bool ams : {false, true}) {
            INFO("bp " << helix::to_int(bp) << " ams " << ams);
            set_ams_slot_count(ams ? 4 : 0);
            auto entries = build_defaults_for(bp);
            auto* bed = find_entry(entries, "bed_temperature");
            REQUIRE(bed);
            CHECK(bed->enabled);
        }
    }
}
```

Replace the `max_cols` map and the `CHECK(id != "tips")` at `test_default_layout.cpp:979-988` with:

```cpp
    // Track budget per breakpoint name, from GridLayout's square-cell sizing.
    // The narrowest panel in each tier sets the budget: cols = panel_w /
    // GRID_CELL[bp], and in portrait the panel width is the cramped axis.
    const std::map<std::string, int> max_cols = {
        {"micro", 8}, {"tiny", 8},    {"small", 10},  {"medium", 8},
        {"large", 10}, {"xlarge", 10}, {"xxlarge", 10},
    };

    for (const auto& anchor : portrait) {
        std::string id = anchor.value("id", std::string{});
        INFO("anchor " << id);
        REQUIRE(helix::find_widget_def(id) != nullptr);
        // ...unchanged loop body...
    }
```

And add a landscape counterpart, which the suite does not have today:

```cpp
TEST_CASE("default_layout: the shipped landscape anchors tile their grid",
          "[default_layout][shipped][1126]") {
    // Every breakpoint key must exist in its own right. micro used to fall
    // through to tiny because both were 6 columns; they are 14 and 12 now, so
    // a missing micro key silently under-fills by two tracks.
    const std::map<std::string, int> cols = {{"micro", 14}, {"tiny", 12},   {"small", 12},
                                             {"medium", 12}, {"large", 16}, {"xlarge", 16}};

    std::string path = helix::find_readable("default_layout.json");
    std::ifstream in(path);
    REQUIRE(in.is_open());
    nlohmann::json layout = nlohmann::json::parse(in);
    REQUIRE(layout.contains("anchors"));

    for (const auto& [bp_name, budget] : cols) {
        int widest_right_edge = 0;
        for (const auto& anchor : layout["anchors"]) {
            REQUIRE(anchor.contains("placements"));
            INFO("anchor " << anchor.value("id", std::string{}) << " bp " << bp_name);
            REQUIRE(anchor["placements"].contains(bp_name));
            const auto& p = anchor["placements"][bp_name];
            const int col = p.value("col", 0);
            const int colspan = p.value("colspan", 1);
            CHECK(col >= 0);
            CHECK(col + colspan <= budget);
            widest_right_edge = std::max(widest_right_edge, col + colspan);
        }
        INFO("bp " << bp_name);
        CHECK(widest_right_edge == budget); // anchors reach the right edge
    }
}
```

- [ ] **Step 2: Run and watch them fail**

```bash
make test -j && ./build/bin/helix-tests "[default_layout]"
```

Expected: FAIL — `REQUIRE(anchor["placements"].contains("micro"))` and the tips/bed_temperature cases.

- [ ] **Step 3: Rewrite `default_layout.json`**

Apply the two tables above. Update the `_comment` at line 2 to describe the square-cell model, and replace the `variants._comment` at line 56 with:

```json
"_comment": "PORTRAIT. The grid is panel_w/GRID_CELL[bp] by panel_h/GRID_CELL[bp] tracks, where a track is half an authored cell, so a rotated panel transposes its grid exactly. Anchors take a fixed band across the top and leave the rest of the column free for auto-placed widgets. Breakpoint keys are named, not indexed: micro(0), tiny(1), small(2), medium(3), large(4), xlarge(5), xxlarge(6) — see include/ui_breakpoint.h. Every tier is spelled out here rather than relying on the micro->tiny->small fallback chain, because tiers no longer share a track count."
```

- [ ] **Step 4: Delete the two scarcity workarounds**

Delete `src/system/panel_widget_config.cpp:717-731` (the portrait `tips` suppression) entirely. Replace `:733-746` (the `bed_temperature` gate) with:

```cpp
    // Bed temperature is always last so it is the last widget placed.
    {
        auto it = std::find_if(result.begin(), result.end(),
                               [](const PanelWidgetEntry& e) { return e.id == "bed_temperature"; });
        if (it != result.end()) {
            auto entry = std::move(*it);
            result.erase(it);
            result.push_back(std::move(entry));
        }
    }
```

`bed_temperature`'s enabled state now comes from the registry's `default_enabled`, which is `false` (`panel_widget_registry.cpp:71`). If the intent is for it to be on by default now that there is room, flip that field to `true` in the same commit.

Update the hardcoded portrait fallback at `:652-666`, which is reached only when `default_layout.json` is missing or malformed:

```cpp
        if (portrait) {
            anchors = {
                {"printer_image", 0, 0, 8, 4},
                {"print_status", 0, 4, 8, 4},
            };
        } else {
            anchors = {
                {"printer_image", 0, 0, 4, 4},
                {"print_status", 0, 4, 4, 4},
                {"tips", 4, 0, 4, 4},
            };
        }
```

- [ ] **Step 5: Run and watch them pass**

```bash
make test -j && ./build/bin/helix-tests "[default_layout]"
```

Expected: PASS. The six `bed_temperature` AMS-conditional cases at `:692-762` all now assert the same thing; collapse them into the single tier-sweep case written in Step 1 and delete the originals.

- [ ] **Step 6: Prove no widget is disabled for want of room**

Add to `tests/unit/test_default_layout.cpp` — this is the regression test #1216 never got:

```cpp
TEST_CASE("default_layout: no widget is disabled for want of room on any shipping panel",
          "[default_layout][1216][1126]") {
    const std::vector<std::pair<int, int>> panels = {
        {480, 272}, {272, 480}, {480, 320}, {320, 480}, {480, 400},
        {800, 480}, {480, 800}, {1024, 600}, {1280, 720},
    };
    auto& lm = helix::LayoutManager::instance();

    for (auto [w, h] : panels) {
        LayoutManagerTestAccess::reset(lm);
        lm.init(w, h);
        auto bp = helix::breakpoint_for(std::min(w, h));
        auto entries = build_defaults_for(bp);

        helix::GridLayout grid(bp);
        for (const auto& e : entries) {
            if (!e.enabled)
                continue;
            const auto* def = helix::find_widget_def(e.id);
            REQUIRE(def != nullptr);
            auto sp = grid.find_available_bottom_min(def->effective_min_colspan(),
                                                     def->effective_min_rowspan());
            INFO(w << "x" << h << " widget " << e.id << " min "
                   << def->effective_min_colspan() << "x" << def->effective_min_rowspan()
                   << " grid " << grid.cols() << "x" << grid.rows());
            REQUIRE(sp.placed());
            grid.place({e.id, sp.col, sp.row, sp.colspan, sp.rowspan});
        }
    }
    LayoutManagerTestAccess::reset(lm);
}
```

- [ ] **Step 7: Commit**

```bash
git add assets/config/default_layout.json src/system/panel_widget_config.cpp tests/unit/test_default_layout.cpp
git commit -m "feat(home): re-author the default layout for the square-cell grid

Every anchor is expressed in half-cell tracks and every breakpoint gets its
own key, since the tiers no longer share a track count. The portrait tips
suppression and the bed-temperature-versus-AMS trade both existed because the
grid ran out of cells; it has three times as many, so both are gone."
```

---

### Task 9: Preset seed layouts in the new units

**Files:**
- Modify: `assets/config/panel_widgets/ad5x/home.json`
- Modify: `assets/config/panel_widgets/ad5x_zmod/home.json`
- Modify: `assets/config/panel_widgets/cc1/home.json`
- Test: `tests/unit/test_panel_widget_config.cpp`

**Interfaces:**
- Consumes: `GridLayout::get_dimensions` (Task 5), registry spans (Task 7).
- Produces: no API change.

All three seeds are authored against the 6x4 medium grid (`printer_image` at `0,0,2,2`, `tips` at `2,0,4,2`, `print_status` at `0,2,3,2`). All three ship on 800x480 panels, which is now 12x8 (owner ruling 6 floors the spec's 13). Re-author them to the medium landscape anchor table from Task 8, keeping each seed's own enabled set and its trailing `-1,-1` entries.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("preset seeds: every placed widget fits the panel's grid",
          "[panel_widget_config][preset][1126]") {
    // The seeds ship on 800x480, which is a 12x8 track grid.
    auto& lm = helix::LayoutManager::instance();
    LayoutManagerTestAccess::reset(lm);
    lm.init(800, 480);
    auto d = helix::GridLayout::get_dimensions(helix::UiBreakpoint::Medium);

    for (const char* preset : {"ad5x", "ad5x_zmod", "cc1"}) {
        const std::string rel = std::string("panel_widgets/") + preset + "/home.json";
        std::ifstream in(helix::find_readable(rel));
        INFO("preset " << preset);
        REQUIRE(in.is_open());
        nlohmann::json seed = nlohmann::json::parse(in);
        REQUIRE(seed.contains("pages"));

        for (const auto& page : seed["pages"]) {
            for (const auto& w : page["widgets"]) {
                const int col = w.value("col", -1);
                const int row = w.value("row", -1);
                if (col < 0 || row < 0)
                    continue;
                INFO("preset " << preset << " widget " << w.value("id", std::string{}));
                CHECK(col + w.value("colspan", 1) <= d.cols);
                CHECK(row + w.value("rowspan", 1) <= d.rows);
            }
        }
    }
    LayoutManagerTestAccess::reset(lm);
}

TEST_CASE("preset seeds: placed widgets do not overlap",
          "[panel_widget_config][preset][1126]") {
    auto& lm = helix::LayoutManager::instance();
    LayoutManagerTestAccess::reset(lm);
    lm.init(800, 480);

    for (const char* preset : {"ad5x", "ad5x_zmod", "cc1"}) {
        std::ifstream in(helix::find_readable(std::string("panel_widgets/") + preset +
                                              "/home.json"));
        REQUIRE(in.is_open());
        nlohmann::json seed = nlohmann::json::parse(in);
        helix::GridLayout grid(helix::UiBreakpoint::Medium);
        for (const auto& w : seed["pages"][0]["widgets"]) {
            const int col = w.value("col", -1);
            const int row = w.value("row", -1);
            if (col < 0 || row < 0 || !w.value("enabled", false))
                continue;
            INFO("preset " << preset << " widget " << w.value("id", std::string{}));
            CHECK(grid.place({w.value("id", std::string{}), col, row, w.value("colspan", 1),
                              w.value("rowspan", 1)}));
        }
    }
    LayoutManagerTestAccess::reset(lm);
}
```

- [ ] **Step 2: Run and watch it fail**

```bash
make test -j && ./build/bin/helix-tests "[preset]"
```

Expected: the fit test passes (old spans are smaller than the new grid) but the overlap test also passes — old spans still tile. The real failure is visual: everything huddles in the top-left third. Confirm by launching:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
rm -rf "$HELIX_CONFIG_DIR" && mkdir -p "$HELIX_CONFIG_DIR"
make -j && ./build/bin/helix-screen --test -vv -s 800x480 --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
sleep 6 && ./build/bin/helix-screen ctl -s "$HELIX_SOCK" geom home_widget_grid
```

- [ ] **Step 3: Re-author the three seeds**

Apply the medium landscape table from Task 8 to the anchored (`col >= 0`) entries:

| id | old | new |
|---|---|---|
| `printer_image` | 0,0,2,2 | 0,0,4,4 |
| `tips` | 2,0,4,2 | 4,0,9,4 |
| `print_status` | 0,2,3,2 | 0,4,7,4 |
| `led` | 3,2,1,1 | 7,4,2,2 |
| `fan_stack` | 4,2,1,1 | 9,4,2,2 |
| `temperature` | 5,2,1,1 | 11,4,2,2 |
| `ams` | 3,3,1,1 | 7,6,2,2 |
| `notifications` | 4,3,1,1 | 9,6,2,2 |
| `bed_temperature` | 5,3,1,1 | 11,6,2,2 |

For `cc1`, whose set differs, apply the same origin scaling: `col *= 13/6` rounded to the tile above, `row *= 2`, spans from the table. `shutdown` at `4,2,1,1` becomes `9,4,2,2`; `led` at `5,2,1,1` becomes `11,4,2,2`; `temperature` at `2,3,1,1` becomes `4,6,2,2`; `filament` at `3,3,1,1` becomes `7,6,2,2`; `bed_temperature` at `2,2,1,1` becomes `4,4,2,2`; `notifications` at `5,3,1,1` becomes `11,6,2,2`.

Update every `-1,-1` entry's `colspan`/`rowspan` to its new registry default from Task 7 (e.g. `nozzle_temps` `1,2` → `2,4`, `temp_graph` `2,2` → `4,4`, `preheat` `3,1` → `6,2`, `clock` `2,1` → `4,2`).

- [ ] **Step 4: Run the tests and watch them pass**

```bash
make test -j && ./build/bin/helix-tests "[preset]" && ./build/bin/helix-tests "[panel_widget_config]"
```

Expected: PASS.

- [ ] **Step 5: Verify live on each preset**

For each of `ad5x`, `ad5x_zmod`, `cc1`, seed a scratch config with that preset and confirm the dashboard fills the panel:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
rm -rf "$HELIX_CONFIG_DIR" && mkdir -p "$HELIX_CONFIG_DIR"
printf '{"config_version":22,"active_printer":"default","printers":{"default":{"preset":"ad5x","moonraker_host":"127.0.0.1"}}}' > "$HELIX_CONFIG_DIR/settings.json"
./build/bin/helix-screen --test -vv -s 800x480 --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
sleep 6 && ./build/bin/helix-screen ctl -s "$HELIX_SOCK" screenshot /tmp/ad5x-home.png
```

- [ ] **Step 6: Commit**

```bash
git add assets/config/panel_widgets/ad5x/home.json assets/config/panel_widgets/ad5x_zmod/home.json assets/config/panel_widgets/cc1/home.json tests/unit/test_panel_widget_config.cpp
git commit -m "feat(presets): re-author the shipped seed layouts in half-cell tracks

The three preset seeds were authored against a 6x4 grid and now ship onto a
12x8 one, so every placed widget covered a third of the panel it was laid out
for. Positions and spans are re-expressed in tracks; two new tests assert the
seeds fit their grid and do not overlap."
```

---

### Task 10: Absent spans fall back to the registry default

**Files:**
- Modify: `src/system/panel_widget_config.cpp:29-31, 93-110`
- Test: `tests/unit/test_panel_widget_config.cpp`

**Interfaces:**
- Consumes: `helix::find_widget_def(std::string_view)`.
- Produces: `PanelWidgetConfig::parse_widget_array` — signature unchanged; an entry with no `colspan`/`rowspan` key now takes the registry default rather than `1`.

**Genuine spec-versus-code conflict, resolved here.** §4c states "absent spans already fall back to registry defaults in `parse_widget_array()` (`panel_widget_config.cpp:95-107`)". They do not — lines 96-99 initialise `colspan`/`rowspan` to `1` and only overwrite when the key is present. Only the *append-new-widget* branch at `:119-128` uses `def.colspan`. Without this task, Task 11's migration leaves every widget one track wide.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("parse_widget_array: an entry with no spans takes the registry default",
          "[panel_widget_config][1126]") {
    // A migrated entry carries id, enabled and a -1 position, nothing more.
    // Falling back to 1 rather than the registry span would put every widget on
    // an upgraded install at one track wide with nothing logged.
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"id", "print_status"}, {"enabled", true}, {"col", -1}, {"row", -1}});
    arr.push_back({{"id", "tips"}, {"enabled", true}, {"col", -1}, {"row", -1}});

    auto entries = helix::PanelWidgetConfigTestAccess::parse(arr, false);
    REQUIRE(entries.size() == 2);

    const auto* ps = helix::find_widget_def("print_status");
    const auto* tips = helix::find_widget_def("tips");
    REQUIRE(ps);
    REQUIRE(tips);
    CHECK(entries[0].colspan == ps->colspan);
    CHECK(entries[0].rowspan == ps->rowspan);
    CHECK(entries[1].colspan == tips->colspan);
    CHECK(entries[1].rowspan == tips->rowspan);
}

TEST_CASE("parse_widget_array: an explicit span still wins over the registry",
          "[panel_widget_config][1126]") {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({{"id", "tips"}, {"enabled", true}, {"col", 0}, {"row", 0},
                   {"colspan", 6}, {"rowspan", 2}});

    auto entries = helix::PanelWidgetConfigTestAccess::parse(arr, false);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].colspan == 6);
    CHECK(entries[0].rowspan == 2);
}
```

`parse_widget_array` is private and static; add a `PanelWidgetConfigTestAccess` friend struct in `tests/test_helpers/panel_widget_config_test_access.h` following the pattern of `tests/test_helpers/config_test_access.h`, and declare it a friend in `include/panel_widget_config.h`.

- [ ] **Step 2: Run and watch it fail**

```bash
make test -j && ./build/bin/helix-tests "[panel_widget_config][1126]"
```

Expected: FAIL — `entries[0].colspan == 1`, expected 4.

- [ ] **Step 3: Seed from the registry**

Replace `src/system/panel_widget_config.cpp:96-110` with:

```cpp
        // Load grid placement coordinates. A missing col/row means auto-place;
        // a missing span means "whatever the widget is authored at", which is
        // the registry's answer and not a bare 1. An entry written before the
        // grid moved to half-cell tracks carries no spans at all, and one track
        // is a quarter of the area the widget expects.
        const auto* def = find_widget_def(id);
        int col = -1;
        int row_val = -1;
        int colspan = def ? def->colspan : 1;
        int rowspan = def ? def->rowspan : 1;
        if (item.contains("col") && item["col"].is_number_integer()) {
            col = item["col"].get<int>();
        }
        if (item.contains("row") && item["row"].is_number_integer()) {
            row_val = item["row"].get<int>();
        }
        if (item.contains("colspan") && item["colspan"].is_number_integer()) {
            colspan = item["colspan"].get<int>();
        }
        if (item.contains("rowspan") && item["rowspan"].is_number_integer()) {
            rowspan = item["rowspan"].get<int>();
        }
```

The `find_widget_def(id) == nullptr` guard at `:78-81` already ran, so `def` is non-null here; the ternaries are belt-and-braces for a future reordering.

- [ ] **Step 4: Run and watch it pass**

```bash
make test -j && ./build/bin/helix-tests "[panel_widget_config]"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/panel_widget_config.h src/system/panel_widget_config.cpp tests/test_helpers/panel_widget_config_test_access.h tests/unit/test_panel_widget_config.cpp
git commit -m "fix(panel-widgets): take an absent span from the registry, not from 1

A saved entry with no colspan/rowspan key loaded at one track regardless of
what the widget is authored at, so a layout that omits its spans rendered
every widget at a quarter of its intended area with nothing logged. The
registry default is the answer; an explicit span still wins."
```

---

### Task 11: `migrate_v21_to_v22` — unplace saved layouts

**Files:**
- Modify: `include/config.h:60`
- Modify: `src/system/config.cpp` (new function after `migrate_v20_to_v21` at `:1233`, one dispatch line after `:1339`)
- Create: `tests/unit/test_config_migration_v22.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks (deliberately — the migration knows nothing about the grid).
- Produces:
  ```cpp
  static constexpr int CURRENT_CONFIG_VERSION = 22;   // include/config.h
  static void migrate_v21_to_v22(json& config);       // file-static in config.cpp
  ```

**THIS IS THE POINT OF NO RETURN FOR SAVED USER LAYOUTS.** Once a config is stamped 22 its `col`/`row`/`colspan`/`rowspan` are gone and reverting the code does not restore them. Every preceding coupled task must be merged first.

`run_versioned_migrations()` runs at config load (`config.cpp:1660`), before `m_screen_width` is final (`application.cpp:1293`) and long before `layout_mgr.init()` (`:1559`). There is no screen size at migration time, so nothing can be rescaled — the coordinates are cleared and the two-pass placement engine re-seats everything.

**Intent-preservation table** (§4c). Only three sites write `col = -1` at runtime — `panel_widget_manager.cpp:372`, `panel_widget_config.cpp:727` (deleted in Task 8), `panel_widget_config.cpp:788` — and none is reachable from a user gesture. The trash button (`grid_edit_mode.cpp:717-725`) sets `enabled = false` and leaves the coordinates intact.

| state on disk | means | migration |
|---|---|---|
| `enabled:false, col >= 0` | user pressed trash | keep disabled |
| `enabled:false, col == -1` | engine auto-disable, or never added | drop `enabled` so the registry default applies |
| `enabled:true` | in use | keep enabled |

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_config_migration_v22.cpp`, modelled on `tests/unit/test_config_migration_v21.cpp:36-80` (same `MigrationV22Fixture` with a sandboxed `HELIX_CONFIG_DIR` and a `write_and_init` that drives the real `Config::init()` path):

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

// The v21 -> v22 migration unplaces every saved home layout. Saved col/row/span
// are cell counts against a grid that no longer exists, and the migration runs
// at config load, before any screen size is known, so there is nothing to
// rescale into. Clearing the coordinates hands the layout to the placement
// engine, which is the path every never-positioned widget already takes on
// every boot.
//
// What survives: the widget set, the array order that drives placement order,
// and a deliberate hide. What does not: hand-arranged positions.

#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class MigrationV22Fixture {
    // ...identical body to MigrationV21Fixture, with temp_dir named
    // "helix_migration_v22_test"...
  protected:
    static json home(std::initializer_list<json> widgets) {
        return json{{"main_page_index", 0},
                    {"next_page_id", 1},
                    {"pages", json::array({json{{"id", "main"}, {"widgets", json(widgets)}}})}};
    }
};

} // namespace

TEST_CASE_METHOD(MigrationV22Fixture,
                 "Config migration v22: clears every coordinate and span", "[config][migration][22]") {
    write_and_init({{"config_version", 21},
                    {"active_printer", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", home({{{"id", "printer_image"}, {"enabled", true}, {"col", 0},
                                          {"row", 0}, {"colspan", 2}, {"rowspan", 2}},
                                         {{"id", "tips"}, {"enabled", true}, {"col", 2},
                                          {"row", 0}, {"colspan", 4}, {"rowspan", 2}}})}}}}}}}});

    auto widgets = config.get<json>("/printers/default/panel_widgets/home/pages/0/widgets", json());
    REQUIRE(widgets.is_array());
    REQUIRE(widgets.size() == 2);
    for (const auto& w : widgets) {
        INFO("widget " << w.value("id", std::string{}));
        CHECK(w["col"] == -1);
        CHECK(w["row"] == -1);
        CHECK_FALSE(w.contains("colspan"));
        CHECK_FALSE(w.contains("rowspan"));
    }
    CHECK(config.get<int>("/config_version", 0) == 22);
}

TEST_CASE_METHOD(MigrationV22Fixture,
                 "Config migration v22: a user trash keeps its hide, an engine disable does not",
                 "[config][migration][22]") {
    write_and_init({{"config_version", 21},
                    {"active_printer", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", home({{{"id", "tips"}, {"enabled", false}, {"col", 2},
                                          {"row", 0}, {"colspan", 4}, {"rowspan", 2}},
                                         {{"id", "fan_stack"}, {"enabled", false}, {"col", -1},
                                          {"row", -1}, {"colspan", 1}, {"rowspan", 1}},
                                         {{"id", "led"}, {"enabled", true}, {"col", 3},
                                          {"row", 2}, {"colspan", 1}, {"rowspan", 1}}})}}}}}}}});

    auto widgets = config.get<json>("/printers/default/panel_widgets/home/pages/0/widgets", json());
    REQUIRE(widgets.size() == 3);
    // tips carried real coordinates, so its disable was a trash press.
    CHECK(widgets[0]["enabled"] == false);
    // fan_stack was already unplaced, so the disable was the engine's; the key
    // goes away and the registry default decides.
    CHECK_FALSE(widgets[1].contains("enabled"));
    CHECK(widgets[2]["enabled"] == true);
    // Array order drives placement order and must not change.
    CHECK(widgets[0]["id"] == "tips");
    CHECK(widgets[1]["id"] == "fan_stack");
    CHECK(widgets[2]["id"] == "led");
}

TEST_CASE_METHOD(MigrationV22Fixture, "Config migration v22: covers every printer profile",
                 "[config][migration][22]") {
    write_and_init(
        {{"config_version", 21},
         {"active_printer", "default"},
         {"printers",
          {{"default",
            {{"panel_widgets",
              {{"home", home({{{"id", "led"}, {"enabled", true}, {"col", 1}, {"row", 1}}})}}}}},
           {"printer-2",
            {{"panel_widgets",
              {{"home",
                home({{{"id", "tips"}, {"enabled", true}, {"col", 3}, {"row", 2}}})}}}}}}}});

    for (const char* id : {"default", "printer-2"}) {
        auto widgets = config.get<json>(std::string("/printers/") + id +
                                            "/panel_widgets/home/pages/0/widgets",
                                        json());
        INFO("printer " << id);
        REQUIRE(widgets.size() == 1);
        CHECK(widgets[0]["col"] == -1);
        CHECK(widgets[0]["row"] == -1);
    }
}

TEST_CASE_METHOD(MigrationV22Fixture, "Config migration v22: handles a legacy flat array",
                 "[config][migration][22]") {
    // Configs written before the multi-page format hold a bare array. After
    // unplacement PanelWidgetConfig::load() sees no entry with a grid position
    // and treats the config as pre-grid, replacing it with build_defaults() —
    // which discards every deliberate hide. The migration converts the array
    // to the page shape so that branch is never reached.
    write_and_init({{"config_version", 21},
                    {"active_printer", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", json::array({{{"id", "tips"}, {"enabled", false}, {"col", 2},
                                                 {"row", 0}, {"colspan", 4}, {"rowspan", 2}},
                                                {{"id", "led"}, {"enabled", true}, {"col", 3},
                                                 {"row", 2}}})}}}}}}}});

    auto homecfg = config.get<json>("/printers/default/panel_widgets/home", json());
    REQUIRE(homecfg.is_object());
    REQUIRE(homecfg.contains("pages"));
    auto widgets = homecfg["pages"][0]["widgets"];
    REQUIRE(widgets.size() == 2);
    CHECK(widgets[0]["enabled"] == false); // deliberate hide survives
    CHECK(widgets[0]["col"] == -1);
}

TEST_CASE_METHOD(MigrationV22Fixture, "Config migration v22: drops the cached grid row count",
                 "[config][migration][22]") {
    // /ui/cached_grid/<panel>/rows is a row count in cells of the old grid.
    write_and_init({{"config_version", 21},
                    {"ui", {{"cached_grid", {{"home", {{"rows", 4}}}}}}},
                    {"active_printer", "default"},
                    {"printers", {{"default", json::object()}}}});

    auto ui = config.get<json>("/ui", json());
    CHECK_FALSE(ui.contains("cached_grid"));
}

TEST_CASE_METHOD(MigrationV22Fixture, "Config migration v22: is idempotent",
                 "[config][migration][22]") {
    write_and_init({{"config_version", 22},
                    {"active_printer", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", home({{{"id", "led"}, {"enabled", true}, {"col", 3},
                                          {"row", 2}, {"colspan", 1}, {"rowspan", 1}}})}}}}}}}});

    // Already stamped 22 — the migration must not run and must not touch a
    // layout the user arranged after upgrading.
    auto widgets = config.get<json>("/printers/default/panel_widgets/home/pages/0/widgets", json());
    CHECK(widgets[0]["col"] == 3);
    CHECK(widgets[0]["colspan"] == 1);
}

TEST_CASE_METHOD(MigrationV22Fixture, "Config migration v22: survives every missing node",
                 "[config][migration][22]") {
    // No printers at all; no panel_widgets; a page that is a string; a widgets
    // value that is an object. Each must migrate and stamp rather than throw.
    write_and_init({{"config_version", 21}});
    CHECK(config.get<int>("/config_version", 0) == 22);

    TearDown();
    SetUp();
    write_and_init({{"config_version", 21},
                    {"printers",
                     {{"default", {{"panel_widgets", {{"home", {{"pages", json::array({"main"})}}}}}}}}}});
    CHECK(config.get<int>("/config_version", 0) == 22);
}
```

- [ ] **Step 2: Run and watch them fail**

```bash
make test -j && ./build/bin/helix-tests "[migration][22]"
```

Expected: FAIL — `config_version` is 21 and coordinates are untouched.

- [ ] **Step 3: Write the migration**

In `src/system/config.cpp`, immediately after `migrate_v20_to_v21` ends at `:1233`:

```cpp
/// Migration v21→v22: unplace every saved home layout.
///
/// Saved col/row/colspan/rowspan are counts of cells in a grid whose track
/// count and cell size both changed, so the numbers no longer name anything.
/// This runs at config load — before Application settles the screen size and
/// long before LayoutManager::init() — so there is no resolution here to
/// rescale against, and no resolution is stored in settings.json either. The
/// coordinates are cleared and the two-pass placement engine seats everything
/// on whatever grid the panel actually turns out to have, which is the path
/// every never-positioned widget already takes on every boot.
///
/// Intent is read before the coordinates are blanked: an entry that is disabled
/// AND holds real coordinates was removed with the trash button, which leaves
/// the position intact. One that is disabled at -1 was auto-disabled by the
/// placement engine or was never added, so the key is dropped and the
/// registry's default_enabled decides.
///
/// Iterates every printer profile, not just the active one — the shipped
/// config/settings.json already carries two.
static void migrate_v21_to_v22(json& config) {
    // A row count in cells of the grid that just changed.
    if (config.contains("ui") && config["ui"].is_object()) {
        config["ui"].erase("cached_grid");
    }

    if (!config.contains("printers") || !config["printers"].is_object()) {
        return;
    }

    int unplaced = 0;
    int profiles = 0;

    auto unplace_array = [&unplaced](json& widgets) {
        if (!widgets.is_array()) {
            return;
        }
        for (auto& entry : widgets) {
            if (!entry.is_object()) {
                continue;
            }
            const bool enabled =
                entry.contains("enabled") && entry["enabled"].is_boolean() && entry["enabled"];
            const int col =
                (entry.contains("col") && entry["col"].is_number_integer()) ? entry["col"].get<int>()
                                                                            : -1;
            if (!enabled && col < 0) {
                entry.erase("enabled");
            }
            entry["col"] = -1;
            entry["row"] = -1;
            entry.erase("colspan");
            entry.erase("rowspan");
            ++unplaced;
        }
    };

    for (auto& printer : config["printers"]) {
        if (!printer.is_object() || !printer.contains("panel_widgets") ||
            !printer["panel_widgets"].is_object()) {
            continue;
        }
        ++profiles;
        for (auto& panel : printer["panel_widgets"]) {
            // Legacy flat array: lift it into the page shape as well. Left as an
            // array, PanelWidgetConfig::load() would find no entry with a grid
            // position, read the config as pre-grid and replace it wholesale
            // with the registry defaults, discarding every deliberate hide.
            if (panel.is_array()) {
                json widgets = panel;
                unplace_array(widgets);
                panel = json{{"main_page_index", 0},
                             {"next_page_id", 1},
                             {"pages", json::array({json{{"id", "main"}, {"widgets", widgets}}})}};
                continue;
            }
            if (!panel.is_object() || !panel.contains("pages") || !panel["pages"].is_array()) {
                continue;
            }
            for (auto& page : panel["pages"]) {
                if (page.is_object() && page.contains("widgets")) {
                    unplace_array(page["widgets"]);
                }
            }
        }
    }

    if (unplaced > 0) {
        spdlog::info("[Config] Migration v22: unplaced {} widget(s) across {} printer profile(s)",
                     unplaced, profiles);
    }
}
```

Add the dispatch line after `config.cpp:1339`:

```cpp
    if (version < 22)
        migrate_v21_to_v22(config);
```

Bump `include/config.h:60`:

```cpp
static constexpr int CURRENT_CONFIG_VERSION = 22;
```

- [ ] **Step 4: Run and watch them pass**

```bash
make test -j && ./build/bin/helix-tests "[migration]"
```

Expected: PASS, including the v18 and v21 suites, which each assert their own stamp.

- [ ] **Step 5: Migrate a real config end to end**

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
rm -rf "$HELIX_CONFIG_DIR" && mkdir -p "$HELIX_CONFIG_DIR"
cp config/settings.json "$HELIX_CONFIG_DIR/settings.json"
python3 -c "import json,os;p=os.environ['HELIX_CONFIG_DIR']+'/settings.json';d=json.load(open(p));d['config_version']=21;json.dump(d,open(p,'w'),indent=2)"
make -j && ./build/bin/helix-screen --test -vv -s 480x272 --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
sleep 6
grep "Migration v22" /tmp/helix-$TREE.log
./build/bin/helix-screen ctl -s "$HELIX_SOCK" screenshot /tmp/migrated-micro.png
```

Expected: the log names the unplaced count and the profile count; the screenshot shows a full, non-clustered dashboard.

- [ ] **Step 6: Commit**

```bash
git add include/config.h src/system/config.cpp tests/unit/test_config_migration_v22.cpp
git commit -m "feat(config): unplace saved home layouts on upgrade to v22

Saved col/row/span are counts of cells in a grid whose track count and cell
size both changed, and the migration runs before any screen size is known, so
there is nothing to rescale into. Clearing them hands the layout to the
placement engine. The widget set, the array order and a deliberate trash
removal all survive; hand-arranged positions do not."
```

---

### Task 12: AMS mini-status spool count

**Files:**
- Modify: `tests/unit/test_widget_size_ams_mini_status.cpp`
- Modify (only if the count is wrong): `src/ui/ui_ams_mini_status.cpp:64`

**Interfaces:**
- Consumes: `void ui_ams_mini_status_set_width(lv_obj_t* obj, int width_px);` (`include/ui_ams_mini_status.h:116`).
- Produces: no API change unless `MIN_SPOOL_W` moves.

The AMS mini-status is not a `PanelWidget`. `PanelWidgetManager` feeds it a pixel width at `panel_widget_manager.cpp:877-880` and it derives its spool count locally: `visible = (avail_w + gap) / (MIN_SPOOL_W + gap)` with `MIN_SPOOL_W = 60` (`ui_ams_mini_status.cpp:64, 609`). It is the one widget whose visible output changes character — at a given fraction of the panel it will show more, smaller spools than it does today.

- [ ] **Step 1: Write the assertion**

Append to `tests/unit/test_widget_size_ams_mini_status.cpp`:

```cpp
TEST_CASE_METHOD(LVGLUITestFixture, "ams mini-status: spool count at each panel's authored width",
                 "[widget_size][ams][1126]") {
    // The widget is authored 2 tracks wide and grows to 8. This pins how many
    // spools each panel actually shows at the authored width, because the count
    // comes from MIN_SPOOL_W against pixels, not from a span.
    struct Case {
        const char* name;
        int width_px;
        int expected_visible;
    };
    // width_px = grid_track_extent(track_w, gutter, 2) at each geometry.
    const std::vector<Case> cases = {
        {"micro 480x272", 60, 1},        {"tiny 480x320", 68, 1},
        {"small 480x400", 66, 1},        {"medium 800x480", 105, 1},
        {"large 1024x600", 101, 1},      {"xlarge 1280x720", 126, 1},
        {"micro portrait 272x480", 65, 1}, {"portrait 480x800", 113, 1},
    };

    for (const auto& c : cases) {
        auto* obj = make_ams_mini_status(/*slots=*/4);
        ui_ams_mini_status_set_width(obj, c.width_px);
        lv_obj_update_layout(obj);
        INFO(c.name << " width " << c.width_px);
        CHECK(visible_spool_count(obj) == c.expected_visible);
    }
}
```

Fill `expected_visible` from the first run: it is `(width_px + gap) / (MIN_SPOOL_W + gap)` capped to the slot count. Run the test, read the reported values, then pin them.

- [ ] **Step 2: Run it**

```bash
make test -j && ./build/bin/helix-tests "[ams][1126]"
```

- [ ] **Step 3: Judge the result and decide**

If a panel shows more spools than its physical width can render legibly, the correction is a single edit to `MIN_SPOOL_W` at `ui_ams_mini_status.cpp:64` — not a span change, since the span is now shared across every panel. Verify live on the two hardest panels:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
rm -rf "$HELIX_CONFIG_DIR" && mkdir -p "$HELIX_CONFIG_DIR"
./build/bin/helix-screen --test -vv -s 480x272 --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
sleep 6 && grep "Width set to" /tmp/helix-$TREE.log
./build/bin/helix-screen ctl -s "$HELIX_SOCK" screenshot /tmp/ams-micro.png
```

Repeat with `-s 272x480`.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_widget_size_ams_mini_status.cpp src/ui/ui_ams_mini_status.cpp
git commit -m "test(ams-mini): pin the visible spool count per panel width

The mini-status derives its spool count from pixels against MIN_SPOOL_W, so
the square-cell grid changes how many spools each panel shows at the widget's
authored width. The count is now pinned per geometry rather than discovered
on a device."
```

---

### Task 13: Documentation

**Files:**
- Modify: `docs/devel/LAYOUT_SYSTEM.md:325-400`
- Modify: `docs/user/guide/home-panel.md:15-26, 231, 538`
- Modify: `docs/user/TROUBLESHOOTING.md:592-629`

**Interfaces:**
- Consumes: everything above.
- Produces: nothing.

- [ ] **Step 1: Rewrite `LAYOUT_SYSTEM.md` § "How the grid is sized"**

Replace lines 337-382 (the `GRID_DIMS` code block, the `LayoutType` table, the constants table and the worked-example table) with:

````markdown
### How the grid is sized

Both axes divide the panel by the same per-breakpoint track size. There is no
layout-type branch and no count table.

```cpp
// src/ui/grid_layout.cpp
GridDimensions GridLayout::get_dimensions(UiBreakpoint bp) {
    const int track = GRID_CELL[clamp_bp(bp)];
    auto& lm = LayoutManager::instance();
    // Floored to a whole number of cells: an odd track count leaves a final
    // half-cell no whole-cell widget can occupy.
    auto tracks = [track](int extent) {
        const int n = std::clamp(extent / track, MIN_TRACKS, MAX_TRACKS);
        return n - (n % GridLayout::TRACKS_PER_CELL);
    };
    return {tracks(lm.width()), tracks(lm.height())};
}
```

A **track is half a cell** (`GridLayout::TRACKS_PER_CELL == 2`, #1126). Authored
spans — in the registry, in `default_layout.json` and in a saved layout — are in
tracks. Only widgets whose `PanelWidgetDef` sets `supports_half_col` /
`supports_half_row` may be placed or sized at an odd track count; edit mode snaps
everything else to even boundaries.

| Constant | Value | Meaning |
|----------|-------|---------|
| `GRID_CELL` | `{34, 40, 40, 60, 60, 72}` | Target track edge in px, indexed by `UiBreakpoint` |
| `TRACKS_PER_CELL` | 2 | Tracks per authored cell |
| `MIN_TRACKS` | 4 | Degenerate-display guard; no shipping panel reaches it |
| `MAX_TRACKS` | 64 | Ceiling; 1920x440 wants 48 columns |

The breakpoint comes from the **narrow** axis (`min(width, height)`), so a tall
portrait panel is classified by its width. See `include/ui_breakpoint.h`.

| panel | breakpoint | track | grid | cell |
|---|---|---|---|---|
| 480x272 | MICRO | 34 | 14x8 | 34.3 x 34.0 |
| 272x480 | MICRO | 34 | 8x14 | 34.0 x 34.3 |
| 800x480 | MEDIUM | 60 | 12x8 | 61.5 x 60.0 |
| 480x800 | MEDIUM | 60 | 8x13 | 60.0 x 61.5 |
| 1024x600 | LARGE | 60 | 16x10 | 60.2 x 60.0 |
| 1920x440 | SMALL | 40 | 48x10 | 40.0 x 40.0 |
| 320x1480 | TINY | 40 | 8x37 | 40.0 x 40.0 |

Rotating a panel transposes its grid exactly, which is the invariant
`tests/unit/test_grid_square_cells.cpp` pins.
````

- [ ] **Step 2: Update the user guide**

`docs/user/guide/home-panel.md:15` — replace "The grid is 8 columns by 5 rows on standard and large screens (6x4 on small screens)" with a sentence stating that the grid is sized so cells are square, that it therefore has more, smaller cells than before, and that a widget occupies several of them. Line 26's ultrawide sentence and line 538's "6x4 grid because its height (480) is 550px or less" both describe the deleted model; rewrite both. Line 231's `2x2 | 1x1 | 6x4` size columns for Temperature Graph become `4x4 | 2x2 | 12x8`, and every other widget row's size columns double per the Task 7 table.

- [ ] **Step 3: Retire the manual-repair entry**

`docs/user/TROUBLESHOOTING.md:592-629` walks the user through hand-editing `panel_widgets` to recover widgets that were auto-disabled for want of room. That condition is gone — the "no widget disabled for want of room" test in Task 8 asserts it on every shipping panel. Replace the section with a short note that upgrading to this version resets widget positions, keeps the widget set and any deliberate removals, and that the layout can be rearranged in edit mode.

- [ ] **Step 4: Prove nothing references the deleted model**

```bash
grep -rn "MIN_PORTRAIT_COLS\|TARGET_CELL_W_PX\|TARGET_CELL_H_PX\|MAX_DYNAMIC\|MIN_DYNAMIC\|GRID_DIMS" src include tests docs
```

Expected: no output.

- [ ] **Step 5: Run every gate and the full relevant suite**

```bash
./scripts/quality-checks.sh
make test -j
./build/bin/helix-tests "[grid_layout]"
./build/bin/helix-tests "[square]"
./build/bin/helix-tests "[grid_edit]"
./build/bin/helix-tests "[default_layout]"
./build/bin/helix-tests "[panel_widget_config]"
./build/bin/helix-tests "[panel_widget_manager]"
./build/bin/helix-tests "[widget_size]"
./build/bin/helix-tests "[span_bands]"
./build/bin/helix-tests "[migration]"
bats tests/shell/test_grid_metrics_gate.bats
```

Expected: all pass; `check_grid_metrics_single_source.py` reports 2 call sites; `check_imperative_ui.py` reports at most 387.

- [ ] **Step 6: Commit**

```bash
git add docs/devel/LAYOUT_SYSTEM.md docs/user/guide/home-panel.md docs/user/TROUBLESHOOTING.md
git commit -m "docs(layout): describe the square-cell home grid

The developer guide's grid section described a per-breakpoint count table and
three layout-type overrides that no longer exist. The user guide's fixed 8x5
and 6x4 figures and the troubleshooting entry for manually re-enabling widgets
that ran out of room are replaced by the current model."
```

---

### Task 14: Widgets must render their content at their minimum size without clipping

**Why this exists.** Every verification in this plan is arithmetic. The band table proves a
span lands in a pixel band; `test_panel_widget_manager_cell_px.cpp` proves the manager hands a
widget the pixels it promised. **Nothing anywhere proves the widget's actual content fits in
those pixels.** A widget can be handed a correct, in-band size and still overflow its box,
ellipsize a label to uselessness, or push a control off its own edge — and the entire test
suite stays green, because no assertion has ever looked. The square-cell change moves every
widget's authored size on eight panels, so this is the moment that gap costs something.

**Files:**
- Modify: `tests/test_helpers/panel_widget_size_harness.h`
- Create: `tests/unit/test_widget_content_fits.cpp`
- Possibly modify: `src/ui/panel_widgets/*.cpp` and `ui_xml/components/panel_widget_*.xml`, for whatever this finds

**Interfaces:**
- Consumes: `PanelWidgetHarness<W>` (existing — creates the real XML component, attaches the widget, drives `on_size_changed()` and settles layout), `find_widget_def()`, `GridLayout::get_dimensions`, `grid_track_extent`.
- Produces: `require_no_overflow()` in `namespace helix`, alongside the existing `require_font_tokens_distinct()`.

**Do not build a new harness.** `PanelWidgetHarness` already does the hard part, and its
`resize()` already handles the ordering trap (set size → `lv_obj_update_layout` →
`on_size_changed` → `lv_obj_update_layout`) that a hand-rolled version gets wrong. Fifteen
`test_widget_size_*.cpp` files already use it. Extend it.

- [ ] **Step 1: Add the overflow detector to the harness**

`require_no_overflow(lv_obj_t* root)`, next to `require_font_tokens_distinct()`. It must catch
three distinct failures, which are not the same check:

1. **Geometric overflow.** Walk every descendant; compare each child's absolute area against
   its parent's *content* coords (`lv_obj_get_content_coords`, not the outer coords — padding
   is not usable space). A child extending past it is clipped.
2. **Scroll overflow.** `lv_obj_get_scroll_bottom(obj) > 0` or `lv_obj_get_scroll_right(obj) > 0`
   means laid-out content exceeds the box even where children were repositioned rather than
   drawn outside it.
3. **Text truncation.** A label whose rendered text is wider than its box is ellipsized or cut.
   Measure with `lv_text_get_size()` using the label's own resolved font and letter/line space,
   and compare against the label's width. A font mismatch here makes the check vacuous — see
   `require_font_tokens_distinct()` for why that failure mode is easy to ship.

**The exceptions list is the hard part, and it decides whether this gate survives.**
`tests/CLAUDE.md`: *"A gate that fires on legitimate code gets switched off, so the silent
cases matter as much as the loud ones."* Some overflow is correct by design — a deliberately
scrollable console, a filename that is *supposed* to ellipsize, a marquee label
(`LV_LABEL_LONG_SCROLL`/`SCROLL_CIRC`). Do not suppress those with a blanket allowance.
Skip a subtree only when the object itself declares the intent — `LV_OBJ_FLAG_SCROLLABLE` set
deliberately, or a label whose `long_mode` is an explicitly scrolling/dotting mode — and
make each skip visible in the failure message. Report every exception you add and why.

- [ ] **Step 2: Prove the detector catches something before trusting it**

Point it at a deliberately under-sized widget and watch it fail. A detector that has never
gone red is not evidence. Then mutate it (drop the scroll check, or compare against outer
coords instead of content coords) and confirm the corresponding case stops failing — mutate
the FEATURE, not a constant.

- [ ] **Step 3: Drive every widget at its authored MINIMUM on every shipping geometry**

For each `PanelWidgetDef`, compute the pixel size of `effective_min_colspan` ×
`effective_min_rowspan` on each of the eight geometries via `grid_track_extent()` — the same
path `PanelWidgetManager` uses — then `resize()` the harness to it and run
`require_no_overflow()`. The minimum is the interesting size: it is what a widget gets when the
grid is full or the user shrinks it, and it is what the placement engine falls back to under
scarcity.

Widgets whose construction needs live state (`ams`, `tool_switcher`, `camera`) already have
working setups in their `test_widget_size_*.cpp` files — reuse those, do not invent fixtures.

- [ ] **Step 4: Report before fixing**

This will find real clipping. **Do not start fixing widgets inside this task.** Produce the
list — widget, geometry, size, which of the three checks fired — and stop. Each fix is a
judgment call between raising the widget's authored minimum, adding a smaller layout branch,
or accepting truncation as correct, and those are not decisions to make thirty times in a row
inside a test task. Route them.

- [ ] **Step 5: Wire it in and commit**

Tag it so it can run alone. Note in the report which widgets pass at minimum but only just —
a widget clearing its box by two pixels is a translation away from clipping, and this suite
runs in one language.

**Sequencing.** Task 14 depends on the authored minimums (Task 7) and on the final sizing
expression, so it runs after both. **Re-run it as a gate at the end of Task 8 and Task 9** —
those tasks re-author anchors and seed layouts, which changes what sizes widgets actually get
on a real dashboard.

---

## Self-review

**Spec coverage.** §1 → Task 5 (+ Task 6 for the row axis the spec's aspect table assumes). §2a → Task 2. §2b → Task 4. §2c → Task 3. §2d → already complete on this branch (`current_metrics()`, `grid_cell_metrics()`). §2e → already complete (`create_drag_ghost` and the `DRAG_SHADOW_*` constants are absent from the current file). §3 → already complete; Task 7 supplies the verification the spec asks for. §4a → Tasks 1 and 7. §4b → Tasks 8 and 9. §4c → Tasks 10 and 11. §5 rewritten tests → Tasks 5, 8; §5 new invariants 1-2 → Task 5, 3 → Task 8, 4-5 → Task 11, 6 → Task 2, 7 → Task 3. "Previously rejected" honoured: the placement algorithm (`find_available_bottom_min`, `grow_to_targets`) is untouched, and no greedy span-stepping or strict-minimum-first is introduced.

**Type consistency.** `TRACKS_PER_CELL` (Task 3, value flipped in Task 5), `snap_step_for` returning `std::pair<int,int>` (Task 3, used in Tasks 3 and 4), `dot_count` (Task 4), `supports_half_col`/`supports_half_row` (Task 1, read in Tasks 3, 4, 7), `migrate_v21_to_v22(json&)` (Task 11) — all consistent across tasks.

---
