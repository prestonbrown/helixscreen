# temp-spark

The reference HelixScreen Lua plugin: one heater's temperature as a home tile
with a 30-sample sparkline. Copy this directory to start your own plugin; every
HelixScreen plugin API it uses appears once, in the shortest honest form.

The author guide is [docs/devel/PLUGIN_DEVELOPMENT.md](../../docs/devel/PLUGIN_DEVELOPMENT.md).

## What it demonstrates

- `helix.subject.int` / `helix.subject.string`: the plugin's whole output is
  subjects; XML binds to them and Lua only sets them.
- `helix.moonraker.call`: the read-only `server.temperature_store` query seeds
  the window with Moonraker's own history (no permission needed).
- `helix.printer.watch` / `helix.printer.get`: live heater readings. There is
  no unwatch, so all three heaters are watched once and filtered by selection.
- `helix.timer.every`: one sample per `interval_s`, not one per status change.
- `helix.settings.get` / `helix.settings.on_change`: heater, interval and
  show_target, declared in the manifest's settings schema.
- `helix.ui.on` + `helix.ui.overlay`: the tile's `plugin_event` opens the
  detail overlay; unload closes it automatically.
- `helix.canvas`: `polyline`, `line`, `rect`, `circle`, `text`, `size`,
  `commit`, `on_size`: one shared draw function renders the sparkline on the
  tile and, with the detail flag set, the gridlines, area fill, newest-sample
  dot and dashed target trace in the overlay. The fill uses the `opa` option.
- `helix.log.warn`: failures degrade to an empty window instead of faulting.

## Run it

```sh
HELIX_PLUGIN_DIR=examples/plugins ./build/bin/helix-screen --test -vv
```

Then Settings > Plugins > Temperature Sparkline > enable (it asks for no
permission), and add the tile from the home panel's widget catalog.
The 2x1 tile needs room: remove or shrink a stock widget first, then add it from the catalog's Plugins category.

## Layout

- `manifest.json` id, one 2x1 widget, empty permissions, three settings.
- `main.lua` subjects, backfill, sampling timer, heater switching.
- `ui/temp-spark__tile.xml` heater glyph and label, current value with the
  target muted beside it, above a canvas sparkline.
- `ui/temp-spark__detail.xml` overlay with a larger canvas carrying labelled
  gridlines and the dashed target line, plus min/max/avg/target stats.

The plot autoscales to the window's own span, widened to at least 10 degrees
and padded 20%, so idle jitter stays a wiggle and a heating curve fills the
height. The overlay's plot also folds a nonzero target into the range, keeping
its dashed line inside; the tile's does not, so its span is the data's own.
