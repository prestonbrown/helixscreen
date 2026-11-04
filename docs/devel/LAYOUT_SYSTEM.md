# Layout System

> Alternative XML layouts for different screen aspect ratios and orientations.

> **For contributors:** If you want to create or fix layouts, start with the **[UI Contributor Guide](UI_CONTRIBUTOR_GUIDE.md)** instead. This document covers the LayoutManager C++ API and auto-detection internals.

## Overview

HelixScreen's UI is built from XML files that define the layout and structure of each panel.
The default XML files are designed for standard landscape displays (4:3 to 16:9, like 800x480
or 1024x600). But users with ultrawide screens (1920x480), portrait-mounted displays, or very
small screens need fundamentally different arrangements — not just scaling, but different
structures.

The **Layout System** lets you create alternative XML files for these different screen shapes.
You only need to override the files that need to change — everything else automatically falls
back to the standard version.

**Key distinction:** Themes control *colors*. Layouts control *structure*. They're independent —
any theme works with any layout.

## Current Status

> **This is early days.** The layout infrastructure is complete and working, but most layouts
> still need to be created. This is a great area for community contributions — you only need
> to know XML, not C++.

| Layout | Status | What Exists |
|--------|--------|-------------|
| `standard` | **Complete** | All panels — this is the default UI everyone uses today |
| `ultrawide` | Not started | Directory doesn't exist yet |
| `portrait` | **Started** | `print_tune_panel.xml` |
| `micro` | **Started** | `controls_panel.xml` |
| `micro_portrait` | Not started | Directory doesn't exist yet; the `portrait/` layer serves it |
| `tiny` | Not started | Directory doesn't exist yet |
| `tiny_portrait` | Not started | Directory doesn't exist yet |

**A whole-file override is not the only way to adapt a panel, and increasingly not the
preferred one.** Forking a panel into `ui_xml/portrait/` duplicates everything that did not
need to change, and both copies then have to be maintained. Where the difference is a row
that becomes a column, or a block that is dropped on a short screen, the panel stays a
single file and branches in place on the `ui_is_portrait` subject:

| Mechanism | Use when | Examples in the tree |
|-----------|----------|----------------------|
| `ui_xml/<variant>/<panel>.xml` | The panel is genuinely a different design in that orientation | `portrait/print_tune_panel.xml`, `micro/controls_panel.xml` |
| `<if cond="ui_is_portrait eq 1">…<else/>…</if>` | An entire subtree differs, and building both would be wasteful | `ui_xml/motion_panel.xml#motion_panel`, `ui_xml/bed_mesh_panel.xml#bed_mesh_panel`, `temp_graph_overlay.xml` (3 sites) |
| `<bind_style_if cond="ui_is_portrait"/>` | Only the styling differs — flex direction, padding, button shape | `ui_xml/app_layout.xml` and `ui_xml/navigation_bar.xml` (the app shell and nav bar switch axis and position), `ui_xml/advanced_panel.xml#advanced_panel` (the E-stop bar goes column instead of row) |
| `<bind_flag_if_eq subject="ui_is_portrait"/>` | Both variants are cheap to build and you want to show one | `ui_xml/components/bed_mesh_current_mesh_card.xml#mesh_max_coord_inline` |

Orientation is also composable with the breakpoint rather than separate from it:
`ui_xml/components/bed_mesh_current_mesh_card.xml` hides a block on
`ui_breakpoint eq 0 or (ui_is_portrait eq 1 and ui_breakpoint_v lt 5)` — the portrait clause
is guarded because a 1024x600 landscape panel and a 480x640 portrait one land on the same
breakpoint while only the portrait one is actually short of room.

`ui_is_ultrawide` is the same kind of subject for the `ultrawide` class (wider than 2.5:1,
the test `is_ultrawide_aspect()` in `src/layout_manager.cpp` owns). It shares
`ui_is_portrait`'s sources: `theme_manager_init` seeds both from `detect_layout_type()`, and
`theme_manager_refresh_orientation()` republishes both from `LayoutManager::type()` once a
`--layout` override is known, and on every resize. A tier cannot stand in for it: 1920x440
reads `ui_breakpoint` small and 1920x480 medium, the same tiers as ordinary screens.
`ui_xml/panel_belt_tension.xml#panel_belt_tension` branches on it in place (chart left,
sidebar right) instead of shipping a `ui_xml/ultrawide/` copy.

C++ that needs the layout class asks `app_layout_type(width, height)` (`include/layout_manager.h`): `LayoutManager::type()` once it is initialized, so a `--layout` override is respected, and the physical `detect_layout_type()` before that. `app_layout_is_portrait()` is built on it. Code that sizes things from raw dimensions before `LayoutManager` exists (`src/ui/overlay_geometry.cpp#nav_width_suffix`) calls `is_ultrawide_aspect(width, height)` rather than restating the threshold.

So "which panels adapt to portrait" is not answered by listing the override directory.
Beyond the two files there, the app shell, the navigation bar, Motion, Bed Mesh, the
temperature graph overlay, the Advanced panel's E-stop bar and the bed mesh cards all adapt
from inside their shared file.

