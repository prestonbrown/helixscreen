# Power-Loss Recovery (PLR)

HelixScreen offers to resume an interrupted print at connect time. Three printer
firmware families expose a recovery mechanism, and they work in fundamentally
different ways: two are **passive** (a status field tells us a snapshot exists),
one is **active** (we have to ask, and asking has side effects). A fourth backend,
**GENERIC**, is the HelixPrint plugin's own recovery for any Klipper printer listed
for it: the plugin keeps the snapshot and renders the resume, and the app walks
the user through a guided wizard.

Code map:

| File | Role |
|------|------|
| `include/plr_backend.h`, `src/printer/plr_backend.cpp` | The one module that knows which firmware carries which mechanism: a provider table (status fields to subscribe, selection markers, availability and recovery-file rules) with the capability questions as free functions (`plr_required_status_fields`, `plr_parse_*`, `plr_offer_availability`, `plr_build_plan`). No LVGL, no network. |
| `include/plr_offer.h`, `src/printer/plr_offer.cpp` | Pure offer decision (`plr_should_offer`) and latch re-arm rule (`plr_should_rearm`). |
| `include/plr_offer_controller.h`, `src/ui/print/ui_plr_offer_controller.cpp` | App-lifetime controller. Observers, one-shot latches, firing the recovery probe; asks `plr_offer_availability` for the one "recovery available" signal and never names a firmware. |
| `src/ui/ui_plr_prompt.{h,cpp}` | The modal and its two button actions. Backend-agnostic — it executes a `PlrRecoveryPlan`. |
| `src/printer/printer_print_state.cpp` | Feeds the status payload through the `plr_parse_*` readers into the capability subjects. |
| `include/plr_wizard.h`, `src/printer/plr_wizard.cpp` | GENERIC: readers for the plugin's `server.helix.plr.*` replies and the wizard's pure decisions (bed state, confirmations, commit gate, step machine). |
| `include/plr_plugin_rpc.h`, `src/printer/plr_plugin_rpc.cpp` | The endpoints as calls: the plugin's (GENERIC), and the firmware's recovery probe (`request_recovery_probe`) and RPC discard (`discard_with_rpc`). |
| `include/ui_plr_wizard_overlay.h`, `src/ui/print/ui_plr_wizard_overlay.cpp`, `ui_xml/plr_wizard_overlay.xml` | GENERIC: the resume wizard. |
| `include/plr_profile.h`, `src/printer/plr_profile.cpp` | GENERIC: the printer database `plr` block, its resolution with the user's overrides, and the `plr_profile` / `plr_user` records. |
| `src/api/moonraker_client_mock_plr.cpp` | The mock plugin's endpoints (`HELIX_MOCK_PLR*`, `MOCK_ENVIRONMENT_VARIABLES.md`). |

Tests: `tests/unit/test_plr_backend.cpp`, `test_plr_offer.cpp`, `test_plr_state.cpp`,
`test_plr_prompt.cpp`, `test_firmware_error_envelope.cpp`, and for GENERIC
`test_plr_wizard.cpp`, `test_plr_wizard_overlay.cpp`, `test_mock_plr.cpp` and the
plugin cases in `test_plr_offer_preparing.cpp` (tag `[plr]`).

Backend precedence in `plr_select_backend()`: SNAPMAKER first, then GENERIC for
the plugin's QIDI `print_state` adapter, then QIDI, then CREALITY, then GENERIC
for the plugin's own recovery. No firmware carries more than one marker set, so
among the three firmware backends the order only makes a hypothetical tie
deterministic - and it puts both passive signals ahead of the probe-based one, so
a marker chosen by mistake never costs a side-effectful probe. Both GENERIC rows
need the plugin's `plr` feature Ready; see "Generic (HelixPrint plugin)" for why
they sit where they do.

---

## The three backends

### Snapmaker U1 — passive

Snapmaker's Klipper fork adds `virtual_sdcard.pl_env_valid`. The firmware
validates a coherent power-loss snapshot against MCU flash during boot and sets
the flag itself. Mainline Klipper has no such field, and our parser only accepts
a JSON boolean, so the flag is **self-gating**: `pl_env_valid == true` already
means "Snapmaker firmware with a valid recovery snapshot".

No printer-model or AMS-backend gate is needed, and none should be added — an
earlier redundant "AMS backend == SNAPMAKER" gate wrongly suppressed the offer
on an AFC-modded U1 (bundle UDZJQVQZ) whose backend is not the Snapmaker one.
`tests/unit/test_plr_offer.cpp` pins that regression.

| | |
|---|---|
| Capability + availability | `virtual_sdcard.pl_env_valid == true` |
| Recovery filename | `virtual_sdcard.file_path` |
| Resume | gcode `SDCARD_PRINT_PL_RESTORE` |
| Discard | gcode `SDCARD_PRINT_PL_CLEAR_ENV` |
| Errors | JSON-coded (`{"coded": ..., "msg": ...}`); `MoonrakerError::from_json_rpc` decodes it through `include/firmware_error_envelope.h` |

### Qidi Q2 / Q1 Pro / Plus 4 - passive

Qidi's stock firmware keeps its recovery state in a save variable. The stock
macros maintain it: `PRINT_START` runs `save_last_file`, which sets
`SAVE_VARIABLE VARIABLE=was_interrupted VALUE=True`; a normal end or cancel runs
`CLEAR_LAST_FILE`, which sets it back to False. After power loss the print never
reaches either, so `save_variables.variables.was_interrupted` reads true at boot.
That is the whole signal - nothing is probed and nothing is asked.

Two halves gate the backend, because the variable name alone proves nothing:

* **Capability**: discovery found the `DETECT_INTERRUPTION` or `RESUME_INTERRUPTED`
  macro (`plr_resume_macro_present()`, `PrinterDiscovery::has_macro`). Newer
  firmware (Q2 01.01.02.03 at least) registers `RESUME_INTERRUPTED` and `CLEAR_LAST_FILE` as Python
  commands, invisible to discovery, so `DETECT_INTERRUPTION` is the macro every
  generation carries. Either identifies Qidi's stock firmware; `was_interrupted` is a user-writable name on ANY
  Klipper, so the variable alone must never select the backend.
* **Availability**: `save_variables.variables.was_interrupted` arrived as a JSON
  boolean and is true.

| | |
|---|---|
| Capability | `DETECT_INTERRUPTION` or `RESUME_INTERRUPTED` macro present (discovery) |
| Availability | `save_variables.variables.was_interrupted` is JSON boolean `true` |
| Recovery filename | `virtual_sdcard.file_path` when non-empty; the prompt falls back to its generic body when empty (no filesystem scan) |
| Resume | gcode `RESUME_INTERRUPTED` (rebuilds the resume gcode from the `.temp/` backup, lifts Z clear of the part before homing X/Y, then prints it) |
| Discard | gcode `CLEAR_LAST_FILE` |
| Errors | plain Klipper error strings; the prompt surfaces the message unchanged |

Two disciplines the parser owes the delta protocol, same shape as the Creality
capability latch:

1. **Booleans only.** `save_variables` values are Python literals Klipper
   re-parses; another type under this name is not our signal. A non-boolean
   value is not-available, and also never a clear: after a `true`, a `1` or
   `"True"` arriving later must not drop the flag.
2. **Deltas must not clear.** Moonraker notifies at top-level-field
   granularity, so a frame without `save_variables`, or a `variables` dict
   without the key, leaves the flag alone. Only a boolean `false` (the
   `CLEAR_LAST_FILE` discard path) or the disconnect edge (the offer controller
   forces the subjects to 0, like `plr_snapshot_valid`) resets it.

The flag is ALSO true during every normal print - it stays true from
`PRINT_START` until the job ends. That is by design of the stock macros, and the
offer decision's `printer_idle` input is what scopes it to a boot after power
loss: mid-print the printer is not idle, so no offer fires. The subscription
(`save_variables` when the macro exists) is added in
`MoonrakerDiscoverySequence::build_subscription_objects()` via the
`plr_required_status_fields()` capability question - no vendor name reaches the
subscription builder.

### Creality K1 / K1C / K1 Max / K2 Plus / Ender 3 V3 / Hi / i7 — active

Verified by reading the Klipper source on a physical K1C and K2 Plus and by
running the endpoint live against an idle K1C.

