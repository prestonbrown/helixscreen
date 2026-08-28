# Mock & Testing Environment Variables

Every variable that shapes the mock printer and its test harnesses: `--test` runs, replay,
mock AMS/printer personalities, forced-modal and demo-injection knobs. Audience: test writers
and CI — a device build reads almost none of them. Runtime, display, networking and logging
variables live in [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md).

These variables control the mock printer simulation, useful for development and testing without a real printer.

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

### `HELIX_MOCK_REMOTE_THUMBS`

Make the mock advertise Moonraker-relative thumbnail paths and fetch them over real HTTP, so `--test` exercises the cold-fetch pipeline instead of short-cutting it.

| Property | Value |
|----------|-------|
| **Values** | `1` / any non-empty, non-`0` value to enable |
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

Applied **after** `calculate_dynamic_max_size()` and deliberately not through it: that function clamps its result up to `MIN_CACHE_SIZE` (5 MB), so a small value fed in via config gets raised straight back and eviction still never fires. Setting it below the cache's real usage (~1.7 MB for the mock's file list) makes eviction run every pass.

```bash
# Cold fetch with eviction live — the decode-vs-evict interaction from #960
HELIX_MOCK_REMOTE_THUMBS=1 HELIX_THUMB_CACHE_MAX_MB=1 \
  HELIX_CACHE_DIR=/tmp/ht ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_AUTO_PRINT`

Boot the mock printer straight into an active print so print-gated features can be exercised under `--test` without manually driving a print-start flow.

| Property | Value |
|----------|-------|
| **Values** | `1` / any non-empty, non-`0` value to enable |
| **Default** | unset (no auto-print) |
| **File** | `src/application/moonraker_manager.cpp` (sets `mock_auto_start_print`); consumed in `src/api/moonraker_client_mock.cpp` |

### `HELIX_MOCK_REPLAY`

Replay a captured print-start sequence through the mock client's real dispatch paths (`notify_gcode_response`, `notify_status_update`) so the full observer chain — manager wiring, MoonrakerAPI callbacks, collector — runs against real data with no printer attached. Pair with `HELIX_MOCK_PRINTER=k1` (the K1C capture's persona) and `--sim-speed` to fast-forward: a 386s capture replays in ~7s at `--sim-speed 60`.

| Property | Value |
|----------|-------|
| **Values** | path to a replay script JSON (see `tests/fixtures/k1c_flowrate_replay.json`) |
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
| **Values** | `1` / any non-empty, non-`0` value to enable |
| **Default** | unset (verdict derived from the live websocket endpoint) |
| **File** | `include/runtime_config.h` (`should_mock_remote_printer()`); consumed in `src/application/moonraker_manager.cpp` |

The mock client connects over loopback, which always reads as same-host — this flag makes remote-gated UI (print-status camera button, remote video playback paths) appear and behave as if HelixScreen were a remote screen.

