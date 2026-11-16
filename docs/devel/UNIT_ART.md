# Unit Art

HelixScreen draws filament units (AMS, ACE, CFS, Box Turtle, ERCF, Tradrack and the rest), dryers and toolheads from real proportions instead of flat logos. One short Python definition per unit produces baked image layers and a generated C++ shape table. The app tints the layers with live filament colours and draws the same unit as a live tray for the AMS detail screen. The recipe for drawing a unit is in `.claude/skills/unit-art/SKILL.md`; this page explains how the pieces fit.

## Key files

| Path | What it is |
|------|------------|
| `scripts/unit_art/kit.py` | Camera, lighting, SVG scene, `UnitDef`, `TrayShellDef`, shared parts (rollers, lids, fronts) |
| `scripts/unit_art/spool.py` | `draw_spool`: the spool drawn the way the live `spool_canvas` draws it |
| `scripts/unit_art/units/<id>.py` | One unit's definition |
| `scripts/unit_art/head.py`, `scripts/unit_art/heads/` | Toolhead definitions, baked by the same kit |
| `scripts/unit_art/build.py` | Bakes every unit and head, writes the two generated tables and a contact sheet |
| `assets/images/units/<id>/`, `assets/images/toolheads/<id>/` | Baked layers and `manifest.json`, committed |
| `src/generated/ams_unit_shells_gen.cpp` | Generated tray shapes, carriage tracks and rail faces |
| `src/rendering/toolhead_art_gen.cpp` | Generated toolhead frames (`kToolheadArt`) |
| `include/ams_unit_shell.h`, `src/ui/filament/ams_unit_shell.cpp` | `UnitShell` and the one lookup (`kShellRows`) that maps hardware to a unit |
| `include/ui_unit_art.h`, `src/ui/filament/ui_unit_art.cpp` | The `<unit_art tier="full\|thumb">` widget |
| `include/ams_tray_shell_draw.h`, `src/ui/filament/ams_tray_shell_draw.cpp` | Draws a unit's shell on the live tray |
| `include/toolhead_art.h` | Placement of baked heads on the filament path |
| `tests/python/test_unit_art_kit.py`, tag `[unit-art]` | Kit and runtime tests |

`make unit-art` runs the kit. It is a development step: the output is committed and devices, cross builds and CI never run Python for it. ESP32 firmware compiles none of it and keeps logos.

## One definition, two outputs

| | Art view | Live tray view |
|---|---|---|
| Used for | Dryer page, AMS overview tiles and cards, selector thumbnails, the path screen | AMS detail screen |
| Camera | `kit.ART_CAMERA`, a three-quarter view (yaw 27, pitch 20) shared by every unit | The tray's flat camera, `include/ams_tray_projection.h` |
| Made by | Baked at build time into PNG layers | Drawn at runtime for N lanes |
| Spools | Baked with a tintable region per lane | The real `spool_canvas` widgets |

A true isometric view was rejected: the near flange hides most of the filament, which carries the live colour.

The kit is the single source. Parts a unit needs are written once in its definition, and nothing per unit is hand-written in C++. A unit with no definition keeps its logo and the generic tray, and hardware that cannot be told apart (a Happy Hare setup that reports only firmware) gets no row in the lookup, so it is never given a guessed drawing. Art contains no wordmarks and one set serves the light and dark themes. A DIY system gets one representative drawing.

## Baked layers

Each unit bakes at two tiers: `full` (320 px wide) and `thumb` (40 px wide, bolder and without fine detail such as air holes). A tier has these layers, listed in `manifest.json`:

| Layer | Content |
|-------|---------|
| `back` | Everything behind the spools |
| `lane<N>` | White alpha mask of lane N's filament region, tinted at runtime with the lane's colour |
| `lane<N>_shade` | The shading drawn over that tint, so the colour reads as a rounded spool |
| `front` | Everything in front of the spools (`scene.layer = "front"`) |
| carriage | For a unit with a sliding selector: the moving part, baked at gate 0 and shifted per gate |

`<unit_art>` stacks them back to front, tints each filled lane, and hides a lane that is empty or has a zero colour. A unit with no matching art hides the widget. Variants:

- A unit built in several lane counts sets `lane_variants` and bakes each count to `assets/images/units/<id>/<n>/`. A lane count without baked art keeps the live tray shell and loses only the picture.
- `stub_art` bakes a second drawing, the body alone with a tube per lane, under `stubs/`. Units that hold no spools (BMCU, MMX, HTLF, 3MS, AD5X IFS) can show either look, chosen by `HELIX_UNIT_ART_FEEDER` (`docs/devel/ENVIRONMENT_VARIABLES.md#helix_unit_art_feeder`).
- `face_art` bakes a selector rail head-on for the path screen (ERCF and Tradrack), with a `RailFaceArt` row per gate count.
- The drying glow is a runtime image over the drawing, limited to the chamber around the spools by `unit_art_chamber_mask`. It runs only while the unit dries, and `unit_art_glow_for` turns it off on reduced-effects boards and stops it pulsing when animations are off.

