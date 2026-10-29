# Mock & Testing Environment Variables

Every variable that shapes the mock printer and its test harnesses: `--test` runs, replay,
mock AMS/printer personalities, forced-modal and demo-injection knobs. Audience: test writers
and CI — a device build reads almost none of them. Runtime, display, networking and logging
variables live in [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md).

These variables control the mock printer simulation, useful for development and testing without a real printer.

A variable read as an on/off flag is on for `1`, `true`, `yes` or `on` (ASCII case-insensitive, surrounding whitespace ignored) and off for anything else, including `0`, `false`, `10` or an empty value (`include/env_knobs.h#env_truthy`).

### `HELIX_AMS_GATES`

Set the number of filament gates in the mock AMS (Automatic Material System).

| Property | Value |
|----------|-------|
| **Values** | `1` to `16` |
| **Default** | `4` |
| **File** | `src/config/environment_config.cpp` (surfaced via `src/application/application.cpp`) |

```bash
# Simulate 8-slot AMS
HELIX_AMS_GATES=8 ./build/bin/helix-screen --test

# Simulate 16-slot MMU
HELIX_AMS_GATES=16 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_DOOR_OPEN`

Makes every door-named `gcode_button` (`frontDoor`, `topDoor` on the Creator 5 personas, or one added with `HELIX_MOCK_OBJECTS="gcode_button door"`) report `RELEASED`, which a Door input reads as open. Without it door buttons report `PRESSED` (closed); every other button reports `RELEASED`.

| Property | Value |
|----------|-------|
| **Values** | any value (presence is what counts) |
| **Default** | unset (doors closed) |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Exercise the door-open print-start warning and the Inputs section of Settings > Sensors
HELIX_MOCK_PRINTER=creator5_zmod HELIX_MOCK_DOOR_OPEN=1 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_WIFI_COUNTRY`

The regulatory country the mock WiFi radio reports at startup, standing in for one the device's own configuration set. With it unset the mock radio is in the world domain, so Network Settings shows the 5 GHz hint.

| Property | Value |
|----------|-------|
| **Values** | ISO 3166-1 alpha-2 code (`US`, `de`, ...); anything else reads as unset |
| **Default** | unset (world) |
| **File** | `src/api/wifi_backend_mock.cpp` |

```bash
# A radio the system already set to Germany: shown as current, left alone
HELIX_MOCK_WIFI_COUNTRY=DE ./build/bin/helix-screen --test
```

### `HELIX_MOCK_REMOTE_THUMBS`

Make the mock advertise Moonraker-relative thumbnail paths and fetch them over real HTTP, so `--test` exercises the cold-fetch pipeline instead of short-cutting it.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | unset (thumbnails resolve from local files) |
| **File** | `src/api/moonraker_client_mock_files.cpp` (advertised path), `src/api/moonraker_api_mock.cpp` (delegates to the real transfer API), served by `src/api/mock_http_file_server.cpp` |

By default the mock advertises a local cache path and `MoonrakerFileTransferAPIMock` copies the file, so download → decode → prescale → evict never runs under `--test`. That is fast and right for normal mock use, but it made the pipeline implicated by debug bundle `6F3QJLFG` unreachable without a printer (prestonbrown/helixscreen#960). With this set, paths become `.thumbs/<name>-300x300.png`, the mock delegates to the real `MoonrakerFileTransferAPI`, and `MockHttpFileServer` answers on a loopback port — so the real HTTP client, HttpExecutor workers and stb_image decode all run.

Pair with `HELIX_THUMB_CACHE_MAX_MB` to make eviction fire too.

```bash
HELIX_MOCK_REMOTE_THUMBS=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_THUMB_CACHE_MAX_MB`

Force a hard ceiling on the thumbnail cache so eviction is reachable on demand.

| Property | Value |
|----------|-------|
| **Values** | Positive integer (MB) |
| **Default** | unset (config `/cache/thumbnail_max_mb`, default 20 MB) |
| **File** | `src/print/thumbnail_cache.cpp` (`ThumbnailCache` constructor) |

Applied **after** `calculate_dynamic_max_size()` and deliberately not through it: that function clamps its result up to `kMinCacheSize` (5 MB), so a small value fed in via config gets raised straight back and eviction still never fires. Setting it below the cache's real usage (~1.7 MB for the mock's file list) makes eviction run every pass.

```bash
# Cold fetch with eviction live — the decode-vs-evict interaction from #960
HELIX_MOCK_REMOTE_THUMBS=1 HELIX_THUMB_CACHE_MAX_MB=1 \
  HELIX_CACHE_DIR=/tmp/ht ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_GCODE_SERVE`

Serve a real file's bytes from `MockHttpFileServer` instead of the tiny synthesised gcode header, reproducing big-file flows end to end at any size.

| Property | Value |
|----------|-------|
| **Values** | path to a gcode file readable by the app |
| **Default** | unset (`.gcode` requests get a synthesised thumbnail-bearing header) |
| **File** | `src/api/mock_http_file_server.cpp` |

With this set, every `.gcode` request the mock server receives returns the named file's bytes, so the whole-file preview download and the byte-range reads the tail/footer scanners issue run against a realistically sized payload. The server starts with every `--test` mock run; pair with `HELIX_MOCK_REMOTE_THUMBS=1` to route the file fetches over real HTTP as the #1706 repro does. Range headers are honoured per `apply_range`, subject to `HELIX_MOCK_RANGE_IGNORE`.

```bash
HELIX_MOCK_REMOTE_THUMBS=1 HELIX_MOCK_GCODE_SERVE=/tmp/huge.gcode \
  ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_RANGE_IGNORE`

Make the mock file server drop every `Range` header, answering `200` with the whole body: the behaviour of server forks that never implemented byte ranges.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | unset (ranges honoured: `206` slices, `416` for unsatisfiable) |
| **File** | `src/api/mock_http_file_server.cpp` (`range_ignore_enabled`, applied in `apply_range`) |

This is the server half of the Qidi Q2 single-file freeze (prestonbrown/helixscreen#1706): a range-ignoring server turned the bounded thumbnail-header fetch into a whole-file download per list entry. Set it to reproduce that class of failure against the client-side clamps, which live in `download_file_partial` / `download_file_tail` (`src/api/moonraker_file_transfer_api.cpp`) and are pinned by `tests/unit/test_moonraker_transfer_range.cpp`. The server's own Range behaviour is pinned by `tests/unit/test_mock_http_file_server_range.cpp`.

```bash
HELIX_MOCK_REMOTE_THUMBS=1 HELIX_MOCK_RANGE_IGNORE=1 \
  HELIX_MOCK_FILE_COUNT=200 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_FILE_COUNT`

Pad the mock's file listing to N entries by cycling the built-in names, so list-scale paths (per-entry metadata fetches, sort, scroll) run against a big list without shipping big files.

| Property | Value |
|----------|-------|
| **Values** | positive integer |
| **Default** | unset (the handful of built-in mock files) |
| **File** | `src/api/moonraker_client_mock_files.cpp` |

Padded entries reuse the built-in filenames in rotation, so they carry the same thumbnails and metadata; the point is the count, not the variety. Pair with `HELIX_MOCK_RANGE_IGNORE` and `HELIX_MOCK_REMOTE_THUMBS` to make each entry's metadata fetch a whole-file download, the #1706 shape at full scale.

### `HELIX_MOCK_MACRO_COUNT`

Add N macros (`MOCK_MACRO_000` onward) to the mock printer, every third with a description long enough to wrap, so the Macros panel's virtual list runs against a long list of rows of mixed height.

| Property | Value |
|----------|-------|
| **Values** | positive integer |
| **Default** | unset (the built-in mock macros only) |
| **File** | `src/api/moonraker_client_mock_objects.cpp` (`helix::sim::mock_padded_macro_names`) |

```bash
HELIX_MOCK_MACRO_COUNT=130 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_METADATA_404`

Make `server.files.metadata` and `server.files.metascan` fail with a 404, as Moonraker forks without the metadata component do.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | unset (metadata answers normally) |
| **File** | `src/api/moonraker_client_mock_files.cpp` |

With metadata unavailable, the UI falls back to reading thumbnails and layer counts straight out of the gcode file, which is the per-file path that #1706 froze. This knob forces that fallback without needing a metadata-less server.

### `HELIX_MOCK_FORCE_LOGINS`

Make `server.connection.identify` fail with `401 Unauthorized`, as a Moonraker with `force_logins` does for a client whose credential it does not accept.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | unset (identify succeeds) |
| **File** | `src/api/moonraker_client_mock_server.cpp` |

Discovery stops at the refused identify and the app shows its one Login Required message, with the button that opens the API key field.

```bash
HELIX_MOCK_FORCE_LOGINS=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_GCODE_DIR`

Add a local directory to the mock printer's gcodes root for the whole run, beside `assets/test_gcodes`: listings, metadata and every transfer see its files.

| Property | Value |
|----------|-------|
| **Values** | path to a directory |
| **Default** | unset (`assets/test_gcodes` alone) |
| **File** | `include/mock_planted_gcodes.h` |

How a `--test` run gets a sliced `.gcode.3mf` onto the print-select screen without adding it to the shared test directory. What the run uploads (a staged copy of a print) is written there too, so it lists, sizes and downloads back as on a printer.

### `HELIX_MOCK_HELIX_PLUGIN_LINK_DIR`

The `symlink_dir` the mock's `server.helix.status` reports with `HELIX_MOCK_HELIX_PLUGIN=1`: where the plugin links a modified print. A visible name (`helix_print`) is how a plugin installed on a printer whose Klipper skips dot-directories answers, and the app then stages its copies visibly too.

| Property | Value |
|----------|-------|
| **Values** | a directory name |
| **Default** | `.helix_print` |
| **File** | `src/api/moonraker_client_mock_server.cpp` |

### `HELIX_MOCK_3MF_METADATA`

Make the mock report metadata for `.3mf` files, as a printer that reads `.3mf` archives itself does. By default the mock answers a `.3mf`'s metadata and metascan with a 404, as a stock Moonraker does, and print select reads the archive instead and prints a plate as extracted G-code.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables |
| **Default** | unset (`.3mf` metadata 404s) |
| **File** | `src/api/moonraker_client_mock_files.cpp` |

### `HELIX_MOCK_3MF_CONVERSIONS`

Serve a JSON file as the result of `server.helix.3mf_conversions`, the printer-side `.3mf` converter's provenance (`{"conversions":[{"source", "source_mtime", "source_size", "plate", "output", "state", ...}]}`). Unset, the method is not found, as on a printer without the converter.

| Property | Value |
|----------|-------|
| **Values** | path to a JSON file |
| **Default** | unset (no converter) |
| **File** | `src/api/moonraker_client_mock_files.cpp` |

### `HELIX_MOCK_PLUGINS_DIR`

Serve the `config/helixscreen/plugins/` folder of the mock printer from a local directory, so a `--test` run lists and downloads plugins exactly like a printer would.

| Property | Value |
|----------|-------|
| **Values** | path to a directory (each `<dir>/<id>/<file>` is served as `config/helixscreen/plugins/<id>/<file>`) |
| **Default** | unset (the config root holds only injected files; plugin downloads are not served) |
| **File** | `src/api/moonraker_api_mock.cpp` (`MoonrakerFileAPIMock::list_files`, `MoonrakerFileTransferAPIMock::lookup_config_root`) |

Every regular file under the directory is listed in the config root under the plugin prefix, with real size and mtime, and downloads (both the partial and the to-path forms) return the fixture bytes. Point it at `tests/fixtures/plugins` or your own plugin folder to exercise the plugin sync pipeline end to end without a printer:

```bash
HELIX_MOCK_PLUGINS_DIR=tests/fixtures/plugins ./build/bin/helix-screen --test -vv
```

The mock answers `printer.objects.subscribe` itself, without the real discovery sequence, so a
`--test` run never exercises the subscription merge that folds plugin objects into the app's
union subscription; that path is covered by the real-sequence unit tests (the `[subscription]`
tag).

### `HELIX_MOCK_DISPLAY_MESSAGE`

Start the mock printer with a `display_status.message` (M117) already set, as a printer whose macros left one before the screen connected. It counts as a sent M117: it shows until the next M117 replaces it or a bare `M117` clears it, and it takes precedence over the print-start chatter. Without it, an idle mock publishes no message, and a print's preheat publishes what a typical `PRINT_START` macro shows for each step ("Homing...", "Heating bed...", "Heating nozzle...", "Leveling Gantry...", "Calibrating bed mesh...", "Purging nozzle"; `MoonrakerClientMock::print_start_display_message`).

| Property | Value |
|----------|-------|
| **Values** | any text; empty sets nothing |
| **Default** | unset (no message while idle) |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
HELIX_MOCK_DISPLAY_MESSAGE="Nozzle wiped, ready" ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_AUTO_PRINT`

Boot the mock printer straight into an active print so print-gated features can be exercised under `--test` without manually driving a print-start flow.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | unset (no auto-print) |
| **File** | `src/application/moonraker_manager.cpp` (sets `mock_auto_start_print`); consumed in `src/api/moonraker_client_mock.cpp` |

### `HELIX_MOCK_REPLAY`

Replay a captured print-start sequence through the mock client's real dispatch paths (`notify_gcode_response`, `notify_status_update`) so the full observer chain — manager wiring, MoonrakerAPI callbacks, collector — runs against real data with no printer attached. Pair with `HELIX_MOCK_PRINTER=k1` (the K1C capture's persona) and `--sim-speed` to fast-forward: a 386s capture replays in ~7s at `--sim-speed 60`.

| Property | Value |
|----------|-------|
| **Values** | path to a replay script JSON (see `tests/fixtures/k1c_flowrate_replay.json`, or `tests/fixtures/voron_trident_afc_replay.json` for a macro-driven Klipper printer whose phases arrive as `display_status` narration) |
| **Default** | unset (no replay) |
| **File** | `src/application/moonraker_manager.cpp` (env read); `src/api/moonraker_client_mock.cpp` (`arm_event_replay`) |
| **Generating** | `scripts/extract_mock_replay.py` — extracts a script from a klippy.log + app log capture pair |

When set truthy, the mock calls its normal `start_print_internal()` on connect (the same path `--print-status` uses), so `print_stats.state` becomes `printing`. Uses `--gcode-file` if given, otherwise the default test gcode. Useful for exercising any UI that depends on an active print.

```bash
HELIX_MOCK_AUTO_PRINT=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_REMOTE_PRINTER`

**Remote-screen simulation:** Forces the `moonraker_is_remote` subject to 1 in `--test` runs.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | unset (verdict derived from the live websocket endpoint) |
| **File** | `include/runtime_config.h` (`should_mock_remote_printer()`); consumed in `src/application/moonraker_manager.cpp` |