```bash
HELIX_MOCK_REMOTE_PRINTER=1 ./build/bin/helix-screen --test -vv
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

### `HELIX_MOCK_AMS`

Select the mock AMS topology/type.

| Property | Value |
|----------|-------|
| **Values** | `none`, `afc`, `toolchanger` / `tc`, `mixed`, `multi`, `torture`, `vivid`, `ifs`, `htlf`, `snapmaker`, `medusahc` / `medusahc-fork` |
| **Default** | Happy Hare, LINEAR, 4 slots |
| **File** | `src/printer/ams_backend.cpp` |

| Value | Units | What it simulates |
|-------|-------|-------------------|
| *(unset)* | 1 | Happy Hare, LINEAR, 4 slots (default constructor) |
| `none` | - | No mock AMS at all |
| `afc` | 1 | AFC Box Turtle, HUB, 4 slots. Aliases: `box_turtle`, `boxturtle` |
| `toolchanger` / `tc` | 1 | Tool Changer, PARALLEL topology. Alias: `tool_changer` |
| `mixed` | 3 | Box Turtle + 2x OpenAMS, 6 tools |
| `multi` | 2 | Box Turtle (4 slots) + Night Owl (2 slots), single toolhead |
| `torture` | **5** | **The only profile whose unit-card row overflows.** See below |
| `vivid` | 3 | 2x Box Turtle + ViViD, 12 slots |
| `ifs` | 1 | AD5X IFS, 4 slots, LINEAR. Aliases: `ad5x`, `ad5x_ifs` |
| `htlf_toolchanger` | 2 | AFC HTLF + Toolchanger: 4 HTLF lanes (2 direct, 2 hub→shared extruder) + 3 standalone toolheads. Tests MIXED topology. Aliases: `htlf_tc`, `htlf` |
| `snapmaker` | 1 | Snapmaker U1, 4 slots, PARALLEL, non-editable mapping. Aliases: `snapswap`, `u1` |
| `medusahc` | 1 | **MedusaHC hotend changer - mock HARDWARE, real backend.** Irbis3D controller. Aliases: `medusa`, `mhc`. See below |
| `medusahc-fork` | 1 | MedusaHC as driven by topi314's fork. Alias: `medusa-fork` |

```bash
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
aliases are neither dense nor unit-ordered (T0 and T10 are absent). Every other
profile tops out at 3 units, and unit cards shrink to `#ams_card_min_width`, so
in every other profile `unit_cards_row` measures `scroll.right == 0` even at
`-s tiny`. Anything that only misbehaves once that row can scroll is
unreproducible without this profile.

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

The two values map to the two shipping configurations in
[FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md), so the schema discrimination
in `read_medusahc()` is exercised at runtime and not only in the unit tests:

| Value | Objects | Phase key | Feeder | Step bar |
|-------|---------|-----------|--------|----------|
| `medusahc` | `pin_watch io` + `toolchanger` + `tool T0..3` + `medusahc`, `MHC_*` and legacy aliases | `operation`: idle/dropping/picking | `feeder_open` | 4 steps |
| `medusahc-fork` | `medusahc` alone, forked `state`/`error`/`toolN_docked` schema | `state`: ready/changing | `feeder_open` | 3 steps |

A swap advances through its phases on the simulation thread over ~6s, so they arrive as
separate status frames rather than collapsing into one update. `SELECT_TOOL`,
`UNSELECT_TOOL`, `DROP_TOOL`, bare `T<n>` and the feeder macros are all handled.

The default `mmu` object is suppressed in these modes - it would detect Happy Hare and
stand a second AMS backend up alongside the changer.

**Multi-extruder and tool testing:** Setting `HELIX_MOCK_AMS=toolchanger` also creates multiple tool definitions and extruders in the mock environment. Multiple extruders (extruder, extruder1, etc.) and tools are auto-discovered from Klipper objects at runtime, so no separate env var is needed to control extruder count. The toolchanger mock provides a complete multi-tool, multi-extruder test environment.

### `HELIX_MOCK_AMS_STATE`

Select the mock AMS visual scenario.

| Property | Value |
|----------|-------|
| **Values** | `idle`, `loading`, `error`, `bypass`, `unaccounted`, `grade` |
| **Default** | `idle` (slot 0 loaded, slot 3 empty, others available) |
| **File** | `src/printer/ams_backend.cpp` |

| Value | What it shows |
|-------|---------------|
| *(unset)* / `idle` | Default idle state |
| `loading` | Active load in progress with realistic segment animation |
| `error` | Slot errors visible; buffer fault also shown when combined with `afc` mode |
| `bypass` | Bypass mode active |
| `unaccounted` | Filament at the toolhead that no lane accounts for (drives the print-start gate warning) |
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
| **Values** | `sensor_name:state` (e.g., `fsensor:empty`, `fsensor:detected`) |
| **Default** | Detected |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Start with empty filament sensor
HELIX_MOCK_FILAMENT_STATE="fsensor:empty" ./build/bin/helix-screen --test
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