Creality's fork registers a Klipper **webhook endpoint**,
`pause_resume/check_continue_print_state`. Moonraker auto-registers every
non-reserved Klipper webhook endpoint (klippy_connection.py:387-396), so it is
reachable both ways:

* HTTP: `POST /printer/pause_resume/check_continue_print_state`
* JSON-RPC: `printer.pause_resume.check_continue_print_state`

HelixScreen uses the JSON-RPC form — we already hold the WebSocket.

Live response on an idle K1C:

```json
{"result": {"file_state": false, "eeprom_state": false}}
```

A recovery is available **only when both `file_state` and `eeprom_state` are
true**.

| | |
|---|---|
| Capability | presence of `print_stats.power_loss` (see below) |
| Availability | one-shot probe returns `file_state && eeprom_state` |
| Recovery filename | sidecar JSON, `<base>/creality/userdata/config/print_file_name.json`, key `file_path` |
| Resume | gcode `SDCARD_PRINT_FILE FILENAME="<file>" ISCONTINUEPRINT=1` — quoted, and `<file>` **relative to the sdcard root** (both are load-bearing; see "Filename safety") |
| Discard | JSON-RPC `printer.pause_resume.cancel_continue_print` |

`<base>` is `/usr/data` on K1-class and `/mnt/UDISK` on the OpenWrt-class K2.
HelixScreen runs on the printer, so the read normally succeeds; it is
best-effort and **read-only** — never write to that path.

### Generic (HelixPrint plugin) - asked

The HelixPrint plugin (`moonraker-plugin/`; its half is in "Inside the plugin"
below) saves its own snapshot during every print on printers listed for it,
decides itself whether a resume can be offered, and renders and sends the resume.
The app gates on the
plugin's `plr` feature (`plugin_feature("plr", 1)` Ready), asks for the plugin's
verdict, and owns only the confirmations a screen can ask.
The feature framework it registers with, and the layer index it shares with file
analysis, are in [HELIXPRINT_PLUGIN.md](HELIXPRINT_PLUGIN.md).

| | |
|---|---|
| Capability | `server.helix.status` `features.plr` at version 2 or newer, available and enabled |
| Availability | `server.helix.plr.status` `offer.available` (the plugin checks the newest history job is `interrupted`, a snapshot exists for it, the file is unchanged, the job got far enough, the tier allows it, Klipper is ready); `backend` says which flavor |
| Recovery filename | `snapshot.filename` |
| Resume | the guided wizard (below); nothing is sent at the offer |
| Discard | JSON-RPC `server.helix.plr.discard` |
| Errors | Moonraker errors whose message leads with the plugin's code (`plr_stale_plan`, `plr_missing_confirmation`, `plr_klippy_error`, ...); the wizard shows the message |

**Where it sits in the precedence.** The plugin's `qidi_print_state` backend serves
the QIDI firmware layout the legacy QIDI macros also live on, and its resume
corrects the firmware's file offset where the legacy one does not, so it outranks
QIDI. The `generic` backend comes last, so it never displaces a firmware's own
recovery. Both select `PlrBackendType::GENERIC`; the plan's `backend` field says
which one is running.

**When the status is asked.** `server.helix.plr.status` is side-effect free. The
offer controller asks it once per connection, when the link is up, Klipper reports
`ready` and the feature is Ready; the plugin-installed subject (set after the
features map), the Klipper state and the connection state each trigger the check,
so whichever arrives last asks. An answer blocked by `klippy_not_ready` is not
kept, so the next Klipper-ready edge asks again. The disconnect edge drops the
answer with the other latches. The plugin also pushes its status as
`helix_print:plr_status` (delivered as `notify_plr_status`, payload = the `status`
reply) each time its own Klippy-ready handling finishes; the controller takes it
in place of its answer, so an offer that turns available after the first ask
still reaches the screen. The one-prompt-per-connection latch still applies, and
no offer shows while the resume wizard is open (`plr_wizard_open()`).

**The profile and the switch.** The plugin acts on two Moonraker database records
in the `helix-screen` namespace, so it has them with the screen off:

- `plr_profile`, which the app resolves and writes: `{"tier", "backend",
  "storage_hint", "z_holds_unpowered", "xy_home_gcode", "z_lift_mm",
  "purge_gcode", "resume_skip_fraction", "macro_vars"}`. `plr_resolve_profile()`
  resolves the printer database's `plr` block
  (`PrinterDetector::get_plr_block()`). A printer with no block is tier 0: the
  plugin reports the `plr` feature `available: false`, reason `not_listed`, and
  saves and offers nothing. Tier 2 needs the database's `z_holds_unpowered`, so without it
  the tier is at most 1. The function also takes user overrides (tier, XY homing,
  purge, skip fraction), but no UI provides them: the caller passes an empty
  `PlrUserOverride`, so the record is the database block as is. `PlrOfferController::publish_profile()` writes
  the record once per connection and printer type whenever the plugin reports a
  `plr` entry at version 2 or newer, available or not
  (`plugin_feature_reported()`): until a profile of tier 1 or more lands, the
  plugin reports the feature unavailable, so the write cannot wait for it. It
  writes only after this connection's plugin probe has answered: discovery resolves
  the printer type before that probe, so the type is this printer's, never the
  previous one's. It reads the stored record first and writes only when it
  differs, so several screens on one printer do not rewrite it in turn. Once the
  stored record matches, written now or already current, it re-reads the
  plugin's features map, since the plugin's availability and saving verdict
  follow the profile: a second screen whose map was read before another screen
  wrote the profile would otherwise keep reading `not_listed`. A failed write
  forgets the type, so the next probe answer or type change writes again.
- `plr_user` = `{"enabled": bool}`, the only record the plugin reads for its
  saving switch. Settings > Printing > Extras > **Power-loss recovery** writes it.
  The row is a `plugin_feature_toggle_row` in its own-switch form: its mode
  (`plugin_feature_mode_plr`, from `plugin_switch_mode()`) makes it a switch while
  the feature is Ready or Disabled, greyed with Install without a plugin, greyed
  asking for an update when the plugin reports no `plr`, and hidden when the
  plugin reports it `available: false` (`python_too_old`, `no_virtual_sdcard`,
  `store_unwritable`, `not_listed`). An unlisted printer (no `plr_profile`, or
  tier 0) is `not_listed`, so it gets no switch; the plugin overview shows the
  feature unavailable with that reason. Its switch follows `plr_saving_enabled`, the
  plugin's own report re-read after each write: the `plr` entry's `saving`, or
  its `enabled` from a plugin that reports no `saving`. The entry's `enabled`
  also holds while a saved snapshot waits, so the offer (gated on Ready) survives
  saving being switched off.

Listed printers: the Qidi Q2 (`backend: qidi_print_state`, eMMC, tier 2). Tier 2
replicates QIDI's own stock resume, which already trusts the Z the steppers held;
the Q2 results are under "Testing" below. The K1 line stays unlisted: Creality's
own recovery serves it and outranks the generic backend, and the Snapmaker, Creality
and legacy QIDI backends are kept as they are.

**The layer index.** The plugin's follower (`moonraker-plugin/helix_plr.py#LayerFollower`) builds the
printing file's layer index while the print runs, so a resume scan starts at a layer
instead of byte 0. It is the same `.helix_index/<path>.layers.jsonl` file that file
analysis (`helix_analysis.py`) writes for every G-code file on upload, in the format
`helix_gcode_reader.py` documents; whichever writes first, the other uses it:

- **One identity.** `helix_plr.file_identity` / `file_hash` are
  `helix_gcode_reader.file_identity` / `file_hash` (size, mtime, sha1 of the first and
  last 64 KB): the snapshot, the index header and the screen's preview check one rule.
- **One writer per index.** Analysis writes a whole index to a `.tmp` file and renames it
  over the old one. The follower appends, so a power cut leaves a valid prefix; its
  `LayerIndexWriter` writes nothing once a complete index for the same identity exists,
  and a follower already appending when analysis's rename lands keeps writing to the file
  it opened, so a complete index is never touched.
- **One prune rule.** `helix_gcode_reader.prune_layer_indexes(root, protected)` removes an
  index whose G-code is gone or no longer matches the header's size or mtime, or that
  indexes a file outside gcodes (`.external`), with no count cap. `protected` comes from
  PLR's state, never from file times: `HelixPlr.protected_index_paths()` names the
  follower's index during a print and the index of the job a waiting snapshot resumes.
  PLR prunes at each job start; analysis prunes at startup and asks PLR for the same paths.
