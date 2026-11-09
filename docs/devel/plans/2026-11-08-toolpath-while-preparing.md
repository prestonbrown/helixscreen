# Toolpath while a thumbnail-less file prepares: design

Status: decided 2026-11-08, see Decisions at the end, which replace the dock design. Option (a)
of prestonbrown/helixscreen#2057. Builds on `fix/no-thumb-preparing` (option (b): no picture,
and the `print_has_thumbnail` subject this plan reads). Deleted in the change that ships it
(`docs/CLAUDE.md`).

## Why

A file with no embedded thumbnail shows nothing of itself during a start that can run 5 to 13
minutes. Option (b) stops the Benchy stand-in from lying about the model; this plan shows the
real model, from the toolpath the panel already renders.

## What exists (measured 2026-11-08, mock `creator5_zmod`, `calicat_nothumb.gcode`)

- The deferred G-code load fires 5.6 s into Preparing
  (`src/ui/print/print_preview_controller.cpp#PrintPreviewController::schedule_deferred_load`),
  and `want_viewer()` is true for every non-Idle state, so `gcode_viewer_mode` goes to 2 and the
  viewer is live under the checklist for the rest of the start.
- **The viewer already draws while it is covered.** `ps_job_checklist` sets
  `style_clip_corner="true"`, and LVGL's cover check answers `LV_COVER_RES_MASKED` for any
  clip-corner object (`lib/lvgl/src/core/lv_obj.c`, `LV_EVENT_COVER_CHECK`). The refresher
  therefore never treats the opaque checklist as covering, and draws everything below it,
  viewer included, on every refresh of that area. That is why making the checklist transparent
  cost almost nothing on x86 (process 4.1-4.5% vs 3.3-4.4% of a core): the render was already
  being paid for, invisibly.
- The 2D renderer re-fits and drops its layer caches whenever its canvas size or bottom
  occlusion changes (`src/rendering/gcode_layer_renderer.cpp#GCodeLayerRenderer::set_bottom_occlusion`,
  `src/ui/gcode/ui_gcode_viewer.cpp#gcode_viewer_size_changed_cb`). A rebuild is the costly part
  of a load on MIPS (#2056: 40-51% of a core for 10-30 s). **Any layout that gives the viewer a
  different box during Preparing than during Printing pays that rebuild again at print start.**
- The metadata strip has two heights (with and without the M117 line), so the art band above it
  does too.

`ctl geom`, Preparing, side-by-side arrangement:

| Box | 800x480 | 480x272 |
|---|---|---|
| Job tile (`ps_job_tile`) | 341 x 281 at (92,66) | 208 x 144 at (50,48) |
| Checklist content box | 327 x 267 | 200 x 136 |
| Art band (`preview_art` = viewer) | 339 x 208, no M117; 339 x 181 with | 206 x 93; 206 x 76 |
| Metadata strip (`metadata_clip`) | 339 x 71; 339 x 98 with M117 | 206 x 49; 206 x 66 |
| Checklist row | 22 tall, 25 stride; 7 rows (header + 6 steps) = 175 | compact form, 74 of 136 used |
| Corner buttons | 36 x 36 at (103,77) | 36 x 36 at (57,55) |

The prototype (checklist made transparent) failed on looks, not cost: the model centres under
the list text, the view-toggle button and the metadata strip show through.

## Options

### S. Side by side: list left, toolpath right

```
800x480 job tile 341x281                 480x272 job tile 208x144
+-----------------+-------------------+  +----------+---------+
| calicat  prep.  |                   |  | calicat  |  ~100px |
| ✓ Homing     0s |     toolpath      |  | Step 4/6 |  model  |
| ✓ Heat bed   1s |     ~137 x 267    |  | Bed mesh |  too    |
| ✓ Heat noz   3s |                   |  |  40s     |  small  |
| ✓ QGL        7s |                   |  | ●●●◐○○   |         |
| ◐ Bed mesh  40s |                   |  +----------+---------+
| ○ Purge         |                   |
+-----------------+-------------------+
   ~190 (longest name 130 + icon + time)
```

The viewer box changes at print start (137 wide to 339), so the whole render is rebuilt then:
the #2056 spike, a second time. At 480x272 the model gets about 100 px. **Rejected.**

### D. Toolpath dimmed behind a translucent list

```
800x480                                   480x272
+-------------------------------------+  +----------------------+
|[scrim card_bg ~70% over the band]   |  | calicat   (scrim)    |
| calicat  preparing                  |  | Step 4 of 6   model  |
| ✓ Homing    ░░▓▓▓▓░░            0s  |  | Bed mesh      behind |
| ✓ Heating   ░▓▓▓▓▓▓▓░           1s  |  | 40s                  |
| ✓ QGL       ░▓▓▓▓▓▓▓░           7s  |  | ●●●◐○○              |
| ◐ Bed mesh  ░░▓▓▓▓░░           40s  |  +----------------------+
| ○ Purge                             |
|-------- opaque over the strip ------|
+-------------------------------------+
```

The viewer keeps its print-time box, so nothing is rebuilt. Hiding the corner buttons and making
the bottom 71/98 px opaque fixes two of the prototype's three faults. The third is structural:
the model centres at x = 262, under the step names (x 118 to 300). The thumbnail treatment avoids
that by sitting the image in the right 65%; the toolpath cannot move there without a pan, which
drops the caches and rebuilds at print start like S. Text over a ghosted toolpath also reads worse
than over a slicer thumbnail, and the scrim is a blended layer over the viewer for every
once-a-second countdown redraw.

### O. The checklist docks into the strip; the art band shows the print view (recommended)

```
800x480, Preparing, toolpath drawn          480x272
+-------------------------------------+    +----------------------+
|[⊞]                                  |    |                      |
|          ░░▓▓▓▓▓▓░░                 |    |      ░▓▓▓▓░          |
|         ░▓▓▓▓▓▓▓▓▓▓░   art band     |    |     ░▓▓▓▓▓▓░ 206x93  |
|          ░▓▓▓▓▓▓▓▓░    339 x 208    |    |      ░▓▓▓░           |
|           ░░▓▓▓░░      (unchanged)  |    +----------------------+
|                                     |    | calicat_nothumb      |
+-------------------------------------+    | ●●●◐○○ Bed mesh  40s |
| calicat_nothumb            text_body|    +----------------------+
| ● ● ● ◐ ○ ○   Bed mesh         40s  |      dock 206 x 49
+-------------------------------------+      (content 198 x 37)
  dock 339 x 71 (content 325 x 51)
```

The step list collapses into the strip's own box: file name on the first row (where the strip
puts it while printing), then a dot per step, the current step and its countdown. The art band is
exactly the box it will have while printing, so the render built during the start is the one the
print uses: no rebuild at print start, no text over the model. When printing begins the dock
hides and the metadata strip is underneath, the same height, already laid out.

