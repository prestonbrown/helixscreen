# Printer Images

Shipped artwork for the printers HelixScreen knows about. Three places draw it: the
first-run configuration wizard (`src/ui/wizard/ui_wizard_printer_identify.cpp`), the Printer
Image picker overlay (`src/ui/printers/ui_overlay_printer_image.cpp`), and the home panel's
printer widget (`src/ui/panel_widgets/printer_image_widget.cpp`).

85 PNGs, ~27 MB. PNG is the only format here; nothing loads a `.jpg` or `.webp` from
this directory.

## Which printer gets which image

`assets/config/printer_database.json` owns that mapping — each entry's `image` field
names a file in this directory, and an optional `image_variants` list names alternates
chosen by detected hardware (a K2 with a CFS on top shows `creality-k2-cfs.png`, a K2 Pro
`creality-k2-pro-cfs.png`;
`docs/devel/PRINTER_MANAGER.md` § Image Lookup). This README does not list them, because a
hand-maintained list drifts from the database silently.

A variant replaces the plain `image` when every condition in its `when` holds; the first
matching variant wins, and none matching leaves the plain image. Three conditions exist:
`filament_system`, which holds when a filament system of that kind (`cfs`, `afc`, ...) has
at least one unit connected, so a backend that has registered but heard from no unit yet
does not count; and `enclosure_top` (`true`/`false`), which follows the firmware's report
of a removable top (a Snapmaker U1 with its top module fitted); and `chamber_heater`
(`true`/`false`), whether the printer has a chamber heater. Two variants naming the same
image show it when either holds. The home widget switches images live as units report in
or drop out, as a top is fitted or removed, and as a heater is assigned. A
user's own pick in the image picker always wins over both.

```bash
scripts/check_printer_images.py --list   # entries whose image is not on disk
```

That gate fails the build when the database names art that does not exist. It matters
because a missing image is otherwise invisible: `get_prerendered_printer_path()` falls
through to `generic-corexy` and the user just sees a CoreXY frame for their bed-slinger.

## How an image reaches the screen

`src/system/prerendered_images.cpp#get_prerendered_printer_path` resolves in this
order, degrading rather than failing:

1. `prerendered/<name>-<300|150>.bin` — LVGL binary, LZ4-compressed, no PNG decode
2. `<name>.png` — this directory, decoded at full resolution by lodepng
3. `generic-corexy` at the same tier, then its PNG

The tier comes from `src/system/prerender_size_class.cpp#get_printer_image_size`:
screen width >= 600 selects 300px, narrower selects 150px.

**Step 2 is the expensive one.** These PNGs are large (see below), LVGL's image cache
is disabled (`LV_CACHE_DEF_SIZE 0` in `lv_conf.h`), and the widget draws with
`inner_align="contain"`, so a miss means decoding the full-resolution bitmap and
scaling it at draw time. Measured on a K1C: 1347 ms to first paint from
`creality-k1c.png`, 78 ms from `creality-k1c-300.bin`.

Note that the fallback in step 2 logs at `trace` while step 1 logs at `debug`, so a
device silently taking the slow path shows no `[Prerendered]` line at debug level.

### Generating the renders

Full render and packaging pipeline (size classes, per-platform pruning): `docs/devel/PRE_RENDERED_IMAGES.md`.

```bash
make gen-printer-images      # -> build/assets/images/printers/prerendered/*.bin
make list-printer-images     # what it would write
```

Build artifacts, not committed. Every `package-*` target depends on this, and the
`deploy-*` targets generate it when the output directory is empty — so a cross-built
device gets the tiers. A plain dev checkout, `make install`, and the Android build do
not, and take the PNG path.

`scripts/platform_manifest.py#prune_assets` then drops the tier a fixed panel cannot
select, and drops the source PNGs only when every database entry has a render at the
kept size.

## Image specifications

- **Format:** PNG, RGBA or palette
- **Background:** transparent preferred
- **Content:** full printer view, cropped to it: no transparent margin, no aspect padding.
  `scripts/trim_printer_images.py` crops every PNG here (and remaps `regions.json`);
  the pre-commit gate runs its `--check`

**Dimensions are not currently standardized.** The shipped set ranges from 146x176 to
1929x1869. New art should land near the low end: the 300px tier is the largest any panel
asks for, so pixels beyond ~800px on the long edge cost disk, RAM and decode time on
every platform that falls back to the PNG, and buy nothing on the ones that do not.

