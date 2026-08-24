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
| `portrait` | **Started** | `app_layout.xml`, `navigation_bar.xml` |
| `micro` | **Started** | `controls_panel.xml`, `header_bar.xml`, `theme_editor_overlay.xml`, `theme_preview_overlay.xml` |
| `micro_portrait` | Not started | Directory exists (empty) |
| `tiny` | Not started | Directory doesn't exist yet |
| `tiny_portrait` | Not started | Directory doesn't exist yet |

The table above is about `ui_xml/` overrides only. The home panel's widget grid adapts to
ultrawide and portrait geometry in C++ regardless of which override files exist — see
[Home Widget Grid](#home-widget-grid).

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

Start from a fixed per-breakpoint table, then let the layout type override one or both axes.

```cpp
// src/ui/grid_layout.cpp — GRID_DIMS, indexed by UiBreakpoint
MICRO / TINY / SMALL / MEDIUM  → 6 cols x 4 rows
LARGE / XLARGE                 → 8 cols x 5 rows
```

The table is `NUM_BREAKPOINTS == 6` long while there are seven tiers, so `XXLARGE` clamps
onto the `XLARGE` row and also gets 8x5.

The breakpoint itself comes from the **narrow** axis (`min(width, height)`), so a tall
portrait panel is classified by its width. See `include/ui_breakpoint.h`.

Then `GridLayout::get_dimensions()` consults `LayoutManager::type()`:

| Layout type | Columns | Rows |
|-------------|---------|------|
| `STANDARD`, `MICRO`, `TINY` | table | table |
| `ULTRAWIDE` | `clamp(width / TARGET_CELL_W_PX, MIN_DYNAMIC_COLS, MAX_DYNAMIC_COLS)` | table |
| `PORTRAIT`, `TINY_PORTRAIT`, `MICRO_PORTRAIT` | `clamp(width / TARGET_CELL_W_PX, MIN_PORTRAIT_COLS, MAX_DYNAMIC_COLS)` | `clamp(height / TARGET_CELL_H_PX, MIN_DYNAMIC_ROWS, MAX_DYNAMIC_ROWS)` |

Constants (all `static constexpr` on `GridLayout`):

| Constant | Value | Meaning |
|----------|-------|---------|
| `TARGET_CELL_W_PX` | 160 | Target cell **width**; drives derived column counts |
| `TARGET_CELL_H_PX` | 120 | Target cell **height**; drives derived row counts (portrait only) |
| `MIN_DYNAMIC_COLS` / `MAX_DYNAMIC_COLS` | 4 / 16 | Column clamp for ultrawide |
| `MIN_PORTRAIT_COLS` | 2 | Column floor for portrait, below the landscape floor of 4 |
| `MIN_DYNAMIC_ROWS` / `MAX_DYNAMIC_ROWS` | 3 / 16 | Row clamp for portrait |

The two cell targets are deliberately different numbers. Ultrawide has always kept the
fixed 4-row table on a 480px panel — a 120px row — so 120 is the row height the dashboard
is actually authored against. Portrait used to reuse the 160px *width* target for both
axes, which handed the tall screen 164px rows: fewer, chunkier cells than the wide screen
got (#1215).

**Worked examples:**

| Screen | Layout type | Narrow axis → tier | Table | Override applied | Final grid | Cell size |
|--------|-------------|--------------------|-------|------------------|------------|-----------|
| 800x480 | `STANDARD` | 480 → MEDIUM | 6x4 | none | **6x4** | 133 x 120 |
| 1920x480 | `ULTRAWIDE` | 480 → MEDIUM | 6x4 | cols = `1920/160` = 12 | **12x4** | 160 x 120 |
| 480x800 | `PORTRAIT` | 480 → MEDIUM | 6x4 | cols = `480/160` = 3, rows = `800/120` = 6 | **3x6** | 160 x 133 |
| 320x1480 | `PORTRAIT` | 320 → TINY | 6x4 | cols = `clamp(2, 2, 16)` = 2, rows = `1480/120` = 12 | **2x12** | 160 x 123 |

Columns are equal `LV_GRID_FR(1)` tracks (`make_col_dsc()` / `make_row_dsc()`), so the cell
sizes above are what the fractions work out to, not fixed pixel values.

### `assets/config/default_layout.json`

The shipped default placements. The file is **two-dimensional** — layout variant, then
breakpoint:

```json
{
  "anchors": [
    { "id": "printer_image",
      "placements": {
        "tiny":   { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 },
        "medium": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 },
        "large":  { "col": 0, "row": 0, "colspan": 3, "rowspan": 3 }
      }
    }
  ],
  "variants": {
    "portrait": [
      { "id": "printer_image",
        "placements": {
          "tiny":   { "col": 0, "row": 0, "colspan": 2, "rowspan": 3 },
          "medium": { "col": 0, "row": 0, "colspan": 3, "rowspan": 2 }
        }
      }
    ]
  }
}
```

`PanelWidgetConfig::build_default_grid()` (`src/system/panel_widget_config.cpp`) picks the
anchor table by walking `LayoutManager::variant_chain()` — the same most-specific-first
search `ui_xml/` overrides use. `variants.portrait` wins over the base `anchors` on a
portrait panel, and a `TINY_PORTRAIT` panel falls through `tiny_portrait` → `portrait` →
base. The keys under `"variants"` are named exactly like the `ui_xml/` override directories.

Widgets not named in the chosen table are auto-placed; the table only fixes the few that
have a deliberate home.

Inside a table, breakpoint keys are named rather than indexed — `micro`, `tiny`, `small`,
`medium`, `large`, `xlarge`, `xxlarge` (seven, matching `UiBreakpoint`). A missing tier
resolves by fallback (`micro`→`tiny`→`small`, `xlarge`→`large`, `xxlarge`→`xlarge`→`large`),
which is why the landscape table defines no `micro` or `xxlarge` rows. If no tier in the
chain matches, the anchor is dropped and that widget is auto-placed instead.

The file is runtime-editable and read via `find_readable()`, so a malformed or missing file
degrades to a small hardcoded fallback rather than an empty dashboard.

**A bad hand-edit fails the build.** `tests/unit/test_default_layout.cpp` has a
`[default_layout][portrait][shipped]` case that parses the *shipped* file and checks every
portrait anchor fits the narrowest grid its breakpoint can produce (`col + colspan <=`
the column budget for that tier). An anchor that overflows would silently fall through to
auto-place, making the anchor decoration; the test catches that instead.

### Widget span authoring

`PanelWidgetDef` (`include/panel_widget_registry.h`, table in
`src/ui/panel_widget_registry.cpp`) carries six span fields:

| Field | Meaning |
|-------|---------|
| `colspan` / `rowspan` | **Authored default** — the size the widget was designed at |
| `min_colspan` / `min_rowspan` | Smallest size the widget is still usable at (0 = fall back to the authored span) |
| `max_colspan` / `max_rowspan` | Largest size the user may resize it to (0 = not scalable) |

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
- If the widget genuinely cannot render below N columns, say so — being dropped on a narrow
  grid is then correct behaviour, not a bug. `tips` is the honest example: authored 4 wide,
  minimum 2, and deliberately absent from the portrait defaults because even at its minimum
  it costs a third to a half of a portrait row for rotating hints.
- Mark every non-scrolling container in the widget's XML `scrollable="false"`. A tile is
  scrolled by dragging it, not by a chevron gutter, so `PageScrollAutoInject` stops its walk
  at the tile root (`src/ui/page_scroll_auto_inject.cpp:67`) - but a scrollable container
  inside a tile still absorbs the drags the grid wants, and LVGL's scrollable default is ON
  unless you say otherwise.

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