- **The worker runner.** `helix_gcode_reader.WorkerProcess` runs a whole-file reader in a
  niced child process (Moonraker's own interpreter, `ionice -c 3` where present, a
  timeout, kill on shutdown or when a print starts on a one-core host). Analysis uses it;
  the follower still runs on its own duty-cycled thread.

**The wizard.** Resume on the offer opens `PlrWizardOverlay`, whose steps are
`plr_next_wizard_step()` (`include/plr_wizard.h`):

| Step | What happens |
|------|-------------|
| Offer | `status`, then `plan`. A long scan answers `pending` and follows with `helix_print:plr_plan_ready` (delivered as `notify_plr_plan_ready`); the wizard asks `plan` again then. A `plan_ready` carrying `error`, or a plan still `pending` after `plan_ready`, fails the wizard with that message instead of starting the scan over |
| Checks | File (its display name, as Print Status shows it), layer, Z, outage age and bed reading from the plan; the confirmations asked before anything moves: `part_attached`, plus `nozzle_clear` at tier 1, `accept_no_mesh` for a `mesh_unavailable` warning, `accept_next_layer` for a `layer_start` warning, `accept_cold_bed` when the bed has cooled more than `kPlrColdBedDiffC` below target, and anything else in the plan's `requires` |
| Preparing | `prepare`: steps `bed_wait`, `chamber_wait`, `nozzle_in_place` (bed and nozzle heat together where the nozzle stopped, so the print re-grips and the nozzle lets go of it before anything moves), then on the generic backend `lift`, `nozzle_standby` (the nozzle drops to standby until commit re-heats it), `home_xy`, and at tier 1 `position` (over the resume point); the QIDI backend's prepare runs `nozzle_standby` after the heat and moves nothing, and the Checks button reads Start Heating there instead of Heat and Home (`plr_prepare_moves_head()`). `helix_print:plr_progress` (`notify_plr_progress`, `{plan_id, step, state}` with state `running` / `done` / `failed`) names each; the wizard shows the `running` step and the live bed temperature |
| ZStep | Tier 2 confirms the height the steppers held (`z_confirmed`); the text follows `plr_z_step_kind()`: lifted and homed on the generic backend, not moved since the outage on QIDI. Tier 1 jogs the nozzle down to touch the part (`z_jogged`), 0.05 / 0.1 / 1 mm steps through the motion panel's coalesced relative move; Resume stays disabled until a downward jog has landed (the part top is where the user lowered the nozzle to), and waits while a jog is still moving |
| Committing | `commit` with every confirmation given and `z_reference` `part_top` (tier 1) or `trusted` (tier 2): steps `bed_wait`, `chamber_wait`, `nozzle_in_place` (back to print temperature where the nozzle stands), then on the generic backend `z_reference` (tier 1), `restore`, `heat_nozzle`, `purge`, `move_to_resume`, `start`; the QIDI backend's commit runs its heat steps inside `start`; the print starts at the resume offset |
| Done / Failed / Aborted | While prepare or commit runs, Cancel is a full-width danger button; once the plugin reports `start`, Cancel hides and an abort is refused. Leaving the wizard after `prepare` started (Cancel, back, closing it, a failed RPC) sends `abort`: the plugin stops, turns the heaters off and keeps the snapshot. A screensaver, display sleep or a view pushed on top (Print Status opens as the print starts) is not leaving: the sequence keeps running. While the wizard is open it holds `display_awake_hold()`, so neither the screensaver nor display sleep starts over a step that waits on the user; the hold ends when the print starts, or on Done, Failed or Aborted. Entering any step drops queued jogs, and a jog ack after the Z step sends nothing more |

Two safety properties are pinned by the tests: the wizard never commits on its
own (Committing is reached only from ZStep with every required confirmation, and
a dropped connection fails the flow from every live step), and the only motion
the app sends itself is the tier-1 Z jog. A plan whose `requires` names a
confirmation this app does not know fails at the offer rather than at commit.

---

## Inside the plugin (GENERIC)

The plugin half of the GENERIC backend: what it saves, when it offers, where it
resumes, and the G-code it sends. Python 3.8, stdlib only, like the rest of the
plugin.

| File | Role |
|------|------|
| `moonraker-plugin/helix_plr.py` | Save side (storage verdict, profile and switch, snapshot record, save policy, two-slot store, layer follower), offer rules, and `HelixPlr`, the Moonraker-facing component with the endpoints |
| `moonraker-plugin/helix_plr_scan.py` | Where to resume: scan to the dispatch head, walk back to the executing line, advance by the skip fraction, the next-layer fallback, leaving an excluded object |
| `moonraker-plugin/helix_plr_render.py` | `ResumePlan` and the prepare, commit and start G-code; pure |
| `moonraker-plugin/helix_plr_qidi.py` | The adapter for newer QIDI firmware's own `print_state` resume |
| `moonraker-plugin/helix_gcode_reader.py` | The G-code reader shared with file analysis: `scan_to_offset`, `ModalState`, the layer index |

Tests: `moonraker-plugin/tests/test_helix_plr.py`, `test_helix_plr_scan.py`,
`test_helix_plr_render.py`, `test_helix_plr_qidi.py`, `test_helix_gcode_reader.py`.

### Why the plugin renders the resume G-code

Elsewhere the app renders G-code and the plugin only sends it, so no backend's G-code
is written twice (`HELIXPRINT_PLUGIN.md` § "Print-Start Hooks"). PLR is the deliberate
exception, and it stays one:

- The resume script is built from a scan of the G-code file (modal state, object
  definitions, E, layer Z), and that scan runs in the plugin, where the file is.
  Shipping the scan to the app so the app can render text the plugin then sends adds a
  round trip and a second consumer of the scan format.
- The K-Touch (ESP32) cannot scan G-code files, so an app-rendered resume would be
  impossible there.
- One endpoint resumes from any client.

No vendor backend's G-code is duplicated: the Snapmaker, Creality and legacy QIDI
resumes keep their app-side plans, and the QIDI adapter calls QIDI's endpoint instead
of re-rendering QIDI's sequence.

### The snapshot

`moonraker-plugin/helix_plr.py#build_record` builds one record from Klipper's status, which the plugin
subscribes to on Klippy ready (`snapshot_objects`: `print_stats`, `virtual_sdcard`,
`toolhead`, `motion_report`, `gcode_move`, `heater_bed`, `fan`, `firmware_retraction`,
`exclude_object`, `bed_mesh`, every extruder, `heater_generic`, `temperature_fan`,
`fan_generic` and `gcode_macro` object). Schema 1:

| Fields | From |
|--------|------|
| `file_position` | `virtual_sdcard.file_position`, the dispatch head |
| `toolhead_pos`, `live_pos`, `gcode_pos` | `toolhead.position` (end of the queue), `motion_report.live_position` (executing), `gcode_move.gcode_position` |
| `layer`, `total_layers`, `print_duration`, `filament_used` | `print_stats` and its `info` |
| `gcode_offset`, `abs_coord`, `abs_extrude`, `speed`, `speed_factor`, `extrude_factor` | `gcode_move` |
| `heaters` (`{name: {target, temp}}`), `fans`, `pa`, `retraction`, `limits`, `active_extruder` | the heater, fan, extruder, `firmware_retraction` and `toolhead` objects |
| `objects`, `excluded` | `exclude_object`, no cap |
| `macro_vars` | the variables `plr_profile.macro_vars` names, per macro |
| `saved_at`, `seq`, `job_id` | the clock, the store, the job record |

**Where it is stored.** `<data>/helix-plr/snapshot.bin` (`SlotStore`): two
preallocated 4 KiB slots written alternately with `pwrite` + `fdatasync`, each with a
magic, `seq`, length, JSON payload and CRC32. A load takes the valid slot with the
higher `seq`. The file never changes size, so a save writes one data block and no inode
metadata, and a cut mid-write can only tear the slot being written: the other holds the
previous snapshot. A record too big for a slot moves `objects` and `macro_vars` to the
job record (`fit_record`, `overflow`).

Moonraker's own database is the wrong place for a 1 Hz record: it opens SQLite with
default pragmas (rollback journal, `synchronous=FULL`) and commits every upsert in its
own transaction, so each save would journal and rewrite two pages, create and delete the
journal and pay several fsyncs with their filesystem metadata, several times the slot
file's one block. Moonraker owns the connection, so the plugin cannot switch it to WAL.

