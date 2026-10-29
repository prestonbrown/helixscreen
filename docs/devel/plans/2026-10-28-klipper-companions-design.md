# Klipper Companions: Klipper-Side Modules Shipped by HelixPrint

**Status:** design, 2026-10-28. First companion: strain-gauge pressure advance on stock
Creality K2 firmware.

## Problem

Some features need code inside Klipper's process, not Moonraker's. The first one: the K2's
nozzle strain gauge measures pressure advance per filament, but the gauge lives on the nozzle
MCU, and only a Klipper extras module can talk to an MCU. HelixPrint is Moonraker-only today,
and the app's Klipper-side content (`helix_macros.cfg`, `helix_skips.cfg`) is config pushed
through Moonraker's file API, which cannot reach `klippy/extras/`.

More will follow: anything that needs MCU access, motion-queue timing (`motion_report`
trapqs), or a Klipper object other clients can query. Shipping each as its own add-on means a
second installer, a second updater and version skew between app, plugin and add-on. So the
plugin becomes the one delivery vehicle for Klipper-side modules ("companions"), with the
install button, version pinning, updater and uninstall it already has.

## Decisions

1. **Companions live in the plugin tree:** `moonraker-plugin/klippy/helix_<capability>.py`,
   one module per capability, GPLv3. `remote-install.sh`'s `PLUGIN_FILES` lists them.
2. **The Moonraker component installs them, not `install.sh`.** At Klippy ready (and on each
   reconnect) a `CompanionManager` in a new `helix_companions.py` sibling:
   - reads `klipper_path` from `printer.info` (Klipper reports it, so no `/proc` scan);
   - for each companion whose feature is enabled AND whose capability probe passes, links (or
     copies, where the extras dir rejects links) the module into `<klipper_path>/klippy/extras/`
     and writes `<config>/helix_companions/<name>.cfg` holding its section;
   - for each companion that is disabled or no longer applies, removes the `.cfg` first, then
     the module.
   `install.sh` changes only one thing: it adds `[include helix_companions/*.cfg]` to
   printer.cfg (backup first, like `helix_skips.cfg`), and uninstall removes it plus the dir.
3. **A glob include, never a direct one.** Klipper accepts an empty glob ("Empty set is OK if
   wildcard but not for direct file reference", `klippy/configfile.py#_resolve_include`,
   checked on stock K2 firmware). With no companions the include is inert, so the printer
   always boots. The one failure left: a firmware update that rewrites `/usr/share/klipper`
   while keeping printer.cfg leaves a `.cfg` naming a module that is gone, and Klipper stops
   with "Section 'helix_x' is not a valid config section". The manager detects exactly that
   state (Klippy `error`, message naming a `helix_` section) at Moonraker start, re-links the
   module, and issues `RESTART`. That recovery needs no user action, and it is the only
   restart the plugin ever sends on its own.
4. **Restarts only at idle and only by consent.** Linking a new or upgraded module takes
   effect at the next Klipper restart. The manager never restarts during a job. The status
   map reports `restart_pending`, and the app shows "Restart Klipper to finish" on the
   feature row, with the restart behind the user's tap (ground rule: nothing interrupts the
   machine on its own). The decision-3 recovery is the exception, because Klipper is already
   down.
5. **Every companion survives anything in Klipper's ready callback.** An exception there
   shuts Klipper down (seen on the K2: Creality's older `motion_report` names its map
   `trapqs`, not `dtrapqs`). Each companion wraps its setup, reports `available: false` with
   a reason in `get_status`, and its commands refuse with that reason.
6. **Versioned and reported.** Each companion's `get_status` carries `version`. The plugin's
   `features` map gains a `companions` entry (`{name: {installed, loaded_version,
   bundled_version, restart_pending, available, reason}}`), so the app decides from data and
   never from the plugin version.
7. **Feature-gated.** A companion exists only for an enabled feature whose capability probe
   passes. A printer that needs no Klipper-side code gets no file, no section and no restart.

## First companion: `helix_pa_gauge` (K2 strain-gauge PA)

Proven on a stock K2 Plus (F008) on 2026-10-28: four runs, about 100k gauge samples each,
no read errors, no MCU shutdown. Creality's own `AUTO_APAX` shuts down the nozzle MCU and
leaves its stream running until the kernel OOM-kills klippy, so it is not used.

- **Force source is pluggable; the routine and the fit are not.** The fit needs timestamped
  nozzle-force samples on Klipper's print-time axis and the extruder trapq, nothing else. Two
  sources, one table, chosen by what the printer has:
  - `prtouch`: stock Creality firmware. Creality's nozzle-MCU commands (`start_prtouch_pres`,
    `read_prtouch_pres`, `stop_prtouch_pres`), read in 64-sample windows. Build our own
    handles with `mcu.lookup_query_command` from the known formats and take only the channel
    oid from `prtouch_v3`, so a Creality host update that renames its internals does not break
    us. Never sample while `prtouch_v3` is probing.
  - `load_cell`: upstream Klipper or Kalico `[load_cell]` (`add_client(callback)` streams
    `{'data': [(print_time, counts, ...)]}`). This is the K2 on mainline MCU firmware (the
    route Jacob10383's firmware is taking, with upstream's existing K2 load-cell support), and
    any other printer with a load-cell nozzle probe. Continuous, so no window gaps.
  Adding a third source is one row. The calibration routine's coordinates and its CFS feed
  commands are per-printer data, not part of the source.
- **Capability probe:** one force source answers (a `prtouch_v3` with a `pres` channel, or a
  `load_cell` object), and `motion_report` has an extruder trapq under `dtrapqs` or `trapqs`.