The home panel's widget grid is a third case again: it adapts to ultrawide and portrait
geometry in C++ from the measured content box, regardless of which override files exist —
see [Home Widget Grid](#home-widget-grid).

## How It Works

When HelixScreen starts up, it detects your screen's aspect ratio and picks a layout:

| Layout | When It's Chosen | Example Screens |
|--------|-----------------|-----------------|
| `standard` | Normal landscape (4:3 to 16:9) | 800x480, 1024x600, 1280x720 |
| `ultrawide` | Very wide (ratio > 2.5:1) | 1920x480, 1920x400 |
| `portrait` | Tall/narrow (ratio < 0.8:1) | 480x800, 600x1024 |
| `micro` | Very small landscape (max dimension ≤ 480 and min dimension ≤ 272) | 480x272 |
| `micro_portrait` | Very small portrait (max dimension ≤ 480 and min dimension ≤ 272) | 272x480 |
| `tiny` | Small landscape (max dimension ≤ 480, min dimension > 272) | 480x320, 480x400 |
| `tiny_portrait` | Small portrait (max dimension ≤ 480, min dimension > 272) | 320x480, 400x480 |

You can also force a layout manually:
- **CLI flag:** `--layout ultrawide`
- **Config file:** Set `display.layout` to `"ultrawide"` in `settings.json`

When a layout is active, HelixScreen searches an ordered chain of override directories
(most specific first) for each file. Portrait sub-classes look in their own dir, then the
shared `portrait/` dir, then the standard file; other layouts check their one override dir,
then the standard file. The first match wins. This means you can override one panel at a
time — you don't have to recreate everything from scratch. See `variant_chain()` under the
Developer Reference below.

## Directory Structure

```
ui_xml/
  globals.xml              ← Shared by ALL layouts (design tokens, never override this)
  app_layout.xml           ← Standard app chrome (navbar + content area)
  home_panel.xml           ← Standard home panel
  controls_panel.xml       ← Standard controls panel
  settings_panel.xml       ← ...and ~200 more XML files (226 total)
  ...

  portrait/                ← Portrait overrides (print_tune_panel.xml)
  micro/                   ← Micro landscape overrides (480x272: controls_panel.xml)
  micro_portrait/          ← Doesn't exist yet — the portrait/ layer serves 272x480

  ultrawide/               ← Doesn't exist yet — create it to override for ultrawide!
  tiny/                    ← Tiny landscape overrides (doesn't exist yet)
  tiny_portrait/           ← Tiny portrait overrides (doesn't exist yet)
```

**The rule is simple:** to override `controls_panel.xml` for ultrawide screens, create
ui_xml/ultrawide/controls_panel.xml. That's it. HelixScreen will automatically pick it up.

---

## Contributing a Layout

This section is for anyone who wants to help create or improve layouts. You don't need to
know C++ — all layout work is pure XML.

### What You Need

1. A clone of the HelixScreen repo
2. A working build (see the project README)
3. Familiarity with the XML layout system (see `LVGL9_XML_GUIDE.md`)

### Step-by-Step: Creating a New Layout Override

Let's say you want to create an ultrawide version of the controls panel.

**1. Find the standard file to use as a starting point**

```bash
# Look at the standard version
cat ui_xml/controls_panel.xml
```

**2. Create your override file**

```bash
# Copy the standard file into the layout directory
cp ui_xml/controls_panel.xml ui_xml/ultrawide/controls_panel.xml
```

**3. Edit the override to rearrange the layout**

Open ui_xml/ultrawide/controls_panel.xml and restructure it for the target screen shape.
The key rules are listed below in [Layout Override Rules](#layout-override-rules).

**4. Test it**

```bash
# Build (only needed if you changed C++ — XML changes don't need a rebuild!)
make -j

# Run with ultrawide layout on a 1920x480 window
./build/bin/helix-screen --test -vv --layout ultrawide -s 1920x480

# Take a screenshot for comparison
HELIX_SCREENSHOT_DISPLAY=0 ./scripts/screenshot.sh helix-screen ultrawide-controls controls --test --layout ultrawide -s 1920x480
```

**5. Compare with standard**

```bash
# Screenshot the standard layout too
HELIX_SCREENSHOT_DISPLAY=0 ./scripts/screenshot.sh helix-screen standard-controls controls --test
```

Screenshots are saved to `/tmp/ui-screenshot-<name>.png`.

### Step-by-Step: Starting a Brand New Layout Family

To start a layout that doesn't exist yet (e.g., `portrait`):

```bash
# Create the directory
mkdir -p ui_xml/portrait

# Start with the most impactful panel — usually home_panel
cp ui_xml/home_panel.xml ui_xml/portrait/home_panel.xml

# Edit it for portrait orientation
# Then test:
./build/bin/helix-screen --test -vv --layout portrait -s 480x800
```

You can override as many or as few files as you want. Any panel you *don't* override will
use the standard version automatically.

### Layout Override Rules

These rules **must** be followed for a layout override to work correctly. The C++ code is
shared between all layouts — only the XML structure changes.

**1. Keep all named widgets that C++ looks up**

The C++ code for each panel uses `lv_obj_find_by_name()` to find specific widgets. If your
layout is missing a named widget, the panel will break.

For `home_panel.xml`, these widget names are required:

| Widget Name | What It Does |
|-------------|-------------|
| `printer_image` | Displays the printer photo (set dynamically by C++) |
| `status_text_label` | Tip text (C++ animates fade transitions on this) |
| `print_card_thumb` | Benchy thumbnail in idle state |
| `print_card_active_thumb` | Print thumbnail during active print |
| `print_card_label` | "Print Files" text / progress text |
| `temp_icon` | Heater icon (C++ controls color animation while heating) |
| `light_icon` | Light bulb icon (C++ sets color/brightness dynamically) |

**How to find required names for other panels:** search the corresponding `.cpp` file for
`lv_obj_find_by_name`. Any name referenced there must exist in your XML.

**2. Keep all subject bindings**

Subject bindings (`bind_text`, `bind_value`, `bind_flag_if_eq`, `bind_style`, etc.) connect
the XML to live data from the C++ code. Your layout must bind to the same subjects as the
standard layout. You can rearrange *where* the bound widgets appear, but the bindings
themselves must stay.

For example, the temperature display must still bind to `extruder_temp` and `extruder_target`:
```xml
<temp_display bind_current="extruder_temp" bind_target="extruder_target" .../>
```

**3. Keep all event callbacks**

Event callbacks (`<event_cb trigger="clicked" callback="..."/>`) connect buttons to C++ code.
Every callback in the standard file must appear in your override, attached to the appropriate
widget.

**4. Don't change existing values in `globals.xml`**

Design tokens (colors, spacing, typography) are defined in `globals.xml` and shared across
all layouts, so retuning an existing token changes every layout at once. *Adding* a new
suffixed responsive token there is a different matter: it is correct, and it is the only
place the declaration can live. Token discovery is top-level-only, so a `<px name="foo_small">`
declared in `ui_xml/portrait/` is never registered and every `#foo` referencing it silently
resolves to nothing. Declare in `globals.xml`, reference from the variant file. See
[UI Contributor Guide](UI_CONTRIBUTOR_GUIDE.md) §2 and `scripts/check_responsive_token_scope.py`.

Your layout XML should use these tokens — not hardcoded values:

```xml
<!-- Good: uses design tokens -->
<lv_obj style_pad_all="#space_md" style_pad_gap="#space_sm">
<text_body style_text_color="#text"/>

<!-- Bad: hardcoded values -->
<lv_obj style_pad_all="16" style_pad_gap="8">
<lv_label style_text_color="0xE0E0E0"/>
```

**5. Use typography components, not raw labels**

```xml
<!-- Good -->
<text_heading text="Status"/>
<text_body bind_text="temperature"/>
<text_small text="Details"/>

<!-- Bad -->
<lv_label style_text_font="..." text="Status"/>
```

### Design Guidelines by Layout Type

**Ultrawide (1920x480):**
- You have tons of horizontal space but very little vertical space (480px minus the navbar)
- Prefer horizontal `flex_flow="row"` layouts — avoid vertical scrolling
- Aim for all content visible at once, no scrolling needed
- Think "dashboard" — information spread across columns

**Portrait (480x800, 600x1024):**
- Lots of vertical space, narrow width
- The navigation bar and app shell already move to the bottom automatically (`ui_is_portrait`) — no override needed
- Content stacks vertically naturally
- Consider which elements can be stacked vs. side-by-side
- Portrait is where narrow-axis breakpoint selection bites hardest: the tier comes from
  `min(width, height)`, so a 320x1480 panel resolves to the Tiny tier from its 320px width,
  not from its 1480px of height. Prefer height tokens over hardcoded pixels so vertical
  sizes scale with the tier you actually land in.

**Micro (480x272):**
- Extremely height-constrained — only 272px vertical space
- Use `#space_xs` padding everywhere (vs `#space_sm`/`#space_md`)
- Header bar reduced to 40px, setting rows use compact padding
- Action buttons should be pinned outside scroll areas (not inside)
- Hide secondary information aggressively (status text, descriptions)

**Tiny (480x320, 480x400):**
- Very limited space in both directions
- Reduce information density — show less, make touch targets bigger
- Consider hiding optional elements (sensor indicators, etc.)
- Larger icons, fewer text labels

### Panels Worth Overriding

Not every panel needs a layout-specific version. Start with the ones that matter most:

| Priority | Panel | Why |
|----------|-------|-----|
| High | `home_panel.xml` | First thing users see, lots of information to arrange |
| Medium | `controls_panel.xml` | Common panel with multiple cards to rearrange |
| Medium | `print_status_panel.xml` | Important during prints |
| Medium | `settings_panel.xml` | Compact 6-row category menu; sub-panels may benefit from multi-column |
| Low | Overlays (`*_overlay.xml`) | Usually modal dialogs that work OK at any size |
| Low | Simple panels | Panels with minimal content adapt naturally |

`app_layout.xml` and `navigation_bar.xml` are not in this table: both already adapt their
chrome and nav position to `ui_is_portrait` in place, so a new layout family only needs to
override them if its chrome is a genuinely different design, not just a different axis.

### XML Tips for Layout Work

- **No rebuild needed for XML changes.** Just relaunch the app.
- **`flex_flow="row"`** = horizontal layout, **`flex_flow="column"`** = vertical layout.
- **`flex_grow="1"`** makes an element stretch to fill available space.
- **`width="50%"` / `height="100%"`** for fixed proportions.
- **`scrollable="false"`** is **required** on any container that is not a real scroll region. LVGL's scrollable default is ON and our `lv_obj` theme does not override it, so a plain layout wrapper absorbs drags and qualifies for a page-scroll gutter - which is how chevrons end up drawn over content. See `CONTRIBUTOR_GOTCHAS.md` and `PAGE_SCROLL_BUTTONS.md`.
- **`hidden="true"`** + `bind_flag_if_*` = conditional visibility (driven by data).
- See `LVGL9_XML_GUIDE.md` for the full XML reference.

### Testing Your Layout

```bash
# Standard (800x480) — should be unchanged
./build/bin/helix-screen --test -vv

# Ultrawide
./build/bin/helix-screen --test -vv --layout ultrawide -s 1920x480

# Portrait
./build/bin/helix-screen --test -vv --layout portrait -s 480x800

# Tiny
./build/bin/helix-screen --test -vv --layout tiny -s 480x320

# Force any layout on any resolution (for testing)
./build/bin/helix-screen --test -vv --layout ultrawide -s 800x480
```

Use `-vv` for debug logging — it shows which layout was detected and which XML paths are
being resolved.

---

## Home Widget Grid

Everything above is about `ui_xml/` overrides. The home panel's dashboard is a *second*
layout system that shares the same variant chain but does not live in XML at all: the grid
is computed in C++ (`include/grid_layout.h`, `src/ui/home/grid_layout.cpp`) and the shipped
default placements live in `assets/config/default_layout.json`. If you are adding a home
widget or wondering why yours vanished on a portrait screen, this is the section.

Rearranging the grid in edit mode (selecting, dragging and resizing widgets, moving them
between pages, and how pages are created and removed) is [HOME_EDIT_MODE.md](HOME_EDIT_MODE.md).
This section is the grid those gestures land on.

### How the grid is sized

The grid is square-cell: both axes are divided by the same per-breakpoint cell edge, so a
rotated panel gets (near enough) the transpose of its landscape grid and a widget's colspan
and rowspan mean the same physical thing.

```cpp
// include/grid_layout.h
TRACKS_PER_CELL = 2                                         // a track is HALF a cell
GRID_CELL[kNumBreakpoints] = {34, 40, 40, 60, 60, 72, 72}   // target track edge, px
```

`GRID_CELL` carries one entry per `UiBreakpoint`, XXLarge included - a `static_assert` ties
its length to `UiBreakpoint::XXLarge + 1` so adding a tier without a track edge fails the
build. XXLarge shares XLarge's 72px edge, so a 1920x1080 panel resolves to a 12x7-cell
(24x14-track) grid. `theme_manager`'s `nav_width_suffix()` and `ui_xml/navigation_bar.xml`
carry the matching `_xlarge` / `_xxlarge` nav widths.

`GridLayout::get_dimensions(bp, content_w, content_h)` divides the container's **content
box** — not the panel resolution — by `TRACKS_PER_CELL * GRID_CELL[bp]`, rounds each axis
to the **nearest** whole cell, and multiplies back up to tracks:

```
cells  = round(content / (TRACKS_PER_CELL * GRID_CELL[bp]))
tracks = TRACKS_PER_CELL * clamp(cells, kMinTracks/2, kMaxTracks/2)
```

Two details do the work here:

- **The content box, not the panel.** Panel chrome takes a different bite out of each axis
  and out of each orientation — 480x272 insets to 430x264, but 272x480 insets to 264x394 —
  so dividing the panel extent sizes every track against a rectangle the grid never
  occupies, and delivers tracks 8-23% narrower than `GRID_CELL`. `PanelWidgetManager`
  measures the container (with an explicit `lv_obj_update_layout` first, because a
  freshly-created container reports a zero content box) and passes it in.
- **Nearest, not largest-that-fits.** Flooring discards up to a full cell and LVGL spreads
  the remainder across the tracks that survive, inflating every one of them — micro's 264px
  height is 3.88 cells, and taking 3 leaves 60px of a 68px cell to redistribute. Rounding
  keeps each track within half a cell of its target.

Because the count is always a whole number of cells, the track count is always even: no
trailing half-cell that a whole-cell widget could never occupy, and edit mode's `TRACKS_PER_CELL`
snap step can always restore a size the user dragged past.

`kMinTracks` (4) and `kMaxTracks` (64) are both whole cells, so clamping cannot produce an
odd count either. Neither is reached by any measured geometry; `kMinTracks` exists for a
container that has not been laid out yet.

The breakpoint comes from the **narrow** axis (`min(width, height)`), so a tall portrait
panel is classified by its width and keeps the same cell edge after rotation. See
`include/ui_breakpoint.h`.

**Gutter.** The gap between tracks is the per-breakpoint `home_grid_gap` token
(`ui_xml/globals.xml`, read by `GridLayout::gutter_px()`): the same as `space_xs` up to
medium (2/4/5px), wider from large up (10px at large and xlarge, 12px at xxlarge). It is grid
spacing, so a user's own layout gets it too. It does not change the track count, which comes
from the content box alone.

**Measured grids.** Content boxes read off a live instance
(`HELIX_SCREEN_SIZE=<WxH> helix-screen --test -vv`, then the `[PanelWidgetManager] Grid
layout:` / `Track geometry:` lines); pinned in `tests/unit/test_grid_square_cells.cpp`.

| Panel | Tier | Content box | Gutter | Grid | Track (w x h) | Aspect |
|-------|------|-------------|--------|------|---------------|--------|
| 480x272 | MICRO | 430x264 | 2 | **12x8** | 34.00 x 31.25 | 1.09 |
| 320x240 | MICRO | 270x232 | 2 | **8x6** | 32.00 x 37.00 | 0.86 |
| 272x480 | MICRO | 264x394 | 2 | **8x12** | 31.25 x 31.00 | 1.01 |
| 480x320 | TINY | 418x312 | 2 | **10x8** | 40.00 x 37.25 | 1.07 |
| 320x480 | TINY | 312x394 | 2 | **8x10** | 37.25 x 37.60 | 0.99 |
| 480x400 | SMALL | 414x388 | 4 | **10x10** | 37.80 x 35.20 | 1.07 |
| 800x480 | MEDIUM | 710x466 | 5 | **12x8** | 54.58 x 53.88 | 1.01 |
| 480x800 | MEDIUM | 466x664 | 5 | **8x12** | 53.88 x 50.75 | 1.06 |
| 1024x600 | LARGE | 904x584 | 10 | **16x10** | 47.12 x 49.40 | 0.95 |
| 1280x720 | XLARGE | 1128x700 | 10 | **16x10** | 61.12 x 61.00 | 1.00 |
| 1920x1080 | XXLARGE | 1720x1056 | 12 | **24x14** | 60.17 x 64.29 | 0.94 |
| 1920x440 | SMALL | 1832x428 | 4 | **46x10** | 35.91 x 39.20 | 0.92 |
| 320x1480 | TINY | 312x1332 | 2 | **8x34** | 37.25 x 37.24 | 1.00 |

**Rotation is a transpose to within one cell, not exactly.** The sizing rule transposes
exactly — feed it a transposed content box and you get a transposed grid. Real panels do
not, because the chrome is not symmetric: 1024x600 insets to 904x584 but 600x1024 insets to
584x**868**, and 868 falls just below the cell boundary 904 clears, giving 14 rows against
16 columns. Every measured rotation pair agrees within `TRACKS_PER_CELL`.

Tracks are equal `LV_GRID_FR(1)` entries (`make_col_dsc(ncols)` / `make_row_dsc(nrows)`), so
the track sizes above are what the fractions work out to after gutters, not fixed pixels.
`grid_cell_metrics(content_w, content_h, cols, rows, gutter)` is the single helper that
converts a content box into track pixels — the gutters sit *between* tracks and are not part
of any track, so dividing content by the track count overstates every one of them.

### `assets/config/default_layout.json`

The shipped default arrangement, resolved against what the connected printer can do.
The file is **two-dimensional** - layout variant, then breakpoint - and every span in it
is in **tracks**, not cells. The resolver is a pure function
(`resolve_default_layout()` in `include/default_layout_resolver.h`): a table, an optional preset
overlay, a tier and grid, and a `CapabilitySnapshot` of subject values go in; placements,
the ids to switch off and a signature come out. It reads no LVGL subject and no file, so
the same inputs always give the same layout and `tests/unit/test_default_layout_resolver.cpp`
drives it with hand-built tables.

The table has two kinds of entry in `anchors`.

**Fixed anchors** are `{"id", "placements"}`: one widget, one rectangle per tier.

```json
{ "id": "print_status",
  "config": { "layout_style": "detailed" },
  "placements": {
    "medium": { "col": 0, "row": 2, "colspan": 8, "rowspan": 4 }
  } }
```

**Slots** are a region per tier and an ordered list of alternatives that fill it. They
exist so a cell can hold whichever widget this printer can use:

```json
{ "slot": "spool_env_rl",
  "placements": { "medium": { "col": 4, "row": 2, "colspan": 4, "rowspan": 2 } },
  "alternatives": [
    { "cells": [ { "candidates": ["ams"], "at": [0, 0, 4, 2] } ] },
    { "cells": [ { "candidates": "ENV_FIL", "at": [2, 0, 2, 2] },
                 { "candidates": "ENV_FIL", "at": [0, 0, 2, 2] } ] }
  ] }
```

- `at` is `[col, row, colspan, rowspan]` in tracks, relative to the slot's region. An odd
  span is allowed only for a widget whose registry def has `supports_half_col` /
  `supports_half_row`.
- `candidates` is an inline array or the name of a top-level `candidate_lists` entry. A
  candidate is an id or `{"id", "requires": ["subject", ...]}`. The shipped table has one
  list, `ENV_FIL`: filament, led, dryer, chamber_temperature requiring `printer_is_enclosed` (a
  chamber sensor on an open frame reads the room), and active_spool requiring
  `printer_has_spoolman`. The bed mesh and the AMS are inline single-candidate lists, and so is
  the tool switcher, which requires `ams_is_tool_changer`: below large it is the first
  alternative of the spool slot, so a tool changer gets it in place of the AMS tile; from large
  up it has a `tool_strip` slot of its own, the 8x2-track strip in the upper half of the planned
  block, whose lower half stays the declared gap. A printer that is not a tool changer resolves
  exactly as it would with no switcher in the table (the "not a tool changer" case in
  `test_default_layout.cpp`), though its signature names `ams_is_tool_changer=0`.
- **A candidate qualifies** when its registry gate subject is open, every subject in its
  `requires` is non-zero, and no earlier cell placed it. A gate subject that is not registered
  reads open; a `requires` subject that is not registered reads 0.
- **Resolution order:** fixed anchors first, then slots in file order. Within a slot, the first
  alternative that fits the region and whose lead cell (its first cell) has a qualifying
  candidate wins; the last alternative has no lead requirement. Each cell takes its first
  qualifying candidate, so cells that share a list fill in the order they are written, across
  slots too, and the cells left over stay empty. Authoring order is how a table decides where
  a printer with fewer readouts shows its empty cells: the cell beside a declared gap or at an
  edge is written last. With a measured grid, an alternative whose cells run off it gives way
  to a later one that stays on it.
- **No stand-ins:** every candidate is gated on hardware or a capability. A printer without
  the hardware gets an empty cell, never a generic widget (clock, network, macros, motion,
  z-offset and the like) filling space, and never a dimmed gated tile. A fixed anchor is
  either an ungated widget or one whose gate is open on every printer that reaches that tier;
  anything else is a slot.
- **Planned gaps:** a table's `gaps` map lists, per placement key, the `[col, row, colspan,
  rowspan]` blocks left empty on purpose. The resolver does not read it; the coverage test
  does (below). Gaps are whole cells, grouped into one calm block where the tier allows, and a
  neighbour is never grown into one. The card behind merging tiles covers the bounding box of
  each connected group, so a gap that sits inside such a box is painted as card: keep a
  tier's planned gap outside its readout band (large keeps one band of readouts on top for
  that reason).
- **Losers are switched off:** every widget a candidate list of the active table names and the
  resolver did not place comes back in `ResolvedDefault::disabled` and is written
  `enabled: false`, so auto-placement never tries to seat it into a full grid. That includes the
  candidates of a slot with no region at this tier.
- `placements` keys inherit through the tier fallback chain (see below), so a slot's last
  alternative has to fit every region it can inherit; the table test enforces it.

`default_layout.json` also carries tier `disabled` maps, `config` seeds and variants.

```json
{
  "disabled": {
    "micro": ["tips"],
    "medium": ["tips"]
  },
  "gaps": {
    "large": [[4, 2, 4, 2], [8, 2, 8, 4]]
  },
  "variants": {
    "portrait": {
      "anchors": [ ... ],
      "disabled": { "tiny": ["tips"] }
    }
  }
}
```

A variant is either a bare anchor array or an object carrying `anchors`, `disabled` and `gaps`; the
resolver uses the first variant in `LayoutManager::variant_chain()` with a non-empty `anchors`
and falls back to the top-level table. A `disabled` map takes grid-qualified keys
(`"micro@8x6"`) as well as tier names; see [Grid-qualified keys](#grid-qualified-keys).

**`disabled` is not optional bookkeeping.** Omitting a widget from a tier does *not* switch
it off - `parse_widget_array()` appends every absent registry widget at its
`default_enabled` and the engine seats it wherever it fits. Worse, an omitted widget
inherits the first key in its tier's fallback chain that *does* have a placement, lands off
a grid of a different size, collides, and is evicted as "grid full" - taking whatever would
have auto-placed after it. Leaving a widget out of a tier therefore **requires** naming it
in that table's `disabled` map, unless a candidate list names it (the resolver switches
losers off itself). The guard is the "no shipped table mixes authored and inherited
placements" case in `test_default_layout.cpp`.

**`config` on an anchor** seeds that widget's per-instance settings, so a layout can ship
`print_status` in its Detailed style without a C++ branch. Anchor-level config is merged
*under* placement-level config.

**No default places `toolhead`, `host_health` or `exclude_map`.** They stay in the catalog.

`PanelWidgetConfig::build_default_grid()` (`src/system/panel_widget_config.cpp`) is snapshot
plus resolve: `snapshot_capabilities()` reads every gate and `requires` subject the table names
from the live XML subject registry, `resolve_default_layout()` does the rest. The shared parse of
the file is kept by `load_default_table()` until its mtime or size changes, so the
runtime-editable table still takes effect without a per-call read.

The three shipped tables are the base (landscape), `variants.ultrawide` (1480x320,
1920x440) and `variants.portrait`. The base and portrait tables author all seven tiers; see
the fallback warning above for why leaving one out is dangerous rather than merely terse.
`variants.ultrawide` authors `tiny` and `small` only, the tiers an ultrawide strip reaches
(`micro` inherits `tiny`). The xxlarge landscape table is authored on the 12x7-cell grid
(portrait xxlarge is 14 tracks wide). Four grids carry grid-qualified tables of their own:
the 320x240 micro panel (`micro@8x6`), the two ultratall portrait strips, 320x1480
(`tiny@8x34`) and 440x1920 (`small@10x44`), and the 1080x2400 portrait panel
(`xxlarge@14x32`), whose eight rows past the 14x24 table hold a taller temperature graph and
a declared empty block.

What every table shares: notifications in the top-right corner; temperatures, fans and the
spool slot (the AMS tile, or a pair of `ENV_FIL` readouts) beside the printer image;
print_status always placed. Micro (480x272) puts print_status (4x2 cells) beside the
temperature graph (2x2); medium keeps the graph and leaves tips off; the ultrawide strips
are two bands, the image, an empty span and the readout cluster on top, print_status and
the graph sharing the bottom half and half.

Inside a table, breakpoint keys are named rather than indexed - `micro`, `tiny`, `small`,
`medium`, `large`, `xlarge`, `xxlarge` (seven, matching `UiBreakpoint`). A missing tier
resolves by fallback (`micro`->`tiny`->`small`, `xlarge`->`large`, `xxlarge`->`xlarge`->`large`).
If no tier in the chain matches, the anchor is dropped and that widget is auto-placed instead.

#### A default stays live until the user edits it

The layout stores a `default` block beside its pages, inside the `serialize_pages()` payload,
so a parked per-grid copy carries its own:

```json
"default": { "signature": "<tier>|<cols>|<rows>|<variant chain>|<preset>|<subject>=<value>,...", "hash": "<arrangement hash>" }
```

- `signature` is what the resolver consulted: tier, grid, variant chain, preset and every
  consulted subject's value, sorted.
- `hash` is `helix::layout::arrangement_hash()` of the pages: per page, the id, column, row and
  spans of each enabled, positioned entry, plus the page count. Per-widget `config` is not in
  it, so choosing an LED strip or a print_status style does not make a layout the user's.
- **Unedited** means the stored pages still hash to `default.hash`
  (`PanelWidgetConfig::is_unedited_default()`). No write path needs to know about this: Edit
  Mode, the catalog, `ctl home`, plugin RPCs, page add and remove and an older build that keeps
  unknown keys all change the arrangement, and any of them makes the layout the user's.
- **When it re-resolves:** at the top of `PanelWidgetManager::populate_widgets`
  (`refresh_live_default`), before enabled ids are collected and before the "Widget list
  unchanged" early return. Only while the printer is connected and Klippy is READY (a teardown
  or retarget zeroes subjects such as `ams_slot_count` and must not reshuffle the panel), only
  for an unedited default, only when the signature differs, and not while home Edit Mode is open
  (`PanelWidgetConfig::set_edit_session_open`). The signature is compared before any entries
  are built. A changed signature saves once; an unchanged one writes nothing.
- **Offline** (not CONNECTED): a pending block, or an unedited default whose grid changed, is
  resolved in memory so the panel has something to show, but the block stays pending (empty
  signature) and nothing is saved, since the subjects may still hold a previous printer's
  capabilities. A default that has already resolved is left as it is. The first READY populate
  resolves the pending block from the printer's own values and writes once. While Klippy is
  starting on a connected printer nothing resolves at all. The gate observers `setup_gate_observers()` registers cover every `requires`
  subject the table names as well as the registry gates, so a capability arriving late (an AMS
  backend coming up after connect) re-resolves the panel.
- A grid change (rotation, UI scale) on an unedited layout re-resolves for the new grid instead
  of parking and porting. During an open Edit Mode session it is ported so the session keeps
  its arrangement, but its block stays pending and resolves when the session closes. A parked
  unedited default that a grid change restores is re-resolved before it is shown.
- A re-resolve carries each surviving widget's `config` across, merged over the table's
  `config` for the new placement, so a key the widget never set keeps the table's value.
- Reset to defaults (`reset_to_defaults()`) stamps a pending block, which the next measured
  populate resolves. Crash-loop safe mode shows defaults and writes nothing.

#### Preset overlays

A preset can adjust the table without forking it: `assets/config/panel_widgets/<preset>/home.json`
is an overlay `load_preset_overlay()` reads (cached by mtime and size, like the table) and
`build_default_grid()` applies over the table. It replaces a base anchor or slot by `id` /
`slot`, adds ones the table lacks, or removes one with `{"id": "tips", "remove": true}`; the
same resolver handles it and the preset is part of the signature. No preset ships an overlay today: the base tables cover every packaged printer.

#### Grid-qualified keys

A breakpoint names a *panel*, not a *grid*. The high-DPI UI scale multiplies the cell edge
(see `include/display_metrics.h`), so one panel at one tier has a different track count per
scale — 1080x2400 is xxlarge portrait at **14x32** tracks unscaled and fewer at each larger
scale. A table authored for one of those does not describe the others.

A placement key may therefore name its grid: `"xxlarge@6x14"`. Within a tier the qualified
key wins over the bare one, and the tier chain still dominates — this tier on some other
grid beats a coarser tier on this exact grid, because the tier is what decides how much text
has to fit. A bare key remains the catch-all for every grid with no entry of its own.

```json
"placements": {
  "xxlarge":      { "col": 8, "row": 0, "colspan": 2, "rowspan": 2 },
  "xxlarge@6x14": { "col": 4, "row": 0, "colspan": 2, "rowspan": 2 }
}
```

`disabled` maps take the same keys. A grid-qualified list replaces the bare tier's list on
that grid, so a grid too small for some of the tier's anchors switches them off there and
nowhere else. The Creality Hi's 320x240 panel is micro on an 8x6 grid: `"micro@8x6"`
switches off printer_image and fan_stack so print_status can be two cells tall (one cell has
no room for its progress ring) under a top row of nozzle, bed, one `ENV_FIL` readout and
notifications in the corner; 480x272 keeps both. Every entry with a bare key for the tier
needs a grid-qualified placement or a place in that list, or it inherits the bare key's
coordinates off the smaller grid. The "every measured grid is covered or declared empty" case
in `test_default_layout.cpp` resolves each measured grid with every gate open, every gate
open but no AMS, and every gate closed. With gates open the empty tracks must be exactly the
grid's declared `gaps`; with gates closed an empty track must be a declared gap or a slot cell
nothing qualified for. An undeclared hole fails, and so does an overlap.

**An anchor that does not fit the measured grid is now dropped rather than clamped.**
`build_default_grid(grid_cols, grid_rows)` checks each anchor against the grid it is being
placed on and auto-places the ones that do not fit, naming them in a warning. Widgets the
tier switches off are exempt — whether a disabled widget's anchor fits is not a fact about
anything.

#### One saved layout per grid

A saved layout is coordinates in **tracks**, and a track means nothing without the grid it
counts against. That grid is no longer a fixed property of the device: the UI scale
multiplies the cell edge, so the same panel yields a different track count per scale, and
restoring a config onto other hardware moves it too.

Rewriting a single stored layout on each grid change destroyed the arrangement. The
write-back at the end of `populate_widgets()` persists computed positions, so the first
populate on a new grid replaced the user's coordinates with that grid's clamped and
auto-placed fallback, and switching back had nothing left to restore. Spans were already
protected from precisely this (#1216, *"a property of the current screen, not of the user's
layout"*); positions were not.

So each grid keeps its own arrangement:

```json
"panel_widgets": { "home": {
  "pages": [ ... ],          // the ACTIVE grid's layout, shape unchanged
  "grid": "6x14",            // which grid those tracks count against
  "parked_grids": {          // arrangements for grids that are not active
    "12x24": { "pages": [ ... ], "main_page_index": 0, "next_page_id": 1 }
  }
}}
```

The active layout stays exactly where it always was, so every existing reader is untouched,
and both new keys are omitted while empty — a single-grid config, which is every printer,
writes byte-identical JSON to what it wrote before.

`PanelWidgetConfig::switch_to_grid(cols, rows)` parks the outgoing arrangement, then either
restores this grid's saved one or seeds it by remapping the outgoing one through
`port_legacy_layout()` (`include/layout_port.h`) — the same remapper the pre-v22 port uses.
Seeding from the layout being left, rather than from the shipped defaults, is deliberate: it
is the arrangement the user was last looking at, so it is the closest thing to their intent
that exists. A layout with no recorded grid is **stamped and otherwise left alone**; which
grid it was arranged on is unrecoverable, and reseating a real arrangement on a guess is the
failure this exists to prevent.

Neither this nor the pending-anchor pass runs while Klipper is not READY. A transient
`firmware_restart` widget occupies a cell then, so the arrangement is not the user's, and
both passes persist — freezing either from a transient layout is the mistake the write-back
already refuses to make.

This is also why defaults are no longer built at config load. `load()` runs before the
widget container exists, and the panel extent it could have guessed from is not what the
track count divides — the *content box* is, and the two disagree enough to pick a different
grid (1042x2141 against 1080x2400 is 6x14 tracks against 8x16). So `build_defaults()` tags
the layout `"anchors": "pending"`, which reads as a `default` block with an empty signature,
and `PanelWidgetManager::populate_widgets()` resolves it at the first measured populate. The
tag is positive and written only by the defaults path, so a layout a user has arranged is
never overwritten.

The file is runtime-editable and read via `find_readable()`, so a malformed or missing file
degrades to a small hardcoded fallback rather than an empty dashboard.

**A bad hand-edit fails the build.** `tests/unit/test_default_layout.cpp` parses the
*shipped* file and runs every table through one `check_anchor_table()` helper carrying a
per-tier `{cols, rows}` track budget. It checks **both** axes, not just the column one, plus
pairwise overlap — an earlier column-only version is precisely why a portrait
`print_status` shipped running two rows off the bottom of a 320x480 grid. It also rejects a
table that mixes authored and inherited placements for the same widget.

Running off an axis is not cosmetic. `panel_widget_manager` clamps the span, pushes the
origin back to fit, that lands on top of the neighbour the widget was authored beside,
`grid.place()` fails, and the widget silently auto-places somewhere else at its registry
span — so the anchor becomes decoration and the tier you carefully authored is not what
ships.

**The tables are generated, not hand-written.** They are authored in *cells*, converted to
tracks, and machine-checked for tiling, registry maxima, the even-span invariant and
overlap before being written out. Reproduce that pass rather than editing spans in place:
an edit that keeps the test green can still leave a tier 40% empty, and the test cannot see
that. Judge the result by rendering the tier, never by the arithmetic — every real defect
in the landscape rework (a clipped card, a nested card, a missing background, a wrapped
axis, a track count that was 24x14 where the arithmetic said 26x16) was found in a
screenshot while the tiling validator reported 100%.

### Widget span authoring

`PanelWidgetDef` (`include/panel_widget_registry.h`, table in
`src/ui/home/panel_widget_registry.cpp`) carries six span fields, **all in tracks** — so the
smallest whole-cell widget is `2`, not `1`:

| Field | Meaning |
|-------|---------|
| `colspan` / `rowspan` | **Authored default** — the size the widget was designed at |
| `min_colspan` / `min_rowspan` | Smallest size the widget is still usable at (0 = fall back to the authored span) |
| `max_colspan` / `max_rowspan` | Largest size the user may resize it to (0 = not scalable) |

Plus three flags that decide how the widget sits in the grid rather than how big it is:

| Flag | Meaning |
|------|---------|
| `supports_half_col` / `supports_half_row` | May be placed and sized at an *odd* track count. Everything else snaps to even boundaries, so a whole-cell widget can never straddle two cells. |
| `merges_into_card` | Wants the shared fused card background drawn behind it. |

**Odd spans are illegal unless the widget opts in.** `snap_step_for()` steps a whole-cell
widget by `TRACKS_PER_CELL` and the edit-mode lattice only draws targets there, so an odd
authored span is a size the user can never restore after one drag. A registry-wide test
asserts this.

**`snap_step_for()` is the single source for that rule, and every placement path reads it.**
Edit mode was once the only caller, which left three ways to seat a whole-cell widget off a
cell boundary (#1126): `find_available()` / `find_available_bottom()` walked one track at a
time, so an odd-aligned gap left by a half-cell neighbour was a valid answer; `grow_once()`
grew one track at a time, so a widget expanding into a two-track gap could stop halfway; and
`clamp_to_grid()` honoured whatever origin the saved layout held. All four now take a
per-axis step, defaulted to `TRACKS_PER_CELL` so a caller that has not thought about it
cannot opt into the permissive behaviour by omission. A whole-cell widget with only an
odd-aligned gap left is reported as `GridFull` and disabled with a toast, which is the honest
answer — there is no position in that grid it is allowed to occupy.
`tests/unit/test_grid_half_cell_placement.cpp` covers the search, the growth and the load
path.

**Which widgets opt in.** Set the flag on an axis when half a cell of extra room buys
something there. Two shapes buy something. The content is *continuous* along the axis - a
chart, an aspect-fit frame, wrapping text, a scrolling strip, stacked readout rows, or a
layout picked by measurement (`active_spool`'s compact/wide switch,
`decide_nozzle_layout()`): half a cell shows more content. Or the widget is a centred-icon
tile whose glyph scales with its box (`decide_tile_layout()`,
`src/ui/panel_widgets/tile_layout.h`): half a cell is a larger icon rung. The
centred-icon tiles - the three heater tiles, `network`, `led`, `filament`, `fan`,
`thermistor`, `bypass`, `notifications`, `macros`, `motion`, `gcode_console`,
`power_device`, `favorite_macro` and the four action tiles - carry both flags
(prestonbrown/helixscreen#1559). Leave it off for a widget authored around a fixed number
of cells - `humidity`, `width_sensor`, `control_buttons` - where the intermediate size
buys whitespace and nothing else, and costs a drag snap twice as fussy on a 34px track.

**A tile's floor is half a cell on both axes.** A tile that measures itself can be smaller
than a cell, because it can refuse a box it cannot draw. It says so through
`PanelWidget::fits_at()`, and `helix::grow_span_to_fit()` (`include/grid_layout.h`) lifts
the span back above the floor - the same refusal path the resize clamp and the anchored
load path both take, so those two cannot disagree. A box too short to stack the glyph over
its reading and label lays them side by side instead (`decide_tile_layout()` answers
`TileDirection::Row`, and the tile binds `styles.tile_row`), so a whole cell wide by half a
cell tall is a real size, not a clipped one. Auto-placement seats a widget at its authored
default without asking, which is safe only because every definition holds that default at
or above its own minimum. A widget authored around whole cells keeps a whole-cell floor on
both axes.

At the micro and tiny tiers, where a track is 31-40px, a tile that shows a reading declines
half a cell outright: no rung carries a glyph and a value there. An icon-only tile is left to
`decide()`, which drops the caption and measures its own glyph against the box, so most take
half a cell on both axes and a tile whose caption is its identity (which fan, which sensor)
still refuses. A tile of one cell or less is capped to the authored face
there so those screens keep the proportions they were designed with. A tile given more
than a cell on both axes grows on every tier. A glyph that sits in a disc
(`power_device`, `favorite_macro`) uses `ui_xml/components/tile_badge.xml`, whose disc
follows the same rung as the glyph, and TileSizing measures the disc's edge
(`Content::icon_box`), so a badged tile scales like any other.

`tests/unit/test_grid_layout.cpp` classifies every registry id and pins the rule that a
sub-cell floor belongs only to a widget that can decline one;
`tests/unit/test_widget_content_fits.cpp` sweeps every tile at every shipping geometry, at
the smallest span it is actually offered rather than its registry floor. A new widget
cannot be added without deciding this.

**Almost no widget paints its own background.** Most home widgets extend `lv_obj`, which
inherits the fully transparent `StyleRole::ObjBase`, so a widget excluded from the card
merge renders on the bare panel. `merges_into_card` defaults to true; the six that turn it
off — `printer_image`, `print_status`, `nozzle_temps`, `temp_graph`, `tips`, `camera` —
either bring their own surface or are large enough that being folded into a neighbour's
card looks wrong. `ams` stays a participant but drops its own inner card when the host is
already painting one (`card="false"`), because a card inside a card reads as a box in a box.

The merge itself is a BFS flood fill over adjacent participants, decomposed into maximal
rectangles so the result is always rectangular rather than a stepped L. It runs in **track**
coordinates, the same units the grid is addressed in, so a widget on an odd track or with an
odd span is backed like any other. It used to work in cells and convert back, which truncated
such a position - so every half-cell placement was excluded from the merge outright and
rendered on the bare panel with no background at all.

Auto-placement (`PanelWidgetManager`, `src/ui/home/panel_widget_manager.cpp`) runs two passes:

1. **Authored** — every widget asks for its `colspan` x `rowspan`. If everyone fits, this
   is the layout the dashboard was designed around, and a roomy grid ends up exactly where
   it always did.
2. **Minimum-first** — used only when pass 1 cannot seat everyone. Every widget is placed at
   `effective_min_colspan()` x `effective_min_rowspan()`, maximising how many widgets
   survive, and the leftover cells are handed back out by `GridLayout::grow_to_targets()`,
   which grows each widget one step at a time, round-robin, toward its authored span.

**This makes `min_colspan` the field that decides whether your widget survives on a narrow
grid.** A portrait grid can be 2 columns wide. A widget that leaves `min_colspan` at 0 is
declaring that its authored span is also its minimum, so a 4-wide widget on a 2-wide grid
does not fit *at any size* - the manager classifies that as `TooLargeForGrid`, gives up,
disables it, and persists that disable to `settings.json`. That was exactly #1216: the
widget did not come back on its own, and the user had to re-add it from the catalog by
hand. (A widget that would fit but finds every cell taken is the *other* failure and is
handled differently - see below.)

So when adding a home widget:

- Set `min_colspan` / `min_rowspan` to the smallest layout your XML actually degrades to.
- Set `max_colspan` / `max_rowspan` if the widget can usefully grow; leaving them at 0 marks
  it fixed-size and `is_scalable()` returns false.
- If the widget genuinely cannot render below N tracks, say so — being dropped on a narrow
  grid is then correct behaviour, not a bug. `tips` is the honest example: authored 8 tracks
  wide with a 4-track minimum, and switched off through the `disabled` map on the tiers
  where even that minimum costs a third to a half of a row for rotating hints.
- If the widget can draw at some sizes and not others, override `PanelWidget::fits_at()`,
  monotonically: fits at a size means fits at every larger size. Edit mode's resize clamp
  and the load path both honour it through `helix::grow_span_to_fit()`
  (`include/grid_layout.h`), with the registry min/max as the walk's bounds.
- Mark every non-scrolling container in the widget's XML `scrollable="false"`. A tile is
  scrolled by dragging it, not by a chevron gutter, so `PageScrollAutoInject` stops its walk
  at the tile root (`src/ui/home/page_scroll_auto_inject.cpp#walk_and_attach`) - but a scrollable container
  inside a tile still absorbs the drags the grid wants, and LVGL's scrollable default is ON
  unless you say otherwise.

A widget's *layout* generally keys off delivered pixels rather than span, through the bands
in `include/panel_widget_size.h` (`w_normal()`, `w_wide()`, `h_tall()`, `h_taller()`). Those
are per-tier ladders scaled by `font_body`, not flat constants: the same 2-cell widget is a
different number of pixels on Micro than on XXLarge, and a threshold that ignores the tier
either starves the small panels or refuses to use the large ones.

### The two placement failures have different outcomes

`GridLayout::PlacementFailure` distinguishes `GridFull` from `TooLargeForGrid`, and
`PanelWidgetManager::populate_widgets()` treats them differently on purpose. They are not
two spellings of "did not fit".

| Failure | What it means | What gets persisted | Toast |
|---------|---------------|---------------------|-------|
| `TooLargeForGrid` | The widget exceeds the whole grid even at its declared minimum span. No arrangement of the other widgets could ever seat it | `enabled = false` - back to the catalog as an available widget | Always |
| `GridFull` | The widget fits fine; this screen's cells are simply all taken | `col = -1`, `row = -1`, `enabled` untouched | Only when the widget actually **was** on screen and lost its cell |

**`GridFull` must never write `enabled = false`.** The layout is stored once per printer
(`/printers/<id>/panel_widgets/<panel>`) with **no breakpoint key**, so a disable forced by
one screen's occupancy removes the widget at *every* size - the same mistake the span
write-back already refuses to make (#1216). It was not even deterministic: the disable only
reached disk if some unrelated `save()` happened to follow, so whether the user permanently
lost a widget depended on what they did next.

Clearing the position is the honest record instead. The widget is configured, it just has
nowhere to go right now, so it re-places itself the moment a cell frees - remove another
widget, close a hardware gate, or lay the same config out on a taller grid. The cleared
position doubles as the memo that stops the nagging: a widget with **no** saved position was
never on the user's screen, so announcing a removal for it would be false. Bundle XGVDYEB5
is the case - a 6x4 grid with ten widgets filling all 24 cells toasted
*"'Fan Speeds' removed — grid full"* on every single launch, because the in-memory disable
never reached disk and the next launch re-ran the same failed placement.

An eviction is its own reason to `save()`. It changes nothing about the widgets that *were*
placed, so the manager's `any_written` flag stays false and the cleared position would
otherwise never reach disk.

**The catalog must ask `is_placed()`, not `is_enabled()`.** An enabled widget at `(-1,-1)` is
on no dashboard, and the Widget Catalog is the only surface that can hand it a cell back.
Asking `PanelWidgetConfig::is_enabled()` dimmed it as *"Placed"* and stripped its click
handler, leaving it invisible on the grid, unselectable in edit mode (`remove_selected_widget`
needs an on-screen object) and unreachable in the catalog - no UI surface at all. That state
is not hypothetical: `include/panel_widget_config.h` carries a one-shot migration
(`migrate_stuck_ams_filament_swap`) written to rescue installs already stuck in it.
`is_placed()` is `enabled && has_grid_position()`; `is_enabled()` keeps meaning "configured
on" for everyone else. The multi-instance `(N Placed)` count needs the same guard.

Note the asymmetry that made this easy to miss: `is_enabled()` was the *only* occupancy-blind
consumer of `enabled` in the widget system. Every other site already pairs it with
`has_grid_position()`, and all of those are occupancy-map builders where skipping a `(-1,-1)`
entry is correct.

A clickable row routes into `GridEditMode::place_widget_from_catalog()`, which tries the
origin cell, then `find_available`, then shrinks toward `effective_min_colspan/rowspan`, and
only then refuses with *"Not enough room for this widget."* Because the catalog offers a
widget that holds no cell on **any** page, that function has to **move** an entry it finds on
another page rather than push a second one with the same ID - two entries would render the
widget on two pages, and `delete_entry()` only ever removes the first. Per-widget `config`
travels with the entry, since those settings belong to the widget and not to the page it sat
on.

A third case is neither: when placement failed only because the temporary `firmware_restart`
widget was injected, the widget is skipped and logged, with nothing written at all.

## Layouts shared by the printer's own screen

A printer's own HelixScreen (the one whose Moonraker runs on its host) publishes its home
layout to Moonraker. A screen that reaches that printer over the network seeds its own home
layout from it, and the widget catalog offers "Copy layout from printer" to pull it again.
There is no live sync: after a copy the layout belongs to the screen that took it.

**Payload.** Moonraker database namespace `helix-screen`, key `home_layout`
(`include/home_layout_share.h#kNamespace`, `kKey`). The value is
`{schema, grid: "<cols>x<rows>", pages, main_page_index, saved_at, app_version}`, built by
`build_payload` (`include/home_layout_share.h`) from `serialize_pages()` with
the `default` block and `next_page_id` left out. `parked_grids` and anchors are not
published. `parse_payload` accepts only the schema it knows (`kSchema`), a grid of
1 to 64 columns and rows, and at least one page that keeps an entry; anything else is
treated as "no published layout". `app_version` is informational and never a gate. Unknown
widget ids and unknown per-widget config keys are tolerated the way `settings.json` is.

**Which widgets travel.** Widgets are printer-scoped or screen-scoped
(`include/panel_widget_registry.h#WidgetScope`). Screen-scoped entries
(`network`, `clock`, `lock`, `tips`, `notifications`, `shutdown`) describe the device they
sit on, so `strip_screen_scoped` removes them from a payload before import, drops the pages
that leaves empty and re-points `main_page_index`. The importing screen keeps its own
defaults for those widgets.

**Who publishes, and when.** `include/home_layout_sync.h#HomeLayoutSync` publishes only
when the connection's locality verdict (the `moonraker_locality` subject,
`include/printer_network_state.h`) is `Local`. `Unknown`, which is what a connection reads
before its verdict is set, publishes nothing. It publishes once per connect and after each
home-layout save, skips a payload identical to the last one posted on the connection
(`saved_at` aside), and never publishes an unedited default.

**Who fetches, and when it seeds.** A screen whose locality is `Remote` fetches on connect and
holds the result tagged with the active printer id. Automatic seeding only happens for a
printer that has never had a saved home layout
(`include/panel_widget_config.h#had_saved_layout`): it is derived from the
printer's settings at load, never persisted, and `mark_printer_changed` re-derives it on a
printer switch. A Reset counts as someone having had a say, so after a Reset the layout stays
the generator default. The seed is taken at the points where a default is resolved:
`HomeLayoutSync::seed_pending_default` when populate applies a pending default, and
`HomeLayoutSync::adopt_late_seed` for a reply that arrives after the default was applied but
while the layout is still an unedited default on a known grid. Both skip while edit mode is
open. No published layout, an unreadable payload or a newer schema falls back to the
generator default without a message; a successful seed toasts.

**`import_layout`.** `include/panel_widget_config.h#import_layout` takes the
parsed payload and the importing screen's grid. The payload is checked in full first, so a
refused import (non-positive grid axis, or no page keeping a known widget) changes and saves
nothing. On success the layout is loaded as authored for its source grid and ported to the
local grid by the same path a first visit to a new grid uses
(`src/ui/home/layout_port.cpp#port_legacy_layout`). Every parked grid is discarded, since each is
a port of the layout being replaced. The result is the user's own layout with no default
marker, saved once.

**After the seed.** The copied layout is owned by the screen that took it. Edits on either
side never propagate, a later publish never replaces it automatically, and Reset in the
catalog gives the generator default, not the printer's layout.

**Manual copy.** `src/ui/home/ui_widget_catalog_overlay.cpp#on_catalog_copy_layout` fetches through
`HomeLayoutSync::fetch_now`, asks for confirmation, and replaces the current layout whether
or not it was edited. It is hidden on the printer's own screen. To try it under `--test`, see
`HELIX_MOCK_HOME_LAYOUT` in `MOCK_ENVIRONMENT_VARIABLES.md`.

---

## Technical Reference

This section is for developers working on the layout infrastructure itself (C++ code).

### LayoutManager API

```cpp
class LayoutManager {
public:
    static LayoutManager& instance();
    void init(int width, int height);
    void set_override(const std::string& name);

    LayoutType type() const;             // Enum value
    const std::string& name() const;     // "standard", "ultrawide", etc.
    bool is_standard() const;
    bool has_override(const std::string& filename) const;
    int width() const;
    int height() const;

    // Returns the first matching "ui_xml/<variant>/filename.xml" along the
    // variant chain, otherwise "ui_xml/filename.xml"
    std::string resolve_xml_path(const std::string& filename) const;

private:
    LayoutType detect(int width, int height) const;      // aspect-ratio → LayoutType
    std::vector<std::string> variant_chain() const;      // ordered override search
};
```

`resolve_xml_path()` / `has_override()` walk `variant_chain()` — an ordered list of
override directories, most specific first, with base `ui_xml/` as the final fallback.
For portrait sub-classes the chain layers the shared `portrait/` dir before base:

```cpp
// LayoutManager::variant_chain(), src/layout_manager.cpp
MICRO_PORTRAIT → {"micro_portrait", "portrait"}
TINY_PORTRAIT  → {"tiny_portrait", "portrait"}
PORTRAIT       → {"portrait"}
ULTRAWIDE      → {"ultrawide"}
MICRO          → {"micro"}
TINY           → {"tiny"}
STANDARD       → {}   // base ui_xml/ only
```

### How XML Registration Works

Components register on first use. When the engine cannot find a component name, the loader installed by `helix::register_xml_on_first_use()` (`src/xml_registration.cpp`) registers `ui_xml/<name>.xml`, or else `ui_xml/components/<name>.xml`. The eager `register_xml()` helper (used for `styles.xml`) and the loader both resolve paths through the LayoutManager, so layout overrides apply identically:

```cpp
static bool register_xml(const char* filename) {
    auto& lm = helix::LayoutManager::instance();
    std::string path = "A:" + lm.resolve_xml_path(filename);
    return lv_xml_register_component_from_file(path.c_str()) == LV_RESULT_OK;
}

// Either path picks portrait/home_panel.xml when the portrait variant is active:
register_xml("styles.xml");              // eager
lv_xml_create(parent, "home_panel", nullptr);  // first use -> loader
```

A component's layout variant and the px tokens its `<styles>` use are fixed when it registers. On a runtime resize (`Application`'s resize callback), `helix::unregister_idle_xml_components()` drops every first-use component no live widget is built from, so its next use registers it at the new geometry. Components with live instances, a C++-registered subject or a style another scope borrowed keep their registration, and so keep the old geometry until they are rebuilt.

