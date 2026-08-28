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
| `portrait` | **Started** | `app_layout.xml`, `navigation_bar.xml`, `print_status_panel.xml`, `print_tune_panel.xml` |
| `micro` | **Started** | `controls_panel.xml`, `header_bar.xml`, `theme_editor_overlay.xml`, `theme_preview_overlay.xml` |
| `micro_portrait` | Not started | Directory exists (empty) |
| `tiny` | Not started | Directory doesn't exist yet |
| `tiny_portrait` | Not started | Directory doesn't exist yet |

**A whole-file override is not the only way to adapt a panel, and increasingly not the
preferred one.** Forking a panel into `ui_xml/portrait/` duplicates everything that did not
need to change, and both copies then have to be maintained. Where the difference is a row
that becomes a column, or a block that is dropped on a short screen, the panel stays a
single file and branches in place on the `ui_is_portrait` subject:

| Mechanism | Use when | Examples in the tree |
|-----------|----------|----------------------|
| `ui_xml/<variant>/<panel>.xml` | The panel is genuinely a different design in that orientation | `portrait/print_status_panel.xml`, `portrait/print_tune_panel.xml` |
| `<if cond="ui_is_portrait eq 1">…<else/>…</if>` | An entire subtree differs, and building both would be wasteful | `motion_panel.xml:45`, `bed_mesh_panel.xml:99`, `temp_graph_overlay.xml` (3 sites) |
| `<bind_style_if cond="ui_is_portrait"/>` | Only the styling differs — flex direction, padding, button shape | `advanced_panel.xml:264-290` (the E-stop bar goes column instead of row) |
| `<bind_flag_if_eq subject="ui_is_portrait"/>` | Both variants are cheap to build and you want to show one | `components/bed_mesh_current_mesh_card.xml:106-131` |

Orientation is also composable with the breakpoint rather than separate from it:
`bed_mesh_current_mesh_card.xml:33` hides a block on
`ui_breakpoint eq 0 or (ui_is_portrait eq 1 and ui_breakpoint_v lt 5)` — the portrait clause
is guarded because a 1024x600 landscape panel and a 480x640 portrait one land on the same
breakpoint while only the portrait one is actually short of room.

So "which panels adapt to portrait" is not answered by listing the override directory.
Beyond the four files there, Motion, Bed Mesh, the temperature graph overlay, the Advanced
panel's E-stop bar and the bed mesh cards all adapt from inside their shared file.

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

  portrait/                ← Portrait overrides (app_layout.xml, navigation_bar.xml)
  micro/                   ← Micro landscape overrides (480x272, e.g. Ender 3 V3 KE)
  micro_portrait/          ← Micro portrait overrides (dir exists, empty)

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
| `panel_widget_area` | Plugin injection point (plugins add widgets here) |

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
- Navigation bar probably needs to move to the bottom (override `navigation_bar.xml`)
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
| High | `app_layout.xml` | The overall chrome (navbar position, content area) |
| High | `navigation_bar.xml` | Nav position/orientation differs per layout |
| Medium | `controls_panel.xml` | Common panel with multiple cards to rearrange |
| Medium | `print_status_panel.xml` | Important during prints |
| Medium | `settings_panel.xml` | Compact 6-row category menu; sub-panels may benefit from multi-column |
| Low | Overlays (`*_overlay.xml`) | Usually modal dialogs that work OK at any size |
| Low | Simple panels | Panels with minimal content adapt naturally |

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
is computed in C++ (`include/grid_layout.h`, `src/ui/grid_layout.cpp`) and the shipped
default placements live in `assets/config/default_layout.json`. If you are adding a home
widget or wondering why yours vanished on a portrait screen, this is the section.

### How the grid is sized

The grid is square-cell: both axes are divided by the same per-breakpoint cell edge, so a
rotated panel gets (near enough) the transpose of its landscape grid and a widget's colspan
and rowspan mean the same physical thing.

```cpp
// include/grid_layout.h
TRACKS_PER_CELL = 2                                         // a track is HALF a cell
GRID_CELL[NUM_BREAKPOINTS] = {34, 40, 40, 60, 60, 72, 96}   // target track edge, px
```

