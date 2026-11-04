# Colour Swatches

Every colour the UI shows as a colour (a filament, a spool, an LED, a theme role, a tool)
is painted by one painter: `include/ui_swatch.h`, `src/ui/components/ui_swatch.cpp`. A swatch in the
LEDs overlay, the colour picker, an AMS lane, the tool switcher and a spool illustration
gets the same edge, the same multi-colour split and the same finish, because none of them
draws its own.

## The rules

| Rule | What it means |
|------|---------------|
| Shape | A pure colour is a **circle**. A labelled chip (colour with text on it, the mapping row's `T0`) is a **rounded rect**. Colour preview **areas** (the colour pickers' previews) and the AMS sidebar's tall loaded bar stay rounded rects: they are areas, not swatches. Bands that are part of a larger chip (`filament_swatch`, `tool_switcher_band`) keep their own geometry. The FILAMENTS card chip (`filament_swatch`) paints its header band with `<swatch_paint bind_color>` and its spool with `<color_dot color_subject>`, both bound to the card's `filament_chip_<i>_*` subjects, as the slot picker's rows bind `filament_picker_row_<i>_*`. |
| Edge | One rule: a swatch gets a `#border` edge when any of its colours has a WCAG contrast ratio under `kSwatchEdgeContrastRatio` (1.75) against what it sits on: its nearest ancestor with an opaque background, judged at draw time, else the theme's `screen_bg` and `elevated_bg` (`swatch_edge_surfaces()`). Nothing republishes on a theme switch; the theme colours are cached per theme generation. No swatch carries a border of its own and no rule keys on dark mode. |
| Selection | One mark: `color_swatch`'s inside ring (`selected_subject` + `index`). |
| Multi-colour | A filament with two or more colours (`SlotInfo::multi_color_hexes`, Spoolman's `multi_color_hexes`) shows them everywhere its colour shows, as equal-area bands across the top-left to bottom-right diagonal, clipped to the shape. A lane spool repaints on `AmsState::get_slot_paint_subject()` (`SlotInfo::paint_key()`) as well as its colour, so a new split or finish under the same colour shows. |
| Glass | A swatch never takes the glass appearance: the swatch painter marks every object it paints `glass_mark(obj, GlassFamily::None)`, because shading would shift the color that identifies a spool (`docs/devel/THEME_SYSTEM.md` § "Glass appearance"). |
| Finish | Silk, matte, marble and sparkle come from the filament's names, never from a widget. A swatch with less area than a `kSwatchFinishMinPx` (16 px) square cannot show a finish and paints plain; a long thin band or a spool body of the same short side keeps it. |

## Finishes

`helix::filament_finish_of()` (`include/filament_variants.h`) is the one place a name
becomes a finish. It reads whole words, case-insensitively:

| Finish | Words | Painted as |
|--------|-------|------------|
| Silk | Silk, Silky, Satin | a diagonal specular highlight |
| Matte | Matte, Matt | flat, a fifth of the way to its own grey (`swatch_matte_color()`) |
| Marble | Marble, Granite, Stone | a fine speckle of dark and light flecks |
| Sparkle | Sparkle, Glitter, Galaxy, Shimmer, Starlight | small four-pointed glints |

A slot's finish is `SlotInfo::finish()`: the first of its material, product name and
spool name that names one, never its colour name ("Stone Grey" is a plain grey). `AvailableSlot::finish` carries it to the mapping
rows; `SpoolInfo::finish()` reads a Spoolman spool's material and filament name. Where a
finish travels through a subject it is the finish's name (`filament_finish_name()`), read
back by `filament_finish_of()`.

## Using it

### XML

| Component | When |
|-----------|------|
| `<color_swatch>` | A swatch the user picks: a touch target with a selection ring. Props `color_subject` or `color`, `colors_subject` (CSV), `finish_subject` (a filament name), `fill` (a style; the LED Custom swatch's hue wheel), `selected_subject` + `index`, `callback`, `clickable`, `bubble`, `size`, `hit`, `reach`, `ring`. Grids repeat it (`<color_swatch_grid>`, below). |
| `<color_swatch_grid>` | The 30-swatch preset grid, for either palette. Props `swatch_callback` (required), `palette` (`general` default, or `theme`), `selected_subject`, `swatch_size` / `gap` / `gap_half` (the `swatch_grid_*` tokens by default), `lead_hidden` + `lead_callback` (a leading `custom_color_swatch`, hidden by default). |
| `<custom_color_swatch>` | The rainbow custom-colour swatch (hue wheel, centre glyph, `index` -2) that opens a colour editor: leading a preset grid, closing the LED colour row. Props `icon` (`pencil` default, `plus`), `selected_subject`, `callback`, `size`, `hit`, `reach`, `ring`; sizing defaults to the preset grids' tokens. |
| `<color_dot>` | A colour that is only shown (a spool in a list, a slot in a row, a tool's dot): no ring, no reach, clicks go to the row. Same colour props plus `size`. |
| `<swatch_paint>` | The bare painter, for a rounded rect (set `style_radius`) or a fill inside another control (the LED power button). Attributes `color`, `bind_color`, `colors`, `bind_colors`, `finish`, `bind_finish`, `edge="off"`. With no colour it paints nothing. |

A click handler reads the tapped colour with `helix::ui::swatch_color_of(target)`.

### Preset grids and touch targets

`color_swatch_grid` is the one preset grid. Its swatches read
`<palette>_swatch_color_<i>`, published from `swatch_palette()`
(`include/ui_color_picker.h`): `general` for filament, LED and general colours, `theme` for
the theme editor's ramp and hue families (`tests/unit/test_theme_swatch_grid.cpp` pins the
order). The colour picker shows one or the other; the spool editor's colour view shows
`general`.

**Every pickable swatch is at least a 44px touch target**, however small its disc. That
swatch floor sits below the 48px `#touch_target_min` that buttons and other tappable controls
use, so a grid never outranks the controls around it. A swatch's target is the disc plus `hit`
on each side, so in a grid `size + 2 * hit >= 44`, and the gap between swatches is at least
`2 * hit`, so neighbouring targets abut instead of overlapping (LVGL gives an overlapped tap to
the topmost widget, which makes taps land unpredictably). In `color_swatch_grid`, `hit` is
`swatch_grid_gap_half`, so the pitch `swatch_grid_size + swatch_grid_gap` is the target, and
every tier keeps that pitch at 7 mm or more on its panel:

| Tier | Swatch | Gap | Pitch | Grid width |
|------|--------|-----|-------|------------|
| micro | 26 | 18 | 44 | 246 |
| tiny (3.5" 480x320, 165 dpi) | 28 | 18 | 46 | 258 |
| small | 28 | 16 | 44 | 248 |
| medium, large | 32 | 12 | 44 | 252 |

The tabbed picker (micro and tiny) gives its presets a whole tab and passes the larger
`picker_tab_swatch_*` tokens (40 + 6). A host that places its own row of `color_swatch`es
(the spool editor's inline grids) uses the same `swatch_grid_*` tokens and pads its container
by `swatch_grid_gap_half`, so the outer row and column are hit-testable: LVGL only hit-tests
a child inside its parent's box.

The shared token ladders (`swatch_grid_*`, `picker_tab_swatch_*` and the LED preset rows'
`led_swatch_*`) hold the 44px floor and the abutting gap at every size class, and
`tests/unit/test_color_swatch_grid.cpp` checks them per class. A grid that needs more room
widens its container (the colour picker modal's `color_picker_modal_width`), never shrinks the
target.

`custom_color_swatch` defaults to those tokens too, so it tiles as one more grid cell where
it leads a row; the LED colour row passes its own sizes.

### C++

| Call | When |
|------|------|
| `apply_swatch_color(obj, rgb, multi_csv, finish)`, `apply_swatch_color(obj, slot)` | Paint an object (or a `color_swatch`/`color_dot` holder's disc) from data. The object keeps its size and radius. |
| `clear_swatch_color(obj)` | An empty slot: paint nothing, keep the object's own (warning) border. |
| `draw_swatch(layer, area, paint)` with `swatch_paint_for(...)` | Draw-task code: the temp-graph legend dot. A finish needs `paint.cache`, a `SwatchCache` the caller keeps. |
| `swatch_paint_finish_pixels(buf, area, radius, finish)` | An illustration writing the finish over filament it draws into its own ARGB8888 buffer. |
| `spool_canvas_set_filament(canvas, slot)` | The spool illustration: the wound filament splits into the same bands, matte mutes it, the other finishes texture it. `spool_canvas_set_view()` picks the side view (the 3D lane style, Active Spool, Spoolman) or face-on (the flat lane style); see "The spool's vented flange" below. |

The painter hangs a `LV_EVENT_DRAW_MAIN_BEGIN` hook on the object, so a style border (a
warning ring, the selection ring) draws over the fill. It forces the style background
transparent and applies the object's `opa`.

A redraw is at most three draw tasks: the fill (one rounded rect, a gradient for a split),
one blit of the finish texture and the edge. The texture (silk highlight, marble flecks,
sparkle glints) is rendered with plain pixel writes into an image the object's
`SwatchCache` owns, once per size, radius and finish, and freed with the object; the split
thresholds are cached the same way.

### The pure decisions

`swatch_needs_edge()`, `swatch_colors_need_edge()`, `parse_multi_colors()`,
`swatch_split_thresholds()` / `swatch_split_polygons()`, `swatch_effective_finish()`,
`swatch_matte_color()` and `swatch_texture_points()` hold no LVGL objects and are unit
tested in `tests/unit/test_ui_swatch.cpp` (`[swatch]`), next to pixel tests of
`draw_swatch()` on a canvas. The split is drawn as one rounded rect with a hard-stopped
linear gradient, so the rim is anti-aliased and the bands have no seams; past four colours
the eight gradient stops blend one stop per colour.

### The spool's vented flange

The spool's front flange is vented like a reusable spool: six radial slots, the first
straight up, and a lattice of small vent dots between them. Each opening shows what is
behind the flange there, the wound filament where it reaches that far out and the dark far
flange where it does not, so the vents double as a fill gauge. The geometry and the
detail-by-size rule live in `include/spool_vents.h`: slots from a 32 px canvas, dots from
96 px, a solid flange below 32 px (`spool_vent_detail()`). `spool_vent_coverage()`
answers per pixel for a face of any skew, so the side view's squashed flange and the
face-on view share one vent shape. Each view composites it its own way in
`src/ui/filament/ui_spool_canvas.cpp`: the side view paints the openings over its flange, and the
face-on view smokes the flange over the filament (55% flange) and leaves the openings
clear. Tests: `[spool_vents]`.

## Looking at it

`HELIX_MOCK_AMS_STATE=finishes` gives the mock lanes a silk, a three-colour silk, a marble,
a sparkle, a matte and a two-colour filament, so every surface that paints a spool shows
each. `HELIX_SWATCH_GALLERY_DIR=<dir> ./build/bin/helix-tests '[swatch_gallery]'` writes
`gallery-light.png` and `gallery-dark.png`: a circle, a chip and a 3D spool per finish on
the theme's own background. `'[swatch_gallery_spools]'` writes `spools-light.png` and `spools-dark.png`: each
filament as a spool from the side and face-on, at `HELIX_SWATCH_GALLERY_SPOOL` px (240 by
default).

## The gate

`scripts/check_hand_rolled_swatch.py` (`qc_hand_rolled_swatch`) fails a new circle filled
with a filament, spool, slot, LED, tool or swatch colour outside the painter and its
components, in XML or C++. A circle that is not a colour swatch (a homing dot) takes
`SWATCH_OK: <reason>`. Meta-tests: `tests/shell/test_hand_rolled_swatch_gate.bats`.