The mock client connects over loopback, which always reads as same-host — this flag makes remote-gated UI (print-status camera button, remote video playback paths) appear and behave as if HelixScreen were a remote screen.

```bash
HELIX_MOCK_REMOTE_PRINTER=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_HOME_LAYOUT`

**Published home layout:** Pre-seeds the mock Moonraker database's `helix-screen` / `home_layout` record, the one a printer's own screen publishes its home layout to.

| Property | Value |
|----------|-------|
| **Values** | Path to a JSON file holding the published payload (`schema`, `grid`, `main_page_index`, `pages`) |
| **Default** | unset (no layout published) |
| **File** | `src/api/moonraker_client_mock.cpp` (constructor); payload shape in `include/home_layout_share.h` |

Pair it with `HELIX_MOCK_REMOTE_PRINTER=1` and a fresh `HELIX_CONFIG_DIR` to watch a new remote client seed its home layout; an unreadable file logs a warning and publishes nothing.

```bash
HELIX_MOCK_REMOTE_PRINTER=1 HELIX_MOCK_HOME_LAYOUT=/tmp/layout.json ./build/bin/helix-screen --test -vv
```

#### Seeing the Adaptive Bed Mesh toggle

Adaptive bed mesh is a property of the **single** Bed Mesh pre-print option (on a
print file's detail view), not a separate row and not behind an active print.
When the printer's `pre_print_options.bed_mesh` entry declares an `adaptive_param`,
the firmware exposes `[exclude_object]`, and there is no custom
`calibration.bed_mesh_gcode` template, the one bed-mesh toggle is **relabeled**
from "Auto Bed Mesh" to **"Adaptive Bed Mesh"**. The default Voron 2.4 mock has
**no** pre-print options, so use the FlashForge AD5M mock (which ships
`pre_print_options` incl. `bed_mesh` with `adaptive_param: "ADAPTIVE"`, and is also
the load-cell-probe demo printer):

```bash
HELIX_MOCK_PRINTER=ad5m ./build/bin/helix-screen --test -vv
```

Then open a print file, tap a file to reach its detail view, and look in the
**PRINT OPTIONS** card: the bed-mesh row reads **"Adaptive Bed Mesh"**. Enabling
it makes the print-start emit `SKIP_LEVELING=0 ADAPTIVE=1` on the `START_PRINT`
invocation. On a non-adaptive printer the same row reads "Auto Bed Mesh" and
behaves exactly as before.

#### Seeing the Adaptive Bed Mesh toggle

Adaptive bed mesh is a property of the **single** Bed Mesh pre-print option (on a
print file's detail view), not a separate row and not behind an active print.
When the printer's `pre_print_options.bed_mesh` entry declares an `adaptive_param`,
the firmware exposes `[exclude_object]`, and there is no custom
`calibration.bed_mesh_gcode` template, the one bed-mesh toggle is **relabeled**
from "Auto Bed Mesh" to **"Adaptive Bed Mesh"**. The default Voron 2.4 mock has
**no** pre-print options, so use the FlashForge AD5M mock (which ships
`pre_print_options` incl. `bed_mesh` with `adaptive_param: "ADAPTIVE"`, and is also
the load-cell-probe demo printer):

```bash
HELIX_MOCK_PRINTER=ad5m ./build/bin/helix-screen --test -vv
```

Then open a print file, tap a file to reach its detail view, and look in the
**PRINT OPTIONS** card: the bed-mesh row reads **"Adaptive Bed Mesh"**. Enabling
it makes the print-start emit `SKIP_LEVELING=0 ADAPTIVE=1` on the `START_PRINT`
invocation. On a non-adaptive printer the same row reads "Auto Bed Mesh" and
behaves exactly as before.

### `HELIX_MOCK_WEBCAMS`

The webcam list the mock publishes at discovery, as `Name[:service]` entries
separated by commas. Unset, the mock presents one unnamed MJPEG feed, which is
what a printer with a single stock webcam looks like and leaves the camera
widget's **Source** picker with nothing to offer. A service other than an MJPEG
family (`mjpegstreamer`, `ustreamer`) makes that entry snapshot-only, exactly
as discovery treats a WebRTC or HLS camera on a real printer. Each entry gets
its own `/webcamN/` path so the `[CameraWidget] Stream started (camera='...')`
log line shows which one a view is streaming. The mock serves no frames, so the
widget stays on "Connecting Camera..." either way; the log line is the evidence.

```bash
# Two MJPEG cameras plus a WebRTC one: open the camera widget's gear icon in
# edit mode and the Source row lists Nozzle, Bed and Chamber ("Snapshot only")
HELIX_MOCK_WEBCAMS="Nozzle,Bed,Chamber:webrtc-go2rtc" ./build/bin/helix-screen --test -vv

# Restart with Bed gone: a widget saved with source=Bed falls back to Nozzle
HELIX_MOCK_WEBCAMS="Nozzle,Chamber:webrtc-go2rtc" ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_EXCLUDE_OBJECTS`

Publish a synthetic multi-object plate at mock print start, so the exclude-object
map and side list are reachable under `--test`.

| Property | Value |
|----------|-------|
| **Values** | `1` (5 objects), `2`–`12` (that many objects), `0` / `off` / unset to disable |
| **Default** | unset (only what the G-code declares) |
| **File** | `src/api/moonraker_client_mock.cpp` (read in the constructor, applied in `start_print_internal()`) |

The stock test G-codes each declare exactly one `EXCLUDE_OBJECT_DEFINE`, and the
print-status **objects** button is gated on two or more (`defined_objects.size() >= 2`),
so by default `exclude_objects_available` never becomes 1 and the feature cannot be
driven in the mock at all. When set, the mock **replaces** the parsed object list with
`n` slicer-style named objects laid out on a grid across the mock bed
(0–250 mm in X and Y, 20 mm inset), published through the same
`exclude_object.objects` status update Klipper sends — `PrinterState`,
`ExcludeObjectMapView` and `ExcludeObjectSideList` see nothing special about it.
`1` is treated as "give me a plausible plate" (5 objects) rather than one object,
since a single object would leave the button hidden.

Excluding still works exactly as in production: tapping a row or a map rect sends
`EXCLUDE_OBJECT NAME=...`, the mock's G-code handler adds it to
`exclude_object.excluded_objects`, and the row/rect re-renders as excluded.

```bash
# Boot into a printing job with 5 objects, objects button visible
HELIX_MOCK_AUTO_PRINT=1 HELIX_MOCK_EXCLUDE_OBJECTS=1 \
  ./build/bin/helix-screen --test --sim-speed 6 -vv

# 9 objects — enough to overflow the side list and force scrolling
HELIX_MOCK_AUTO_PRINT=1 HELIX_MOCK_EXCLUDE_OBJECTS=9 \
  ./build/bin/helix-screen --test --sim-speed 6 -vv
```

Confirm via the log: `Published <n> synthetic exclude_object entries`.

### `HELIX_MOCK_HELIX_PLUGIN`

Report the HelixPrint Moonraker plugin as installed.

| Property | Value |
|----------|-------|
| Values | `1` = installed, `disabled` = installed with `enabled: False`, anything else = absent |
| Default | absent |
| Affects | `server.helix.status`, and every surface gated on it |

Absent is the default because it is the state a fresh printer is in, and it is
what shows the Advanced panel's **Install HelixPrint Plugin** row. Set it to `1`
to get the **Uninstall** row instead. `disabled` is a plugin whose
`[helix_print]` section sets `enabled: False`: the component answers, but it
serves nothing, so the app reads it as not installed (Install row, install
prompt), and an install switches it back on.

Installed, the status is in the plugin's current shape: `api: 2` and a `features` map
whose `convert_3mf` entry owns its switch. With `1` it is `Ready` to
`plugin_feature("convert_3mf", 1)`; with `disabled` it reports `available: false`
(`reason: "plugin_disabled"`) and reads `Absent`. `event_journal` follows the same
pattern, and `server.helix.journal` answers with five events of the last hour (a runout,
the pause it caused, a shutdown and the job it ended), so the notification history's
Printer Events button shows the plugin's journal. `HELIX_MOCK_PLR` adds the `plr` entry
(below). Installed and not `disabled`, it also reports:

| Feature | `enabled` |
|---------|-----------|
| `print_hooks` | `helix-screen`/`plugin_features.print_hooks`, off until Settings > Printing writes it. `HELIX_MOCK_PRINT_HOOKS=unavailable` reports it `available: false`, `reason: "load_failed"` |
| `fit_check_blocks_autostart` | `plugin_features.fit_check_blocks_autostart`, off until Settings > Safety & Alerts writes it |
| `filament_accounting` | `plugin_features.filament_accounting`, off until Settings > Devices > Spoolman writes it |
| `storage_auto_cleanup` | `plugin_features.storage_auto_cleanup`, off until Settings > System > Storage writes it |
| `photos` | on unless `plugin_features.photos` is `false`. `HELIX_MOCK_PHOTOS=unavailable` reports it `available: false`, `reason: "snapshot_unreachable: ..."`, so the screen takes the photos |
| `file_analysis`, `server_edits`, `config_edit`, `storage` | always on, `Ready` |

The mock reports no `vendor_ad5x` or `vendor_ace` entry, so its AD5X and ACE backends poll as
they do without the plugin. The mock has no `server.helix.config`, so a config edit falls back
to the file API. `server.helix.filament_usage` answers a two-spool record for every job.
`server.helix.storage` answers a fixed breakdown on the mock's gcodes disk, and
`server.helix.storage_cleanup` of `helix_temp` frees its 412 MB of print copies for the
rest of the run; any other consumer frees nothing. `HELIX_MOCK_PRINT_PHOTOS` (below) gives the
newest history job its photos.

The mock answers the absent case with a JSON-RPC error, which is what Moonraker
does for an endpoint it has no component for. That distinction is the point: a
method the mock leaves unregistered invokes NEITHER callback, so the plugin
subject stays at its `-1` unknown and every surface gated on it is unreachable
in a mock run, including the pre-print options and the G-code rewrite remap.

```bash
./build/bin/helix-screen --test -vv                          # Install row
HELIX_MOCK_HELIX_PLUGIN=1 ./build/bin/helix-screen --test -vv # Uninstall row
HELIX_MOCK_HELIX_PLUGIN=disabled ./build/bin/helix-screen --test -vv # Install row, as absent
```

### `HELIX_MOCK_PRINT_PHOTOS`

Give the newest mock history job its two camera photos, linked from its
`auxiliary_data` the way the HelixPrint plugin's `photos` feature links them
(`.helix_photos/<job>_<start>_layer2.jpg` and `_end.jpg`). Downloading any path
under `.helix_photos/` serves one of two stock pictures,
`assets/test_gcodes/mock_print_photo_layer2.jpg` or `mock_print_photo_end.jpg`,
by the path's kind.

| Property | Value |
|----------|-------|
| Values | `1` = on |
| Default | off |
| Affects | `server.history.list` (the newest job only), `download_file` under `.helix_photos/` |

The history detail overlay then opens on the end photo with Preview / Layer 2 /
End tabs. Independent of `HELIX_MOCK_HELIX_PLUGIN`: the history shape is the
same whoever took the photos.

```bash
HELIX_MOCK_PRINT_PHOTOS=1 ./build/bin/helix-screen --test -vv   # Advanced > Print History > View Full History > newest job
```

### `HELIX_MOCK_PLR`

Answer the HelixPrint plugin's power-loss endpoints (`server.helix.plr.status`,
`plan`, `prepare`, `commit`, `abort`, `discard`), so the GENERIC resume offer and
wizard run under `--test` ([POWER_LOSS_RECOVERY.md](POWER_LOSS_RECOVERY.md)
§ "Generic (HelixPrint plugin)"). Needs `HELIX_MOCK_HELIX_PLUGIN=1`; with the
plugin absent or `disabled` the knob does nothing.

| Property | Value |
|----------|-------|
| Values | `generic`, `qidi_print_state` (the status `backend`); anything else = off |
| Default | off |
| Affects | `server.helix.status` (`features.plr` ready, version 2), `server.helix.plr.*`, the newest `server.history.list` job (`interrupted`) |

The status reports an eMMC host, a snapshot of `3DBenchy.gcode` at layer 42 saved
12 minutes ago, and `offer.available: true`, so the offer modal comes up at
connect. Each `plan` issues a new `plan_id`; `prepare` and `commit` refuse any
other id (`plr_stale_plan`), and `commit` refuses a missing confirmation
(`plr_missing_confirmation`). `prepare` sets the bed target to 60°C, emits
`notify_plr_progress` for `bed_wait` and `nozzle_in_place`, then for `generic`
`lift`, `nozzle_standby`, `home_xy` (and `position` at tier 1), for
`qidi_print_state` only `nozzle_standby`, 1.5 s apart, then answers. `commit` emits `z_reference`, `restore`, `bed_wait`,
`heat_nozzle`, `move_to_resume`, `start` (the plugin's step names),
starts the mock print of `3DBenchy.gcode` at 42 % and clears the snapshot.
`discard` clears it too; `abort` keeps it and turns the bed off. The feature's
`saving` (and the status `saving`) follow a `helix-screen` / `plr_user` record
written to the mock database, on when none is, so the Settings switch round-trips;
its `enabled` is `saving` or a snapshot waiting, as the plugin reports it.
`server.helix.plr.mesh_include` answers from the mock's own include flag,
with no snapshot involved, so it works without this knob; but the Settings
mesh-include row shows only while the `plr` feature is present, so driving
it under `--test` still needs `HELIX_MOCK_HELIX_PLUGIN=1` and this knob set.
The endpoint's add and remove flip the flag.

### `HELIX_MOCK_PLR_TIER`

| Property | Value |
|----------|-------|
| Values | `1`, `2` |
| Default | `2` |

The tier in the status and the plan: `1` shows the wizard's Z jog step, `2` its
confirm step, and the plan's `requires` follows (`nozzle_clear` + `z_jogged`, or
`z_confirmed`).

### `HELIX_MOCK_PLR_WARNINGS`

| Property | Value |
|----------|-------|
| Values | comma list of `mesh_unavailable`, `cold_bed`, `layer_start` |
| Default | none |

Plan warnings. `mesh_unavailable` adds `accept_no_mesh` to `requires`; `cold_bed`
reports the bed 30°C under its 60°C target, which the app reads as a cold bed and
asks `accept_cold_bed` for; `layer_start` reports `method: "layer_start"`, the
wizard's note that the print resumes at the start of the next layer, and adds
`accept_next_layer` to `requires`.

### `HELIX_MOCK_PLR_FAIL`

| Property | Value |
|----------|-------|
| Values | `plan`, `prepare`, `commit`, `plan_background`, `plan_loop`, `no_lift_room` |
| Default | none |

`plan`, `prepare`, `commit`: that request fails with `plr_klippy_error`, which
drives the wizard to its Failed step. A failed `prepare` or `commit` makes the
wizard send `abort`. `plan_background`: `plan` answers `pending`, then
`notify_plr_plan_ready` carries an `error` (a background scan that failed).
`plan_loop`: `plan` answers `pending` every time and `plan_ready` carries no error,
so the wizard's re-request comes back pending again; both end on the Failed step.
`no_lift_room`: the status withholds the offer with `blocked_by: ["no_lift_room"]`
(not enough Z headroom to lift clear of the print), so no offer appears.

```bash
# Offer at connect; tap Resume for the wizard (tier 2)
HELIX_MOCK_HELIX_PLUGIN=1 HELIX_MOCK_PLR=generic ./build/bin/helix-screen --test -vv
# Tier 1 with every confirmation the Checks step can ask
HELIX_MOCK_HELIX_PLUGIN=1 HELIX_MOCK_PLR=generic HELIX_MOCK_PLR_TIER=1 \
  HELIX_MOCK_PLR_WARNINGS=mesh_unavailable,cold_bed,layer_start ./build/bin/helix-screen --test -vv
# The Failed step
HELIX_MOCK_HELIX_PLUGIN=1 HELIX_MOCK_PLR=generic HELIX_MOCK_PLR_FAIL=prepare \
  ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_SKIP_WRAPPERS`

Behave as if `helix_skips.cfg` is loaded, so the print-detail leveling skips
([PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md) § "Skipping steps
PRINT_START always runs") can be driven under `--test`.

| Property | Value |
|----------|-------|
| Values | `1`/`true`/`yes`/`on` = loaded |
| Default | not loaded |
| Affects | `configfile` (the wrapper sections, plus `[bed_mesh]` / `[quad_gantry_level]` / `[z_tilt]` for the persona's leveling objects), the objects list, `gcode_macro _HELIX_PREP` and leveling `applied` status, the simulated print start |

QGL and Z-tilt report `applied: true` from the start, so their toggles show
without leveling first. The pre-start block's `SET_GCODE_VARIABLE` lines set the
flags, and the simulated print start prints `HelixScreen: ... skipped for this
print` in place of the step it skips, restoring the flag as the real wrapper does.
File: `src/api/moonraker_client_mock_skips.cpp`.

```bash
HELIX_MOCK_SKIP_WRAPPERS=1 ./build/bin/helix-screen --test -vv   # default persona: Voron 2.4
```

### `HELIX_MOCK_LAYER_PAUSE`

The client.cfg layer-pause macros (`SET_PAUSE_NEXT_LAYER`, `SET_PAUSE_AT_LAYER`
and the `SET_PRINT_STATS_INFO` wrapper holding their state), so the Pause at
Layer modal on the print status panel can be driven under `--test`
(`include/layer_pause.h`, prestonbrown/helixscreen#1703).

| Property | Value |
|----------|-------|
| Values | `0`/`false`/`no`/`off` = absent; anything else, or unset, = present |
| Default | present |
| Affects | the objects list, `gcode_macro SET_PRINT_STATS_INFO` status (`pause_next_layer`, `pause_at_layer`), the simulated print (pauses at the scheduled layer), `CANCEL_PRINT` (clears both) |

The macros mirror the client.cfg bodies: `ENABLE` defaults to 1 for
`SET_PAUSE_NEXT_LAYER`, and to "was `LAYER` given" for `SET_PAUSE_AT_LAYER`.
Set it to `0` to see the print status panel of a printer without them, where the
layer readout is not tappable. File: `src/api/moonraker_client_mock_layer_pause.cpp`.

```bash
HELIX_MOCK_AUTO_PRINT=1 ./build/bin/helix-screen --test --sim-speed 6 -vv   # tap the layer readout
```

### `HELIX_MOCK_MOONRAKER_VERSION`

Override the Moonraker version the mock reports in `server.info`.

| Property | Value |
|----------|-------|
| Values | any version string, e.g. `v0.8.0` |
| Default | `v0.9.3-mock` |
| Affects | `server.info` -> `moonraker_version`, Settings -> Help & About -> About, and the startup too-old warning |

The default is deliberately above `Application::kMinMoonrakerVersion` so no
`--test` run trips the warning. Set an older version to reach the warning, which
is otherwise unreachable in mock:

```bash
HELIX_MOCK_MOONRAKER_VERSION=v0.8.0 ./build/bin/helix-screen --test -vv
```

Confirm via the log: `Moonraker v0.8.0 is older than 0.9.0`.

### `HELIX_MOCK_KLIPPER_VERSION`

Override the Klipper version the mock reports in `printer.info`.

| Property | Value |
|----------|-------|
| Values | any version string |
| Default | `v0.12.0-mock` |
| Affects | `printer.info` -> `software_version`: the firmware row in Printer Manager and Settings -> Help & About -> About |

A real Kalico build reports a long `git describe` string. With `HELIX_MOCK_KALICO`
it reproduces the widest firmware row:

```bash
HELIX_MOCK_KALICO=1 HELIX_MOCK_KLIPPER_VERSION=v2026.07.00-0-g30c65e09-dirty ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_AMS`

Select the mock AMS topology/type.

| Property | Value |
|----------|-------|
| **Values** | `none`, `afc`, `toolchanger` / `tc`, `mixed`, `multi`, `torture`, `stealth`, `vivid`, `ifs`, `htlf`, `snapmaker`, `medusahc` / `medusahc-ktc` / `medusahc-tn`, `ifs-module`, `cfs`, `openams`, `multiace` |
| **Default** | The persona's own (`helix::mock::effective_mock_ams`): `toolchanger` on `creator5`, `ifs` on `ad5x`, `cfs` on `k2`, `snapmaker` on `snapmaker_u1`; Happy Hare, LINEAR, 4 slots on every other persona |
| **File** | `src/printer/ams_backend.cpp` |

| Value | Units | What it simulates |
|-------|-------|-------------------|
| *(unset)* | 1 | The persona's default AMS (see **Default**); Happy Hare, LINEAR, 4 slots where the persona has none |
| `none` | - | No mock AMS at all |
| `afc` | 1 | AFC Box Turtle, HUB, 4 slots. Aliases: `box_turtle`, `boxturtle` |
| `toolchanger` / `tc` | 1 | Tool Changer, PARALLEL topology. Alias: `tool_changer` |
| `mixed` | 3 | Box Turtle + 2x OpenAMS, 6 tools |
| `multi` | 2 | Box Turtle (4 slots) + Night Owl (2 slots), single toolhead |
| `torture` | **5** | **Its unit-card row overflows.** See below |
| `stealth` | 5 | StealthChanger-style AFC toolchanger: ACE straight to T0-T3, four hub units on T4-T6 (two share T6), 7 toolheads close together. Exercises the overview's mini-hub row |
| `vivid` | 3 | 2x Box Turtle + ViViD, 12 slots |
| `ifs` | 1 | AD5X IFS, 4 slots, LINEAR. Aliases: `ad5x`, `ad5x_ifs` |
| `htlf_toolchanger` | 2 | AFC HTLF + Toolchanger: 4 HTLF lanes (2 direct, 2 hub→shared extruder) + 3 standalone toolheads. Tests MIXED topology. Aliases: `htlf_tc`, `htlf` |
| `snapmaker` | 1 | Snapmaker U1, 4 slots, PARALLEL, non-editable mapping. Aliases: `snapswap`, `u1` |
| `medusahc` | 1 | **MedusaHC hotend changer - mock HARDWARE, real backend.** The stock Irbis3D install: controller + dock sensors, no klipper-toolchanger. Aliases: `medusa`, `mhc`. See below |
| `medusahc-ktc` | 1 | The same controller with the optional klipper-toolchanger beside it. Alias: `medusa-ktc` |
| `medusahc-tn` | 1 | A six-tool MedusaHC on the controller alone (topi's machine). Alias: `medusa-tn` |
| `ifs-module` | 1 | **Standalone AD5X IFS module - mock HARDWARE, real backend.** The Forge-X drop-in's `ifs`/`ifs_materials` objects + stock-named sensors. Aliases: `ifs_module`, `ad5x-module`. See below |
| `cfs` | 1 | **Creality CFS, K1 stock dialect - mock HARDWARE, real backend.** The stock `box` status object plus the calibration command surface. Alias: `cfs-k1`. See below |
| `openams` | 1 | **OpenAMS hub - mock HARDWARE, real backend.** Lists and pushes the `oams_manager` status object (4-bay hub unit, FPS lane, groups T0/T1/T2, slot 4 loaded) so real discovery claims OpenAMS and the production `AmsBackendOpenAms` runs. Slots 3 and 4 get ASA identity seeded into `lane_data`; `OPENAMS_UNLOAD` / `OPENAMS_LOAD GROUP=Tn` flip the loaded slot |
| `multiace` | 1 | **multiACE on a U1 - mock HARDWARE, real backend.** Pushes the U1 feed objects plus the plain `ace` object so real discovery builds the production `AmsBackendMultiAce` and runs it against engine-shaped frames. Pair with `HELIX_MOCK_PRINTER=snapmaker_u1`. See below |
| `indx` | 1 | **Bondtech INDX - mock HARDWARE, real backend.** Publishes the toolboard module, `save_variables` and Bondtech's macro objects so real discovery builds the production `AmsBackendIndx`. `T<n>` / `CHANGE_TOOL` / `PARK_TOOL` walk park and pickup over a few notifications, and heat is refused with no tool on the head or on a no-hotend tool. See below |

`HELIX_MOCK_AMS=multiace` takes two knobs (read once at start by `MoonrakerClientMock`):

| Variable | Default | Effect |
|----------|---------|--------|
| `HELIX_MOCK_MULTIACE_MODE` | `head` | The engine's mode: `head` (each unit a HUB wired to one head) or `multi` (bay s of every unit feeds head s, PARALLEL) |
| `HELIX_MOCK_MULTIACE_UNITS` | `1` | Connected ACE Pro units, 1-4 |

Head 1 starts routed to ACE 1 bay 1 (a red PLA), and `ACE_LOAD_HEAD HEAD=h [ACE=k SLOT=s]` / `ACE_UNLOAD_HEAD HEAD=h` answer by routing (or clearing) and walking the head's `channel_state` the way multiACE's shadow `filament_feed` does - an unload parks at `preload_finish`. `ACE_DRY` / `ACE_STOP_DRYING` are acknowledged; the dryer state itself is static in the mock. The frame is pinned to the real parser and backend by `tests/unit/test_ams_mock_multiace_parity.cpp` (`[multiace][mock]`).

```bash
# multiACE, head mode, one unit
HELIX_MOCK_PRINTER=snapmaker_u1 HELIX_MOCK_AMS=multiace ./build/bin/helix-screen --test

# multi mode, two units: the path canvas draws two PARALLEL units over four heads
HELIX_MOCK_PRINTER=snapmaker_u1 HELIX_MOCK_AMS=multiace HELIX_MOCK_MULTIACE_MODE=multi \
HELIX_MOCK_MULTIACE_UNITS=2 ./build/bin/helix-screen --test
```

`HELIX_MOCK_AMS=indx` takes these knobs (read when the mock builds its object list):

| Variable | Default | Effect |
|----------|---------|--------|
| `HELIX_MOCK_INDX_TOOLS` | `6` | Tools, 2-10. The last one is listed in `no_heat_tools` (a pen) |
| `HELIX_MOCK_INDX_FAULT` | unset | `seat_suspect` (T1 may be half seated), `docks_uncalibrated`, `unrecognized` (no `CHANGE_TOOL`, the "macros not recognized" state), or `kalico` (`[indx]` in configfile only, not in the object list) |
| `HELIX_MOCK_INDX_SPOOL_MACROS` | off | The #1333 reporter's shape: each `T<n>` declares `spool_id` and its template calls `SET_ACTIVE_SPOOL` |

```bash
HELIX_MOCK_PRINTER=voron_trident HELIX_MOCK_AMS=indx ./build/bin/helix-screen --test -vv
```

`HELIX_MOCK_AMS=openams` takes more knobs (all read by `MoonrakerClientMock`):

| Variable | Value | Effect |
|----------|-------|--------|
| `HELIX_MOCK_OPENAMS_UNITS` | `fleet` | Twelve units: one AMS HT (1 bay) then eleven AMS 2 Pro (4 bays), 45 bays, groups `T0`-`T44` one bay each. Units 1-10 feed lane `fps`, units 11-12 feed `fps2`, which names `extruder1` (two hubs, two toolheads). Every unit publishes its own `devices.<unit>` environment (varied temperature and humidity), unit 6 is mid dryer cycle (55 C, 3 h 25 min left), and the first bay of unit 3 (slot 5) is loaded. `OAMS_DRYER_START OAMS=<1-12>` works on every unit. Same `HELIX_MOCK_OPENAMS_API` / `HELIX_MOCK_OPENAMS_FAULT` behavior as `shared` |
| `HELIX_MOCK_OPENAMS_UNITS` | `shared` | An AMS HT (1 bay, slot 0) and an AMS 2 Pro (4 bays, slots 1-4) on ONE `fps` lane, groups `T0`-`T4` one slot each, slot 0 loaded: the shape of the reference printer. The overview draws one hub, one FPS and one toolhead. Advertises `OAMSM_LOAD_TO_TOOLHEAD` / `OAMSM_UNLOAD_FROM_TOOLHEAD` and adds the openams plugin's `lanes_by_fps` and `topology` |
| `HELIX_MOCK_OPENAMS_UNITS` | `two_lanes` | `shared`'s pair on lane `fps` (extruder), plus two AMS 2 Pro (`ams2b`, `ams2c`, slots 5-12) on lane `fps2` (`extruder1`, which the mock printer also reports). Groups `T0`-`T12`, one slot each; slot 0 loaded. The overview draws two chains, each a hub, an FPS and a toolhead. Takes the same `HELIX_MOCK_OPENAMS_API` / fault / dryer behavior as `shared` |
| `HELIX_MOCK_OPENAMS_API` | `legacy` | With `shared`: the klipper_openams shape. Only `api_version`, `schema`, `ready`, `commands` (`OPENAMS_LOAD` / `OPENAMS_UNLOAD`), `lanes`, `units`, `groups` |
| `HELIX_MOCK_OPENAMS_FAULT` | fault code, e.g. `motor_drive_fault` | With `shared` (not `legacy`): the AMS HT publishes that unit fault in `devices.ams_ht.faults[]` until `OAMS_CLEAR_FAULT OAMS=1` is sent |
| (no variable) | with `shared`, not `legacy` | Both units publish `devices.<unit>.environment` (temperature, humidity), `capabilities` (dryer range: AMS HT 45-80 C, AMS 2 Pro 45-65 C and `dryer_requires_unloaded`), `dryer` and `supported_actions`. `OAMS_DRYER_START OAMS=<1\|2> TARGET=<C> DURATION=<s>` heats the unit (state `heating`, then `holding`, counting down) and `OAMS_DRYER_STOP OAMS=<n>` returns it to `off`; both clamp like the plugin. `legacy` publishes none of it |
| `HELIX_MOCK_OPENAMS_LATE_LINKS` | any | With `shared`: ~10s after start, the spool links of slots 3 and 4 are written to `lane_data` and `notify_openams_spoolman_status` is sent, exercising the late refresh |

```bash
# Two OpenAMS units on one lane, klipper_openams shape
HELIX_MOCK_AMS=openams HELIX_MOCK_OPENAMS_UNITS=shared HELIX_MOCK_OPENAMS_API=legacy ./build/bin/helix-screen --test

# Twelve OpenAMS units on two hubs: the overview's cards row scrolls, and a tapped card opens
# the unit view on that unit's page (both hubs' stubs, the drying glyph)
HELIX_MOCK_AMS=openams HELIX_MOCK_OPENAMS_UNITS=fleet ./build/bin/helix-screen --test

# Simulate AFC Box Turtle
HELIX_MOCK_AMS=afc ./build/bin/helix-screen --test

# Simulate toolchanger
HELIX_MOCK_AMS=toolchanger ./build/bin/helix-screen --test

# Simulate mixed topology (BT + 2x OpenAMS)
HELIX_MOCK_AMS=mixed ./build/bin/helix-screen --test

# Simulate multi-unit (Box Turtle + Night Owl, 6 slots, single toolhead)
HELIX_MOCK_AMS=multi ./build/bin/helix-screen --test
```

#### `torture` - the multi-unit stress profile

Modelled on a real user rig captured 2026-08-16. **Five** units / 16 lanes / **4**
Klipper extruders:

| Unit | Lanes | Topology | Extruder |
|------|-------|----------|----------|
| Box_Turtle Turtle_1 | lane1-4 | HUB | **e0** |
| Toolchanger Tools | e1, e2 | PARALLEL | e1, e2 |
| ViViD Vivid_1 | lane5-8 | HUB | **e3** |
| EMU EMU_1 | lane9-10 | HUB | **e3** |
| Claymore HTLF_claymore_1 | lane11-14 | HUB | **e0** |

Two pairs of HUB units share a nozzle, two lanes are unmapped, and the AFC tool
aliases are neither dense nor unit-ordered (T0 and T10 are absent). Only
`torture` and `stealth` have more than 3 units. In the profiles with 3 or fewer, unit cards
shrink to `#ams_card_min_width`, so there `unit_cards_row` measures `scroll.right == 0` even at
`-s tiny`. Anything that only misbehaves once that row can scroll is
unreproducible without `torture`.

```bash
HELIX_MOCK_AMS=torture ./build/bin/helix-screen --test -vv
```

#### `medusahc` - mock hardware, real backend

Unlike every other value here, the MedusaHC modes do **not** build an `AmsBackendMock`.
`MoonrakerClientMock` seeds the Klipper objects and status a real hotend changer publishes,
and `try_create_mock()` declines these values so real discovery runs and the production
`AmsBackendToolChanger` + `toolchanger_addon` drive them. That is the whole point: it is
the only way to exercise detection, dock sensors, the feeder and the step bar outside unit
tests.

They imply `--real-ams` (`cli_args.cpp`), so no second flag is needed:

```bash
HELIX_MOCK_AMS=medusahc ./build/bin/helix-screen --test -vv
```

The three values map to the configurations in
[FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md), so detection and the swap
command route are exercised at runtime and not only in the unit tests:

| Value | Objects | Phase key | Feeder | Step bar |
|-------|---------|-----------|--------|----------|
| `medusahc` | four tools: `pin_watch io` + `medusahc` + `gcode_macro T0..3` + `extruder`..`extruder3`, MHC_macros.cfg's `SET`/`DROP_TOOL`/`OPEN`/`CLOSE`/`CLEAN` and MHC_variables.cfg's `_TOOL_OFFSET`/`_GLOBAL_STATE`, `save_variables`; no `toolchanger` | `operation`: idle/changing/dropping/picking | `feeder_open` | 4 steps |
| `medusahc-ktc` | as `medusahc`, plus `toolchanger` + `tool T0..3` | as `medusahc` | `feeder_open` | 4 steps |
| `medusahc-tn` | as `medusahc` with six tools (`T0..5`, `extruder`..`extruder5`) | as `medusahc` | `feeder_open` | 4 steps |

Every variant swaps with `T<n>` and parks with `DROP_TOOL`, the machine's own commands.

A swap advances through its phases on the simulation thread over ~7s, so they arrive as
separate status frames rather than collapsing into one update. `MHC_SET`/`SET`,
`SELECT_TOOL`, bare `T<n>`, `MHC_DROP`/`_DROP` (parks and leaves the gripper open),
`DROP_TOOL`/`UNSELECT_TOOL` (parks and re-grips) and the feeder commands are all handled.
`medusahc` and `medusahc-ktc` carry four tools, `medusahc-tn` six.

The controller's state macros are served too: `gcode_macro _TOOL_OFFSET` (`t<n>_off_x/y/z`,
written by `SET_GCODE_VARIABLE MACRO=_TOOL_OFFSET`), `save_variables` (written by
`SAVE_VARIABLE`) and `gcode_macro _GLOBAL_STATE` (`max_tool`, `target_tool`, `error_state`,
`layer`, `feeder_open`). `MHC_TOOL_OFFSET T=<n>` applies tool `n`'s Z to `gcode_move` (the
mock models Z only), `MHC_LAYER_SET L=` / `_LAYER_SET L=` sets `medusahc.layer`, and
`CLEAN` / `MHC_CLEAN` brush the mounted nozzle without changing `operation` (with the head
empty they answer `MHC_CLEAN: no tool installed`).

Pause and resume follow upstream's [recovery guide](https://github.com/Irbis3D/MedusaHC/blob/main/docs/recovery.md): a failed swap while a print is active
sets `_GLOBAL_STATE.error_state`, a manual `PAUSE` saves the mounted tool as
`target_tool`, and `RESUME` while paused runs `MHC_RESUME T=<target_tool>`. That re-picks the
target (seat the hot ends first, or the pick reads the docks as ambiguous) and continues the
print once the swap succeeds, and it refuses with `MHC_RESUME: heat T<n> above 190C before
resuming` while that nozzle is at or below 190C. The mock's secondary nozzles hold fixed
readings (extruder1 150C, extruder2 248C, extruder3 200C, the rest 25C), so a recovery to T1
is refused and one to T2 goes through. A manual pause with the target still mounted resumes
at once. `tests/unit/test_mock_medusahc_controller.cpp` pins these.

A swap's `printer.gcode.script` reply waits for the swap to end, as the controller's
`_wait_for_tool()` blocks the command: lines after the swap run only then. A failure with no
print running answers with the controller's message as an error (`MHC_SET: sensors did not
confirm T<n>`, `MHC_SET: ambiguous sensor state`, `MHC_DROP: ...`) and runs nothing after it;
during a print the failure pauses it and the reply succeeds, as `_OperationPaused` does. A
swap command sent while another runs is refused with `MedusaHC is busy: <operation>`.
Beside klipper-toolchanger (`medusahc-ktc`), `toolchanger.tool_number` follows the docks as
pin_watch's `sync_toolchanger` keeps it: the docks' tool, -1 when they name none, and left
alone mid-print.

The controller variants simulate the dock pins and derive `current_tool` from them the
way Irbis3D's pin_watch extra does: -1 when the toolhead switch is released and every dock seated, a
tool when the toolhead switch is pressed and exactly that dock is vacant, -2 for anything
else. `sensor_error` and `last_error` are a separate signal, the controller's latch: an
operation that fails sets them, and only the next operation clears them, so a hot end put
back by hand makes `current_tool` definite again while `sensor_error` stays `true`.

The default `mmu` object is suppressed in these modes - it would detect Happy Hare and
stand a second AMS backend up alongside the changer.

`HELIX_MOCK_MEDUSA_SENSOR_ERROR=1` starts as if a pick of T0 failed before the screen
connected: T0 is on the carriage with its dock switch still reading seated, so
`current_tool` is -2, and the controller holds `sensor_error:
true` with `last_error` set, so the backend raises the changer fault. A swap retried in
that state fails at once with `MHC_SET: ambiguous sensor state`, as the controller does.

`HELIX_MOCK_MEDUSA_SWAP_FAIL=<n>` makes every pick of tool `n` fail the controller's sensor
check: the hot end ends on the carriage with its dock switch still pressed, `current_tool`
-2, `sensor_error: true`, `last_error: "MHC_SET: sensors did not confirm T<n>"`, the gripper
left open, and a running print paused, as `MHC_ERROR` does.
`HELIX_MOCK_MEDUSA_REDOCK_S=<s>` then has the simulated user lift that hot end back into its
dock `s` seconds after a failure (either of the two above): `current_tool` reads -1 and
`sensor_error` stays latched, the sequence a real user produces.
`tests/unit/test_toolchanger_medusahc.cpp` (`[medusahc][mock]`) pins all three.

```bash
# Swap to T2 fails, then the hot end is docked by hand 5s later
HELIX_MOCK_AMS=medusahc HELIX_MOCK_MEDUSA_SWAP_FAIL=2 HELIX_MOCK_MEDUSA_REDOCK_S=5 \
    ./build/bin/helix-screen --test -vv
```

#### `ifs-module` - mock hardware, real backend

Same rule as the MedusaHC modes: no `AmsBackendMock` is built. The mock publishes the
standalone IFS module's objects (`ifs`, `ifs_materials`, `save_variables` with
`ifs_loaded`) plus its stock-named sensors (`filament_switch_sensor lane1..4`,
`filament_switch_sensor toolhead`), so real discovery sets `AmsType::AD5X_IFS` and the
production `AmsBackendAd5xIfs` runs its module path — detection, subscription, the frame
parse, `IFS_SET_MATERIAL` writes and the `T<n>`/`IFS_*` op dispatch all get exercised at
runtime, not only in the unit tests. The default `mmu` object is suppressed (it would
win detection over the IFS objects).

`gcode_script()` handles `T<n>`, `IFS_SELECT`/`IFS_LOAD` (slot loads), `IFS_UNLOAD`,
`IFS_EJECT` (clears the lane's presence) and `IFS_SET_MATERIAL` (updates the slot
registry), with the result published on the next status notification:

```bash
HELIX_MOCK_AMS=ifs-module ./build/bin/helix-screen --test -vv
```

`HELIX_MOCK_IFS_UNACCOUNTED=1` starts it with filament at the toolhead switch and no lane
in `ifs_loaded`: the state a firmware that lost its loaded-lane record leaves behind. The
head switch is tracked apart from `ifs_loaded` (`IFS_LOAD`/`T<n>` set it, `IFS_UNLOAD` and
ejecting the loaded lane clear it), so Unload has to ask which lane is in the head.

Two more variables stage the rest of the unknown-lane states (#1324):

- `HELIX_MOCK_IFS_LANES=<lanes>`: which lanes hold filament, as 1-based digits (`3`,
  `1,3`, `none`). Default: all four.
- `HELIX_MOCK_IFS_AT_HUB=<1-4>`: the lane the module's `ifs_at_hub` record names. Default:
  none. `IFS_LOAD`/`T<n>` set it, `IFS_UNLOAD` and ejecting that lane clear it.

`_IFS_CLEAR_EXTRUDER` empties the toolhead switch and changes no record. A script carrying
several commands, one per line, runs each.

```bash
# Unload offers lane 3, the only one holding filament, plus Clear extruder only and Cancel
HELIX_MOCK_AMS=ifs-module HELIX_MOCK_IFS_UNACCOUNTED=1 HELIX_MOCK_IFS_LANES=3 \
  ./build/bin/helix-screen --test -vv
# Unload lists lanes 1, 2 and 4, plus Clear extruder only and Cancel
HELIX_MOCK_AMS=ifs-module HELIX_MOCK_IFS_UNACCOUNTED=1 HELIX_MOCK_IFS_LANES=1,2,4 \
  ./build/bin/helix-screen --test -vv
# Unload sends IFS_UNLOAD SLOT=2 without asking
HELIX_MOCK_AMS=ifs-module HELIX_MOCK_IFS_UNACCOUNTED=1 HELIX_MOCK_IFS_AT_HUB=2 \
  ./build/bin/helix-screen --test -vv
```

#### `cfs` - mock hardware, real backend

Same rule as the modes above: no `AmsBackendMock` is built. The mock publishes the
stock K1 `box` status object (bay states, `map`, vendor/color/material arrays), so real
discovery sets `AmsType::CFS` and the production `AmsBackendCfs` runs its full path.
Pair with `HELIX_MOCK_PRINTER=k1` to latch the K1 stock dialect the calibration
surface gates on; on any other persona the backend runs but hides the calibration
section (the K2 dialect has no such commands).

`gcode_script()` answers `BOX_FIND_CUT_POS` (streams the verified terminal response
lines on a short timer: `Found cut position y: …`, `MODIFY_BOX_CFG: success, …`,
`SAVE_BOX_CFG ok: …`) and `BOX_CUSTOM_COMMAND CMD=…` (`XYZ_ZERO`, `COORDINATES_ADJUST_PREPARE`,
`COORDINATES_ADJUST_SAVE_POS`, `Y_SAFE`), with park/extrude geometry scaled off the
persona envelope so a K1 Max persona reproduces the captured 291.5/304.0 values. The
chute jog script rides the existing `SAVE_GCODE_STATE`/`G91`/`G0` simulation, so the
live Y readout moves as the overlay jogs.

```bash
HELIX_MOCK_PRINTER=k1 HELIX_MOCK_AMS=cfs ./build/bin/helix-screen --test -vv
```

`HELIX_MOCK_CFS_BOXES` lists the box addresses on the bus, e.g. `1,2,3,4` for a full chain
or `1,3` for a chain with box 2 off the bus (it shows as an absent unit). Box 1 is always
present and carries the one spool; the others are empty. Default: box 1 only. `none` is a
K2 with no CFS attached: `box` is still published and every address reads `"None"`, so
the backend runs external-spool-only and the UI shows no filament system
(`FILAMENT_BACKEND_CFS.md` § No CFS on the bus):

```bash
HELIX_MOCK_PRINTER=k2 HELIX_MOCK_CFS_BOXES=none ./build/bin/helix-screen --test -vv
```

**Multi-extruder and tool testing:** Setting `HELIX_MOCK_AMS=toolchanger` also creates multiple tool definitions and extruders in the mock environment. Multiple extruders (extruder, extruder1, etc.) and tools are auto-discovered from Klipper objects at runtime, so no separate env var is needed to control extruder count. The toolchanger mock provides a complete multi-tool, multi-extruder test environment.

**Per-tool offsets:** each `tool T{n}` serves live `gcode_x_offset` / `gcode_y_offset` / `gcode_z_offset`, seeded distinct per tool *and* per axis (T{n}: x = 0.100·n, y = −0.050·n, z = −0.025·n; T0 is zero everywhere) so a display that shows every tool the same number, or X where Z belongs, cannot look right. `SET_TOOL_PARAMETER T=<n> PARAMETER=gcode_{x,y,z}_offset VALUE=<mm>` moves one axis live and republishes only that field; `SAVE_TOOL_PARAMETER` stages it under `configfile.save_config_pending_items["tool T<n>"]`; `SAVE_CONFIG` commits and restarts; a restart (`RESTART` or `printer.restart`) reverts anything not committed. `tests/unit/test_mock_save_config.cpp` pins all of it.

**Automatic tool offset calibration:** the toolchanger persona also advertises
`gcode_macro CALIBRATE_TOOL_OFFSETS` (and `tools_calibrate`), so the Tool Offsets calibration
screen is reachable. Sending that macro through `printer.gcode.script` runs a simulated
calibration on an `lv_timer`: the rpc is answered only when the run is over (the real macro
blocks Klipper), and along the way the mock prints what the firmware prints — `Selected tool
N (TN)` per tool with a `toolchanger.tool_number` republish, the probe's `Probe made contact`
lines, `Sensor location at x,y,z` for the first tool and `Tool offset is x,y,z` for the
others — and writes each measured tool's X/Y/Z exactly as `_SAVE_TOOL_OFFSET` does (live +
staged for SAVE_CONFIG). The macro's `gcode_macro` section, `description:` included, is in
`configfile.config`, so `MacroParamCache` carries the panel's instruction text as it does every
other macro description.
`HELIX_MOCK_TOOL_CAL_FAIL=<n>` makes tool `n`'s pass fail with the probe tolerance error, for
the failure path. `tests/unit/test_mock_tool_offset_calibration.cpp` pins it.

**Snapmaker U1 toolhead offset calibration:** the `snapmaker_u1` persona lists the
`_EXTRUDER_OFFSET_ACTION_*` hook macros, so AMS Management shows **Calibration > Calibrate
Toolhead Offsets**. Each `EXTRUDER_OFFSET_ACTION_*` command answers about a second later, every
step but PRESTART is refused with the firmware's coded fault until PRESTART has run, and a
head's PROBE_CALIBRATE moves its `extruder_offset`, which `printer.objects.query` reports.
`HELIX_MOCK_NOZZLE_CAL_PLATE_REFUSE=<n>` makes the first `n` plate checks report the PEI sheet
still on the bed; `HELIX_MOCK_NOZZLE_CAL_FAIL=<n>` fails head `n`'s probe.
`HELIX_MOCK_NOZZLE_CAL_HELD=1` starts with the state already held, as another screen's run
would leave it, so the connect-time "End it now?" prompt shows.
`tests/unit/test_mock_nozzle_offset_calibration.cpp` pins it.

### `HELIX_MOCK_AMS_STATE`

Select the mock AMS visual scenario.

| Property | Value |
|----------|-------|
| **Values** | `idle`, `loading`, `error`, `bypass`, `unaccounted`, `grade`, `blocked`, `disconnected`, `feeder`, `finishes` |
| **Default** | `idle` (slot 0 loaded, slot 3 empty, others available) |
| **File** | `src/printer/ams_backend.cpp` |

| Value | What it shows |
|-------|---------------|
| *(unset)* / `idle` | Default idle state |
| `loading` | Active load in progress with realistic segment animation |
| `error` | Slot errors visible; buffer fault also shown when combined with `afc` mode |
| `bypass` | Bypass mode active |
| `unaccounted` | Filament at the toolhead that no lane accounts for (drives the print-start gate warning) |
| `blocked` | Lane 2 reports BLOCKED (jammed) with no error object, the QIDI Box shape: error-red on the lane bar, spool dot and unit card badge |
| `feeder` | Lane 3's filament waits at its feeder: its entry sensor reads it, its toolhead has not taken it (`PREP`). Shows the PARALLEL entry band with `HELIX_MOCK_AMS=snapmaker` |
| `disconnected` | The last unit reports itself offline: its overview card ghosts with a "Disconnected" chip, and the detail header carries the chip. With one unit, the AMS panel header does |
| `finishes` | One lane per swatch finish (silk, marble, sparkle, matte) plus a three-colour silk and a two-colour lane, repeating across the lanes, named for their colours and unlinked from Spoolman: every surface that paints a spool shows each (`docs/devel/COLOR_SWATCHES.md`) |
| `grade` | Every lane holds `PLA-CF` instead of its usual filament — same compat group, so the mapper routes a PLA tool exactly as before and the print-start **grade** dialog is what fires. All four lanes, not one, because a tool lands on a lane by colour and then by positional fallback over the file's whole palette |

```bash
# Show error states (slot errors + buffer fault)
HELIX_MOCK_AMS_STATE=error ./build/bin/helix-screen --test

# Show realistic loading animation
HELIX_MOCK_AMS_STATE=loading ./build/bin/helix-screen --test

# Show bypass mode
HELIX_MOCK_AMS_STATE=bypass ./build/bin/helix-screen --test

# Filled-grade lanes: drives the "Filament Grade Mismatch" print-start dialog.
# Open any PLA file (xyz-10mm-calibration-cube) and tap Print.
HELIX_MOCK_AMS_STATE=grade ./build/bin/helix-screen --test -vv

# Combine with topology selection
HELIX_MOCK_AMS=afc HELIX_MOCK_AMS_STATE=error ./build/bin/helix-screen --test
HELIX_MOCK_AMS=mixed HELIX_MOCK_AMS_STATE=loading ./build/bin/helix-screen --test
```


### `HELIX_MOCK_BATCH_FAIL_SLOT`

Make one feeder channel fail its batch operation. When the mock client simulates
a Snapmaker U1 `AUTO_FEEDING` / `AUTO_FEEDING_BATCH` line for extruder `n`, that
channel terminates in `load_fail` / `unload_fail` instead of `load_finish` /
`unload_finish`, so failure paths (the batch cursor stopping, the firmware
interlock clear) are reachable in tests and `--test` runs.

| Property | Value |
|----------|-------|
| **Values** | Extruder index `0`-`3` |
| **Default** | unset (every channel reaches its success terminal) |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Head 1 fails its load while heads 0 and 2 succeed
HELIX_MOCK_BATCH_FAIL_SLOT=1 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_DRYER`

Enable filament dryer simulation in mock mode.

| Property | Value |
|----------|-------|
| **Values** | `1` or `true` |
| **Default** | Disabled |
| **File** | `src/printer/ams_backend.cpp` |

```bash
# Enable mock dryer
HELIX_MOCK_DRYER=1 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_DRYER_SPEED`

Speed multiplier for dryer simulation (for faster testing).

| Property | Value |
|----------|-------|
| **Values** | Integer multiplier (e.g., `2` = 2x speed) |
| **Default** | `1` (real-time) |
| **File** | `src/printer/ams_backend_mock.cpp` |

```bash
# Run dryer simulation at 10x speed
HELIX_MOCK_DRYER=1 HELIX_MOCK_DRYER_SPEED=10 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_DRYER_HUMIDITY_TARGET`

Make the mock dryer take a humidity target, the way a Happy Hare 4 dryer does
(`DryerInfo::supports_humidity_target`). The environment overlay then shows its
optional **Target RH** field beside Minutes; unset, the field stays hidden, as on
Happy Hare 3 and every other dryer. The mock logs the target and runs its normal
timed cycle.

| Property | Value |
|----------|-------|
| **Values** | `1` / `true` to enable; `0` / `false` or unset to disable |
| **Default** | unset (no humidity target) |
| **File** | `src/printer/ams_backend.cpp` |

```bash
HELIX_MOCK_DRYER=1 HELIX_MOCK_DRYER_HUMIDITY_TARGET=1 ./build/bin/helix-screen --test
```

Reach the field from the AMS panel's environment indicator (`ctl demo ams`, then
`ctl click env_indicator`). Blank, it sends no target and the printer's own
default applies; the placeholder shows that default when the mock reports one
(`HELIX_MOCK_DRYER_DEFAULT_HUMIDITY`), else `auto`.

### `HELIX_MOCK_DRYER_DEFAULT_HUMIDITY`

The target, in %RH, the mock dryer reports applying to a start that names none
(`DryerInfo::default_humidity_target`), the way Happy Hare 4 reports
`heater_default_dry_humidity`. Only shown with `HELIX_MOCK_DRYER_HUMIDITY_TARGET`.

| Property | Value |
|----------|-------|
| **Values** | a positive number, e.g. `25` |
| **Default** | unset (the field's placeholder reads `auto`) |
| **File** | `src/printer/ams_backend.cpp` |

```bash
HELIX_MOCK_DRYER=1 HELIX_MOCK_DRYER_HUMIDITY_TARGET=1 HELIX_MOCK_DRYER_DEFAULT_HUMIDITY=25 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_DRYING`

Start a live *active* drying session at boot (55 °C target, 6 h session already 2 h in,
so 4 h remain) so the environment
overlay renders its drying state and the countdown actually ticks down. Uses the
real mock countdown thread, so it honors `HELIX_MOCK_DRYER_SPEED`. Requires
`HELIX_MOCK_DRYER=1`.

| Property | Value |
|----------|-------|
| **Values** | Set (any value) to start drying; unset for idle |
| **Default** | unset (dryer idle) |
| **File** | `src/printer/ams_backend.cpp` |

```bash
# Active drying, ticking at 10x, no humidity sensor, 600x480
HELIX_SCREEN_SIZE=600x480 HELIX_MOCK_DRYER=1 HELIX_MOCK_DRYING=1 \
  HELIX_MOCK_DRYER_SPEED=10 ./build/bin/helix-screen --test &
./build/bin/helix-screen ctl demo ams
```

### `HELIX_MOCK_NO_HUMIDITY`

Simulate a filament unit with no humidity sensor (e.g. a Happy Hare dryer). The
environment overlay then shows the temp-only layout instead of the temp +
humidity + comfort-ranges layout. Useful for verifying both overlay states.

| Property | Value |
|----------|-------|
| **Values** | Set (any value) to disable humidity; unset to keep humidity |
| **Default** | unset (humidity sensor present) |
| **File** | `src/printer/ams_backend_mock.cpp` |

```bash
# No humidity sensor + active dryer at 600x480
HELIX_SCREEN_SIZE=600x480 HELIX_MOCK_DRYER=1 HELIX_MOCK_NO_HUMIDITY=1 \
  ./build/bin/helix-screen --test
```

### `HELIX_MOCK_SPOOLMAN`

Enable or disable mock Spoolman integration. When disabled, `get_spoolman_status()` reports as disconnected.

| Property | Value |
|----------|-------|
| **Values** | `0` or `off` to disable; any other value keeps enabled |
| **Default** | Enabled (mock Spoolman always connected in test mode) |
| **File** | `src/api/moonraker_client_mock.cpp` (set via `src/application/moonraker_manager.cpp`) |

```bash
# Disable mock Spoolman to test "no Spoolman" scenarios
HELIX_MOCK_SPOOLMAN=0 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_SPOOLMAN_SPOOLS`

Pad the mock Spoolman inventory with deterministic synthetic spools, so search/filter
cost in the spool pickers can be measured at realistic inventory sizes. The hand-written
inventory tops out at 19 spools; real Spoolman databases run to hundreds. Padding only
extends the inventory - the curated spools the mock backends link against keep their ids
(`tests/unit/test_mock_spool_consistency.cpp` pins those).

| Property | Value |
|----------|-------|
| **Values** | integer target count (clamped to 5000) |
| **Default** | 19 (unset - no padding) |
| **File** | `src/api/moonraker_client_mock_spoolman.cpp` (`init_mock_spools`) |

```bash
# Measure picker search cost against a 300-spool inventory
HELIX_MOCK_SPOOLMAN_SPOOLS=300 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_SPOOLMAN_DB_SEARCH`

Turn off the mock Spoolman's SpoolmanDB search route (`/v1/external/filament/search`), so
it answers 404 the way a Spoolman older than 0.26.0 does. The spool wizard then hides its
catalog search.

| Property | Value |
|----------|-------|
| **Values** | `0` turns the route off; anything else, or unset, leaves it on |
| **Default** | on |
| **File** | `src/api/moonraker_client_mock_spoolman.cpp` (`MockSpoolmanServer`) |

```bash
HELIX_MOCK_SPOOLMAN_DB_SEARCH=0 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_SPOOLMAN_DB_SEARCH_LATENCY_MS`

Delay the mock Spoolman's SpoolmanDB search answers and deliver them from a worker
thread, the way a real response arrives on the WebSocket thread. Use it to watch the
spool wizard's search drop superseded answers and parse off the UI thread
(`-vv` logs each step: keystroke, request sent, parsed, rows applied).

| Property | Value |
|----------|-------|
| **Values** | milliseconds, 0-10000 |
| **Default** | 0 (answers at once, on the calling thread) |
| **File** | `src/api/moonraker_client_mock_server.cpp` (`server.spoolman.proxy`) |

```bash
HELIX_MOCK_SPOOLMAN_DB_SEARCH_LATENCY_MS=150 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_FILAMENT_SENSORS`

Configure custom filament sensor configurations for testing.

| Property | Value |
|----------|-------|
| **Values** | Comma-separated `type:name` pairs, or `"none"` |
| **Default** | Single runout switch sensor |
| **File** | `src/api/moonraker_client_mock.cpp` |

**Sensor Types:**
- `switch` - Simple on/off runout switch
- `motion` - Motion-based encoder sensor

```bash
# Multiple sensors
HELIX_MOCK_FILAMENT_SENSORS="switch:fsensor,motion:encoder" ./build/bin/helix-screen --test

# No sensors
HELIX_MOCK_FILAMENT_SENSORS=none ./build/bin/helix-screen --test
```

### `HELIX_MOCK_FILAMENT_STATE`

Set the initial state of filament sensors.

| Property | Value |
|----------|-------|
| **Values** | `sensor_name:state`, comma-separated. `state` is `detected`, `empty`, or `disabled` (Klipper reports the sensor stood down, `enabled: false`) |
| **Default** | Detected |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Start with empty filament sensor
HELIX_MOCK_FILAMENT_STATE="fsensor:empty" ./build/bin/helix-screen --test
```

The Clog Detection tile reading a plain `filament_motion_sensor` (#1823): `HELIX_MOCK_AMS=none`
keeps the default mock's Happy Hare encoder from outranking the sensor.

```bash
# OK (armed, filament moving)
HELIX_MOCK_AMS=none HELIX_MOCK_FILAMENT_SENSORS="motion:encoder" ./build/bin/helix-screen --test
# Jammed
HELIX_MOCK_AMS=none HELIX_MOCK_FILAMENT_SENSORS="motion:encoder" HELIX_MOCK_FILAMENT_STATE="encoder:empty" ./build/bin/helix-screen --test
# Disarmed
HELIX_MOCK_AMS=none HELIX_MOCK_FILAMENT_SENSORS="motion:encoder" HELIX_MOCK_FILAMENT_STATE="encoder:disabled" ./build/bin/helix-screen --test
```

### `HELIX_FORCE_RUNOUT_MODAL`

Force the filament runout guidance modal to appear even when an AMS/MMU system is present. Normally, runout modals are suppressed for AMS systems because filament runout during swaps is expected behavior.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (normal behavior) |
| **Default** | Unset (modal suppressed with AMS) |
| **File** | `src/system/runtime_config.cpp` |

```bash
# Force runout modal with real AMS system
HELIX_FORCE_RUNOUT_MODAL=1 ./build/bin/helix-screen

# In test mode, use --no-ams instead (simpler)
./build/bin/helix-screen --test --no-ams
```

**Note:** In test mode, a mock AMS is created by default (4 gates). Use `--no-ams` flag to disable the mock AMS, which enables runout modal testing without needing this environment variable.

### `MOCK_EMPTY_POWER`

Return an empty power devices list from mock Moonraker API.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Populated power device list |
| **File** | `src/api/moonraker_api_mock.cpp` |

```bash
# Simulate printer with no controllable power devices
MOCK_EMPTY_POWER=1 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_PRINTER`

Select which printer the mock Moonraker client impersonates. Drives the mock's reported identity, kinematics defaults, bed dimensions, hardware objects, and the printer type detection resolves. Each named persona is asserted to auto-detect as its printer by `tests/unit/test_mock_persona_detection.cpp`.

| Property | Value |
|----------|-------|
| **Values** | see `include/mock_persona.h#kPersonas`: currently `voron_24`, `voron_trident`, `k1`, `k1max`, `ad5m`, `creator5`, `creator5_zmod`, `generic_corexy`, `generic_bedslinger`, `multi_extruder`, `delta`, `snapmaker_u1`, `cc1`, `ad5x`, `k2`. Matched exactly (case-sensitive); an unrecognised value falls back to `voron_24` with a warning listing the valid ids |
| **Default** | `voron_24` (Voron 2.4); unset and empty both select it silently |
| **File** | `include/mock_persona.h` |

```bash
# FlashForge AD5M mock (ships pre_print_options + load-cell probe)
HELIX_MOCK_PRINTER=ad5m ./build/bin/helix-screen --test -vv

# Multi-extruder mock
HELIX_MOCK_PRINTER=multi_extruder ./build/bin/helix-screen --test -vv

# Snapmaker U1: four extruders, auto-detected, with the mock Snapmaker AMS
HELIX_MOCK_PRINTER=snapmaker_u1 ./build/bin/helix-screen --test -s tiny -vv

# Linear delta: reports kinematics=delta, so per-axis homing is hidden
HELIX_MOCK_PRINTER=delta ./build/bin/helix-screen --test -vv

# FlashForge Creator 5 Pro mock (4-head tool changer)
HELIX_MOCK_PRINTER=creator5 ./build/bin/helix-screen --test -vv

# FlashForge Creator 5 Pro on Z-Mod firmware (mock hardware, real tool changer backend)
HELIX_MOCK_PRINTER=creator5_zmod ./build/bin/helix-screen --test -vv

# Elegoo Centauri Carbon on COSMOS, at the CC1's own 480x272 screen size
HELIX_MOCK_PRINTER=cc1 ./build/bin/helix-screen --test -s micro -vv

# FlashForge Adventurer 5X with the mock IFS (4 slots)
HELIX_MOCK_PRINTER=ad5x ./build/bin/helix-screen --test -vv

# Creality K2 Plus with the CFS box
HELIX_MOCK_PRINTER=k2 ./build/bin/helix-screen --test -s 800x480 -vv
```

`cc1`: run with `-s micro`, the CC1's 480x272 screen. The persona mirrors the
bench CC1 capture (`tests/fixtures/printers/elegoo_centauri_carbon.json`): hostname
`cosmos`, a 256x265x258 CoreXY volume, the mainline `load_cell_probe` (probe type
`load_cell_probe`), the chassis switch `filament_switch_sensor filament_sensor`,
LEDs `led case` / `led hotend`, and the COSMOS macros that name the machine.

`ad5x`: hostname `ad5x-mock`, a 220x220x220 CoreXY volume, the hardware in
`assets/config/presets/ad5x.json` `hardware/expected`, the Flashforge `loadcell`
probe, and `gcode_macro SET_EXTRUDER_SLOT`, which names the machine. Its default
`HELIX_MOCK_AMS` is `ifs`, the mock IFS simulation. It publishes none of the IFS
module's own objects (`ifs`, `ifs_materials`, `zmod_ifs`, `_ifs_port_sensor_*`),
since those make discovery stand up the production AD5X IFS backend; use
`HELIX_MOCK_AMS=ifs-module` for that.

`k2`: the Creality **K2 Plus**. Hostname `K2Plus-50C1`, the capture's 352.5x400x360
CoreXY volume, and the hardware in `tests/fixtures/printers/creality_k2_plus.json` and
`assets/config/presets/k2.json` `hardware/expected`: `motor_control`, `fan_feedback`,
`load_ai`, `filament_rack`, the chamber heater `heater_generic chamber_heater` with
`temperature_sensor chamber_temp`, and fans `fan` / `heater_fan chamber_fan`. It reports the
declared 350x350 bed through `gcode_macro product_param`, which is what separates the K2 Plus
from the K2 Pro. Its default `HELIX_MOCK_AMS` is `cfs`, so the `box` object is published
and the production `AmsBackendCfs` latches the K2 `CR_BOX_*` dialect; `HELIX_MOCK_AMS=none`
removes the box. `CR_BOX_EXTRUDE TNN=T<n><bay>` loads that bay and `CR_BOX_RETRUDE` unloads
it: the next `box` frame names the bay, and the toolhead `filament_switch_sensor
filament_sensor` follows it. The script's RPC answer follows its frames in the same call, so
frame and answer arrive back to back, the tightest ordering real hardware can produce.

`snapmaker_u1`: the **Snapmaker U1**. Hostname `snapmaker-u1`, the capture's 270x270x400
Cartesian volume, heaters `extruder`..`extruder3` and `heater_bed`, the per-head fans and
`filament_motion_sensor e0_filament`..`e3_filament`, `led cavity_led`, and
`temperature_sensor cavity`, from `tests/fixtures/printers/snapmaker_u1.json` and
`assets/config/presets/snapmaker_u1.json`. It publishes the capture's identifying objects
(`fm175xx_reader`, `tmc2240 stepper_x`, `purifier`, `camera`, the `FILAMENT_DT_*` and
`EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL` macros), so detection resolves it to Snapmaker U1
and applies its preset; no printer type is saved for it. Its default `HELIX_MOCK_AMS` is
`snapmaker`, the mock Snapmaker simulation. It does not publish `filament_detect`, which
would make discovery stand up the production Snapmaker backend. It does publish
`defect_detection`, answering `printer.objects.query` with the module's stored config, whose
`main_enable` and `clean_bed.enable` follow `DEFECT_DETECTION_CONFIG MAIN_ENABLE=0|1` and
`CLEAN_BED_ENABLE=0|1`. Its `purifier` reports `power_detected: true`, the top module
fitted, so the home panel shows the U1's with-top picture; `HELIX_MOCK_ENCLOSURE_TOP=0`
reports it removed.

The `delta` persona changes the kinematics and hardware only. Its build volume is the same 0-based 250x250x300 box the other generic personas report, not a real delta's centred round bed, so it does not exercise negative coordinates or a round bed mesh.

#### The `creator5` persona

Both Creator 5 personas model the **Creator 5 Pro** (the heated-chamber model), so
`creator5` here names the mock persona, not the printer: the preset it mirrors is
`assets/config/presets/creator5_pro.json`. The mock and the shipped preset
describe the same machine: 4 extruders (`extruder`, `extruder1..3`), chamber heater
`heater_generic chamber_heater`, fans `heater_fan heat_fan` / `fan_generic fanM106`
/ `fan_generic chamber_fan` / `fan_generic chamber_loop_fan`, LED `led chamber_led`,
and one runout switch per head (`fd_ex0..fd_ex3`).

It also advertises the Creator 5 Pro fingerprint in `printer.objects.list`
(`ff_toolchange`, `gcode_button extruder_grab0..3`, `heater_generic chamber_heater`,
`gcode_macro TOOLCHANGE_PARK`, `gcode_macro BED_MESH_CALIBRATE`), so
`PrinterDetector` resolves it to **FlashForge Creator 5 Pro** at 99% confidence
rather than a generic CoreXY; without the chamber heater object it would resolve
to the heater-free Creator 5.

**The persona implies a tool changer.** A Creator 5 Pro is a 4-head changer, so
with no `HELIX_MOCK_AMS` set it selects the toolchanger backend rather than
falling back to the generic Happy Hare default: you get 4 tools mapped to
`extruder`/`extruder1..3` from `HELIX_MOCK_PRINTER=creator5` alone. An explicit
`HELIX_MOCK_AMS` always wins, so other topologies stay testable against the
persona:

```bash
# Tool changer, implied by the persona
HELIX_MOCK_PRINTER=creator5 ./build/bin/helix-screen --test -vv

# Force a different topology on the same persona
HELIX_MOCK_PRINTER=creator5 HELIX_MOCK_AMS=afc ./build/bin/helix-screen --test -vv
```

The default is the persona's `default_mock_ams` in
`include/mock_persona.h#descriptor`, applied by
`include/mock_persona.h#effective_mock_ams`, which every `HELIX_MOCK_AMS` reader
consults so they cannot disagree about what the mock is presenting.

Build volume is deliberately left at the generic mock value: the Creator 5 Pro's
real travel limits are not documented in this repo, and detection keys off
`ff_toolchange`, not the volume.

#### The `creator5_zmod` persona

The same machine running Z-Mod firmware: mock HARDWARE, not a mock backend.
Where `creator5` advertises the Reforge fingerprint (`ff_toolchange`,
`gcode_button extruder_grab0..3`) and stands the mock toolchanger up, this
persona publishes Z-Mod's own objects (`zmod`, `zmod_color`, `save_variables`,
`gcode_button extruder_pos1..4` and `extruder_grab1..4`; Z-Mod's buttons are
1-based) plus the Pro's `heater_generic chamber_heater` and the per-head sensor
pairs `fd_ex0..3` / `fm_ex0..3`, and pushes no `mmu` and no `toolchanger` object.

The point is that real discovery runs: `AmsBackend::try_create_mock()` declines
this persona (see `MoonrakerClientMock::mock_hardware_persona()`), so
`toolchanger_addon` detects the Z-Mod row and the production
`AmsBackendToolChanger` drives 4 slots against the mock's `zmod_color` status,
the same escape hatch the MedusaHC modes use. The persona implies `--real-ams`
(`cli_args.cpp`), so no second flag is needed. An explicit `HELIX_MOCK_AMS`
still wins over the persona.

The mock answers the firmware's macros: `_T_IN T=<n>` / `_T_OUT` republish
`zmod_color.active_tool_id`, and `CHANGE_ZCOLOR SLOT=<n> HEX=<hex> TYPE=<t>`
stores the slot's Material/HEX: upper-cased when the HEX is one of Z-Mod's 24
palette colours, snapped to white (`FFFFFF`, palette index 0) when it is not,
exactly as the firmware does. An unknown `TYPE` is refused with a gcode error.

It also publishes Z-Mod's calibration and chamber air macros. `CALIBRATE_EXTRUDERS`
answers with the firmware's action:prompt (plate warning, OK/Cancel) and its OK
(`_T_CALIBRATE_EXTRUDERS`) closes it; `CALIBRATE_VFA T=<n>` narrates a grab and a
finish line but does not restart (the real macro ends in `SAVE_CONFIG`);
`AIR_CIRCULATION_INTERNAL` / `_EXTERNAL` / `_STOP` set the four chamber fans to the
firmware's speeds through the mock's own `SET_FAN_SPEED`.

**Unrecognized values fall back to Voron 2.4** with a warning listing the valid set — they are not fatal. K2 and CC1 have no dedicated mock type yet and hit that fallback.

**Side effect on `settings.json`:** when this variable is set *at all* (even to an invalid value), `MoonrakerManager::init()` clears the saved printer type before detection runs, so a stale "Voron 2.4" from a previous launch cannot win over the env var. `PrinterDetector::auto_detect_and_save()` then re-resolves from the mock's reported identity on every launch. This writes to your config file — expect the persisted printer type to change.

Confirm what you got from the log line: `[MoonrakerManager] Creating MOCK client (<printer>, <n>x speed)`.

### `HELIX_MOCK_PROBE_TYPE`

Choose which Z-probe the mock printer advertises. Controls both the Klipper object added to the mock object list and the probe status payload in the initial state dispatch.

| Property | Value |
|----------|-------|
| **Values** | `cartographer`, `beacon`, `bltouch`, `loadcell`, `load_cell_probe`, `tap`, `klicky`, `standard`, `none` |
| **Default** | the persona's probe: `cartographer`, except `load_cell_probe` on `cc1` and `loadcell` on `ad5x` |
| **File** | `src/api/moonraker_client_mock_objects.cpp` |

Each value exposes the objects and full `get_status()` payload the real module publishes (`helix::sim::mock_probe_status()`; per-type table in `docs/devel/SENSOR_MANAGEMENT.md` § Probe status keys).

| Value | Objects exposed | Status detail |
|-------|-----------------|---------------|
| `cartographer` *(default on most personas)* | `cartographer`, `probe` | `cartographer`: `scan`/`touch`/`mcu`; `probe`: `last_query: 0`, `last_z_result: -0.425` |
| `beacon` | `beacon`, `probe` | `beacon`: `last_z_result: -0.312` plus Beacon's other keys; `probe`: `{name: "beacon"}` |
| `bltouch` | `bltouch`, `probe` (same payload) | `last_query: false`, `last_z_result: 0.130` |
| `loadcell` | generic `probe` | `last_z_result: 0.0`, `z_offset: null` (the Flashforge shape, as on the AD5X) |
| `load_cell_probe` | `load_cell_probe`, `probe` (same payload) | `last_query: false`, `last_z_result: 0.0` (mainline Klipper's load cell, as on the CC1) |
| `tap` / `klicky` / `standard` / anything else | generic `probe` | `last_query: false`, `last_z_result: 0.0` |
| `none` | *(no probe object)* | *(no probe status)* |

```bash
# Test the BLTouch-specific Z-offset UI
HELIX_MOCK_PROBE_TYPE=bltouch ./build/bin/helix-screen --test -vv

# Printer with no probe at all
HELIX_MOCK_PROBE_TYPE=none ./build/bin/helix-screen --test -vv
```

Unrecognized values are not rejected — they land in the generic `probe` bucket and are logged as `Mock probe: <value> (as generic probe)`.

### `HELIX_MOCK_KINEMATICS`

Override the kinematics string the mock reports in `configfile.config.printer.kinematics`. Bed-moves detection (which drives bed-slinger vs. CoreXY UI decisions) and the Belt Tension Start gate (which requires CoreXY) read this.

| Property | Value |
|----------|-------|
| **Values** | Any Klipper kinematics name (e.g. `corexy`, `cartesian`, `delta`, `corexz`) |
| **Default** | The persona descriptor's kinematics (`include/mock_persona.h#descriptor`): `cartesian` for `snapmaker_u1`, `generic_bedslinger` and `multi_extruder`; `delta` for `delta`; `corexy` for every other persona |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Force a bed-slinger layout on the default Voron mock
HELIX_MOCK_KINEMATICS=cartesian ./build/bin/helix-screen --test -vv
```

The value is passed through verbatim — no validation. A nonsense string simply produces a printer whose kinematics match nothing.

Read once, when the mock is constructed: the simulation thread reports kinematics every tick, and reading the environment there would race a `setenv()` on another thread. Set it before the mock exists.

### `HELIX_MOCK_STEPPER_RESONANCE`

Expose the `stepper_resonance_tester` klippy extra on any persona, so the generic
Vertical Artifact Compensation row (`helix::stepper_resonance_cal`) can be exercised on a
printer that is not a Z-Mod Creator 5.

| Property | Value |
|----------|-------|
| **Values** | `1` (the extra plus the stock `STEPPER_RESONANCE_FACTORY_CALIBRATE` macro), `bare` (the extra only, so the two `STEPPER_RESONANCE_SEARCH_CALIBRATE` lines are sent), `0` or unset (neither) |
| **Default** | unset |
| **File** | `src/api/moonraker_client_mock.cpp` |

The mock accepts the calibration commands without simulating a sweep; the closing
`SAVE_CONFIG` takes the mock's usual restart path.

```bash
HELIX_MOCK_STEPPER_RESONANCE=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_OBJECTS`

Append additional Klipper objects to the mock's advertised object list, so capability detection paths that depend on an object being present can be exercised without a matching mock printer type.

| Property | Value |
|----------|-------|
| **Values** | Space-separated object names |
| **Default** | Unset — only the mock printer's built-in object set |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Add a chamber temperature_fan and a generic heater
HELIX_MOCK_OBJECTS="temperature_fan chamber heater_generic chamber_heater" \
  ./build/bin/helix-screen --test -vv

# A filament dryer heater that is not the chamber: graphed as its own
# series, heats toward a target set with SET_HEATER_TEMPERATURE HEATER=filament_dryer
HELIX_MOCK_OBJECTS="heater_generic filament_dryer" \
  ./build/bin/helix-screen --test -vv

# Materialize the dragonbreath chamber-heater trio: heater, diagnostics
# object, and filter-fan output pin (drives status frames, SET_PIN
# round-trip, and a configfile max_temp of 75)
HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter" \
  ./build/bin/helix-screen --test -vv
```

**Two-word object names are reassembled by prefix.** The parser splits on whitespace, then treats a token starting with `heater_generic`, `temperature_fan`, `temperature_sensor`, or `output_pin` as the start of a *new* object and glues any following tokens onto the current one. So `temperature_fan chamber` becomes the single object `temperature_fan chamber`. A token that is not a prefix glues onto the current object — with one exception: a token that exactly names a chamber-heater backend's diagnostics object (e.g. the bare `dragonbreath` after a completed `heater_generic dragonbreath`) starts a new standalone object instead of appending. A chamber heater accepted from this list also replaces the mock profile's built-in chamber heater. Any other `heater_generic` is simulated on its own (`src/api/moonraker_client_mock.cpp#append_aux_heater_status`): it starts at 25°C with no target, steps 1°C per simulated second toward the target `SET_HEATER_TEMPERATURE HEATER=<bare name>` gives it, and reports `temperature`, `target` and `power`. Each accepted object is logged as `[MoonrakerClientMock] Added mock object: <name>`.

### `HELIX_MOCK_DETECTION_CAPABLE`

Force the K2 spaghetti-detection source's capability probe, so the Settings > Safety & Alerts detection rows and the detection loop can be exercised in a mock run. The `k2` persona detects as a K2, but no mock carries `/usr/bin/detection`, so without this the source reports incapable everywhere off a real printer. Capability normally requires `PrinterDetector::is_creality_k2()` AND `/usr/bin/detection` present and executable; the U1 source is unaffected (its capability comes from the `defect_detection` object probe).

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` reads as capable; any other value as incapable |
| **Default** | Unset: the real probe runs |
| **File** | `src/printer/k2_stock_detection_source.cpp` |

```bash
# Show the Spaghetti Detection / Pause on Detection rows in a mock
HELIX_MOCK_DETECTION_CAPABLE=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_DRAGONBREATH_FAULT`

Latch a fault into every synthesized dragonbreath status frame — the diagnostics object reports `fault: true` with a `fault_reason` instead of the nominal healthy payload. Pairs with the `HELIX_MOCK_OBJECTS` dragonbreath trio to exercise fault UI paths without hardware.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | Unset — nominal frame (`fault: false`, null `fault_reason`) |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Faulted dragonbreath chamber heater
HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter" \
  HELIX_MOCK_DRAGONBREATH_FAULT=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_DRAGONBREATH_REJECT`

Make the appliance turn every chamber target down with this reason code (the glue's
stable codes: `countermanded`, `fault_latched`, `inhibited`, `auth_failed`,
`conflict_retries_exhausted`, `heater_busy`, ...). A target arriving by
`printer.dragonbreath.set_target` or `SET_HEATER_TEMPERATURE` leaves the heater at 0, the
status object reports `protocol_error`, `protocol_error_code`, `protocol_error_seq` and
`protocol_error_target`, and the route answers `accepted: false`. Unset, the mock's route
takes the target and clears any rejection. Pair with the dragonbreath trio; to reach the
chamber prompt, also replay `tests/fixtures/u1_chamber_stall_replay.json`:

```bash
HELIX_MOCK_PRINTER=snapmaker_u1 \
HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter" \
HELIX_MOCK_DRAGONBREATH_REJECT=countermanded \
HELIX_MOCK_REPLAY=tests/fixtures/u1_chamber_stall_replay.json \
  ./build/bin/helix-screen --test --sim-speed 6 -vv
```

### `HELIX_MOCK_DRAGONBREATH_OFFLINE`

Drop the appliance off its radio link: every synthesized dragonbreath status frame reports `connected: false` instead of `true`, so the diagnostics card banners the offline state and hides its Reset button. A real link drop is brief and unpredictable (the U1 rig flaps for one or two polls roughly every 20 minutes), so this hook is how the sustained-outage path gets exercised on demand.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | Unset — nominal frame (`connected: true`) |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Chamber heater that dropped off WiFi
HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter" \
  HELIX_MOCK_DRAGONBREATH_OFFLINE=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_DRAGONBREATH_EXTERNAL`

Have the appliance drive the heater itself: every synthesized dragonbreath status frame reports `mode: "power_on"` with `source: "device"` and `lease_owned: false` - heating with neither our lease nor a klipper source, the frame shape the backend parses into the External marker. Pairs with `HELIX_MOCK_DRAGONBREATH_FAULT` (and a preset click, which brings the Device fan badge with it) to stage the widest chamber card.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | Unset - nominal frame (`source: "klipper"`) |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Chamber heater driven by the appliance's own control
HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter" \
  HELIX_MOCK_DRAGONBREATH_EXTERNAL=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_PANDA_BREATH_AUTO`

Put the stock Panda Breath into its own auto cycle: the status frame reports `work_mode: 1`, `work_on: true`, `auto_enabled: true` and a `device_target` of its own while our `target` stays 0. This is the state the appliance sits in at rest, and the only one that raises the diagnostics card's External badge.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | Unset — the appliance idles with `work_on: false` |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Stock Panda Breath holding its own auto target
HELIX_MOCK_OBJECTS="heater_generic panda_breath panda_breath" \
  HELIX_MOCK_PANDA_BREATH_AUTO=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_PANDA_BREATH_OFFLINE`

The stock counterpart to `HELIX_MOCK_DRAGONBREATH_OFFLINE`: every synthesized `panda_breath` frame reports `connected: false` while the heater section keeps answering, which is exactly what a dropped WebSocket looks like from Klipper.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | Unset — nominal frame (`connected: true`) |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Stock Panda Breath that dropped off WiFi
HELIX_MOCK_OBJECTS="heater_generic panda_breath panda_breath" \
  HELIX_MOCK_PANDA_BREATH_OFFLINE=1 ./build/bin/helix-screen --test -vv
```

The stock pair also answers the binding's drying commands with no extra variable:
`PANDA_BREATH_DRY_START TEMP= HOURS=` starts a `work_mode: 3` cycle whose
`remaining_seconds` counts down at the `--sim-speed` rate and ends by itself, and
`PANDA_BREATH_DRY_STOP` ends it. `--sim-speed 1000` plays a four-hour preset through in
about fifteen seconds, which is how to watch the chamber dryer's bed assist switch off.

### `HELIX_MOCK_KALICO`

Make the mock report Kalico-style MPC heater control instead of Klipper's PID. The extruder's `configfile` settings then carry `control: mpc` + `heater_power` rather than `control: pid` + the three PID coefficients — the discriminator HelixScreen uses to decide which tuning UI to show.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | Unset — Klipper PID (`pid_kp: 22.865`, `pid_ki: 1.292`, `pid_kd: 101.178`) |
| **File** | `src/api/moonraker_client_mock_objects.cpp` |

```bash
# Mock a Kalico (Danger Klipper) firmware with MPC heater control
HELIX_MOCK_KALICO=1 ./build/bin/helix-screen --test -vv
```

**Strict equality:** the check is `std::string(env) == "1"`. `true`, `yes` and `on` do *not* work here, unlike most other mock flags.

### `HELIX_MOCK_REMAP`

Seed a non-identity tool-to-slot mapping in the mock AMS so the two-tone slot swatch and the remap-aware filament UI can be exercised. Each pair sets the named slot's firmware tool mapping, which is what `FilamentMapper::compute_defaults()` resolves against.

| Property | Value |
|----------|-------|
| **Values** | Comma-separated `tool:slot` pairs, 0-based global slot indices (e.g. `0:3,2:1`) |
| **Default** | Unset — no firmware mapping (color/positional defaults) |
| **Files** | `src/printer/ams_backend.cpp` (reads env), `src/printer/ams_backend_mock.cpp` (`apply_remap_overrides`) |

```bash
# T0 prints from slot 3, T2 from slot 1
HELIX_MOCK_REMAP="0:3,2:1" ./build/bin/helix-screen --test -vv
```

**Applying a partial CSV clears everything first.** All slots' `mapped_tool` are reset to `-1` before parsing, so tools you did not list deterministically fall back to color/positional resolution rather than keeping a stale mapping. Malformed pairs (no colon) are skipped silently; a non-numeric side raises and aborts the rest of the parse. Each accepted pair logs `[AmsBackendMock] Remap: T<tool> -> slot <slot>`.

### `HELIX_MOCK_AMS_ENV`

Force the mock filament unit's environment-sensor mode, overriding the auto-detection that keys off whether the dryer is enabled. Determines whether the environment overlay renders at all and which sensor layout it uses.

| Property | Value |
|----------|-------|
| **Values** | `passive`, `dryer`, `slot`, `emu`, `emu_dryer`, `mixed`, `capped` (lowercased before use). Any other value leaves the unit with **no** environment sensors. |
| **Default** | Unset — auto: `dryer` when `HELIX_MOCK_DRYER` is on, otherwise `passive` |
| **File** | `src/printer/ams_backend.cpp` (applied via `AmsBackendMock::set_environment_mode`) |

```bash
# Per-slot environment sensors
HELIX_MOCK_AMS_ENV=slot ./build/bin/helix-screen --test -vv

# Explicitly no environment sensors (any unrecognized value works)
HELIX_MOCK_AMS_ENV=off ./build/bin/helix-screen --test -vv
```

Pairs with [`HELIX_MOCK_NO_HUMIDITY`](#helix_mock_no_humidity), which strips the humidity channel from whichever mode is active.

`emu`, `mixed` and `capped` each rig a specific `EnvironmentZone` shape so every zone-selector
presentation branch has something to exercise. They pair with `HELIX_MOCK_AMS=multi` (2 units,
6 lanes); `emu` also runs standalone on the default single unit.

| Mode | Pairs with | Zones | Presentation |
|---|---|---|---|
| `emu` | `HELIX_MOCK_AMS=multi` | 6 passive, one per lane over 2 units | `List` (6 > `kMaxZoneTabs`) |
| `emu` | default single unit | 4 passive lanes | `Tabs` |
| `mixed` | `HELIX_MOCK_AMS=multi` | 1 heated unit zone + 2 passive lane zones | `List` (capability split) |
| `capped` | `HELIX_MOCK_AMS=multi` | 2 heated unit zones, one Active one Queued | `Tabs` |
| `emu_dryer` | default single unit | 4 heated lanes; the dryer takes a lane selection and rotation, lane chips in the overlay and a drying badge on each slot. A cycle is running at start: lane 1 Active, lane 2 Queued, lane 3 Complete | `Tabs` |

```bash
# 6-lane EMU rig, forced to the list presentation
HELIX_MOCK_AMS=multi HELIX_MOCK_AMS_ENV=emu ./build/bin/helix-screen --test -vv

# Two heated boxes sharing one heater's worth of power: the second zone queues
HELIX_MOCK_AMS=multi HELIX_MOCK_AMS_ENV=capped ./build/bin/helix-screen --test -vv

# A heater per lane: pick lanes to dry, watch each slot's drying badge
HELIX_MOCK_AMS_ENV=emu_dryer ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_BUFFER_STATE`

Cycle the mock AFC TurtleNeck buffer through its health states, so the buffer indicator and its fault-proximity coloring can be checked without a real Box Turtle. Applies to the Box Turtle mock unit (`HELIX_MOCK_AMS=afc` and the multi-unit topologies that include one).

| Property | Value |
|----------|-------|
| **Values** | `neutral`, `advancing`, `trailing`, `fault` |
| **Default** | `neutral` (also the fallback for any unrecognized value) |
| **File** | `src/printer/ams_backend_mock.cpp` |

| Value | Reported state | `distance_to_fault` | Danger reading |
|-------|----------------|---------------------|----------------|
| `neutral` *(default)* | `Neutral` | `40.0` | 0% — exactly at the threshold |
| `advancing` | `Advancing` | `-100.0` | fault timer stopped / stale — safe |
| `trailing` | `Trailing` | `25.0` | 37.5% |
| `fault` | `Trailing` | `5.0` | 87.5% |

The fault threshold is derived from `error_sensitivity = 7.0` as `(11 - 7) * 10 = 40mm`.

```bash
# Buffer nearly at fault
HELIX_MOCK_AMS=afc HELIX_MOCK_BUFFER_STATE=fault ./build/bin/helix-screen --test -vv
```

Note that `fault` does not report a distinct state string — it reports `Trailing` with a distance deep inside the threshold, which is what a real imminent fault looks like.

### Buffer reading scenarios

Mock scenarios that put a filament buffer reading on the Filament Buffer widget, the loaded-spool card, the path box and the Buffer Status modal. Apply one with `helix-screen ctl scenario <name>` (they drive the mock AMS backend, so the whole reading chain runs). `buffer_fps*` set the pressure sensor on the mock AMS units, so they work with any `HELIX_MOCK_AMS` type but Happy Hare; `sync_feedback_tight` is Happy Hare's. The set point is 50% unless noted.

| Scenario | Reading |
|----------|---------|
| `buffer_fps` | Two-ended pressure (as an AFC `FPS_PSF` reports) 32%, below the set point: running tight, amber |
| `buffer_fps_loose` | Pressure 71%, above the set point: running loose, red |
| `buffer_fps_on_target` | Pressure 52%: balanced, neutral grey |
| `buffer_fps_danger` | Pressure 8%, pinned near the tight end: red |
| `buffer_fps_no_target` | Pressure 32% with no set point: "Pressure: 32%" as text, no slider |
| `buffer_fps_with_clog` | Pressure 32% plus AFC fault detection reporting, so the buffer reading and the clog arc show together |
| `buffer_pressure_on_target` | One-sided pressure 52% (compression only, as OpenAMS reports): "At target", neutral |
| `buffer_pressure_below` | One-sided pressure 32%: "Below target", neutral (less compression is not a fault) |
| `buffer_pressure_above` | One-sided pressure 71%: "Above target", neutral |
| `buffer_pressure_full` | One-sided pressure 97%: danger (jammed or the toolhead is not taking it) |
| `buffer_pressure_empty_loaded` | One-sided pressure 3% with filament loaded: warning (feed not engaging) |
| `buffer_pressure_empty_unloaded` | One-sided pressure 3% with no filament: neutral |
| `buffer_pressure_no_target` | One-sided pressure 53% with no set point: the gauge with no tick, "Pressure: 53%" |
| `sync_feedback_tight` | Happy Hare sync feedback at -45%, leaning to tension; labelled "Sync" |

The trace holds each reading as a step, so a scenario change shows as a step in the last minute.

### `HELIX_MOCK_THROTTLE`

Inject host throttle flags into the mock performance sampler so the performance panel's under-voltage / frequency-capped warnings can be seen without an actually-throttled Pi.

| Property | Value |
|----------|-------|
| **Values** | `freq_capped_prev` → `0x40000` "Frequency previously capped". Any other non-empty value → `0x50000` "Under-voltage detected (now)". |
| **Default** | Unset — no throttle flags |
| **File** | `src/system/mock_performance_source.cpp` |

```bash
# Under-voltage warning (any non-empty value that isn't freq_capped_prev)
HELIX_MOCK_THROTTLE=1 ./build/bin/helix-screen --test -vv

# "Frequency previously capped" instead
HELIX_MOCK_THROTTLE=freq_capped_prev ./build/bin/helix-screen --test -vv
```

Applied on every sampler tick, so the flags persist for the whole run rather than firing once.

### `HELIX_MOCK_DISK_FREE_MB`

Set the free space the mock reports for the gcodes disk, so the host-health widget's low-disk warning (below 500 MB) can be seen without filling a disk. Both the mock performance sampler and the mock Moonraker client's `server.files.get_directory` answer read it.

| Property | Value |
|----------|-------|
| **Values** | A whole number of megabytes, capped at the 29 GB mock disk |
| **Default** | Unset: 18 GB free of 29 GB |
| **Files** | `include/mock_disk_usage.h`, `src/system/mock_performance_source.cpp`, `src/api/moonraker_client_mock_files.cpp` |

```bash
HELIX_MOCK_DISK_FREE_MB=300 ./build/bin/helix-screen --test -vv
```

### `INPUT_SHAPER_DEMO_KALICO`

Swap the input-shaper panel's injected demo results for a Kalico-shaped shaper list. Kalico reports smooth shapers (`smooth_zv`, `smooth_mzv`, `smooth_ei`, `smooth_2hump_ei`, `smooth_zvd_ei`, `smooth_si`) alongside the discrete ones; stock Klipper reports five discrete shapers only. Used for screenshots and for checking that the recommendation UI handles the longer list.

| Property | Value |
|----------|-------|
| **Values** | `1`, `true`, `yes` or `on` enables; anything else leaves it off |
| **Default** | Unset — standard Klipper shaper list |
| **File** | `src/ui/ui_panel_input_shaper.cpp` (`inject_demo_results()`) |

```bash
# Kalico-style shaper results in the demo/screenshot path
INPUT_SHAPER_DEMO_KALICO=1 ./build/bin/helix-screen --test -vv &
./build/bin/helix-screen ctl demo input-shaper
```

**Only affects the demo injection path.** A real (or mock-driven) calibration run ignores this variable entirely. Like `HELIX_MOCK_KALICO`, the comparison is strict equality against `"1"`.

### `HELIX_MOCK_BELT_A_HZ`

Set belt path A's simulated resonance peak (Hz) in the mock's `TEST_RESONANCES` simulation. Path A is the `AXIS=1,-1` diagonal (Voron/Shake&Tune naming). Re-measuring a path with a full sweep, while no tune script is set, walks its peak toward the other path's by up to 4 Hz per run, so the tuning loop converges the way cranking a belt tensioner does.

| Property | Value |
|----------|-------|
| **Values** | Positive float (Hz) |
| **Default** | `110` |
| **File** | `src/api/moonraker_client_mock.cpp` (`MoonrakerClientMock` constructor) |

```bash
# Start both belts 2 Hz apart, just inside the MATCHED band
HELIX_MOCK_BELT_A_HZ=104 HELIX_MOCK_BELT_B_HZ=102 ./build/bin/helix-screen --test --sim-speed 6 -vv
```

### `HELIX_MOCK_BELT_B_HZ`

Set belt path B's simulated resonance peak (Hz), the `AXIS=1,1` diagonal. Read once at mock construction, alongside `HELIX_MOCK_BELT_A_HZ`. The defaults (110/98) sit 12 Hz apart, deliberately in "Poor match" territory so the belt-tension loop is visible from the first sweep.

| Property | Value |
|----------|-------|
| **Values** | Positive float (Hz) |
| **Default** | `98` |
| **File** | `src/api/moonraker_client_mock.cpp` (`MoonrakerClientMock` constructor) |

```bash
# A stiff A belt and a loose B belt
HELIX_MOCK_BELT_A_HZ=115 HELIX_MOCK_BELT_B_HZ=95 ./build/bin/helix-screen --test --sim-speed 6 -vv
```

### `HELIX_MOCK_BELT_TUNE_SCRIPT`

Script one belt path's peak across the guided tuning loop. A path's first sweep reads its seeded peak (`HELIX_MOCK_BELT_A_HZ` / `_B_HZ`); every sweep of the named path after that, full or windowed (`TEST_RESONANCES ... FREQ_START= FREQ_END=`), plays the next value in the list, and the last value holds once the list runs out. So the calibration turn's full sweeps and the narrow rounds after it can all be scripted. While either path has a script, no sweep applies the re-measure walk described under `HELIX_MOCK_BELT_A_HZ`. The CSV always spans the configured range, as Klipper's does. Read once at mock construction. An empty or non-numeric item is skipped with a warning, and a prefix other than `A:` or `B:` drops the whole value.

Without a script the walk runs on full re-measures, so a plain `--test` run still registers the calibration turn: A's 110 Hz and B's 98 Hz walk to 106 and 102 on the calibration sweep.

| Property | Value |
|----------|-------|
| **Values** | `<A\|B>:<hz>,<hz>,...`; the item `flat` writes a featureless curve (noise floor, no peak) for that sweep, which reaches the no-peak round |
| **Default** | unset: sweeps after the first walk the peaks as `HELIX_MOCK_BELT_A_HZ` describes |
| **File** | `src/api/moonraker_client_mock.cpp` (`apply_belt_tune_script`, `dispatch_test_resonances_response`) |

```bash
# B: the calibration sweep barely moves (98.3), a second turn registers (102.1),
# then the narrow rounds climb toward A (110 Hz)
HELIX_MOCK_BELT_TUNE_SCRIPT="B:98.3,102.1,102.1,105.0,108.5,109.9" ./build/bin/helix-screen --test --sim-speed 6 -vv

# B's second sweep finds no peak
HELIX_MOCK_BELT_TUNE_SCRIPT="B:flat,104" ./build/bin/helix-screen --test --sim-speed 6 -vv
```

### Live sweep preview under `--test`

No variable switches this on: under `--test` the live sweep charts (Input Shaper panel and
wizard step, Belt Tension) are fed by the mock printer's own accelerometer stream, so the
whole path from the loop thread to the chart runs without hardware.

- **The stream.** `MockAccelBatchSource` (`include/accel_batch_source_mock.h`) plays a synthetic
  sweep (`SweepSynth`) through the callbacks klippy's dump endpoint feeds, on a private
  libhv loop thread, one batch per `kBatchIntervalMs` of wall time. `make_accel_batch_source`
  picks it when `RuntimeConfig::should_mock_moonraker()`; the mock's
  `MoonrakerClientMock::make_accel_batch_source` hands it the response of the sweep it
  started last. The stream's ramp starts `kDriveLeadS` after the stream, as a real one
  trails the console line the preview attaches on.
- **The response.** It matches the CSV the mock writes for the same sweep, so the swap to
  the CSV curve is seamless. Belt paths follow `HELIX_MOCK_BELT_A_HZ` / `_B_HZ` (and the
  tune script); `SHAPER_CALIBRATE` peaks at 53.8 Hz (X) and 48.2 Hz (Y).
- **Pacing.** `--sim-speed` scales the stream (stream seconds per wall second) and the
  console lines together, so the two stay in step at any factor. Check the preview at 4-10x, as for
  any active sweep.
- **Realtime shaper sweep.** `SHAPER_CALIBRATE` plays one `Testing frequency` line per whole
  Hz at the belt sweep's cadence when `MoonrakerClientMock::set_shaper_sweep_realtime` is on,
  which `MoonrakerManager` sets for the app. Unit tests keep the compact 20-line transcript.
- **Failure modes.** `HELIX_MOCK_BELT_FAIL=stall` and the other modes end the console
  transcript as before, and the preview stops with the run. `HELIX_SWEEP_PREVIEW_OFF=1`
  (`ENVIRONMENT_VARIABLES.md`) removes the preview under `--test` too.

### `HELIX_MOCK_BELT_FAIL`

Make the mock's next `TEST_RESONANCES` run fail the way real hardware does, so the belt-tension UI's error paths are reachable without a printer. Read once at mock construction.

| Property | Value |
|----------|-------|
| **Values** | `stall` \| `nofile` \| `multichip` \| `error` \| `kalico` |
| **Default** | unset — a normal sweep that writes its CSV |
| **File** | `src/api/moonraker_client_mock.cpp` (`MoonrakerClientMock` constructor, `dispatch_test_resonances_response`) |

```bash
# Kalico dialect: two-part axis names and an extra accel_per_hz CSV column
HELIX_MOCK_BELT_FAIL=kalico ./build/bin/helix-screen --test --sim-speed 6 -vv

# Sweep stalls at its midpoint and never reports a file
HELIX_MOCK_BELT_FAIL=stall ./build/bin/helix-screen --test --sim-speed 6 -vv
```

`stall` stops emitting at the sweep midpoint, `nofile` names a CSV path it never writes, `multichip` writes per-chip columns (`adxl345`, `adxl345_hotend`) instead of a summed `psd_xyz`, `error` dies after three lines with `!! Invalid adxl345 id (got 0 vs e5).`, and `kalico` switches the transcript and CSV to Kalico's dialect. An unrecognized value leaves the normal behavior in place.

### `HELIX_MOCK_BELT_CSV_A` / `HELIX_MOCK_BELT_CSV_B`

Replay a real `TEST_RESONANCES OUTPUT=resonances` capture as that path's result, instead of the synthetic curve, so the belt-tension RESULTS screen can be seen on real data. The file is copied to the path the mock's "Resonances data written to" line names. Read at the end of each sweep; ignored under a `HELIX_MOCK_BELT_FAIL` mode. Pair it with `HELIX_MOCK_BELT_RANGE` so the analysis uses the band the capture was swept over.

| Property | Value |
|----------|-------|
| **Values** | Path to a resonances CSV (Path A = the `AXIS=1,-1` capture, Path B = `AXIS=1,1`) |
| **Default** | unset — the synthetic curve from `HELIX_MOCK_BELT_A_HZ` / `_B_HZ` |
| **File** | `src/api/moonraker_client_mock.cpp` (`dispatch_test_resonances_response`) |

```bash
# The real Voron 2.4 pair from the test fixtures, over its 5-135 Hz sweep
F=tests/fixtures/belt_sweeps
HELIX_MOCK_BELT_CSV_A=$F/voron24_kalico_axis_1_-1.csv HELIX_MOCK_BELT_CSV_B=$F/voron24_kalico_axis_1_1.csv \
  ./build/bin/helix-screen --test --sim-speed 50 -vv
```

### `HELIX_MOCK_BELT_RANGE`

The `[resonance_tester]` sweep range the mock reports in its configfile and sweeps in `TEST_RESONANCES`, as `<min>-<max>` in Hz. Read once at mock construction. An unparsable value, or one where max is not above min, leaves the default.

| Property | Value |
|----------|-------|
| **Values** | `<min>-<max>`, e.g. `5-100` |
| **Default** | `5-135` |
| **File** | `src/api/moonraker_client_mock.cpp` (`MoonrakerClientMock` constructor) |

```bash
# The Snapmaker U1's band, with its real capture
F=tests/fixtures/belt_sweeps
HELIX_MOCK_BELT_RANGE=5-100 HELIX_MOCK_BELT_CSV_A=$F/u1_axis_1_-1.csv HELIX_MOCK_BELT_CSV_B=$F/u1_axis_1_1.csv \
  ./build/bin/helix-screen --test --sim-speed 50 -vv
```