`GRID_CELL` carries one entry per `UiBreakpoint`, XXLarge included — a `static_assert` ties
its length to `UiBreakpoint::XXLarge + 1` so adding a tier without a track edge fails the
build. XXLarge went in late (it had been clamping onto XLarge, so a 1080p panel drew a
720p-sized grid while fonts and icons scaled 1.6-2x around it); `theme_manager`'s
`nav_width_suffix()` and `ui_xml/navigation_bar.xml` carry the matching `_xlarge` /
`_xxlarge` nav widths.

`GridLayout::get_dimensions(bp, content_w, content_h)` divides the container's **content
box** — not the panel resolution — by `TRACKS_PER_CELL * GRID_CELL[bp]`, rounds each axis
to the **nearest** whole cell, and multiplies back up to tracks:

```
cells  = round(content / (TRACKS_PER_CELL * GRID_CELL[bp]))
tracks = TRACKS_PER_CELL * clamp(cells, MIN_TRACKS/2, MAX_TRACKS/2)
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

`MIN_TRACKS` (4) and `MAX_TRACKS` (64) are both whole cells, so clamping cannot produce an
odd count either. Neither is reached by any measured geometry; `MIN_TRACKS` exists for a
container that has not been laid out yet.

The breakpoint comes from the **narrow** axis (`min(width, height)`), so a tall portrait
panel is classified by its width and keeps the same cell edge after rotation. See
`include/ui_breakpoint.h`.

**Measured grids.** Content boxes read off a live instance
(`HELIX_SCREEN_SIZE=<WxH> helix-screen --test -vv`, then the `[PanelWidgetManager] Grid
layout:` / `Track geometry:` lines); pinned in `tests/unit/test_grid_square_cells.cpp`.

| Panel | Tier | Content box | Gutter | Grid | Track (w x h) | Aspect |
|-------|------|-------------|--------|------|---------------|--------|
| 480x272 | MICRO | 430x264 | 2 | **12x8** | 34.00 x 31.25 | 1.09 |
| 272x480 | MICRO | 264x394 | 2 | **8x12** | 31.25 x 31.00 | 1.01 |
| 480x320 | TINY | 418x312 | 2 | **10x8** | 40.00 x 37.25 | 1.07 |
| 320x480 | TINY | 312x394 | 2 | **8x10** | 37.25 x 37.60 | 0.99 |
| 480x400 | SMALL | 414x388 | 4 | **10x10** | 37.80 x 35.20 | 1.07 |
| 800x480 | MEDIUM | 710x466 | 5 | **12x8** | 54.58 x 53.88 | 1.01 |
| 480x800 | MEDIUM | 466x664 | 5 | **8x12** | 53.88 x 50.75 | 1.06 |
| 1024x600 | LARGE | 904x584 | 6 | **16x10** | 50.88 x 53.00 | 0.96 |
| 1280x720 | XLARGE | 1128x700 | 8 | **16x10** | 63.00 x 62.80 | 1.00 |
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

The shipped default placements. The file is **two-dimensional** — layout variant, then
breakpoint — and every span in it is in **tracks**, not cells:

```json
{
  "anchors": [
    { "id": "printer_image",
      "placements": {
        "tiny":   { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 },
        "medium": { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 },
        "large":  { "col": 0, "row": 0, "colspan": 6, "rowspan": 4 }
      }
    },
    { "id": "print_status",
      "config": { "layout_style": "detailed" },
      "placements": {
        "medium": { "col": 0, "row": 4, "colspan": 8, "rowspan": 4 }
      }
    }
  ],
  "disabled": {
    "micro": ["tips", "temp_graph"],
    "tiny":  ["tips", "temp_graph"]
  },
  "variants": {
    "portrait": {
      "anchors": [
        { "id": "printer_image",
          "placements": {
            "tiny":   { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 },
            "medium": { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 }
          }
        }
      ],
      "disabled": { "tiny": ["tips"] }
    }
  }
}
```

A variant is either a bare anchor array (the legacy shape, still accepted) or an object
carrying `anchors` plus `disabled`. Both forms have a test.

**`disabled` is not optional bookkeeping.** Omitting a widget from a tier does *not* switch
it off — `parse_widget_array()` appends every absent registry widget at its
`default_enabled` and the engine seats it wherever it fits. Worse, an omitted widget
inherits the first key in its tier's fallback chain that *does* have a placement, lands off
a grid of a different size, collides, and is evicted as "grid full" — taking whatever would
have auto-placed after it. Leaving a widget out of a tier therefore **requires** naming it
in that table's `disabled` map. This bit twice during the landscape rework; the guard is the
"no shipped table mixes authored and inherited placements" case in `test_default_layout.cpp`.

**`config` on an anchor** seeds that widget's per-instance settings, so a layout can ship
`print_status` in its Detailed style without a C++ branch. Anchor-level config is merged
*under* placement-level config.

`PanelWidgetConfig::build_default_grid()` (`src/system/panel_widget_config.cpp`) picks the
anchor table by walking `LayoutManager::variant_chain()` — the same most-specific-first
search `ui_xml/` overrides use. `variants.portrait` wins over the base `anchors` on a
portrait panel, and a `TINY_PORTRAIT` panel falls through `tiny_portrait` → `portrait` →
base. The keys under `"variants"` are named exactly like the `ui_xml/` override directories.

Widgets not named in the chosen table are auto-placed; the table only fixes the few that
have a deliberate home.

The three shipped tables are the base (landscape), `variants.ultrawide` (1480x320,
1920x440) and `variants.portrait`. Each authors **all seven tiers**; see the fallback
warning above for why leaving one out is dangerous rather than merely terse.

Inside a table, breakpoint keys are named rather than indexed — `micro`, `tiny`, `small`,
`medium`, `large`, `xlarge`, `xxlarge` (seven, matching `UiBreakpoint`). A missing tier
resolves by fallback (`micro`→`tiny`→`small`, `xlarge`→`large`, `xxlarge`→`xlarge`→`large`).
If no tier in the chain matches, the anchor is dropped and that widget is auto-placed instead.

#### Grid-qualified keys

A breakpoint names a *panel*, not a *grid*. The high-DPI UI scale multiplies the cell edge
(see `include/display_metrics.h`), so one panel at one tier has a different track count per
scale — 1080x2400 is xxlarge portrait at **12x24** tracks unscaled, **8x18** at 125% and
**6x14** at 158%. A table authored for one of those does not describe the others.

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
the layout `"anchors": "pending"` and `PanelWidgetManager::populate_widgets()` resolves it
at the first measured populate, then clears the tag. The tag is positive and written only by
the defaults path, so a layout a user has arranged is never overwritten.

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
`src/ui/panel_widget_registry.cpp`) carries six span fields, **all in tracks** — so the
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

**Which widgets opt in.** Set the flag on an axis when the widget's content is *continuous*
along it - a chart, an aspect-fit frame, wrapping text, a scrolling strip, stacked readout
rows, or a layout picked by measurement (`active_spool`'s compact/wide switch,
`decide_nozzle_layout()`). Half a cell of extra room shows more content there. Leave it off
for a centred fixed glyph over a short label - `network`, `led`, `filament`, `humidity`, the
heater tiles - where the intermediate size buys whitespace and nothing else, and costs a drag
snap twice as fussy on a 34px track. Every minimum is a whole cell, so the flag only ever
*adds* sizes above one the content already fits; it can never shrink a widget. The four
fixed-footprint buttons that carry `supports_half_col` with `max == min` - `shutdown`,
`lock`, `firmware_restart`, `led_controls` - use it for **placement** alone: it lets a lone
button centre in a two-cell gap. They cannot be resized at all.
`tests/unit/test_grid_layout.cpp` classifies every registry id, so a new widget cannot be
added without deciding this.

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

Auto-placement (`PanelWidgetManager`, `src/ui/panel_widget_manager.cpp`) runs two passes:

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
- Mark every non-scrolling container in the widget's XML `scrollable="false"`. A tile is
  scrolled by dragging it, not by a chevron gutter, so `PageScrollAutoInject` stops its walk
  at the tile root (`src/ui/page_scroll_auto_inject.cpp:67`) - but a scrollable container
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

In `xml_registration.cpp`, a helper function resolves paths through the LayoutManager:

```cpp
static void register_xml(const char* filename) {
    auto& lm = helix::LayoutManager::instance();
    std::string path = "A:" + lm.resolve_xml_path(filename);
    lv_xml_register_component_from_file(path.c_str());
}

// Usage — automatically resolves layout overrides:
register_xml("home_panel.xml");
```

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