Select which printer the mock Moonraker client impersonates. Drives the mock's reported identity, kinematics defaults, bed dimensions, and (for AD5M) the `pre_print_options` set that gates print-option UI.

| Property | Value |
|----------|-------|
| **Values** | `voron_24`, `voron_trident`, `k1`, `ad5m`, `generic_corexy`, `generic_bedslinger`, `multi_extruder` |
| **Default** | `voron_24` (Voron 2.4) |
| **File** | `src/application/moonraker_manager.cpp` |

```bash
# FlashForge AD5M mock (ships pre_print_options + load-cell probe)
HELIX_MOCK_PRINTER=ad5m ./build/bin/helix-screen --test -vv

# Multi-extruder mock
HELIX_MOCK_PRINTER=multi_extruder ./build/bin/helix-screen --test -vv
```

**Unrecognized values fall back to Voron 2.4** with a warning listing the valid set — they are not fatal. K2 and CC1 have no dedicated mock type yet and hit that fallback.

**Side effect on `settings.json`:** when this variable is set *at all* (even to an invalid value), `MoonrakerManager::init()` clears the saved printer type before detection runs, so a stale "Voron 2.4" from a previous launch cannot win over the env var. `PrinterDetector::auto_detect_and_save()` then re-resolves from the mock's reported identity on every launch. This writes to your config file — expect the persisted printer type to change.

Confirm what you got from the log line: `[MoonrakerManager] Creating MOCK client (<printer>, <n>x speed)`.

### `HELIX_MOCK_PROBE_TYPE`

Choose which Z-probe the mock printer advertises. Controls both the Klipper object added to the mock object list and the probe status payload in the initial state dispatch.

| Property | Value |
|----------|-------|
| **Values** | `cartographer`, `beacon`, `bltouch`, `loadcell`, `tap`, `klicky`, `standard`, `none` |
| **Default** | `cartographer` |
| **File** | `src/api/moonraker_client_mock.cpp` |

| Value | Object exposed | Status detail |
|-------|----------------|---------------|
| `cartographer` *(default)* | `cartographer` | `last_z_result: -0.425`, `z_offset: 0.0` |
| `beacon` | `beacon` | `last_z_result: -0.312`, `z_offset: 0.0` |
| `bltouch` | `bltouch` | `last_z_result: 0.130`, `z_offset: -1.850` |
| `loadcell` | generic `probe` | `z_offset: null` (the load-cell-probe case) |
| `tap` / `klicky` / `standard` / anything else | generic `probe` | `last_z_result: 0.0`, `z_offset: -0.250` |
| `none` | *(no probe object)* | *(no probe status)* |

```bash
# Test the BLTouch-specific Z-offset UI
HELIX_MOCK_PROBE_TYPE=bltouch ./build/bin/helix-screen --test -vv

# Printer with no probe at all
HELIX_MOCK_PROBE_TYPE=none ./build/bin/helix-screen --test -vv
```

Unrecognized values are not rejected — they land in the generic `probe` bucket and are logged as `Mock probe: <value> (as generic probe)`.

### `HELIX_MOCK_KINEMATICS`

Override the kinematics string the mock reports in `configfile.config.printer.kinematics`. Bed-moves detection (which drives bed-slinger vs. CoreXY UI decisions) reads this.

| Property | Value |
|----------|-------|
| **Values** | Any Klipper kinematics name (e.g. `corexy`, `cartesian`, `delta`, `corexz`) |
| **Default** | Derived from the mock printer type: `corexy` for Voron 2.4, Voron Trident and Creality K1; `cartesian` for everything else |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Force a bed-slinger layout on the default Voron mock
HELIX_MOCK_KINEMATICS=cartesian ./build/bin/helix-screen --test -vv
```

The value is passed through verbatim — no validation. A nonsense string simply produces a printer whose kinematics match nothing.

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

# Materialize the dragonbreath chamber-heater trio: heater, diagnostics
# object, and filter-fan output pin (drives status frames, SET_PIN
# round-trip, and a configfile max_temp of 75)
HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter" \
  ./build/bin/helix-screen --test -vv
```

