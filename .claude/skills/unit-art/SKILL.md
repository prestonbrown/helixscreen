---
name: unit-art
description: Use when adding or changing the drawing of a filament unit, dryer or multi-material system (AMS, ACE, CFS, Box Turtle, ERCF, Tradrack and the like), its thumbnail, or its shape on the live AMS tray.
---

# Unit Art

A unit is defined once in `scripts/unit_art/units/<id>.py`. `make unit-art` turns it into baked layers in `assets/images/units/<id>/` and a row in `src/generated/ams_unit_shells_gen.cpp`. Devices never run the kit.

## Before drawing

1. Get real photos: a three-quarter view and a front view. Keep them out of git: they are usually vendor marketing images. The local gitignored docs/superpowers folder is the place.
2. Read proportions off the photos: width to depth to height, and spool diameter against width. Spools must stay inside the body unless the photo shows otherwise.
3. Note what changes between builds. DIY units get one default colour scheme, chosen with Preston.

## What a unit defines

| Piece | Rule |
|---|---|
| `draw_art(scene, colors, tier)` | Items in draw order, back to front. `tier` is `"full"` or `"thumb"`; thumbnails drop fine detail |
| Filament | Only through `spool.draw_spool`, one `lane` each. It reads the live widget's values from the C++ |
| `scene.layer = "front"` | Everything that stands in front of the spools. A part behind them, such as a rear leg, is drawn before them |
| Side profile | One list of `(y, z)` points. Art uses it directly; `kit.tray_profile` converts it for `dome` |
| `TrayShellDef` | `cheek` is the body's top edge; `dome` the lid, empty for an open unit; `corner` is 0 for none |
| Wordmarks | None. Text never goes in art |

Match the lid to the real unit's profile, whatever the spool shape suggests.

The camera is `kit.ART_CAMERA` for every unit, set by the bake. A unit never picks its own.

## Connecting a unit

Add a row to `kShellRows` in `src/ui/filament/ams_unit_shell.cpp`: backend slug, a lowercase part of the unit's name when one backend covers several kinds of unit, the shell id, and an enclosed variant's id if there is one. Hardware that cannot be identified gets no row and keeps its logo.

Add the unit to `UNITS` in `scripts/unit_art/build.py`.

A unit built in several lane counts sets `lane_variants` on its `UnitDef` and draws from `len(colors)`, as left cap, N lanes, right cap. Each count bakes to `assets/images/units/<id>/<n>/`. Lane counts without a baked set keep the live tray shell and lose only the picture.

A unit with a moving selector has no spools: it draws one `lane` stub per gate and puts the moving part in `scene.layer = "carriage"` at gate 0, and sets `carriage_step` on its `UnitDef`. The build writes a `CarriageTrack` row per gate count for the widget. `rail_face` gives its colours for the flat rail the path screen draws in place of the selector bar. `scripts/unit_art/units/rail.py` is the model.

## Tray rules

The tray shares the app's flat camera and draws real spool widgets between `tray_back_draw_cb` and `tray_front_draw_cb`. Depth barely shows there, so the end caps carry the side profile. Parts behind the spools belong in the back callback. Left cap, N lanes, right cap; lids stretch across the row.

## Verify

```bash
python3 -m pytest tests/python/test_unit_art_kit.py -q
make t F='[ams][tray],[unit-art]'
```

Then look at `build/unit-art/contact_sheet.png` beside the photos, on both the dark and light rows, at real size. Check each of these:

- Do the spools fit inside the body?
- Do parts behind the spools sit behind them?
- Does the lid outline match the photo's?
- Are the thumbnail's colours readable?

To see a unit in the app, run the mock with its `HELIX_MOCK_AMS` value. `HELIX_MOCK_AMS=ace` gives an ACE; `afc` gives a Box Turtle, and `HELIX_MOCK_DRYER=0 HELIX_MOCK_AMS_ENV=off` shows it without an enclosure. Only `make` rebuilds the app: `test-host-run.sh exec --build` builds only the tests.

## Common mistakes

| Symptom | Cause |
|---|---|
| Spools overflow the body | Spool radius or depth position not taken from the photo |
| Rear leg in front of a spool | Drawn in the front layer |
| A colour bleeds through the shell | Filament drawn without its `lane`, or shell drawn with one |
| `test_generated_shell_table_is_current` fails | `make unit-art` not rerun, or generated table edited by hand |
| Theme golden fails after a new token | Run `make regen-tokens`, then update `tests/fixtures/theme_const_golden.txt` |