```bash
magick input.jpg -resize 800x800 -background none -gravity center output.png
```

## Adding new art

1. Source a product photo, alpha-cut the background
2. Resize per above, save here with a `vendor-model` filename
3. Point the `printer_database.json` entry's `image` field at it
4. `scripts/trim_printer_images.py` to crop it to its content
5. `scripts/check_printer_images.py` to confirm it resolves

For a variant (the printer with optional hardware attached), save the art as
`<plain image>-<hardware>.png` (`creality-k2-cfs.png`), run steps 4 and 5, tag it in
`regions.json` under its own name (below), and add it to the entry's `image_variants`
instead of replacing `image`:

```json
"image_variants": [{"when": {"filament_system": "cfs"}, "image": "creality-k2-cfs.png"}]
```

The `[printer_database][schema]` test fails on a condition key or value the app does not
know, since such a variant would never match.

## Tagging parts for live callouts

The home panel's printer widget pins live chips (nozzle, part fan, chamber, light, bed) onto
the image at points hand-tagged in `assets/images/printers/regions.json`:

```json
"creality-k1c": {"size": [947, 1188], "nozzle": [0.522, 0.276], "part_fan": [0.48, 0.202],
                 "chamber": [0.184, 0.377], "light": [0.197, 0.159],
                 "bed": [[0.175, 0.572], [0.688, 0.574]]}
```

- Points are normalized 0..1 over the source PNG. The prerendered tiers and the exact-size
  cache are aspect-preserving, centred resizes of that PNG, so one set of numbers holds at
  every rendered size.
- `bed` is the plate's near edge as seen in the picture: left end, right end.
- `size`, `nozzle` and `bed` are required; `part_fan`, `chamber` and `light` are optional -
  omit whichever the printer doesn't have.
- `size` is the source PNG's width and height. `tests/unit/test_printer_image_regions.cpp`
  (`[regions]`) fails, naming the image, when a PNG no longer matches its recorded size.
  `scripts/trim_printer_images.py` rewrites the entry when it crops; **re-cropping a tagged
  PNG by hand needs re-tagging**, because every point on it shifts silently otherwise.

### Tagging tool

```bash
python3 -m http.server -d . 8000
# open http://localhost:8000/tools/printer-regions-tagger.html
```

Tap-to-tag: nozzle tip, part fan, bed near-left corner, bed near-right corner, an empty spot
inside the enclosure, the light. Part fan, chamber and light can be skipped. A review step
renders chips at the tapped points before saving. Progress is kept in the browser; paste the
tool's output over `regions.json` to ship. Images are listed in telemetry popularity,
refreshed by hand, then alphabetically.

### Tags users make on the device

Tag parts in the printer image picker runs the same six prompts on the screen, for the image
the home widget shows, and writes `<config dir>/printer_image_regions.json` in this same
format. Its entries override `regions.json` for their key only: a shipped image by basename,
a custom image as `custom:<name>`. Their `size` is the natural size of the image that was
tagged (the prerendered tier or imported `.bin` the widget draws, not the source PNG), and an
entry whose `size` no longer matches that image (another screen tier, or re-cut art) is
ignored until it is re-tagged. Importing a custom image, or deleting it, clears its tags,
since a new photo of the same aspect has the same size. Reset tags deletes the entry.

## Custom images

Users add their own without touching this directory:

1. Drop a PNG/JPEG/BMP/GIF into `config/custom_images/` in the install directory
2. Open the picker (Home Panel -> tap printer image, or Settings -> Printer; then tap the image in the Printer Manager)
3. Custom images appear under the "Custom" section

**Limits:** 5 MB, 2048x2048 (`MAX_FILE_SIZE` / `MAX_IMAGE_DIMENSION` in
`src/system/printer_image_manager.cpp`). On first picker open,
`#PrinterImageManager::auto_import_raw_images` converts each raw file to `-300.bin`
and `-150.bin` beside it, so custom art skips the PNG decode that shipped art hits.

Selection is stored per printer at `/printers/<id>/printer_image` as `"custom:<filename>"`
(no extension); shipped art uses `"shipped:<name>"`, and an unset value means auto-detect
from the detected printer type and its `image_variants`.