Row budget: 800x480 needs 24 (text_body) + 4 + 22 = 50 of 51; 480x272 needs 18 + 2 + 17 = 37 of
37. Both fit at their tightest, which is why the dock has two rows and drops the "Step 4 of 6"
line the compact form has (the dots carry it).

Costs and limits:
- The full list with per-step times is not visible while docked. Before the toolpath has drawn
  (the first 6 to 35 s), the full list shows as today, picture-less (option (b)).
- One visible change mid-start: list to dock when the first frame lands.
- The M117 line must stay hidden while docked, or a message arriving mid-start resizes the art
  band and rebuilds the render. At print start an M117 that is set grows the strip once; that
  re-fit exists today.

## Recommendation: O

It is the only option whose viewer box is identical in Preparing and Printing at every size, so it
adds no rebuild; it puts no text over the model; and the 480x272 tile reads better than in D,
where 5 lines of compact text sit over a 206 x 93 render.

## MIPS cost, honestly

- **Steady state:** O draws the viewer the same number of times as today, because today it draws
  under the clip-corner checklist anyway. The checklist stops drawing its 6 to 8 rows; the dock
  draws 2. Expected net: zero to slightly negative. Not yet measured on MIPS.
- **The load spike:** unchanged in size and already inside Preparing (the load fires 5 s in). If
  the Creator 5 spike of #2056 really lands at print start, that is the progressive cache build
  finishing; O does not move it. Showing the render does not start any work that is not already
  running.
- **What could make it worse:** anything that resizes the viewer during Preparing. The M117 rule
  above and test 5 below are there for that.
- **Measure before landing:** K1C and AD5M, `--test --sim-speed 3`, print status open, a
  thumbnail-less file, 10 s windows through Preparing and the first 60 s of Printing, on main and
  on the branch, both orders (method in `docs/devel/PERFORMANCE.md`, device recipe in the
  `reference_device_measurement_recipe_via_zeus` memory). Gate: the branch's Preparing median is
  within 0.5 points of main's, and no second spike at print start.
- **Separate finding for (b), worth its own issue:** the viewer drawing under an opaque checklist
  is wasted work for every file, thumbnail or not. Excluding the art band from that draw (pause
  the viewer while the full checklist shows, or drop `clip_corner` there) would cut Preparing CPU
  for every print. Not part of this plan.

## Interfaces

**New subject, panel-owned:** `ps_toolpath_drawn` (int). 1 when the print status viewer holds the
effective print's geometry and has drawn its first frame; 0 otherwise.

```cpp
// include/print_preview_controller.h
struct Host {                   // PrintPreviewController::Host, one member appended
    std::function<void(bool show_viewer)> show_viewer;
    std::function<void()> parsed_file_loaded;
    std::function<bool()> is_active;
    std::function<void(bool drawn)> toolpath_drawn;  // first frame of the effective print; false on clear
};
```

- `PrintPreviewController::on_viewer_loaded` registers
  `helix::ui::gcode_viewer_set_first_frame_callback(viewer, &on_first_frame, this)` for the load
  it accepts; `on_first_frame` re-checks `is_load_for_effective_print()` and calls
  `host_.toolpath_drawn(true)`.
