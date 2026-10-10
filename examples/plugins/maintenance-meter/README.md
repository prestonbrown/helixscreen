# maintenance-meter

Hour meters for consumable parts - a HEPA filter, a nozzle, linear rails -
as one adaptive home tile. Elapsed hours accrue from print time, persist on
the screen, and survive restarts; a ring gauge shows the meter closest to
due, and the tile earns information as it grows.

The author guide is [docs/devel/PLUGIN_DEVELOPMENT.md](../../docs/devel/PLUGIN_DEVELOPMENT.md),
and the step-by-step build of this plugin is the tutorial:
[docs/devel/PLUGIN_TUTORIAL.md](../../docs/devel/PLUGIN_TUTORIAL.md).

## What it demonstrates

- `helix.storage.get` / `helix.storage.set`: the whole state under one key,
  every field validated on load with a fallback, because the store is
  user-editable bytes on disk. This is the plugin's one permission.
- `helix.printer.watch` + `helix.timer.every`: the printer state arrives by
  watch, the clock by timer; a tick while printing accrues one interval to
  every enabled meter. Crossing the interval fires a due toast once - a
  `notified` latch in the store re-arms only on reset.
- `helix.settings.get` / `helix.settings.on_change`: intervals are declared
  settings; an interval of 0 hides that meter everywhere, through a
  per-meter visibility subject the tile rows and overlay cards both bind.
- `helix.ui.confirm`: a reset discards accrued hours, so it asks first;
  the stamp it records is the screen's total print hours - the sandbox has
  no wall clock, and hours-of-printing is the clock this plugin keeps.
- `helix.canvas` arcs: the ring draws as 10-degree segments with both
  angles normalized into [0, 360] and one degree of overlap closing each
  seam; the stroke width scales down with the radius so a micro cell does
  not carry a dashboard-sized band.
- The adaptive size ladder: one component, authored at 1x1 and resizable
  to 4x2, tiers its content on the pixels `on_size` reports - a micro 2x1
  is narrower than a medium 1x1, so thresholds key on pixels, not cells.

## Run it

```sh
HELIX_PLUGIN_DIR=examples/plugins ./build/bin/helix-screen --test -vv
```

Then Settings > Plugins > Maintenance Meters > enable (it asks to keep its
own data on the screen), and add the tile from the home panel's widget
catalog, under the Plugins category. To watch it accrue, run with
`HELIX_MOCK_AUTO_PRINT=1 --sim-speed 6`: a print starts within seconds and
each minute of it lands in the meters. Seed interesting values by writing
`plugin-data/maintenance-meter.json` in the config directory - set a
meter's `used` just under its `interval` to see the due toast and the
warning color.

## Layout

- `manifest.json` id, one 1x1-to-4x2 widget, the `storage` permission,
  three interval settings.
- `main.lua` subjects, the store, the accrual tick, settings reactions,
  the size ladder, reset confirmation.
- `ui/maintenance-meter__tile.xml` header, dial zone (ring canvas with the
  value label centered over it), per-meter list zone, bars strip.
- `ui/maintenance-meter__detail.xml` one card per meter with its dial and
  reset button, and the screen total.

Centered text is a label, never canvas text: the sandbox has no font
metrics, and a label with `align="center"` is positioned by the layout
engine that owns the font. When the ring is too small for the number, the
number is dropped rather than relocated - the caption keeps the meter
name, the ring carries the proportion, and the exact hours are one tap
away.