Spools follow the app. The kit reads the flange colours, vent pattern and sizes from the C++ spool values (`spool_constants`), so art cannot drift from the live widget. Every unit draws its spools at one real spool proportion in a common frame (`kSpoolWidth` is 0.294 of the spool size).

## The live tray

The tray draws real spool widgets between `tray_back_draw_cb` and `tray_front_draw_cb` (`src/ui/filament/ui_ams_detail_tray.cpp`). Depth barely shows in its flat camera, so the unit's shape is carried by the end caps. The layout is a left cap, N lane segments and a right cap. A lane segment holds that lane's rail section, tick, tab and badge. The lid spans the row and is stretched, never tiled, and features centred on the row are placed after layout. The caps clear only the outer flange (half a spool width plus the wall and a gap), with no dead space at the ends.

`build.py` turns each unit's `TrayShellDef` into a `UnitShell` row:

| Field | Meaning |
|-------|---------|
| `cheek` | The body's side profile: depth 0 (front) to 1 (back) against height above the front wall, as `ProfilePoint`s at the 110 px reference spool |
| `dome` | The lid's profile; empty for an open unit |
| `back_extra_px`, `front_wall_px`, `cap_clearance_px` | Back wall height, front wall height, cap spacing |
| `lid_spans_row`, `lid_color` | Whether one lid covers every lane, or a box per lane with a solid top around its glass |
| `lane_join` | How lanes meet on the path screen: `Hub`, `Junction`, `Inside` (a short internal merge) or `FixedBlock` |
| `corner`, `body`, `front` | Rear corner block colour, body colour and the front parts (`FrontPart` rectangles) |
| `spool_lift_px` | How far the base reaches below the spools |
| `carriage`, `face_art` | Per gate count tracks for a moving selector |

One side profile feeds both outputs: the art draws it directly and `kit.tray_profile` converts it for the tray, so match it to the real unit's lid whatever the spool shape suggests. The tray shell's colours come from theme tokens; the baked art carries its own shading.

Selecting a unit's shell is the only place that names hardware. `unit_shell_for` walks `kShellRows` in order (backend slug, a part of the unit's name when one backend covers several kinds of unit, the shell id, an enclosed variant used when the unit has climate control, an optional lane count and a selector-rail flag), so a more specific row goes above a general one. Everything else in the UI asks for a shell and never names a vendor.

## Rendering on small boards

The shell is static between state changes, so it is drawn once and kept, and only spools repaint on data changes. Boards where `helix::ui::reduced_effects()` is true skip the drying glow and spool rotation. A printing lane turns slowly and a loading or unloading lane spins fast, both only with animations on (`decide_spool_spin` in `include/spool_spin.h`); the vents step through cached phases, so a step is a buffer copy.

## Toolheads

Toolheads use the same kit. `scripts/unit_art/heads/<id>.py` defines a head with its nozzle tip and filament entry, and `build.py` bakes a body, a tint mask and a shade mask at two heights (`full` 160 px and `path` 72 px) into `assets/images/toolheads/<id>/`. `src/rendering/toolhead_art_gen.cpp` records each head's aspect, tip, entry and height share, so heads keep their real relative sizes and the filament enters at the head's fitting. `toolhead_art_for(style)` returns the art, or nullptr when a style keeps its vector renderer. Styles are named in `include/toolhead_style.h`, and vendor names for them live in that module.

## Adding a unit

Follow `.claude/skills/unit-art/SKILL.md`. In outline:

1. Collect photos for the proportions. Keep them out of git.
2. Write `scripts/unit_art/units/<id>.py` with a `UnitDef` and a `TrayShellDef`, and add it to `UNITS` in `build.py`.
3. Add a row to `kShellRows` in `src/ui/filament/ams_unit_shell.cpp`.
4. Run `make unit-art` and commit the baked layers and the regenerated `src/generated/ams_unit_shells_gen.cpp`.
5. Run `python3 -m pytest tests/python/test_unit_art_kit.py -q` and `make t F='[ams][tray],[unit-art]'`, then check `build/unit-art/contact_sheet.png` on dark and light at real size.
6. Look at it in the mock with the unit's `HELIX_MOCK_AMS` value (`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`).

The baked layers add to install size (about 6 MB across all units at present), so a new unit's `full` and `thumb` sizes are worth checking before it is committed.
