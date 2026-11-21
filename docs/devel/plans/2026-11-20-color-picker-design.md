# Color Picker Redesign

Status: design agreed with Preston 2026-11-20 from an interactive prototype
(https://claude.ai/artifact/8MBW5X9mNr6QgcKLGrujCn, private to Preston). Building on
`feature/color-picker-v2`. Delete this file in the change that ships the work.

## Why

The app picks colors in two places that disagree: the `ColorPicker` modal
(`ui_xml/color_picker.xml`, `src/ui/components/ui_color_picker.cpp`; used by the LED
control overlay, the Spoolman edit modal, the spool wizard and the theme editor) and the
slot editor's own inline color grid (`ui_xml/ams_edit_overlay.xml`, `color_grid` /
`color_grid_split`). Both are hard to use with a finger on a 480x272 panel: 26 px chips,
a small gradient square and a thin hue bar nobody can hit precisely, hex entry through the
full keyboard, and a weak preview of the chosen color.

## What we are building

One embeddable picker, used everywhere. A caller puts it inline in a page, in an overlay
page, or in a modal; the picker itself is never a modal. It follows what iOS, macOS and
Android share: one primary surface you tap, exact controls behind a segmented switch, one
strip of your own colors, nothing decorating the colors.

### The picker body

- **Segmented switch: Grid | Sliders.** It lives in the host's header row when the host
  has one (overlay page, modal), otherwise at the top of the body (inline).
- **Grid tab.** A grid of contiguous color cells filling the body. Cells touch, so a
  missed tap lands on a near-identical shade. The selected cell gets a double ring (2 px
  white inside 2 px black) that reads on light and dark cells. Nothing is selected when the
  current color is not in the grid.
- **Sliders tab.** Three rows, Hue, Saturation, Brightness (HSV). Each row: label, a
  [-] button, a track painted with the colors that row produces at the other two values, a
  [+] button, the value (229 deg, 53%). The thumb is a white-bordered pill filled with the
  current color. Dragging anywhere on the track sets the value; [-]/[+] step (hue 2 deg,
  saturation and brightness 1%) and repeat while held. Track height: 29 px at 480x272,
  46 px at 800x480. Hue is kept while saturation or brightness is 0.
  Below the sliders: a **Hex** row with a button showing `#RRGGBB` and an edit glyph.
  Tapping it covers the body with a hex keypad: a preview swatch, the digits typed so far,
  backspace, Cancel and OK (enabled at 6 digits); 16 keys 0-9 A-F in an 8x2 grid.
- **Bottom strip**, on both tabs: a caption and one row of 40 px chips (60 px at
  800x480) that scrolls sideways. Tap a chip to pick it. The current color's chip gets a
  ring. What the strip holds comes from the caller (below).
- **Current color readout**: a swatch, the nearest color name and the hex value. In the
  host header when there is one.

### Each caller brings its palette and strip

| Caller | Grid | Strip | Recents key |
|---|---|---|---|
| Filament (slot editor, Spoolman edit, spool wizard) | Filament grid: 10 columns x 6 rows | "Recent": filament recents | `filament` |
| LED control | LED grid: 10 x 3 | "Recent": LED recents | `led` |
| Theme editor | Theme presets, 6 x 5 (`Palette::Theme`, the existing 30) | "This theme": the theme's own colors, de-duplicated | none |

- **Filament grid**: row 1 grays (OKLCH L 1.0, .90, .80, .70, .60, .50, .41, .33, .25,
  .17, chroma 0); rows 2-6 are ten hues (OKLCH h 27, 55, 90, 125, 150, 190, 235, 268, 305,
  350) at tones (L, C) = (.90, .07) pastel, (.70, .08) muted, (.64, .21) vivid,
  (.46, .17) deep, (.32, .10) dark, chroma reduced to fit sRGB. Generate the 60 values once
  and commit them as a constant table; the device never does OKLCH math. The prototype's
  generator is the reference (`oklch()` in the artifact).
- **LED grid**: row 1 white temperatures 1900, 2300, 2700, 3000, 3500, 4000, 4500, 5000,
  5800, 6500 K; row 2 full hues (HSV h 0, 25, 50, 90, 130, 165, 195, 225, 270, 310 at S=1,
  V=1); row 3 the same hues at S=0.4. No dark rows: brightness is the light's own control.
- The existing `Palette::General` list (30 filament colors) goes away with the old picker.

### Recent colors

- Two lists in settings, `filament` and `led`, newest first, de-duplicated, capped at 12.
  No pins, no badges, no drag. Preston rejected the star badge and pins as clutter.
- A color joins its list when the host **commits** it: the slot editor's Save, the
  Spoolman edit's save, the wizard's finish, the LED modal's Done. Browsing does not.
- Long-press a chip (550 ms): every chip in the strip shows a small x, and a Done button
  appears at the strip's end. Tapping a chip then removes it. The theme strip has no
  removal.
