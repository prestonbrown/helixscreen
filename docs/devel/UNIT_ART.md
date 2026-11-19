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

Every drawing comes from real photos and proportions. A drawing that looks plausible but is wrong is a failure, so detail is never invented to fill a gap: the unit keeps its logo until it can be drawn properly.

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

`<unit_art>` stacks them back to front, tints each lane that holds or still names a spool, and hides the tint of an empty one. A unit with no matching art hides the widget. Variants:

- A unit built in several lane counts sets `lane_variants` and bakes each count to `assets/images/units/<id>/<n>/`. A lane count without baked art keeps the live tray shell and loses only the picture.
- `stub_art` bakes a second drawing, the body alone with a tube per lane, under `stubs/`. Units that hold no spools (BMCU, MMX, 3MS, AD5X IFS) can show either look, chosen by `HELIX_UNIT_ART_FEEDER` (`docs/devel/ENVIRONMENT_VARIABLES.md#helix_unit_art_feeder`).
- A selector that stands in front of the spools (ERCF, Tradrack, HTLF) draws no spools, only its body and a filament stub per gate.
- `face_art` bakes a selector rail head-on for the path screen (ERCF and Tradrack), with a `RailFaceArt` row per gate count. A unit with face art and no carriage, the HTLF, passes each lane through to its own outlet below its inlet: its cam turns in place. Its art is baked at its one lane count, and on a HUB or MIXED path it stands between the spools and the hub or the lane sensors, with each lane bending in to its inlet and leaving its own outlet (`src/ui/filament/ui_filament_path_plan.cpp#through_art_rect`).
- The drying glow is a runtime image over the drawing, limited to the chamber around the spools by `unit_art_chamber_mask`. It runs only while the unit dries, and `unit_art_glow_for` turns it off on reduced-effects boards and stops it pulsing when animations are off.

The bake also records each lane's geometry, in shares of the drawing's width (`manifest.json` `geometry` and `drawn`, and a `UnitArtGeom` row in `src/generated/ams_unit_shells_gen.cpp`): the two flange centers and the flange's radius vectors, so the spool is every flange disc between them; the full and bare-core radii of the wound filament; the flange cylinder's bounds; and the lane's outlet, where its filament leaves the unit (`kit.outlet`, else the front bottom edge under the spool). The runtime reads these and never measures the art's pixels (`include/unit_art_geometry.h`):

- Fill level masks each lane's tint and shade to the filament wound at its weight, by the one rule the drawn spool uses (`tray::wound_radius`).
- `unit_art_set_lane_marks` draws, per lane, a thin rim just outside the spool's outline (the current lane in the accent color, a lane in error in its severity's) and a dim over a bay that is not there.

Spools follow the app. The kit reads the flange colours, vent pattern and sizes from the C++ spool values (`spool_constants`), so art cannot drift from the live widget. Every unit draws its spools at one real spool proportion in a common frame (`kSpoolWidth` is 0.294 of the spool size).

## On the AMS overview

Each unit on the overview row (`src/ui/filament/ui_panel_ams_overview_cards.cpp`) is one of three things:

| The unit | It draws as |
|----------|-------------|
| Has art for its lane count | `ams_unit_tile`: the full art with no card chrome, the name under it and the climate readout over its lower right corner |
| Has no art, beside a unit that has | `ams_unit_tile` holding one lane bar per lane at a fixed compact width |
| Has no art, in a row where no unit has | `ams_unit_card`, the logo card with lane bars and a lane count |

Each unit resolves on its own, so a rig of mixed units mixes tiles. A unit that gains or loses art, such as after a lane count change, rebuilds the row. The readout is hidden at the two smallest breakpoints. A row that overflows sizes every tile to one width chosen so the next tile stands partly past the right edge (`overview_peek_card_width`); a bars tile's compact width counts as its floor there, so art is never narrower than a bars tile beside it. A disconnected unit's art takes the `unit_offline` fade, and an absent one shows disabled with "Not connected".

## The unit view

A unit with art and lane geometry for its lane count is drawn from its art in the AMS unit view too (`src/ui/filament/ui_ams_detail.cpp#place_unit_view_art`). The art is scaled so its first and last spools stand over the first and last slots, which stay as tap targets with their own spool drawing transparent. Each lane's tube starts at the drawing's foot, in its baked outlet's column, so no tube runs over the unit. Labels and their dotted leaders end at each spool's top, the readout tucks over the drawing's right end, and a tool badge stands beside its label, off the drawing. Any other unit keeps the drawn tray below.