- The viewer's clear callback (already set in `PrintStatusPanel::create`) and
  `PrintPreviewController::on_filename_changed` / `on_print_ended` call `toolpath_drawn(false)`.
- `detach_widgets()` unregisters the first-frame callback (null), as `PrintSelectDetailView` does.
- Thumbnail Only (`preview_viewer_enabled()` false) and a load skipped for cost never fire it, so
  those prints keep the full picture-less list.

**No C++ derived subject** (`.claude/rules/declarative-ui.md` rule 7). The dock condition is an
inline `cond=`, because `preparing_visible` and `ps_toolpath_drawn` register in
`init_subjects()`, after component registration:

`preparing_visible eq 1 and print_has_thumbnail eq 0 and ps_toolpath_drawn eq 1`

## XML structure

New component `ui_xml/components/print_status_prep_dock.xml`:

```xml
<component>
  <view extends="lv_obj" width="100%" height="100%" ignore_layout="true" align="center"
        style_bg_color="#card_bg" style_bg_opa="255" style_radius="0"
        style_pad_hor="#space_md" style_pad_ver="#space_sm" style_pad_gap="#space_xxs"
        flex_flow="column" style_flex_main_place="center" scrollable="false" clickable="false">
    <text_body name="ps_dock_file" width="100%" bind_text="print_display_filename" long_mode="dots"/>
    <lv_obj name="ps_dock_step_row" width="100%" height="content" flex_flow="row"
            style_flex_cross_place="center" style_pad_gap="#space_xs" ...>
      <lv_obj name="ps_dock_dots" ...> <repeat count="8"> dot bound to ps_step_${i}_status </repeat> </lv_obj>
      <text_small name="ps_dock_current" bind_text="ps_step_current_name" flex_grow="1"
                  long_mode="dots" style_text_color="#warning"/>
      <text_small name="ps_dock_time" bind_text="ps_step_current_time" style_text_color="#warning"/>
    </lv_obj>
  </view>
</component>
```

- The dot styles move from `print_status_job_checklist.xml` to `ui_xml/styles.xml`
  (`styles.prep_dot_done/current/upcoming`) so the checklist and the dock share them.
- `print_status_preview_card.xml`: the dock is the last child of `metadata_clip`, so it covers
  exactly the strip's box at either height, opaque (the prototype's strip show-through):
  `<print_status_prep_dock name="ps_prep_dock" hidden="true"><bind_flag_if cond="<dock> " flag="hidden" invert="true"/></print_status_prep_dock>`.
- Same file: `display_message` gains `bind_flag_if cond="<dock>"` hidden, and
  `preview_corner_buttons` hides while `preparing_visible eq 1`.
- `print_status_body.xml`: `ps_job_checklist` hides on `cond="preparing_visible eq 0 or (print_has_thumbnail eq 0 and ps_toolpath_drawn eq 1)"`.

## Tests

`[print_status]` in `tests/unit/test_print_status_redesign.cpp`; the viewer's first frame is
driven by calling the host callback the fixture already stubs.

| # | Case | Fixture | Pins |
|---|---|---|---|
| 1 | No thumbnail, Preparing, before first frame: full list shows, dock hidden | Landscape800 | (b) stays the fallback |
| 2 | No thumbnail, first frame lands: list hidden, `ps_prep_dock` shown, `ps_dock_current` = current step | Landscape800, Portrait272 | the dock condition |
| 3 | Thumbnail present, first frame lands: list stays, dock hidden | Landscape800 | thumbnail files unchanged |
| 4 | Phase back to IDLE (printing): dock hidden, metadata strip shown | Landscape800 | the hand-off |
| 5 | `print_gcode_viewer` coords identical docked in Preparing, after an M117 arrives, and after the switch to Printing | Landscape800, Portrait272 | no rebuild at print start (the MIPS rule) |
| 6 | Filename changes while docked: `ps_toolpath_drawn` 0, list back | Landscape800 | stale model never docks |
| 7 | A first frame for a print that is no longer effective: subject stays 0 | unit, `PrintPreviewController` | the load race |
| 8 | Dock content fits: `ps_dock_step_row` bottom inside `metadata_clip` at 71 and 49 px | Landscape800, Portrait272 | the row budget |
| 9 | Corner buttons hidden while preparing | Landscape800 | prototype fault |

Plus one screenshot per size docked, and the MIPS measurement above.

## Decisions (Preston, 2026-11-08)

These override the dock design above, which is kept only for its measurements.

- Keep the whole checklist, with the model drawn under it (option D). No dock.
- Each checklist row, and its time, gets a dark pill behind it so the text stays readable over
  the model.
- The model stays centered. Moving it clear of the labels would force a re-render at print
  start, which this plan's measurements show is the costly part on MIPS.
- Thumbnail files stay as they are.
- The viewer drawing under the opaque checklist is tracked in its own issue.