- Seed lists for a fresh install: filament = black, white, gray, red, orange, yellow,
  green, blue (from the old palette's lead colors); LED = warm white 2700 K, white, red,
  cyan, purple.

### Hosts

1. **Slot editor (filament), the main case.** The editor's Color line shows the color
   name; below it one row: a big current-color swatch (1.25 x chip, ringed), then the first
   4 (480x272) or 6 (800x480) filament recents other than the current color, then an
   "All colors" pill with a small hue wheel. A recent chip applies at once, in the editor's
   draft. The swatch or "All colors" pushes a **color overlay page**: header = back, the
   current color readout, the Grid | Sliders switch; body = the picker. The picker edits the
   editor's draft live (one draft, `feedback_one_draft_model`); Back returns; the editor's
   Save commits. This replaces `color_grid` / `color_grid_split` in
   `ams_edit_overlay.xml`.
2. **Multi-color (filament).** The Colors line has the style dropdown (Gradient =
   Spoolman `longitudinal`, Coaxial = `coaxial`) and, beside it, a preview bar of the
   colors. Below: a dot per color (2 to 4) and a dashed "+" while fewer than 4. Tap a dot
   to select it (ring, label "Color 2: Gold"); tap the selected dot again to open the color
   overlay for it. Below the dots: the recents row and "All colors" for the selected dot.
   Long-press a dot to remove it; removing down to one color makes the spool single-color.
   Data: `multi_color_hexes` and `multi_color_direction` (`include/ams_types.h`,
   `include/spoolman_types.h`). Check how each backend writes these before wiring Save; a
   backend that cannot store multi-color keeps the old single-color behavior and hides "+".
3. **LED control.** A `ColorPickerModal` (Modal subclass): header = Cancel, the readout,
   the switch, Done. Full screen at 480x272; a 660x430 card at 800x480. Live preview on the
   light while open; Cancel restores the original; Done keeps it and adds it to LED recents.
4. **Theme editor, inline.** At 800x480: a left column of the theme's 16 roles as color
   bars (the role's own color as the bar fill, its name and hex inside in a contrasting
   text color, 44 px tall, scrolls), and the picker beside it with the readout and switch
   above the grid. At 480x272: the role bars become a sideways-scrolling row of 34 px bars
   above the grid, and the readout and switch move to the page header. The role being
   edited has a white ring and stays scrolled into view; picking repaints its bar live.
   Role names in order: Screen, Overlay, Card, Elevated, Border, Text, Text muted,
   Text subtle, Primary, Secondary, Tertiary, Info, Success, Warning, Danger, Focus
   (`ModePalette::color_names()`).
5. **Spoolman edit modal and spool wizard**: the `ColorPickerModal` with the filament
   palette, filament recents.

## Code shape (decisions the builder can't make alone)

- `include/ui_color_picker_view.h` / `src/ui/components/ui_color_picker_view.cpp`:
  `helix::ui::ColorPickerView`, owning one `color_picker_view` XML component instance.
  - `enum class ColorPalette { Filament, Led, Theme };`
  - `enum class ColorStrip { FilamentRecents, LedRecents, Custom };`
  - `struct ColorPickerConfig { ColorPalette palette; ColorStrip strip; std::string strip_caption; std::function<std::vector<uint32_t>()> custom_strip; bool tabs_in_body; };`
  - `void attach(lv_obj_t* container, uint32_t initial_rgb, const ColorPickerConfig&);`
  - `void set_color(uint32_t rgb);` `uint32_t color() const;`
  - `void on_change(std::function<void(uint32_t rgb)>);` fired on every pick, drag step and
    keypad OK.
  - `void set_tab(Tab)` / `Tab tab() const` so a host header switch can drive it.
- `include/color_recents.h` / `src/ui/color_recents.cpp`: `ColorRecents` with
  `list(Kind)`, `remember(Kind, uint32_t)`, `remove(Kind, uint32_t)`; `Kind { Filament, Led }`;
  stored through `SettingsManager` like other list settings; cap 12.
- `ColorPickerModal` replaces `ColorPicker` (same callers, same callback shape
  `void(uint32_t rgb, const std::string& name)`). Delete `ColorPicker`, `color_picker.xml`,
  the slot editor's grid, and `ui_hsv_picker` if nothing else uses it (grep first).
- Every swatch, dot and chip goes through the one swatch painter (`include/ui_swatch.h`,
  `docs/devel/COLOR_SWATCHES.md`); a hand-rolled swatch fails its gate.
- The nearest-name lookup reuses the existing color-naming code (`color_utils.h`); do not
  add a second name table.
- Grid cells and strip chips use the existing indexed subject pool pattern
  (`helix::xml::IndexedSubjectPool`, see `s_theme_colors` in `ui_color_picker.cpp`) and
  declarative XML (`.claude/rules/` declarative UI). Slider tracks: LVGL gradients take up
  to 8 stops here (`LV_GRADIENT_MAX_STOPS 8`), enough for the hue track (7 stops); check
  `ui_hsv_picker.cpp`'s hue bar before choosing an image instead.
- Touch targets: grid cells are about 42x28 at 480x272 (contiguous); every button is at
  least 40 px.

## Tests

Unit tests (follow the `writing-tests` skill), each seen red once:
- `ColorRecents`: newest first, de-dup moves to front, cap 12 drops the oldest, remove,
  the two kinds are independent, persistence round-trip.
- Palette tables: sizes (60, 30, 30), no duplicates in the filament grid, LED row 1 is
  warm to cool.
- HSV round-trip keeps hue at S=0 and V=0; stepping wraps hue and clamps S/V.
- Hex keypad: OK disabled under 6 digits, backspace, the result applies.
- Slot editor: a recent chip changes the draft, not the saved slot; Save adds the color
  to filament recents; Back from the overlay keeps the draft color.
- Multi-color: add up to 4, remove down to 1 turns single-color, direction maps to
  `longitudinal`/`coaxial`.
- LED modal: Cancel restores the original color; Done adds to LED recents.

Screenshots at 480x272 and 800x480 of: slot editor, color overlay (grid and sliders),
keypad, multi-color, LED modal, theme editor. Compare against the prototype.

## Not in scope

Per-material memory (Preston said no), pins, drag to reorder, alpha, an eyedropper.