**The job record.** Moonraker namespace `helix_plr`, key `job`: `job_id`, `filename`,
the file identity (`file_size`, `file_mtime`, `file_hash`: sha1 of the first and last
64 KiB, `helix_gcode_reader.file_identity`), `started_at`, and as they become known
`print_path` / `print_identity` (the file Klipper actually reads), `mesh`, `overflow`,
`lifted` and `finished_status`. Key `newest_job` names the last job history added,
whatever client started it.

**Lifecycle.** History's `added` event starts a job: the slots are cleared and a new job
record written (`_job_added`). Snapshots persist only while `print_stats.state` is
`printing`, plus one on entering pause. A job that finishes `completed` or `cancelled`
discards everything (`discard`: slots, job record, mesh file); any other end keeps it,
with the status in `finished_status`, and it survives restarts.

**Cadence** (`SavePolicy`). Every status frame is merged in memory. A save runs at most
once a second (`SAVE_INTERVAL_S`) while `file_position` moves, and at once on a layer
change, an exclusion, a change of speed or extrude factor, gcode offset, a heater target
or a fan, and on pause. The mesh is recorded once, when layer 1 is done.

**The file that is scanned.** `file_position` counts bytes of `virtual_sdcard.file_path`,
which is not always the gcodes file: QIDI prints from a `.cache` copy, and a `.3mf` from
an extracted plate. The first save records that path and its identity in the job record,
and the follower and the resume scan use it. The generic start selects the job's own
file, so the generic path refuses a plan when the two differ
(`plr_scan_failed: printed_copy`).

**The layer follower.** A full scan from byte 0 is too slow on a weak host: a 100 MB
OrcaSlicer file takes a median 394 s to scan on a K1C (MIPS, CPython 3.8), against
4.3 s on a desktop, and the time is CPU spent iterating lines, which no reducer change
closes (`moonraker-plugin/tools/plr_t1_scan_timing.py`, `make_t1_gcode.py`). So while
the print runs, `LayerFollower` reads the file up to `file_position` on a niced thread,
held to 15 % of one core (`DutyCycleLimiter`), and appends each layer start's offset
and modal state to the file's layer index. The index format, its prune rule and how it
is shared with file analysis are in `HELIXPRINT_PLUGIN.md` § "File Analysis".

### Storage safety and the default

Saving writes about once a second for the whole print, which an SD card with poor wear
levelling should not take by default. `classify_block_device` finds the disk behind the
plugin's data directory (`st_dev` -> `/sys/dev/block/<maj>:<min>`, a partition resolved
to its disk):

| Device | Evidence | Class |
|--------|----------|-------|
| `mmcblk*` | `device/type` reads `MMC` | `emmc` |
| `mmcblk*` | `device/type` reads `SD` | `sd` |
| `nvme*` | name | `ssd` |
| `sd*` | not removable and not rotational | `ssd` |
| `sd*` | otherwise (a USB stick) | `unknown` |
| `ubi*`, `mtdblock*` | name | `nand` |
| anything else, no block device | | `unknown` |

`storage_verdict` lets definite runtime evidence (`emmc`, `sd`, `ssd`) win over the
printer database's `storage` hint, so a CM4 with eMMC qualifies whatever its listing
says and a listed eMMC model booted from an SD card does not; `unknown` falls back to
the hint. `emmc` and `ssd` are wear-tolerant; raw NAND only when the database says the
model has it. `saving_enabled` is the user's `plr_user.enabled` when set, else
wear-tolerant storage on a printer listed at tier 1 or above. Saving also needs the
feature available (`_saving()` and the `saving` field both require
`availability()`), so an unlisted printer saves nothing even with `plr_user.enabled`
true. The settings switch is offered wherever the feature is available, which needs
the profile's tier to be 1 or more.

`HelixPlr.availability()` is the feature's probe, read synchronously on every status
call: Python 3.8 or newer, a `[virtual_sdcard]`, a writable snapshot store, and a
profile tier of 1 or more (`not_listed` otherwise, a missing record included). The
probe cannot read the database, so it answers from `listed_tier`, which every
`profile()` read updates. The plugin reads the profile at component init, and a
features status reads the switch (`feature_enabled()`, which reads the profile) before
the probe, so a profile the app writes shows in the next status; the app re-reads the
features map once the stored profile matches what it resolved.

### Tiers and the printer database `plr` block

| Tier | Z handling |
|------|-----------|
| 0 | Nothing offered, saving off by default. Every printer without a `plr` block |
| 1 | Assisted: the user jogs the nozzle down onto the top of the print and confirms |
| 2 | Trusted: the Z the steppers held, measured in the snapshot, with one visual confirm. Needs `z_holds_unpowered` |
| 3 | Re-referenced (probe a part-free spot, or home Z to max). Not built; acts as tier 2 |

The block in `assets/config/printer_database.json`, parsed by `plr_parse_db_block`:

| Key | Default | Meaning |
|-----|---------|---------|
| `tier` | required | Highest tier the model supports |
| `backend` | `generic` | `generic`, `qidi_print_state`, or `vendor` (a firmware backend serves it; the plugin does nothing) |
| `storage` | `unknown` | `emmc`, `sd`, `nand`, `ssd`, `unknown`: the storage hint |
| `z_holds_unpowered` | false | Required for tier 2 |
| `xy_home_gcode` | `G28 X Y` | The XY-only homing command |
| `z_lift_mm` | 5.0 | The lift before XY homing (never below 2 mm in the plugin) |
| `purge_gcode` | none | A purge macro; none means the generic purge line |
| `resume_skip_fraction` | 0.0 | See "Where it resumes" |
| `macro_vars` | none | `{macro: [variable, ...]}` saved in the snapshot and restored with `SET_GCODE_VARIABLE` |

The app writes the resolved profile as `plr_profile` (above). The plugin also reads
`nozzle_standby_drop_c` from that record (default 70 °C); the app does not write it, so
the default applies. `parse_profile` gives any missing or wrongly typed key its default,
and a missing record is tier 0.