### Config

```json
// settings.json
{
  "display": {
    "layout": "auto"
  }
}
```

Valid values: `"auto"` (default), `"standard"`, `"ultrawide"`, `"portrait"`, `"micro"`,
`"micro_portrait"`, `"tiny"`, `"tiny_portrait"`.

CLI flag `--layout <name>` overrides the config file.

### Auto-Detection Logic

```
ratio = width / height

if (max dimension ≤ 480 and min dimension ≤ 272)
    → micro (landscape) or micro_portrait (portrait)
else if (max dimension ≤ 480)
    → tiny (landscape) or tiny_portrait (portrait)
else if (ratio > 2.5)
    → ultrawide
else if (ratio < 0.8)
    → portrait
else
    → standard
```

### Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Naming | "Layouts" | Distinct from "themes" (colors) and "profiles" (print start) |
| Detection | Auto with override | Users shouldn't need to configure, but can |
| Fallback | Per-file to standard | New layouts start empty, override incrementally |
| globals.xml | Never overridden; new responsive tokens still go there | Design tokens are universal across all layouts, and discovery only reads the top level of `ui_xml/` |
| Runtime layout-variant switching | Not supported | Would require full widget tree rebuild; startup-only is fine |
| Runtime breakpoint/token refresh | Supported on resize | `theme_manager_refresh_layout_constants()` re-registers tokens and moves the `ui_breakpoint` subject; driven by the resize callback in `src/application/application.cpp` (desktop SDL, Android fold/unfold). On-device rotation is fixed at startup, so nothing fires there |
