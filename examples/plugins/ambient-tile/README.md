# ambient-tile

Room temperature and humidity from Home Assistant or any JSON endpoint, as
one adaptive home tile with a history overlay. The plugin polls on an
interval, resolves the readings out of the response, and shows them with
their age - a failing source is a status line and a growing age, never stale
data presented as fresh.

The author guide is [docs/devel/PLUGIN_DEVELOPMENT.md](../../docs/devel/PLUGIN_DEVELOPMENT.md).
This is the `http` **reading** example (print-watcher is the `http` sending
one): `http.get` against untrusted input, `helix.json.decode`, pointer
resolution, polling cadence with `helix.timer.every`, and a canvas sparkline
fed from a plugin-side window.

## What it demonstrates

- `helix.http.get`: the one permission. Two source shapes - a Home Assistant
  instance (base URL, long-lived access token, one entity per reading via
  `/api/states/<entity>`) or any JSON endpoint (URL plus a dot-separated
  pointer per reading, numbers indexing arrays from zero). Every destination
  is validated to its shape before anything reaches the network.
- Untrusted input discipline: the body is decoded with `helix.json.decode`
  (which cannot raise on bad JSON), each pointer walk is nil-checked at every
  step, and a non-numeric value is refused - a malformed source becomes a
  status line, never a fault.
- The age line without a wall clock: the sandbox has none, so a one-second
  `helix.timer.every` tick counts from the last good reading and re-renders
  only when the wording changes ("Updated just now" ... "Updated 3m ago").
- The failure policy: values from the last good reading stay on the tile,
  the status line carries the error ("HTTP 500", "bad data",
  "not configured"), and the age line keeps growing - the stale reading is
  always visibly stale.
- A canvas sparkline from a plugin-side window: samples accumulate in memory
  (capped), the tile draws them at two-row tiers, the overlay at a larger
  size, and `canvas:on_size` hands the drawing its size - the callback's
  `(w, h)` arguments, not `canvas:size()` read back at draw time.
- The size ladder: one component, authored 1x1, resizable 4x2. Narrow tiles
  stack the two readings; wide ones put them side by side under per-reading
  icons (`thermometer`, `water`); two rows add the sparkline and the age
  line; micro heights drop the header and let the numbers carry the tile.
- `i18n/de.xml`: a translation pack covering the tile, the overlay, the
  status strings and the manifest fields - including a `%d` format specifier
  the German entry preserves.

## Run it

```sh
HELIX_PLUGIN_DIR=examples/plugins ./build/bin/helix-screen --test -vv
```

Then Settings > Plugins > Ambient > enable (it asks to connect to servers on
your network and the internet - the `http` permission), and configure the
source in its settings. For a quick live demo without a sensor, point the
JSON endpoint at a weather API:

- Source: JSON endpoint
- URL: `https://api.open-meteo.com/v1/forecast?latitude=39.74&longitude=-104.98&current=temperature_2m,relative_humidity_2m`
- Temperature pointer: `current.temperature_2m`
- Humidity pointer: `current.relative_humidity_2m`

For Home Assistant, create a long-lived access token in your HA profile
page, and use your sensor entity ids (`sensor.room_temperature`, ...). The
token is stored in the plugin's settings and never shown on the tile or in
the overlay - the source line names only the host.

## Layout

- `manifest.json` id, one 1x1-to-4x2 widget, the `http` permission, the
  source/endpoint/pointer/poll settings.
- `main.lua` pointer resolution, the two fetchers, the apply/age/window
  pipeline, the size ladder, translation adoption.
- `ui/ambient-tile__tile.xml` header, stacked and side-by-side reading
  layouts, spark zone, age line.
- `ui/ambient-tile__detail.xml` the reading overlay: current values, the
  last hour's plot, source and status lines.
- `i18n/de.xml` the translation pack.