`HelixPlr.backend` turns the profile into what PLR acts with: the tier is capped at 2
and lowered to 1 without `z_holds_unpowered`; `qidi_print_state` needs the QIDI adapter
selected and tier 2 (QIDI's sequence declares Z itself and has no jog step), else
nothing; `generic` acts at the effective tier below.

**XY homing check** (`check_xy_homing`, from `configfile.settings` at Klippy ready).
`G28 X Y` is unsafe on a delta (`delta_kinematics`) and when a `[homing_override]` runs
for X or Y and its G-code homes Z or a bare `G28` (`override_homes_z`), sets
`set_position_z` (`override_sets_z`), or runs any command not on the short list known to
leave Z alone (`override_unknown`; template tags are stripped first). Unsafe with the
default `xy_home_gcode` makes the effective tier 0.

### Offer rules

`offer_verdict` makes the offer available only when every rule holds; `blocked_by` lists
each that fails:

| Rule | `blocked_by` |
|------|--------------|
| The newest history job is `interrupted` (not `klippy_shutdown`, `error` or a finished status: only a power loss is offered) | `not_interrupted` |
| A snapshot exists for that job (QIDI backend: and QIDI's `file_position` row exists) | `no_snapshot` |
| The file's size, mtime and hash still match | `file_changed` |
| Layer 2 or later; without a layer count, 120 s of print time (`OFFER_MIN_PRINT_S`) | `too_early` |
| The effective tier is 1 or more | `not_supported` |
| At least 2 mm (`MIN_LIFT_MM`) between the snapshot Z and the top of travel, less 1 mm of sag allowance (generic backend) | `no_lift_room` |
| Klipper is ready | `klippy_not_ready` |

The app adds its idle, one-prompt and wizard rules on top (`plr_should_offer()`).

### Where it resumes

`virtual_sdcard` advances `file_position` when a line's move enters the toolhead's
lookahead, so the dispatch head leads the nozzle by up to a few seconds of motion
(Klipper's `BUFFER_TIME_HIGH`, 1-2 s by version, plus the lookahead queue). In bytes that ranges from a few hundred on long
infill lines to over 100 KB on fine curved walls, so no byte constant is right. Klipper
publishes both ends in one status frame, `toolhead.position` and
`motion_report.live_position`, and `helix_plr_scan.resume_point` uses the difference:

1. **Scan** (`scan_for_resume`) to `file_position`, reducing the modal state the way
   `gcode_move` does (G90/G91, M82/M83, G92, E with the extrude factor, X/Y/Z/F, `T<n>`,
   heater and fan commands, pressure advance, velocity limits, M220/M221, G10/G11,
   object blocks, layer markers). It starts at the last layer checkpoint at least a
   window (`WINDOW_BYTES`, 256 KiB) behind the head, else byte 0: the line executing at
   the snapshot can lie in the layer before the head's, because a layer-change save is
   taken as `SET_PRINT_STATS_INFO` is read, seconds ahead of the nozzle.
2. **Walk back** (`locate_executed_line`) from the head, subtracting each line's
   toolhead-E change (firmware retracts count `retract_length`) until it reaches
   `live_pos.e`; a line with no E change is matched by XY. A candidate counts only when
   its XY path passes within 0.5 mm of the nozzle (`MATCH_TOL_MM`, arcs by arc geometry)
   and its Z within 2 mm. The runtime gcode offset is taken off the nozzle first.
3. **Advance** (`advance_by_time`) by `resume_skip_fraction` x the save interval of
   motion, at each line's feed rate, because the nozzle kept going for up to one interval
   after the snapshot. The resume starts at the beginning of the line that lands in.
   **The default is 0.0, re-trace:** a resume reprints up to one interval rather than
   risk skipping plastic, since a ridge bonds and a gap does not. The plan reports
   `retrace_estimate_mm`.
4. **Fallback** (`next_layer_start`). When the walk finds nothing, the start of the next
   layer, with the `layer_start` warning and the `accept_next_layer` confirmation.
   Printing the rest of the current layer again over itself at the same Z risks a
   collision; skipping it leaves a cosmetic gap. The scan reads on past the head when it
   has not reached that layer. On the last layer, or in a file without layer markers,
   there is no plan. Never a byte rewind.
5. **Excluded objects** (`leave_excluded_object`). A point inside the block of an object
   the snapshot lists as excluded moves forward to the first move after it that extrudes
   outside every excluded object, with the modal state carried across the skipped lines.
   Those lines printed nothing in the original print, and the landing falls where the
   next printed path starts, never on the excluded (often the failed) object. Both
   estimates are 0. When nothing printed follows, the plan is refused
   (`excluded_object_open`). Names compare upper-cased, as Klipper's do.

The plan then checks what it can before anything moves: a single extruder (a snapshot
on another extruder or a `T<n>` other than 0 at the resume point is refused,
`multi_material`), a nozzle temperature to resume at, and that every name and value can
be placed in a command (`_check_renders`; refused as `unsafe_value`).

### The resume sequence (generic)

`prepare` and `commit` run around the app's confirmations; the steps are their
`helix_print:plr_progress` names.

**prepare**

1. `bed_wait`, `chamber_wait`, `nozzle_in_place`: `M140`, the chamber's
   `SET_HEATER_TEMPERATURE` and `M104` go out together, the nozzle heated where it
   stands, then the plugin waits for each. Nothing moves until the bed and the nozzle
   are hot: the nozzle froze into the part at the cut, so a cold lift or XY homing tears
   the part off the bed, and a reheated bed is the best chance for a part that released
   as it cooled to grip again. The nozzle temperature is the snapshot's extruder target,
   or the file's own target at the resume point when that is higher (a snapshot taken
   with the heater off still resumes at the temperature the resume line prints at).
2. `lift`: the nozzle lifts by `z_lift_mm`, or less when that does not fit under the
   top of travel. With Z homed, nothing is declared first. With Z unhomed, the first
   prepare declares the snapshot's `live_pos.z`
   (`SET_KINEMATIC_POSITION Z=... SET_HOMED=Z`); a later prepare of the same snapshot declares the
   recorded lifted Z when nothing has moved the nozzle since, and is refused otherwise
   (`plr_stale_plan: position_unknown`). The lift record is invalidated when a tier-1
   prepare ends and when a commit starts.
3. `nozzle_standby`: retract 1 mm while still at print temperature, then drop to
   standby: the print temperature less `nozzle_standby_drop_c`, no lower than 140 °C and
   never above the print temperature.
4. `home_xy`: `xy_home_gcode`. `SET_HOMED=Z` leaves X and Y unhomed, so a macro that
   homes only unhomed axes still homes them. Klipper builds whose
   `SET_KINEMATIC_POSITION` lacks `SET_HOMED` mark X and Y homed too, so before homing
   the plugin checks `homed_axes` and refuses (`set_homed_unsupported`) unless the
   homing is a plain `G28 X Y` with no `[homing_override]`.
5. `position` (tier 1): to the resume line's XY at the lifted height; the app's jog
   follows, with the nozzle at standby.

**commit**

1. The prepare heat steps again, so the nozzle is back at print temperature where it
   stands before anything moves; the bed may also have been turned off by
   `idle_timeout` while the screen waited. A commit is refused (`plr_stale_plan`) when
   Klipper restarted since prepare or the axes prepare homed are no longer homed.
2. `z_reference` (tier 1): the nozzle rests on the plastic, so Z is declared as the
   G-code Z of the last extruding move before the resume point, plus the gcode offset,
   plus the mesh (faded as `bed_mesh` fades it) when the resume loads one; then it lifts
   again.
3. `restore`: mesh, `SET_GCODE_OFFSET`, pressure advance, velocity limits,
   `SET_RETRACTION`, M220/M221, other heater and temperature-fan targets, macro
   variables.
4. `heat_nozzle`, `purge`: to the purge line (or, without one or before a purge macro,
   lifted to the XY homing position, `stepper_x/y position_endstop`), `M104`, the
   plugin's wait, then the purge macro, the generic purge line, or a short prime at the XY homing position.
   The generic line runs along a bed edge clear of every object's bounding box
   (`generic_purge_xy`), inside `[bed_mesh] mesh_min/mesh_max` or else the job mesh's
   bounds, clamped to the axis limits. With no clear edge (no objects known, or every edge
   covered) the plan carries `no_purge_location`, and at tier 1 with no XY homing
   position either it is refused.
5. `move_to_resume`: lifted travel to the resume line's XY, down to its Z, a small
   prime, then the coordinate and extrude modes, `G92 E`, F, fans and retract state.
6. `start`: one command, `M23 <file>`, `M26 S<offset>`, the state selecting a file
   clears, `M24` (`render_start`). `M23` resets `exclude_object` and `print_stats`'
   layer info, so between `M26` and `M24` go the scan's `EXCLUDE_OBJECT_DEFINE` lines,
   `EXCLUDE_OBJECT NAME=` per exclusion, `SET_PRINT_STATS_INFO`, and
   `EXCLUDE_OBJECT_START` for the object whose block the resume offset lies in, so its
   `EXCLUDE_OBJECT_END` matches. The first `EXCLUDE_OBJECT` after `M23` registers a
   transform that lets its first five extruding moves through unskipped
   (Klipper's `exclude_object` module, `_register_transform`), so with anything excluded the start
   follows the exclusions with six net-zero `G1 E` moves inside
   `SAVE_GCODE_STATE` / `RESTORE_GCODE_STATE` (`EXCLUDE_COUNTER_MOVES`), which use the
   count up and leave the file's E unchanged. Immediately before the start the plugin
   marks the job helix-initiated with kind `plr_resume` (`HELIXPRINT_PLUGIN.md`), so
   start-of-job features leave the resumed job alone, and withdraws the marker if the
   start fails.

`M23` finds files at the gcodes root only, so a file in a subfolder (or one whose name
`M23` would cut) starts through the root-level link `helix_plr_resume.gcode`; the
finished history entry is renamed back to the file (`_patch_history_entry`) and the link
removed when the job completes or is cancelled.

Heat waits are the plugin's own polls (every 2 s; 20 min bed, 10 min nozzle, 45 min
chamber), never `M190` or `TEMPERATURE_WAIT`: those hold Klipper's G-code lock, and an
abort's heaters-off could not run until the target was reached.

**Mesh carry-over.** A saved profile resumes with `BED_MESH_PROFILE LOAD=<profile>`. An
adaptive or unsaved mesh is written once layer 1 is done to `<config>/helix-plr/mesh.cfg` as a
`[bed_mesh helix_plr]` section in Klipper's autosave format, which Klipper loads after
the reboot if `printer.cfg` has `[include helix-plr/*.cfg]` (a wildcard include with no
match is legal). The Settings > Printing row "Resume reloads the saved bed mesh" adds
that line through `server.helix.plr.mesh_include`, behind a confirmation, with a single
rolling `printer.cfg.helixscreen-bak` beside the file; the same endpoint removes it, and
the plugin's uninstaller strips it. Without the line the plan carries `mesh_unavailable`
and the resume needs `accept_no_mesh`. A resumed job keeps the mesh file it resumed
with, so a second outage can load it again.

### Endpoints, events and confirmations

`server.helix.status` reports the feature as
`"plr": {"version": 2, "available", "enabled", "reason", "saving"}`. `enabled` is "saving
is on, or a saved snapshot is waiting", so switching saving off does not withdraw an
offer already saved; `saving` is the switch as applied and is what the settings row
shows. `reason` when unavailable: `python_too_old`, `no_virtual_sdcard`,
`store_unwritable`, `not_listed`.

| Method (JSON-RPC / HTTP) | Params | Reply |
|---|---|---|
| `server.helix.plr.status` / `GET /server/helix/plr/status` | none | `{"saving", "storage": {storage_class, wear_tolerant, source, device}, "tier", "backend": "generic"\|"qidi_print_state"\|"none", "snapshot": {"job_id", "filename", "layer", "z", "saved_at", "age_s"} \| null, "offer": {"available", "blocked_by"}}` |
| `server.helix.plr.plan` / `POST .../plan` | none | `{"plan_id", "tier", "filename", "method": "measured"\|"layer_start", "resume_offset", "layer", "z_gcode", "z_physical", "retrace_estimate_mm", "bed": {"target", "current"}, "restores", "warnings", "requires"}`; a scan over 10 s replies `{"pending": true, "plan_id"}` and sends `plr_plan_ready` |
| `server.helix.plr.prepare` / `POST .../prepare` | `{"plan_id", "skip_chamber_wait"}` | `{"ok": true}` once prepare is done |
| `server.helix.plr.commit` / `POST .../commit` | `{"plan_id", "confirmations": {name: true}, "z_reference": "part_top"\|"trusted"}` | `{"ok": true, "job_started": true}` |
| `server.helix.plr.abort` / `POST .../abort` | `{"plan_id"}` | `{"ok": true}`: stops the run, turns off the bed, extruder, every `heater_generic` and the part fan, keeps the snapshot |
| `server.helix.plr.discard` / `POST .../discard` | none | `{"ok": true}`: deletes the snapshot, job record and mesh file |
| `server.helix.plr.mesh_include` / `GET .../mesh_include` (status) or `POST` (`{"action": "add"\|"remove"}`) | see Params | status: `{"present"}`; an action: `{"present", "changed"}`, idempotent both ways. `add` appends the marker comment and the include to printer.cfg, `remove` strips exactly those two lines (whitespace-tolerant, a user's own include of the directory in any other spelling stays); both back the file up to `printer.cfg.helixscreen-bak` beside it first and write atomically, and Klipper reads the change at its next restart, which the app offers. Refuses with `plr_config_read` / `plr_config_write` / `plr_bad_action` |

- `status` answers from what Klippy ready decided (objects, homing, the QIDI check).
  Asked while Klippy is up or starting and that has not finished, it waits for the
  result, at most 45 s (`READY_WAIT_S`, under the app's 60 s request timeout; on the Q2
  Klippy's startup plus the QIDI check passes 15 s). The finished check also pushes the
  reply as `helix_print:plr_status`.
- Events go out as `helix_print:plr_progress` (`{plan_id, step, state}`, state
  `running` / `done` / `failed`), `helix_print:plr_plan_ready` (`{plan_id}`, plus
  `error` when the background scan failed) and `helix_print:plr_status`; clients receive
  them as `notify_plr_progress`, `notify_plr_plan_ready`, `notify_plr_status`. The heat
  steps go `running` together and `done` as each reaches temperature.
- A kept plan answered again re-reads the bed, so `bed.current` and `cold_bed` are
  current. A background scan that failed answers that error until the snapshot changes.
- Confirmations (`requires`): `part_attached` and `z_confirmed` at tier 2;
  `part_attached`, `nozzle_clear` and `z_jogged` at tier 1; plus `accept_no_mesh`
  (`mesh_unavailable`), `accept_cold_bed` (bed more than 10 °C under target,
  `COLD_BED_DIFF_C`) and `accept_next_layer` (`layer_start`). `commit` refuses a missing
  one, the wrong `z_reference` for the tier, and a `plan_id` that is not the latest or
  whose snapshot changed.
- Warnings: `layer_start`, `mesh_unavailable`, `cold_bed`, `no_purge_location`.
  `restores`: `bed_mesh`, `gcode_offset`, `pressure_advance`, `velocity_limits`,
  `retraction`, `speed_factor`, `extrude_factor`, `objects`, `excluded_objects`, `fans`,
  `macro_vars`.
- Errors are Moonraker errors whose message is `"<code>: <detail>"`: `plr_no_plan`
  (detail = the offer's `blocked_by`), `plr_stale_plan`, `plr_missing_confirmation`,
  `plr_busy`, `plr_scan_failed` (`no_live_position`, `multi_material`,
  `no_nozzle_target`, `no_purge_location`, `no_part_top`, `excluded_object_open`,
  `unsafe_value`, `printed_copy`, `qidi_landing_below_part`, `qidi_cache_mismatch`),
  `plr_klippy_error` (Klipper's text, a heat timeout, `set_homed_unsupported`,
  `aborted`).
- Single flight: each prepare or commit is a run with its own abort flag, taken before
  its first await; `plan`, `prepare`, `commit` and `discard` answer `plr_busy` until the
  previous run has exited. An abort is checked before every G-code send and immediately
  before the start, and clears the plan.

### The QIDI adapter

Newer QIDI firmware (the `/home/qidi` layout: Q2, Max 4) resumes from rows in its
Moonraker's SQLite `config` table, section `print_state`, which only QIDI's own screen
writes. `helix_plr_qidi.py` writes those rows from the generic snapshot and resumes
through QIDI's endpoint, so a printer running HelixScreen in place of QIDI's screen
still leaves a resumable print (#1716). `helix_plr.py` stays vendor-free and asks the
module capability questions.

**Selection** (at Klippy ready): `qidi_print_state_present` (Klipper lists
`SDCARD_POWER_OFF_CONTINUES_PINT_FILE` in `/printer/gcode/help`, or `gcode_move` carries
`speed_factor_2`) and `moonraker_resumes_print_state` (Moonraker has `power_off_print`
and the config-table methods). `speed_factor_2` alone also appears on older QIDI
firmware without the endpoint, hence both. The legacy QIDI backend
(`DETECT_INTERRUPTION` + `was_interrupted`) is outranked on this layout.

**Save side.** From the same snapshot loop, every row at job start (delayed 2 s,
`FIRST_WRITE_DELAY_S`, so it lands after QIDI's own start-time delete of the section),
then changed rows at most once a second, event saves at once; one transaction through
QIDI's `update_value_from_config_batch` where it exists. Every value is a string in the
format QIDI's parser reads:

| Row | Written as |
|-----|-----------|
| `file_position` | integer string |
| `gcode_position`, `homing_position`, `base_position` | JSON 4-vectors |
| `speed` | gcode_move's internal speed (`speed_2`) |
| `speed_factor` | the internal factor (`speed_factor_2`, 1/60 at 100 %), never the public 1.0 |
| `extrude_factor` | `%f` |
| `absolute_coord`, `absolute_extrude` | `"1"` / `"0"` |
| `extruder_target`, `heater_bed_target`, `chamber_target` | integer string, `%f` when fractional |
| `cooling_fan_speed`, `auxiliary_cooling_fan_speed`, `chamber_circulation_fan_speed` | `%f`, 0..1 |
| `all_objects`, `exclude_objects` | JSON |
| `plate_index`, `current_layer` | strings |

QIDI's parser truncates with `int()`, so a plain `%f` sends one less than intended for
many values (`int(float("0.015000") * 60 * 100)` is 89, not 90; QIDI's own screen has
the same fault). `_fmt_truncated` raises the value by the fewest millionths that make
the parser send `round(value x scale)`.

**Resume side.** `prepare` is the heat wait (bed, chamber and nozzle in place, set
together), then `nozzle_standby` in place, with no lift: QIDI declares Z from its row and
lands on it, so a lift would shift the frame. `commit` checks that the file in QIDI's
`.cache` is the one printed (size and hash, `qidi_cache_mismatch`), then inside its
`start` step runs the heat wait again, rewrites every row for the measured resume point
in one transaction, marks the job and calls `printer.print.power_off_print`; the marker
is withdrawn if the call fails. QIDI's sequence sends `M140` and `M104` without waiting,
lifts and homes X and Y, and only then waits with `M109`, so without the plugin's wait a
nozzle that cooled during the confirmations would be lifted and homed cold. `abort`
keeps the rows, so the offer stands; `discard` deletes the section as well. The plan
skips the purge and the mesh, which QIDI's sequence does itself.

- **+4096.** QIDI starts at `max(row - 4096, 0)`, so the `file_position` row is the
  resume offset clamped into the file, plus 4096. QIDI never checks the row against the
  file size, so a point in the file's last 4 KiB still lands exactly.
- **The Z row.** QIDI declares `SET_KINEMATIC_POSITION Z=<row z>`, applies the gcode
  offset and the mesh, then lands with `G1 Z<row z>`. The declared value is the frame
  the rest of the print runs in, so the adapter writes the physical Z (`live_pos.z`, else
  `toolhead_pos.z`; `frame_z`), not the G-code Z QIDI's screen stores, which leaves the
  whole remaining print off by offset + mesh. What no row can fix: the declaration and the
  landing share it, so the rest of the resumed layer, until the file's next Z move,
  prints off by `gcode_offset_z + mesh(x, y at the cut)`. A plan whose landing would end
  more than 0.05 mm below the plastic is refused (`qidi_landing_below_part`). QIDI also
  loads the mesh named in `save_variables.profile_name`, not the print's own.
- **Objects.** With more than 64 objects QIDI clears all objects and exclusions before
  resuming, so excluded objects print again.
- **An object open at the resume point.** QIDI starts the file with no object entered
  and nothing can run between its actions and the file, so the rest of that block prints
  outside any object: its `EXCLUDE_OBJECT_END` logs `does not match the current object
  NAME=None`, and excluding that object before the block ends does not stop it. Later
  blocks are tracked normally. An object excluded before the cut is unaffected: the
  resume point moves past its block.
- **Excluded objects after the resume.** QIDI sends `EXCLUDE_OBJECT` for its row before
  starting the file, which registers the transform with its five-move allowance, and
  nothing can be sent in between, so up to five extruding moves of an excluded object can
  print after a QIDI resume. QIDI resumes are not refused for it.
- **The resumed job's name.** QIDI opens `glob(".cache/*")[0]` and passes that path on, so
  history records `/home/qidi/printer_data/.cache/<name>`; the marker is keyed on that
  path.

### Testing

The C++ side is tag `[plr]` (files in the code map at the top); the plugin side is the
pytest files above, run by `make land-gate`. The mock (`HELIX_MOCK_PLR`,
`HELIX_MOCK_PLR_TIER`, `HELIX_MOCK_PLR_WARNINGS`, `HELIX_MOCK_PLR_FAIL`;
`MOCK_ENVIRONMENT_VARIABLES.md`) drives the offer and every wizard step in `--test`.

Most of what a power cut exercises can be produced without pulling a plug. Every run of
the first two methods heats and moves a printer, so each needs Preston's OK.

**M1: host-stack kill.** SIGKILL Moonraker, then klippy, mid-print, in one SSH command,
then restart both. Name the processes by PID or an anchored `pgrep -x`: a `pgrep -f
klippy` inside `sh -c '...'` also matches that shell's command line.

- The MCU shuts down on host loss: heater PWM outputs carry a 3 s maximum duration, the
  MCU's timer trips `Missed scheduling of next ...`, its shutdown drives every digital
  output to its default (off) and stops stepping. A stepper with a dedicated
  `enable_pin` is disabled; one with no `enable_pin`, or a TMC driver enabled through
  `toff` register writes, keeps holding current. So M1 says nothing about Z holding
  unpowered unless every Z stepper has a dedicated `enable_pin`.
- Kill Moonraker first. On startup Moonraker rewrites every `in_progress` job to
  `interrupted`; if klippy dies while Moonraker is up, history finishes the job as
  `klippy_disconnect` and no offer appears. The snapshot is as stale as the gap between
  the two kills.
- After the restart klippy refuses to configure the shut-down MCU and needs a
  `FIRMWARE_RESTART`, which moves nothing; a real cut resets the MCU itself.

**M2: host hard reboot without sync.** `echo b > /proc/sysrq-trigger` as root (after
`echo 1 > /proc/sys/kernel/sysrq`). The kernel reboots at once and drops dirty pages, as
a cut does, which tests the slot file on a real filesystem. Torn-slot handling itself is
proven in pytest. A USB-powered MCU usually resets; one powered from the 24 V rail may
only shut down as in M1. Use sparingly.

**M3: no interruption.** Log `file_position`, `toolhead.position` and
`motion_report.live_position` during a normal print and run `locate_executed_line`
offline; the scan timing tools above; the mock.

**What only a real cut adds:** PSU hold-up timing, vendor firmware we do not run, and
final end-to-end confirmation. A long outage's bed cool-down is approximated by a long
wait after M1, since the MCU has already turned the heaters off.

**Results.** On a QIDI Q2 (firmware 01.01.02.01, QIDI path), M1 kills at layer 10 of a
40 x 40 x 6 mm PLA box resumed. With `resume_skip_fraction` 0.5 one seam separated and
one bonded; with 0.0 (re-trace, the default) the seam was solid. The generic path has
run only in pytest and the mock: the Q2 cannot exercise it, since QIDI prints from its
`.cache` copy and the generic path refuses that (`printed_copy`). Hardware testing on a
stock-Klipper printer is open: #1863.

---

## Capability detection: `print_stats.power_loss`

`power_loss` is a `print_stats` field that exists **only** in Creality's fork.
Mainline Klipper has no such key. So the marker is the **presence** of the key,
not its value — it normally reads `0` and only becomes `1` after the probe.

Two traps:

1. **Presence must mean "present *and* numeric".** We subscribe to a narrowed
   field list, and Moonraker answers a subscribed-but-unpopulated field with an
   explicit `null`. So on mainline Klipper the key *is* present in the payload,
   as `null`. `is_number()` is what actually discriminates the fork.
2. **The capability latches up and never down.** Moonraker sends deltas; a later
   `print_stats` notification carrying only `print_duration` has no `power_loss`
   key at all. Clearing on absence would produce a spurious 1→0→1 edge and
   re-fire the side-effectful probe. Capability is reset only on the disconnect
   edge, by the offer controller.

Subscription: `power_loss` comes from this backend's row in
`plr_required_status_fields()`. Nothing in discovery marks the fork, so the row
subscribes it on every printer; the others answer it with `null`.

---

## SAFETY: the probe must precede the resume

**This ordering is mandatory, not stylistic.**

The stock Creality sensorless-homing macro gates its pre-homing Z clearance lift
on:

```jinja
{% if printer.print_stats.z_pos|float <= 20.0 or printer.print_stats.power_loss == 1 %}
    ... big lift ...
{% else %}
    ... 0.1mm lift ...
{% endif %}
```

After a power loss high up in a tall print, `z_pos` correctly reads e.g. 150. So
**without** `power_loss == 1` the machine lifts only 0.1 mm and then
sensorless-homes X/Y — dragging the nozzle straight through the part.

Calling `check_continue_print_state` is what sets `print_stats.power_loss = 1`
(and only when both states are true and `print_stats.state == "standby"`).

> **Never issue the Creality resume gcode unless the probe has been made this
> connection and returned both states true.**

This is enforced in code, not by comment:

* `PlrDetectResult::completed` is set **only** by
  `plr_parse_check_continue_response()` on a well-formed
  `result.{file_state,eeprom_state}` boolean pair.
* `plr_build_plan(CREALITY, file, detect)` returns an **empty `resume_gcode`**
  unless `detect.completed && detect.file_state && detect.eeprom_state`.
* `PlrOfferController` refuses to show the prompt when
  `plan.resume_allowed()` is false, and does not latch the one-shot.
* The prompt's Resume handler re-checks `plan.resume_allowed()` before sending.

Pinned by `tests/unit/test_plr_backend.cpp` — "CREALITY refuses resume when the
probe never ran" and its half-confirmed siblings.

### Second footgun: the EEPROM gate

The Klipper-side resume branch requires the `bl24c16f` EEPROM object. If it is
absent, `ISCONTINUEPRINT=1` **silently starts the print from the beginning**
rather than failing. The `file_state && eeprom_state` gate covers this. Do not
weaken it to `file_state` alone.

### Probe discipline

`check_continue_print_state` is **side-effectful**:

* on JSON parse failure it **deletes** the recovery sidecar;
* it clears `exclude_object_info` when the state is false;
* it sets `print_stats.power_loss = 1` when both states are true.

Therefore the probe is issued:

* **at most once per connection** (latched by `PlrProbeSession::fired`,
  re-armed only on a CONNECTED → not-CONNECTED edge);
* **only when `print_stats.state == "standby"`**;
* **never polled**.

---

## Normalization and reuse

All four backends collapse into one internal signal so the existing latch /
re-arm / wizard logic is reused unchanged:

```
Snapmaker:  plr_snapshot_valid subject ────────────────┐
Qidi:       DETECT_INTERRUPTION macro + was_interrupted ┤
Creality:   power_loss key ──> one-shot probe ──> both ─┤──> recovery_available ──> plr_should_offer()
                               (standby only)    states │
Plugin:     plr feature Ready ──> plr.status ──> offer ─┘
```

`PlrOfferSignals::recovery_available` is that normalized field, and
`plr_offer_availability()` computes it from the provider table.
`plr_should_offer()` stays pure — no LVGL, no threading, no singletons — and its
other three inputs (`printer_idle`, `already_prompted`, `wizard_active`) are
unchanged.

The `printer_idle` input comes from the derived lifecycle, not the wire: the
controller computes `idle = !job_holds_machine(get_print_lifecycle())`
(`src/ui/print/ui_plr_offer_controller.cpp#idle`). During a host-side pre-print block
`print_stats` still reads `standby` — it describes the previous job — so an
idle check derived from the wire would offer "Resume interrupted print?" on top
of a start the user has already committed to, and the Resume button would start
a different file than the one they chose. Anchoring idle to the lifecycle means
a committed start never gets the offer. (`PRINT_STATE_MACHINE.md` § "Asking
whether a job owns the machine" covers the predicate and why `print_active` is
the wrong signal for this question.)

The one-shot latch, the wizard-close re-evaluation, and the disconnect re-arm
are documented at the decision site, `PlrOfferController::evaluate_offer()`.

### Why the prompt carries a plan

`show_plr_recovery_prompt()` takes a `PlrRecoveryPlan` by value. The plan owns
its strings, so the button handlers never re-derive the backend or re-check the
invariant against live state that may have moved on between the offer and the
tap. The modal itself stays backend-agnostic: primary button runs
`plan.resume_gcode`, or opens the wizard when the plan carries a
`plan.resume_rpc` (GENERIC); secondary runs `plan.discard_gcode` **or**
`plan.discard_rpc_method`.

### Filename safety

The Creality resume gcode embeds the filename as an extended-command parameter,
and **it must be double-quoted**:

```
SDCARD_PRINT_FILE FILENAME="/usr/data/printer_data/gcodes/My Part_PLA.gcode" ISCONTINUEPRINT=1
```

Klipper tokenizes extended parameters with `shlex` in POSIX mode
(`whitespace_split=True`, `commenters="#;"`), then splits each token on the
first `=`. An unquoted filename containing a space therefore produces tokens
with no `=` at all, and the whole command is rejected before the file is ever
looked up:

```
{"code":"key514", "msg": "Malformed command args 'SDCARD_PRINT_FILE FILENAME=/usr/data/
 printer_data/gcodes/PTOP Phone Stand_Elegoo PLA Matte Slate Grey_1h55m.gcode
 ISCONTINUEPRINT=1 ; from helixscreen'", "values": ["not enough values to unpack
 (expected 2, got 1)"]}
```

Spaces are the common case, not the exotic one — slicers put the model and
filament names in the filename. Moonraker's own print-start path quotes it
identically (klippy_apis.py, `SDCARD_PRINT_FILE FILENAME="{filename}"`).

Inside the quotes, `\n`, `\r`, `;`, `#`, `*`, `=`, `"` and `\` are still
rejected: `"` closes the value early, `\` is shlex's POSIX escape, and the rest
are comment/terminator characters that older regex-path Klipper forks strip
before shlex ever runs. `plr_is_safe_recovery_filename()` enforces that;
`MoonrakerAPI::is_safe_gcode_param()` is the wrong helper here because it
rejects whitespace. `IMoonrakerAPI::is_safe_material_param()` allows whitespace
but is the wrong helper too, and in the other direction: it is an **allowlist**
(alphanumerics plus `+ - _ . ( ) /`), while a slicer filename legitimately
carries commas, brackets, percent signs and non-ASCII. `kGcodeParamBreakers`
is the **blocklist** form of the same idea - `\n\r;#*="\` - which is what a
free-form filename needs. Same shlex mechanism, three call-site-specific
charsets; the material one is in `FILAMENT_MANAGEMENT.md` § "Material names as
G-code parameter values".

> A trailing ` ; from helixscreen` comment would ALSO break this command, and
> for a non-obvious reason: Creality special-cases `SDCARD_PRINT_FILE` to use a
> second regex (`extended_r1`) whose terminators are `|` and `*`, *not* `;` and
> `#` — "Support filename contain '#'". So the `;` survives into
> `shlex.split()`, which is called without `comments=True` and therefore treats
> `;` as an ordinary character. HelixScreen no longer annotates outgoing G-code
> at all (`src/api/moonraker_gcode_guards.h`), so this is historical — but it is
> why the field report showed both faults in one line.

### Path form: relative, not absolute

The sidecar stores an **absolute** path (`/usr/data/printer_data/gcodes/…`),
because Klipper writes it from the opened file handle's `.name`.
`SDCARD_PRINT_FILE` cannot be given that path:

```python
if filename[0] == '/':
    filename = filename[1:]          # cmd_SDCARD_PRINT_FILE
self._load_file(gcmd, filename, check_subdirs=True)
```

`_load_file` looks the name up in `get_file_list()`, whose entries are relative
to `sdcard_dirname` (`/usr/data/printer_data/gcodes` on K1-class). A
de-slashed absolute path is not in that list, so the lookup raises and the user
gets `{"code":"key121", "msg": "Unable to open file"}`.

There is a second, quieter reason. Once the file is open, the resume branch
does:

```python
if result.get("file_path", "") == self.current_file.name:
    sameFileName = True
else:
    os.remove(self.print_file_name_path)   # recovery data discarded
```

`current_file.name` is `os.path.join(sdcard_dirname, <relative name>)`. Sending
the relative name is what makes that comparison succeed; anything else that
still manages to open would **delete the recovery snapshot and print from the
beginning**.

`plr_creality_sdcard_relative_name()` does the stripping — known data roots
first (`/usr/data`, `/mnt/UDISK`), then the first `/gcodes/` segment as a
fallback for a relocated `virtual_sdcard: path`.

Verified by reading klippy/extras/virtual_sdcard.py and klippy/gcode.py on a
physical K1C.

With no resolvable filename there is no safe command to send, so the offer is
suppressed entirely rather than showing a Resume button that cannot work.

---

## Threading

Status notifications arrive on the libhv WebSocket thread, but
`MoonrakerManager` queues them and its main-thread drain calls
`update_from_status()`, so the capability parsers (which only mutate an
already-initialized `lv_subject_t` int in place) execute on the main thread,
matching the surrounding code in `printer_print_state.cpp`.

`PlrOfferController`'s observers are registered with `observe<int>`, which
defers through `UpdateQueue`, so every callback body runs on the main thread.
The one place that genuinely crosses the boundary is the probe response — a
JSON-RPC callback that arrives on the WebSocket thread — so it is wrapped in
`AsyncLifetimeGuard::bg_cb()`. The plugin's status reply takes the same path,
and carries the connection generation it was asked in, so a reply from a dropped
link is ignored.

The wizard's replies, progress notifications and jog acks also return through its
`lifetime_.bg_cb()`, each tagged with the opening it was asked in: a reply that
lands after the wizard closed, or after it was reopened, is dropped.