- **Klipper side (sampling only):** `HELIX_PA_LOG_START`, `HELIX_PA_LOG_MARK LABEL=`,
  `HELIX_PA_LOG_STOP NAME=` writing `<name>_{pressure,extruder,marks}.csv`. Contact thresholds
  raised out of reach (`min_hold 50000`, `max_hold 100000`) so a flow step never freezes the
  buffer. Method and wire protocol adapted from grant0013/K2-OpenKlipper (GPLv3), credited in
  the file.
- **Plugin side:** a `pa_gauge` feature (opt-in, motion + heat, so its own switch) runs the
  routine as one G-code script, then fits in a `WorkerProcess` (pure Python, no numpy), then
  stores the result per filament slot. Routine:
  `BOX_GO_TO_EXTRUDE_POS`, heat, `BOX_START_PRINT`, prime, then per PA value 8 cycles of
  2 and 12 mm³/s, wiggling X 133-147 at Y 378 (inside the chute that stock swings through,
  X 135-160; the wiper starts at 160), `BOX_END_PRINT`, `BOX_NOZZLE_CLEAN`, cool, park.
  Coordinates come from the `[box]` section's `extrude_pos_*` and `clean_left_pos_x`, never
  constants.
- **Must hold:** `BOX_START_PRINT` around the extrusion (without it the CFS does not feed, the
  extruder slips after about 10 s and grinds the filament); X/Y motion in every logged move
  (Klipper applies PA only to moves with X/Y, so extrude-only moves measure nothing);
  glitch filter (~70 decode glitches per run: zeros and about -6.5M).
- **App side:** a Calibrate button on a loaded CFS slot's sheet, with the result shown with
  its quality verdict. Storage and apply are the Pressure Advance Tuning feature's, not new
  ones (`docs/devel/PRESSURE_ADVANCE_TUNING.md`): the measured K goes through Save to Spool
  (Spoolman filament extra `pressure_advance`, plus the `helix-screen`/`pressure_advance`
  Moonraker key), is sent through `tune_controller.cpp#set_pressure_advance`, and comes back
  at spool activation through `spoolman_active_spool_sync.cpp#apply_saved_pressure_advance`.
  A measured result and a by-eye result are the same record, with a `source` field telling
  them apart.

### Print-time calibration (what Bambu calls flow dynamics calibration)

An optional pre-print step, a toggle beside the bed-mesh and leveling toggles on the print
start screen, plus a printer default:

- **Where it runs:** inside the start sequence, right after the start loads and purges the
  first filament. The head is already at the chute, hot and loaded, so the routine adds only
  its own measuring time. The plugin's print hooks fire too late (history `added` means the
  print already runs), so this is an in-Klipper command the companion registers
  (`HELIX_PA_CALIBRATE SLOT=<lane> MODE=auto|always`), inserted by the same pre-print
  machinery that wraps the leveling steps (`preprint_skip_wrappers.h`, `helix_skips.cfg`).
  It blocks the start until it finishes, then the print continues.
- **Which filaments:** the tools the file uses (file analysis already knows them). `auto`
  measures only a filament with no saved measured value for this nozzle and temperature, so
  most prints cost nothing; `always` measures every used filament, every print. Filaments
  after the first need a load, measure, unload cycle each at start (about 2-3 minutes and a
  purge each), which the toggle's hint states.
- **Shorter routine:** a narrow sweep around the saved or material-default value (about 4 PA
  values x 6 cycles, ~80 s and ~0.3 m per filament) instead of the 3.5-minute full sweep. A
  result outside the sweep falls back to the full one. These numbers are targets: measure
  them before the toggle's hint quotes any.
- **Apply on every tool change, inside Klipper:** the plugin hands the job's per-lane table to
  the companion at start (`HELIX_PA_TABLE LANE=<lane> PA=<k>`), and the companion applies the
  lane's K on each CFS change. This keeps working when the screen is disconnected, which the
  app's activation apply cannot.
- **A failed or unreliable measurement never blocks the print:** it reports, keeps the saved
  or default value, and the print continues.

### Open before shipping the PA feature

- **Validation.** PLA at 220 measured 0.0865 and 0.0812 in two runs (stock 0.038). Calibrating
  each PA block from its own steady levels flattens the curve, so the answer currently rests on
  mean pressure shifting with PA under the reversing wiggle. A printed comparison (corners and
  line ends at 0.04 vs 0.08) decides whether the fit is right before any value is applied
  automatically.
- Which K2 variants carry the same `prtouch_v3` binary (K2, K2 Pro, K2 Plus checked).

## Tests

- `moonraker-plugin/tests/test_helix_companions.py`: link/write order (module before `.cfg`
  on install, `.cfg` before module on removal); disabled feature removes both; a failing
  capability probe installs nothing; `klipper_path` missing reports, never guesses; the
  decision-3 recovery fires only for a `helix_` section error, never another Klippy error;
  no restart while `print_stats.state` is printing or paused.
- Companion unit tests run under the plugin's pytest with a fake printer/MCU: the ready hook
  with `prtouch_v3` absent, with a `pres` lacking `cmd_read`, with `trapqs` instead of
  `dtrapqs` (each reports unavailable, never raises); the window decoder against captured
  frames.
- Fit tests on the captured logs: run3 and run4 pass with a result in 0.08-0.09; run2
  (extrude-only) is rejected as flat; a log with its glitches left in still passes.
- App: `[plugin_feature]` cases for the `companions` entry and the restart-pending row.