**Two-word object names are reassembled by prefix.** The parser splits on whitespace, then treats a token starting with `heater_generic`, `temperature_fan`, `temperature_sensor`, or `output_pin` as the start of a *new* object and glues any following tokens onto the current one. So `temperature_fan chamber` becomes the single object `temperature_fan chamber`. A token that is not a prefix glues onto the current object — with one exception: a token that exactly names a chamber-heater backend's diagnostics object (e.g. the bare `dragonbreath` after a completed `heater_generic dragonbreath`) starts a new standalone object instead of appending. A chamber heater accepted from this list also replaces the mock profile's built-in chamber heater. Each accepted object is logged as `[MoonrakerClientMock] Added mock object: <name>`.

### `HELIX_MOCK_DRAGONBREATH_FAULT`

Latch a fault into every synthesized dragonbreath status frame — the diagnostics object reports `fault: true` with a `fault_reason` instead of the nominal healthy payload. Pairs with the `HELIX_MOCK_OBJECTS` dragonbreath trio to exercise fault UI paths without hardware.

| Property | Value |
|----------|-------|
| **Values** | Exactly `1` |
| **Default** | Unset — nominal frame (`fault: false`, null `fault_reason`) |
| **File** | `src/api/moonraker_client_mock.cpp` |

```bash
# Faulted dragonbreath chamber heater
HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter" \
  HELIX_MOCK_DRAGONBREATH_FAULT=1 ./build/bin/helix-screen --test -vv
```

### `HELIX_MOCK_KALICO`

Make the mock report Kalico-style MPC heater control instead of Klipper's PID. The extruder's `configfile` settings then carry `control: mpc` + `heater_power` rather than `control: pid` + the three PID coefficients — the discriminator HelixScreen uses to decide which tuning UI to show.

| Property | Value |
|----------|-------|
| **Values** | Exactly `1` |
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
| **Values** | `passive`, `dryer`, `slot` (lowercased before use). Any other value leaves the unit with **no** environment sensors. |
| **Default** | Unset — auto: `dryer` when `HELIX_MOCK_DRYER` is on, otherwise `passive` |
| **File** | `src/printer/ams_backend.cpp` (applied via `AmsBackendMock::set_environment_mode`) |

```bash
# Per-slot environment sensors
HELIX_MOCK_AMS_ENV=slot ./build/bin/helix-screen --test -vv

# Explicitly no environment sensors (any unrecognized value works)
HELIX_MOCK_AMS_ENV=off ./build/bin/helix-screen --test -vv
```

Pairs with [`HELIX_MOCK_NO_HUMIDITY`](#helix_mock_no_humidity), which strips the humidity channel from whichever mode is active.

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

### `INPUT_SHAPER_DEMO_KALICO`

Swap the input-shaper panel's injected demo results for a Kalico-shaped shaper list. Kalico reports smooth shapers (`smooth_zv`, `smooth_mzv`, `smooth_ei`, `smooth_2hump_ei`, `smooth_zvd_ei`, `smooth_si`) alongside the discrete ones; stock Klipper reports five discrete shapers only. Used for screenshots and for checking that the recommendation UI handles the longer list.

| Property | Value |
|----------|-------|
| **Values** | Exactly `1` |
| **Default** | Unset — standard Klipper shaper list |
| **File** | `src/ui/ui_panel_input_shaper.cpp` (`inject_demo_results()`) |

```bash
# Kalico-style shaper results in the demo/screenshot path
INPUT_SHAPER_DEMO_KALICO=1 ./build/bin/helix-screen --test -vv &
./build/bin/helix-screen ctl demo input-shaper
```

**Only affects the demo injection path.** A real (or mock-driven) calibration run ignores this variable entirely. Like `HELIX_MOCK_KALICO`, the comparison is strict equality against `"1"`.