The drawing's size comes from the view scale (`include/path_view_scale.h`): it takes the width its row allows and the height the path below leaves (`src/ui/filament/ui_ams_detail_labels.cpp#fit_scaled_unit`).

Each lane's number stands alone, in one row, on the unit's face over its outlet, shrunk to the outlets' pitch where it is wider. A lane with no spool draws none in its bay, and its number is outlined: the badges' dark fill with a ring round it. A ghost lane (a spool taken out with its identity kept) draws its spool see-through and grayed, with dashed rims on both flanges (`laneN_rim`, in the lane's own place in the depth). A lane a finger is down on is outlined whole: solid bright rims on both its flanges (the same `laneN_rim` layer) and a ring on its number. Where the outlets stand closer than a finger's width (28 px), the lanes are not each a tap target: a tap anywhere on the drawing opens a picker of the unit's lanes (`FilamentSlotPicker`), and a chosen row opens that lane's menu as a tap on it would.

A unit def declares where each lane's prep sensor stands with `kit.sensor_region(scene, lane, "prep", points)`; the build bakes its outline into `ArtLaneGeom::prep`, and the art tints it the lane's filament color while the lane holds filament and red when that sensor reports a fault (`sensor_faults`, over the front layer). Box Turtle (its drive blocks), QuattroBox (its feeder blocks) and Night Owl (its pods) declare one.

## The live tray

The tray draws real spool widgets between `tray_back_draw_cb` and `tray_front_draw_cb` (`src/ui/filament/ui_ams_detail_tray.cpp`). Depth barely shows in its flat camera, so the unit's shape is carried by the end caps. The layout is a left cap, N lane segments and a right cap. A lane segment holds that lane's rail section, tick, tab and badge. The lid spans the row and is stretched, never tiled, and features centred on the row are placed after layout. The caps clear only the outer flange (half a spool width plus the wall and a gap), with no dead space at the ends.

`build.py` turns each unit's `TrayShellDef` into a `UnitShell` row:

| Field | Meaning |
|-------|---------|
| `cheek` | The body's side profile: depth 0 (front) to 1 (back) against height above the front wall, as `ProfilePoint`s at the 110 px reference spool |
| `dome` | The lid's profile; empty for an open unit |
| `back_extra_px`, `front_wall_px`, `cap_clearance_px` | Back wall height, front wall height, cap spacing |
| `lid_spans_row`, `lid_color` | Whether one lid covers every lane, or a box per lane with a solid top around its glass |
| `combiner`, `port_block` | Where the lanes merge into one output (§ "Combiner"), and whether the unit's on-head selector is a fixed block with a port per lane |
| `corner`, `body`, `front` | Rear corner block colour, body colour and the front parts (`FrontPart` rectangles) |
| `spool_lift_px` | How far the base reaches below the spools |
| `carriage`, `face_art` | Per gate count tracks for a moving selector |

One side profile feeds both outputs: the art draws it directly and `kit.tray_profile` converts it for the tray, so match it to the real unit's lid whatever the spool shape suggests. The tray shell's colours come from theme tokens; the baked art carries its own shading.

A unit whose shell sets `lid_color` is a row of separate boxes, such as the EMU. The tray draws them left to right with solid sides in the shell's body and lid colours around the glass, then draws the unit's front as one dock band across every box after them (`src/ui/filament/ams_tray_shell_draw.cpp#fill_box_side`, `src/ui/filament/ui_ams_detail_tray.cpp`), instead of drawing the front before the spools as other shells do.

On the path screen a selector rail (ERCF, Tradrack) carries its buffer's tension control on the final drop below the jog from its outlet to the center; the rail rises toward the lanes to make the room. Only where there is no room does its sync sensor show as a band on the outlet tube. The band takes its colour from the sensor's state: the accent colour when active, the error colour on error, the lane's filament colour when loaded or in sync, and the idle wall colour when empty (`src/ui/filament/ui_filament_path_plan.cpp#band_color`).

Selecting a unit's shell is the only place that names hardware. `unit_shell_for` walks `kShellRows` in order (backend slug, a part of the unit's name when one backend covers several kinds of unit, the shell id, an enclosed variant used when the unit has climate control, an optional lane count and a selector-rail flag), so a more specific row goes above a general one. Everything else in the UI asks for a shell and never names a vendor.

## Combiner

Every unit with more than one lane merges its lanes into one output that feeds a single extruder. Where that merge happens is Preston's call per unit, recorded as `TrayShellDef.combiner` in the unit's own file and generated into `UnitShell::combiner`:

- **internal**: the unit merges its lanes itself, and one path leaves it.
- **external**: a separate hub or combiner below the unit, with each lane leaving by its own outlet into it.

The unit view draws both the same way, Preston's call: the lanes leave the unit and meet in a hub box below it, with the unit's head-on art above it when the unit has one. The field records the hardware for the docs, the unit art and the overview; it does not change the drawing.
- **none**: one lane, so nothing to merge.

| Combiner | Units |
|----------|-------|
| internal | Box Turtle (open and enclosed), Bambu AMS 2 Pro, original Bambu AMS, OpenAMS, YAMMU, Creality CFS, Night Owl, ERCF and Tradrack (selectors) |
| external | ACE Pro and ACE Pro 2 (each ACE under multiACE too), QIDI Box, HTLF, AD5X IFS, Elegoo Canvas, EMU, BTT ViViD, BMCU, MMX, 3MS, KMS, Quattro Box, Bambu AMS Lite |
| none | AMS HT (one lane) |
| no combiner at all | Toolchangers, the Snapmaker U1's SnapSwap, INDX, MedusaHC: every lane runs to its own head |

**A unit not in this table: ask Preston, don't guess.** `build.py` refuses a unit that leaves `combiner` unset.

The combiner's presence sensor is optional. The hub box draws its sensor band only when the backend reports a hub sensor for that unit (`AmsUnit::has_hub_sensor`); the AD5X IFS has none, only the toolhead sensor. Lanes bound for separate extruders never meet a combiner: a PARALLEL path draws none, and on a MIXED path only the lanes that share an extruder converge. A unit whose selector slides (ERCF, Tradrack) keeps its rail in the selector's place whatever its combiner. The combiner shows only in the unit view. On the AMS overview a unit's own combiner is implicit, since the unit card stands for the merge of its lanes, and a hub box appears only where two or more routes meet (`nozzle_feed` in `src/ui/filament/ui_system_path_plan.h`). The rule is `helix::ui::fpath::combined_topology` (`src/ui/filament/ui_filament_path_plan.h`); `docs/devel/FILAMENT_PATH_CANVAS.md` § "Topology Renderers" says how the canvas draws each case.

## Rendering on small boards

The shell is static between state changes, so it is drawn once and kept, and only spools repaint on data changes. The bar for the full tier is no visible frame drop while a spool repaints and no more than 2 points of added idle CPU on MIPS; on the AD5X the ACE shell measured 2.50% idle against 2.48% for the generic tray. A theme change repaints the shell from the new tokens without leaving the screen. Boards where `helix::ui::reduced_effects()` is true skip the drying glow and spool rotation. A printing lane turns slowly and a loading or unloading lane spins fast, both only with animations on (`decide_spool_spin` in `include/spool_spin.h`); the vents step through cached phases, so a step is a buffer copy.

## Toolheads

Toolheads use the same kit. `scripts/unit_art/heads/<id>.py` defines a head with its nozzle tip and filament entry, and `build.py` bakes a body, a tint mask and a shade mask at two heights (`full` 160 px and `path` 72 px) into `assets/images/toolheads/<id>/`. `src/rendering/toolhead_art_gen.cpp` records each head's aspect, tip, entry and height share, so heads keep their real relative sizes and the filament enters at the head's fitting. `toolhead_art_for(style)` returns the art, or nullptr when a style keeps its vector renderer. Styles are named in `include/toolhead_style.h`, and vendor names for them live in that module.

## Adding a unit

Follow `.claude/skills/unit-art/SKILL.md`. In outline:

1. Collect photos for the proportions. Keep them out of git.
2. Write `scripts/unit_art/units/<id>.py` with a `UnitDef` and a `TrayShellDef`, set its `combiner` from § "Combiner" (ask Preston for a unit not in the table), and add it to `UNITS` in `build.py`.
3. Add a row to `kShellRows` in `src/ui/filament/ams_unit_shell.cpp`.
4. Run `make unit-art` and commit the baked layers and the regenerated `src/generated/ams_unit_shells_gen.cpp`.
5. Run `python3 -m pytest tests/python/test_unit_art_kit.py -q` and `make t F='[ams][tray],[unit-art]'`, then check `build/unit-art/contact_sheet.png` on dark and light at real size.
6. Look at it in the mock with the unit's `HELIX_MOCK_AMS` value (`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`).

The baked layers add to install size (about 6 MB across all units at present), so a new unit's `full` and `thumb` sizes are worth checking before it is committed.
