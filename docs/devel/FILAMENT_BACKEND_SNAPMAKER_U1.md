# Snapmaker U1 (SnapSwap) Filament Backend

> HW-VERIFY-PENDING: drafted from source; not yet verified against the U1 rig.

The Snapmaker U1 is a 4-toolhead parallel toolchanger: four independent extruders
(`extruder`, `extruder1`-`extruder3`), each with its own filament path from its own
feeder slot to its own nozzle — no hub, no selector, no shared bowden. Topology is
`PathTopology::PARALLEL`; tool N sources slot N directly (fixed 1:1 mapping). Spools
identify per channel via RFID (`filament_detect.info`). Platform context:
[printers/SNAPMAKER_U1_SUPPORT.md](printers/SNAPMAKER_U1_SUPPORT.md).

## Snapmaker U1 (SnapSwap)

> **Status: drafted from source; hardware verification pending.** Code comments record
> the `AUTO_FEEDING` load/unload commands and pause classification as verified live on a
> physical U1 (firmware 20260608, #991), and the 39 `channel_state` values were captured
> live from that firmware. The RFID read path below is code-verified line-by-line but not
> against physical tags, and the resume-after-runout chain is **not field-tested**
> end-to-end. The rest of this document has not been exercised against the rig
> (192.168.30.103). Treat every claim here as source-derived until the banner above comes
> off.

### Hardware and Topology

One unit ("SnapSwap"), four slots, one per toolhead:

```
  Slot 0 ── Toolhead 0
  Slot 1 ── Toolhead 1     (each lane: own feeder, own path, own nozzle)
  Slot 2 ── Toolhead 2
  Slot 3 ── Toolhead 3
```

- `NUM_TOOLS = 4`, slot `i` carries `extruder_name` `"extruder"` / `"extruder{i}"`
  (`ams_backend_snapmaker.cpp:259-267`).
- `PathTopology::PARALLEL` on both the unit and `get_topology()`
  (`include/ams_backend_snapmaker.h:100-103`). Because every lane has an independent
  path, `needs_unload_before_load()` is answered by the base class — the serial
  lane rule never applies (`include/ams_backend_snapmaker.h:105-108`).
- `tip_method = TipMethod::NONE` — the U1 has no cutter and forms no discrete tip;
  unload is heat + retract, so the unload stepper renders "Heat nozzle -> Retract"
  (`ams_backend_snapmaker.cpp:243-247`).
- `tool_to_slot_map` is seeded with identity because that is the literal truth about
  this machine's **physical attachment**: four heads, each permanently holding its own
  spool (`ams_backend_snapmaker.cpp:273-289`). Its consumers are the ones that need
  attachment — the Load/Unload slot resolver (`filament_op_slot_resolver.h`) and the
  persisted tool-map ledger (`ams_tool_map_sync.h`).

  It is **not** the print routing, and nothing may read it as such. Which head prints
  logical tool N is a separate question answered by `print_task_config.extruder_map_table`
  and published by `get_tool_mapping()`. The two coincide only when nothing is remapped.
  (The identity seed was originally added so the gcode viewer would stop falling back to
  a single palette color; the viewer read it as routing, which painted a whole
  4-tool model in one head's filament. The viewer now resolves through
  `get_tool_mapping()` instead, so this map has no display job.)
- A slot select IS a physical tool change: `do_select_slot()` forwards to
  `do_change_tool()`, which emits `T{n}` and moves the carriage
  (`select_slot_moves_toolhead() = true`, `ams_backend_snapmaker.cpp:537-547`).

### Detection

**Wired to one object.** `PrinterDiscovery::parse_objects()` sets `has_snapmaker_` when
the Klipper object list contains `filament_detect` — unique to U1 firmware
(`include/printer_discovery.h:413-416`). Registration order matters: a real aftermarket
MMU (AFC, Happy Hare, …) always wins even on U1 hardware that also reports
`filament_detect`; the Snapmaker backend is the fallback for a stock U1 with no MMU,
and a bare `toolchanger` object alone is not enough (`include/printer_discovery.h:552-580`).

### Status the Backend Reads

The subscription is the standing whole-frame `notify_status_update` hook every
`AmsSubscriptionBackend` gets (`src/printer/ams_subscription_backend.cpp:57-68`); the
parse keys off whatever objects the frame carries:

| Klipper object | Fields used | Meaning |
|----------------|-------------|---------|
| `extruder`, `extruder1`-`extruder3` | `state`, `park_pin`, `active_pin`, `activating_move`, `extruder_offset`, `switch_count`, `retry_count`, `error_count` | Per-tool toolchanger state (`ExtruderToolState`) |
| `toolhead` | `extruder` | Which extruder the carriage holds — authority for the active tool |
| `filament_detect` | `info` (per-channel RFID array), `state` (`[int x4]`) | Spool metadata per channel; 1 = filament present |
| `filament_feed left` / `filament_feed right` | per-`extruder{N}`: `filament_detected`, `channel_state`, `channel_error` | Port/buffer presence, the feed state machine, per-channel errors |
| `print_task_config` | `filament_exist`, `filament_type`, `filament_vendor`, `filament_color_rgba` | Firmware-authoritative filament metadata (see the native API doc) |
| `filament_motion_sensor e{N}_filament` (and the `filament_switch_sensor` form) | `filament_detected` | Per-tool runout encoder |

Slot status arbitration across those sources, in parse order: extruder pins
(`active_pin` -> LOADED, `park_pin` -> AVAILABLE), then `filament_detect.state` only
when status is still UNKNOWN, then the port sensor, then `print_task_config.filament_exist`.
The active tool is detected from extruder pin state or `toolhead.extruder`, and
`current_slot`/`current_tool` track the picked-up tool 1:1
(`ams_backend_snapmaker.cpp:1094-1165`, `1456-1529`).

### RFID (filament_detect.info)

`parse_rfid_info()` reads per-channel tag fields
(`ams_backend_snapmaker.cpp:999-1054`):

| Tag field | Maps to | Notes |
|-----------|---------|-------|
| `MAIN_TYPE` | `material` | e.g. "PLA", "PETG"; `"NONE"` skips the whole slot's RFID apply |
| `SUB_TYPE` | `spool_name` | Snapmaker product line, e.g. "SnapSpeed" |
| `MANUFACTURER` (fallback `VENDOR`) | `brand` | |
| `ARGB_COLOR` | `color_rgb` | masked to the low 24 bits |
| `HOTEND_MIN_TEMP` / `HOTEND_MAX_TEMP` / `BED_TEMP` | `nozzle_temp_min` / `nozzle_temp_max` / `bed_temp` | |
| `WEIGHT` | `total_weight_g` | |
| `CARD_UID` | fingerprint string | 4-byte array canonicalized to `"144,32,196,2"`; drives the override-clear hardware event |

The RFID tag exposes no color *name* — `color_name` stays firmware-unset and is
user-editable only. `SUB_TYPE` is recognized as a product line only when it matches one
of eight known literals ("Basic", "Matte", "SnapSpeed", "Silk", "Support", "HF", "95A",
"95A HF"); a free-form user `spool_name` is never round-tripped to firmware as a
SUB_TYPE (`ams_backend_snapmaker.cpp:39-49`, `860-869`).

Every row above is code-verified against `parse_rfid_info()` and the apply loop
(`ams_backend_snapmaker.cpp:999-1054`, `1171-1204`): tag identity rides
`filament_detect.info[ch].CARD_UID`, and a `MAIN_TYPE == "NONE"` tag skips the field
apply while its UID is still captured for swap detection (`:1173-1182`). Physical reads
from real RFID spools remain rig-pending; code-verified is not field-verified.

### Commands the Backend Emits

| Command / call | Used for |
|----------------|----------|
| `AUTO_FEEDING EXTRUDER=<n> LOAD=1` | Load (`do_load_filament`, `ams_backend_snapmaker.cpp:436-462`) |
| `AUTO_FEEDING EXTRUDER=<n> UNLOAD=1` | Unload (`do_unload_filament`, `:456-489`) |
| `INNER_FILAMENT_UNLOAD` | Bare unload fallback only when no slot/extruder can be resolved (`:480-482`) |
| `T<n>` | Tool change / slot select (`do_change_tool`, `:532-538`) |
| `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<l> MAP_EXTRUDER=<p>` | Pre-print logical->physical remap (`build_preprint_gcode`, `:1887-1889`) |
| `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv>` | Pre-print feed gating — always sent, remap or not (`:1907`) |
| `POST /printer/filament_detect/set` | Slot-metadata writeback (paxx12 Extended Firmware REST, `:854-924`) |

Why `AUTO_FEEDING ... LOAD=1` and not the obvious alternatives — the source records the
trail (`ams_backend_snapmaker.cpp:452-461`): bare `T{n}` is a no-op when the target tool
is already active; `AUTO_FEEDING EXTRUDER={n}` alone is a silent no-op because
`FEED_AUTO` falls through without a LOAD/UNLOAD parameter; `SM_PRINT_AUTO_FEED` is gated
on `print_task_config.extruders_used`, which sits all-false during a paused print. The
same comment names the firmware reference (the cmd_FEED_AUTO handler in Snapmaker's
filament_feed extension, ~line 1681). Unload
must use the same envelope: the bare `INNER_FILAMENT_UNLOAD` leaf skips the feed state
machine and breaks aftermarket feeders that hook `unload_finish` (the DnG-Crafts U1-Ace
ACE-Pro adapter, #974).

### The channel_state Feed Machine

The firmware exposes 39 distinct `filament_feed` channel states (captured live from
firmware 20260608); `classify_channel_state()` maps each to
`{action, phase, terminal, fail, sets_loaded, clears_loaded}` off one table, with a
conservative prefix/suffix fallback for unknown future states
(`ams_backend_snapmaker.cpp:79-227`). That single classification drives:

- **The operation step bar.** LOAD/manual/preload share a 5-step model
  (Home -> Select -> Heat -> Feed -> Purge); UNLOAD uses 4 steps ending in Retract; the
  Heat step shows a live nozzle temperature. The current index is published through the
  shared `ams_operation_phase` subject, which the sidebar consumes generically
  (`ams_backend_snapmaker.cpp:341-380`).
- **The per-tool "loaded at toolhead" latch.** Set on `load_finish`; cleared on
  `unload_finish` / `wait_insert` / `preload_finish`; left unchanged on transient and
  fail states. This latch — not the motion sensor — is the authority for
  `slot_has_filament_at_toolhead()`, `can_unload_from_toolhead()`, and the NOZZLE path
  segment, because the per-tool encoder fails to drop to false after an unload on
  current firmware (`include/ams_backend_snapmaker.h:332-348`,
  `ams_backend_snapmaker.cpp:500-526`).
- **Action lifecycle and errors.** `*_fail` states and `channel_error` tokens surface as
  `AmsAction::ERROR` with a direction-aware message ("No filament in lane N. Load
  filament and retry." for the `no_filament` token), except when the lane is empty,
  idle, and not the active lane — the firmware reports `no_filament` for any empty lane,
  which must not latch a spurious error modal on a deliberately unloaded head in a
  multi-color print (`ams_backend_snapmaker.cpp:57-77`, `1321-1354`).
- **`preload_finish` is terminal-for-latch but does not end the operation** — a re-unload
  of a staged lane keeps that state while the nozzle heats, and dropping to Idle there
  killed the unload step display mid-heat (`ams_backend_snapmaker.cpp:1424-1439`).

A `*_finish` that clears the latch also demotes the slot LOADED -> AVAILABLE and clears
`filament_loaded` for the active tool, but never resets `current_slot`/`current_tool`:
those track the picked-up tool, and resetting them mis-routed a bare unload to T0 for a
user printing TPU without feeders (field report recorded at
`ams_backend_snapmaker.cpp:1387-1401`). Lanes reaching `unload_finish` are reported to
`AmsState::mark_slot_unloaded()` after the mutex is released so `FilamentSensorManager`
suppresses the runout modal during the expected pull-out grace window
(`ams_backend_snapmaker.cpp:1414-1423`, `1694-1699`) — the deferral exists because
calling into `AmsState` under our mutex inverted `add_backend()`'s lock order (TSan,
2026-08-16).

### Runout and Resume

`prepare_for_resume()` (`ams_backend_snapmaker.cpp:597-735`) classifies the pause first:
dirty-bed exceptions (`{id:532, code:1}`, or the message text) are Terminal and surface
the restart UX; runout (`{id:523, code:0}`, "e{N}_filament runout") is Recoverable
(`include/snapmaker_resume.h:11-19` — `sdcard` state deliberately not consulted because
runout also clears it). With the motion sensor reading filament, RESUME needs no prep.
With runout latched, the backend drives
`AUTO_FEEDING EXTRUDER=<n> LOAD=1 PRINTING=1` itself — the firmware's own
`INNER_RESUME` auto-feed is gated on `extruders_used`, which stays false mid-print, so a
plain RESUME re-pauses immediately. The chain heats, feeds, and flushes (~86 s measured;
150 s timeout, silent send), raises an info toast for the wait, and hands control back so
the caller dispatches RESUME; `on_ready` always fires on the main thread. What is
recorded as live-verified (#991) is the `AUTO_FEEDING` command itself - it blocks until
`load_finish`, is idempotent, and the ~86 s figure was measured live
(`ams_backend_snapmaker.cpp:662-675`, `:722-724`). The full chain - runout pause ->
runout dialog -> refeed -> RESUME -> print continues - is **not field-tested**.

Related capability flags: `recovers_filament_on_resume() = true` (Resume re-feeds, so
the runout dialog presents Resume as primary) and
`should_suppress_idle_runout_modal() = true` (the U1 drives load/unload itself, so an
idle lane going empty needs no operator action) (`include/ams_backend_snapmaker.h:194-200`).

`is_stuck_motion_sensor_runout()` (motion sensor false, port sensor true = stale encoder)
currently has **no caller in tree** — the auto-recover path that consumed it was pulled
because that signal cannot distinguish "stale encoder" from "preloaded 4 inches short of
the gear"; it is kept as detection infrastructure for a deferred follow-up
(`ams_backend_snapmaker.cpp:569-593`). The active tool's port-present flag it builds on
is still published to `AmsState::set_active_tool_port_present()` on change (#991), which
is what gates Resume in the runout dialog (`ams_backend_snapmaker.cpp:1721-1742`).

### Pre-Print Remap (RemapStrategy::SnapmakerNative)

`SET_PRINT_EXTRUDER_MAP` / `SET_PRINT_USED_EXTRUDERS` error mid-print (firmware id 531),
so the config must land before `PRINT_START`. `requires_preprint_send() = true` is
**always-on, even with no remap**: `SET_PRINT_USED_EXTRUDERS` suppresses the spurious
auto-feed of unused heads baked into every Orca-sliced file, which otherwise feeds an
empty head and cancels the print on runout (`include/ams_backend_snapmaker.h:230-237`,
`src/ui/ui_print_start_controller.cpp:315-332`).

Send ordering is guaranteed on our side of the wire. Both start paths gate on
`requires_preprint_send()` and hand the real start step to
`send_snapmaker_preprint_then()` as its completion continuation, then `return` - the
start cannot fire first (`src/ui/ui_print_start_controller.cpp:323-337`, reprint
`:454-471`). The built gcode goes out as a single `printer.gcode.script` JSON-RPC with a
15 s timeout (`:355-404`, dispatched at `src/api/moonraker_api_controls.cpp:426`); the
print-start request is issued only from that request's success callback, and an error or
timeout aborts with a modal so the print never starts half-configured (`:388-403`). The
upload/prep window that follows only widens the gap. What this does NOT settle is
firmware-side: whether the ack means `print_task_config` is already mutated - and stays
mutated - by the time the baked `PRINT_START` block executes. That stays in the config
doc's "Still UNCERTAIN" list.

`build_preprint_gcode(tools_used, remap)` is pure (no API access; unit-tested directly):
one `SET_PRINT_EXTRUDER_MAP` per user remap entry, then one
`SET_PRINT_USED_EXTRUDERS` with the deduplicated, ascending physical-head CSV resolved
through the remap. Logical tools 4-31 without an explicit remap fall to head 0, matching
the firmware's default map (`ams_backend_snapmaker.cpp:1937-1986`). Full command
semantics — logical (0-31) vs physical (0-3) index rules, persistence behavior, the
`filament_official` FORCE gate — live in
[Firmware API: `print_task_config`](#firmware-api-print_task_config) below.

`set_tool_mapping()` itself returns `not_supported` (`system_info_.supports_tool_mapping
= false`): the physical head-to-spool attachment is fixed 1:1 and cannot be edited, and
remaps happen per-print through the pre-print path above. Routing is not fixed — the
firmware's `extruder_map_table` holds the live logical-to-physical answer for the current
print, and `get_tool_mapping()` publishes it (read-only; the write side is the pre-print
gcode).

### Slot Overrides and lane_data

Per-slot user overrides persist through `FilamentSlotOverrideStore` under the
`"snapmaker"` key style, bulk-loaded in `on_started()` before any status parse
(`ams_backend_snapmaker.cpp:298-319`). Every parse tail runs the shared convergence:
`check_hardware_event_clear()` first (a `CARD_UID` change means the physical spool was
swapped — clear the stale override; empty UID is "no signal" and never clears; first
observation only sets the baseline), then `mirror_firmware_to_lane_data()` under
`OverwriteAlways` so OrcaSlicer's MoonrakerPrinterAgent sees the spool, then
`apply_overrides()` layering the user's fields back over firmware truth
(`ams_backend_snapmaker.cpp:1672-1719`, `1755-1796`).

Because the UID is a hardware identifier the UI cannot write, this backend registers no
expected-echo value with the fingerprint tracker — user edits can never masquerade as a
hardware swap (`include/ams_backend_snapmaker.h:361-377`). Clears preserve
firmware-populated fields (`brand`, `spool_name`, `total_weight_g`) and reset only
override-exclusive ones (`spoolman_*`, `remaining_weight_g`, `color_name`, catalog
identity) (`ams_backend_snapmaker.cpp:1848-1884`).

User edits round-trip to firmware through `POST /printer/filament_detect/set`
(`channel` + `info` with `VENDOR`/`MAIN_TYPE`/`SUB_TYPE`/`RGB_1`/`ALPHA`/temps) — an
Extended Firmware endpoint that 404s on stock firmware; the override still persists to
`lane_data`, so HelixScreen's UI is correct either way
(`ams_backend_snapmaker.cpp:853-933`).

### Capabilities

| Feature | Supported | Notes |
|---------|-----------|-------|
| Endless Spool | `Unsupported` | No `get_endless_spool_capabilities()` override — base default |
| Tool Mapping | Per-print only | Physical attachment fixed 1:1 and non-editable (`set_tool_mapping()` = `not_supported`); per-print ROUTING is set via `SnapmakerNative` pre-print gcode and read back from `extruder_map_table` by `get_tool_mapping()` |
| Bypass | No | `supports_bypass = false`; both entry points `not_supported` — no external spool on a toolchanger (`ams_backend_snapmaker.cpp:241`, `942-948`) |
| Dryer | No | Not supported |
| Recover / Reset / Cancel | No | All three return `not_supported` (`ams_backend_snapmaker.cpp:553-563`) |
| Operation step bar | Yes | Firmware-driven per-direction steps via `ams_operation_phase`; Heat step live |
| Per-slot loaded authority | Override | `slot_is_actively_loaded()` returns `status == LOADED` verbatim (hub table, `ams_backend_snapmaker.cpp:528-535`) |
| Path visualization | Yes | NOZZLE when the latch is set, OUTPUT when port/motion sensor still sees filament, NONE otherwise (`ams_backend_snapmaker.cpp:394-426`) |
| RFID | Yes | Per-channel tag read; UID change clears the slot override |
| Spoolman | Fields only | `spoolman_id`/`spoolman_vendor_id` persist in slot overrides; no Snapmaker-specific Spoolman wiring exists in the backend |
| Mock mode | Yes | `HELIX_MOCK_AMS=snapmaker` (aliases `snapswap`, `u1`): 4 slots, PARALLEL, non-editable mapping ([MOCK_ENVIRONMENT_VARIABLES.md](MOCK_ENVIRONMENT_VARIABLES.md)) |

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend_snapmaker.h` | Backend class, `ExtruderToolState` / `SnapmakerRfidInfo`, capability overrides, per-slot state arrays |
| `src/printer/ams_backend_snapmaker.cpp` | Full implementation: status parse, channel_state table, gcode, overrides, pre-print builder |
| `include/printer_discovery.h` | `filament_detect` detection + registration order (MMU wins over stock U1) |
| `include/snapmaker_resume.h` + `src/printer/snapmaker_resume.cpp` | Terminal-pause matchers (dirty bed vs runout) and coded-error `msg` extraction |
| [Firmware API: `print_task_config`](#firmware-api-print_task_config) | The firmware-native remap/feed-gate API this backend's pre-print path emits, in this file |
| `tests/unit/test_ams_backend_snapmaker.cpp` | 54 cases: parsers, status handling, latch, overrides |
| `tests/unit/test_snapmaker_preprint_gcode.cpp` | 8 cases: the pure `build_preprint_gcode` builder |
| `tests/unit/test_snapmaker_resume.cpp` | 4 cases: pause classification / resume prep |

### Follow-up Work

1. **Hardware verification** on the U1 rig (192.168.30.103) — the banner on this doc.
   Everything above is source-derived; the code comments' live-verified markers
   (#991, firmware 20260608) cover the `AUTO_FEEDING` load/unload commands and pause
   classification, not the RFID read path, the resume-after-runout chain, or the
   pre-print send timing.
2. `is_stuck_motion_sensor_runout()` has no caller — revive when a verifiable
   "filament at the gear" signal exists (`ams_backend_snapmaker.cpp:569-593`). Checked
   2026-08-21: the status model carries **no dedicated feeder/gear-presence field** -
   the three presence signals are `filament_detect.state` (per channel),
   `filament_feed` per-extruder `filament_detected` (port), and the per-tool motion
   sensor. The code's own candidate is `filament_feed.channel_state`: `load_finish`
   (fed to nozzle) vs `preload_finish` (firmware assist stops short of the gear) -
   both already parsed into the channel-state machine
   (`ams_backend_snapmaker.cpp:135-138`, `:560-567`). What is missing is rig
   confirmation that the state reliably means "filament at the gear" before the gate
   is revived.
3. End-to-end timing of the pre-print `SET_PRINT_USED_EXTRUDERS` is unverified live.
   Code-side ordering is established (see Pre-Print Remap above): the print-start
   request is issued only after the config gcode's JSON-RPC ack. Still open,
   firmware-side: whether the ack means the config is mutated and persists by the time
   the baked `PRINT_START` block runs — flagged in
   [Firmware API: `print_task_config`](#firmware-api-print_task_config) § "Still
   UNCERTAIN".
4. The `prepare_for_resume` doc comment in `include/ams_backend_snapmaker.h:162-169`
   still describes the retired sensor-disable chain; the implementation drives
   `AUTO_FEEDING` (see Runout and Resume above). Comment is stale, code is right.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.

---

## Firmware API: `print_task_config`

The U1's firmware ships a Klipper extra, /home/lava/klipper/klippy/extras/print_task_config.py
(on the printer), registering ~14 gcode
commands that the stock Snapmaker screen uses for per-slot filament metadata,
**logical→physical extruder remapping** (`SET_PRINT_EXTRUDER_MAP`), and **per-head "used"
gating** (`SET_PRINT_USED_EXTRUDERS`) that the print macros consult before auto-feeding a
head. This is the reference for that API and for how HelixScreen drives it.

Constants: `LOGICAL_EXTRUDER_NUM = 32`, `PHYSICAL_EXTRUDER_NUM = 4`.

**Derived from:** that extra (1351 lines) on the device; a live U1
(read-only SSH + Moonraker); the stock UI binary `/usr/bin/gui` (`strings`, for exact
command format strings); Klipper config macros via
`printer/objects/query?configfile=settings`; the printer's toolhead.py and
kinematics/extruder.py for
`Tn` routing; and a real Orca-sliced file.

### Command reference

All commands are registered in `PrintTaskConfig.__init__` (lines 111–137). Constants:
`LOGICAL_EXTRUDER_NUM = 32`, `PHYSICAL_EXTRUDER_NUM = 4`.

| Command | Signature | Purpose | Blocked while printing? |
|---|---|---|---|
| `SET_PRINT_EXTRUDER_MAP` | `CONFIG_EXTRUDER=<0..31> MAP_EXTRUDER=<0..3>` | Remap one logical tool → physical head | **Yes** (id 531, code 15) |
| `GET_PRINT_EXTRUDER_MAP` | *(none)* | Dump `Tn -> Tm` map table to console | No |
| `SET_PRINT_FILAMENT_CONFIG` | `CONFIG_EXTRUDER=<0..3>` + filament fields (below) | Set per-physical-head filament vendor/type/color | No (but rejects official RFID unless `FORCE=1`) |
| `GET_PRINT_TASK_CONFIG` | *(none)* | Dump the entire `print_task_config` dict | No |
| `SAVE_CURRENT_PRINT_TASK_CONFIG` | *(none)* | Persist current config to print_task.json | No |
| `RESET_PRINT_TASK_CONFIG` | *(none)* | Reset whole config to defaults + persist | No |
| `LOAD_PRINT_TASK_CONFIG` | *(none)* | Reload config from disk | No |
| `SET_PRINT_PREFERENCES` | many flags (below) | Bed-level / flow-calib / timelapse / replenish / entangle prefs | Partially (id 531, code 16) |
| `SET_PRINT_USED_EXTRUDERS` | `EXTRUDERS=<csv of 0..3>` | Mark which physical heads this task uses | **Yes** (id 531, code 16) |
| `SET_PRINT_TASK_PARAMETERS` | bulk (map + prefs + per-tool gcode params) | One-shot "full task setup" the slicer/cloud uses | **Yes** (id 531, code 16/17) |
| `INNER_CHECK_AND_RELOAD_FILAMENT_INFO` | `EXTRUDER=<0..3> IS_RUNOUT=<0/1>` | Internal: restore filament metadata after runout | n/a (internal) |
| `INNER_AUTO_REPLENISH_FILAMENT` | `EXTRUDER=<0..3>` | Internal: find a matching head and continue print | n/a (internal, paused only) |
| `INNER_PRINT_END` | *(none)* | Internal: sets `is_exec_print_end_action=True` | n/a (internal) |
| webhook `print_task_config/set_print_preferences` | JSON-RPC params | Moonraker endpoint mirroring a subset of prefs | n/a |

#### `SET_PRINT_EXTRUDER_MAP` — the remap primitive
```python
def cmd_SET_PRINT_EXTRUDER_MAP(self, gcmd):
    config_extruder = gcmd.get_int("CONFIG_EXTRUDER", None)   # logical, 0..31
    map_extruder    = gcmd.get_int("MAP_EXTRUDER", None)      # physical, 0..3
    ...
    if print_stats.state in ['printing', 'paused']:
        raise gcmd.error(message="...not allowed to set extruder map during printing!",
                         id=531, index=0, code=15, oneshot=1, level=1)
    if (config_extruder < 0 or config_extruder >= LOGICAL_EXTRUDER_NUM) or \
            (map_extruder < 0 or map_extruder >= PHYSICAL_EXTRUDER_NUM):
        raise gcmd.error("...invalid extruder index!!!")
    tmp_map_table[config_extruder] = map_extruder
    tmp_reprint_info['extruder_map_table'][config_extruder] = map_extruder
```
- `CONFIG_EXTRUDER` is the **logical/slicer tool index** (0–31); `MAP_EXTRUDER` is the **physical head** (0–3).
- Writes both the live `extruder_map_table` **and** `reprint_info.extruder_map_table` (so a re-print keeps the remap).
- **Does NOT auto-persist to disk** — it only mutates the in-memory dict. The stock screen calls
  `SAVE_CURRENT_PRINT_TASK_CONFIG` (or the bulk `SET_PRINT_TASK_PARAMETERS`, which does persist) to commit.
- Rejected during `printing`/`paused`.

##### What HelixScreen does with it

**Writes.** `AmsBackendSnapmaker::build_preprint_gcode()` emits one
`SET_PRINT_EXTRUDER_MAP` for **every logical tool the file uses** — including the ones
landing on their firmware-default head — followed by `SET_PRINT_USED_EXTRUDERS` derived
from that same resolution. Emitting only the genuine remaps was wrong twice over: the
command sets one entry and resets nothing, so unmentioned tools kept whatever the
previous print left, and the used-heads line assumed the default applied.

**Reads.** `AmsBackendSnapmaker::get_tool_mapping()` publishes the table as the applied
logical-to-physical routing, and the live gcode preview colors each tool by the lane that
will actually print it. Three gating rules, all from observation on a real U1:

- The firmware restores identity when the print **completes** and when it is
  **cancelled** — both observed directly (idle after either: `extruders_used`
  `[F,F,F,F]`, `extruder_map_table[0:4]` `[0,1,2,3]`).
- Idle therefore reads `[0,1,2,3]`, which is **indistinguishable from "this print needs
  no remap"** and is flatly wrong for a file whose tools do not line up with the lanes.
  Observed concretely: after cancelling a print whose correct routing was `[2,1,0,3]`,
  the table read identity. So the read is gated on `extruders_used` having at least one
  true — the firmware's own "a task is configured" signal — and answers "no opinion"
  otherwise.
- Power loss and a klippy crash mid-print are **not** covered by those observations. The
  gate above is what makes the read safe on those paths too: it does not depend on the
  reset happening, only on a task being configured.

#### `SET_PRINT_USED_EXTRUDERS` — the feed-gate primitive
```python
def cmd_SET_PRINT_USED_EXTRUDERS(self, gcmd):
    extruders_str = gcmd.get('EXTRUDERS', None)          # e.g. "0,2"
    if print_stats.state in ['printing', 'paused']:
        raise gcmd.error(message="...not allow to set used_extruders during printing!",
                         id=531, index=0, code=16, oneshot=1, level=1)
    tmp_extruders_used = [False] * PHYSICAL_EXTRUDER_NUM
    used_extruders = [int(value) for value in extruders_str.split(',')]
    for i in range(min(len(used_extruders), LOGICAL_EXTRUDER_NUM)):
        tmp_extruders_used[used_extruders[i]] = True
        tmp_reprint_info['extruders_used'][used_extruders[i]] = True
    self.print_task_config['extruders_used'] = tmp_extruders_used
    ...  # persists to disk
```
- `EXTRUDERS` is a **comma-separated list of PHYSICAL head indices** (0–3) — the heads that are
  actually used by this task. Anything not listed becomes `extruders_used[i] = False`.
- **This is the flag the prestart feed/preheat macros consult** (§3). Setting it persists to disk.
- Rejected during `printing`/`paused`.

#### `SET_PRINT_FILAMENT_CONFIG` — per-head filament metadata
```python
config_extruder        = gcmd.get_int('CONFIG_EXTRUDER')      # physical 0..3 (NOT logical)
filament_vendor        = gcmd.get('VENDOR', None)
filament_type          = gcmd.get('FILAMENT_TYPE', None)
filament_sub_type      = gcmd.get('FILAMENT_SUBTYPE', None)
filament_soft          = gcmd.get_int('SOFT', None)
filament_color         = gcmd.get_int('FILAMENT_COLOR', None)        # 0xAARRGGBB int
filament_color_rgba    = gcmd.get('FILAMENT_COLOR_RGBA', None)       # "RRGGBB" or "RRGGBBAA"
filament_alpha         = gcmd.get_int('ALPHA', None, minval=0, maxval=255)
filament_color_nums    = gcmd.get_int('COLOR_NUMS', None, minval=1, maxval=5)
filament_colors_str    = gcmd.get('COLORS', None)                    # csv of RRGGBB, multicolor
filament_color_multi_mode = gcmd.get_int('MULTI_MODE', 0, minval=0, maxval=255)
force                  = gcmd.get_int('FORCE', False)
```
- `CONFIG_EXTRUDER` here is a **physical head** (0–3), validated against `PHYSICAL_EXTRUDER_NUM`.
- If the slot currently holds an **official (RFID) filament**, the call is rejected unless `FORCE=1`
  (`filament_official[config_extruder] and bool(force) == False` → error).
- Three mutually-exclusive color forms: multicolor (`COLOR_NUMS`+`COLORS`), `FILAMENT_COLOR_RGBA`, or `FILAMENT_COLOR` int.
- Persists to disk and runs `FLOW_RESET_K EXTRUDER=<n>`.
- **Not** blocked during printing (no `print_stats` guard).

#### `SET_PRINT_PREFERENCES`
```python
BED_LEVEL=<0/1>  FLOW_CALIBRATE=<0/1>  FLOW_CALIBRATE_EXTRUDERS=<csv>  SHAPER_CALIBRATE=<0/1>
TIME_LAPSE_CAMERA=<0/1>  AUTO_REPLENISH_FILAMENT=<0/1>  REPLENISH_IGNORE_COLOR=<0/1>
FILAMENT_ENTANGLE_DETECT=<0/1>  FILAMENT_ENTANGLE_SEN=<low|medium|high>
END_LED_TURN_OFF=<0/1>  END_UNLOAD_FILAMENT=<python-list-literal>  FORCE=<0/1>
```
- During `printing`/`paused`, setting `BED_LEVEL`/`FLOW_CALIBRATE`/`SHAPER_CALIBRATE`/`TIME_LAPSE_CAMERA`/`END_UNLOAD_FILAMENT`
  is rejected (id 531, code 16) **unless** `FORCE=1`. The replenish/entangle/LED prefs are always allowed.
- `END_UNLOAD_FILAMENT` is parsed with `ast.literal_eval` and must be a Python list (e.g. `[1,0,1,0]`).

#### `SET_PRINT_TASK_PARAMETERS` — the bulk one-shot
The "do everything" command. Accepts a superset:
```python
MAP_TABLE=<list of [logical,physical] pairs>   # e.g. "[[0,0],[1,2]]"
BED_LEVEL  FLOW_CALIBRATE  FLOW_CALIBRATE_EXTRUDERS  SHAPER_CALIBRATE  TIME_LAPSE_CAMERA
END_UNLOAD_FILAMENT
LINE_WIDTH  LAYER_HEIGHT  OUTER_WALL_SPEED
NOZZLE_DIAMETER_LIST  NOZZLE_TEMP  FILAMENT_TYPE  FILAMENT_FLOW_RATIO  FILAMENT_MAX_VOL_SPEED
FILAMENT_USED_G  FILAMENT_USED_MM
```
- `MAP_TABLE` is a list of `[logical, physical]` pairs; each pair updates `extruder_map_table[logical]=physical`.
- **`extruders_used` is derived automatically** here (lines 1299–1301):
  ```python
  for i in range(LOGICAL_EXTRUDER_NUM):
      if filament_used_g[i] > 0.0001 or filament_used_mm[i] > 0.0001:
          extruders_used[ extruder_map_table[i] ] = True
  ```
  i.e. any logical tool with nonzero filament usage marks its **mapped physical head** as used.
- Enforces a nozzle-diameter match per used head (code 14) and a flow-calibrate-allowed check (code 18).
- Rejected during `printing`/`paused` (code 16; otherwise code 17 on generic error).
- Persists both print_task.json and print_task_2.json.

#### Internal / runout commands
- `INNER_CHECK_AND_RELOAD_FILAMENT_INFO EXTRUDER=<0..3> IS_RUNOUT=<0/1>` — on runout, restores the
  pre-runout filament metadata for that head from `filament_info_backup`; if the head still has no
  filament type it raises a **pause action** error (`action='pause', id=523, index=<extruder>, code=39`).
- `INNER_AUTO_REPLENISH_FILAMENT EXTRUDER=<0..3>` — when paused on a runout and `auto_replenish_filament`
  is on, searches the other heads for a colour/type match, **rewrites `extruder_map_table` so every logical
  tool that pointed at the runout head now points at the replacement**, flips `extruders_used`, records
  `extruders_replenished[old]=new`, then `RESUME REPLENISH=1 REPLENISH_EXTRUDER=<new>`. This is the firmware's
  own live-remap-on-runout path and is the clearest proof the map is the routing authority.
- `INNER_PRINT_END` — sets `is_exec_print_end_action=True`.

#### Error-id convention
Blocked-during-print errors all use `id=531` with a distinguishing `code` (15 = set map, 16 = used/prefs/params,
17 = params generic, 14 = nozzle mismatch, 18 = flow-calib not allowed). The runout "no filament edited"
pause uses `id=523, code=39`. These ids match the U1 exception-object scheme documented in
`project_991_u1_pause_signals` (523/532 families).

---

### Data model

`DEFAULT_PRINT_TASK_CONFIG` (lines 23–61). Per-**physical-head** arrays are length 4
(`PHYSICAL_EXTRUDER_NUM`); the map table is length 32 (`LOGICAL_EXTRUDER_NUM`).

| Field | Shape | Meaning |
|---|---|---|
| `extruder_map_table` | `int[32]` | **logical tool index → physical head index.** Default identity: `[0,1,2,3, 0,0,…]` (indices 4–31 default to 0) |
| `extruders_used` | `bool[4]` | which physical heads this task uses (gates feed/preheat/switch-check) |
| `extruders_replenished` | `int[4]` | per-head: which head replaced it after auto-replenish (default identity) |
| `filament_exist` | `bool[4]` | sensor-derived: filament physically present at head |
| `filament_vendor/type/sub_type` | `str[4]` | filament metadata per head (`'NONE'` = unset) |
| `filament_color` | `int[4]` | `0xAARRGGBB` |
| `filament_color_rgba` | `str[4]` | 8-char `"RRGGBBAA"` |
| `filament_color_multi` | `dict[4]` | `{nums, alpha, mode, colors:[…]}` for multicolor spools |
| `filament_official` | `bool[4]` | true = Snapmaker RFID spool (locks `SET_PRINT_FILAMENT_CONFIG` w/o FORCE) |
| `filament_sku`, `filament_soft`, `filament_edit` | per-head | SKU id; "soft" flag; UI-editable flag |
| `flow_calibrate`, `flow_calib_extruders[4]`, `auto_bed_leveling`, `time_lapse_camera`, `shaper_calibrate` | prefs | task prefs |
| `auto_replenish_filament`, `replenish_ignore_color` | prefs | runout auto-replenish behaviour |
| `filament_entangle_detect`, `filament_entangle_sen` | prefs | tangle detection |
| `end_led_turn_off`, `end_unload_filament[4]` | prefs | end-of-print actions |
| `reprint_info` | dict | snapshot of `{extruder_map_table, extruders_used, flow_*, time_lapse, bed_level, end_unload}` for re-print |

A second file print_task_2.json (`DEFAULT_PRINT_TASK_CONFIG_2`) holds **per-logical-tool gcode params**
(`nozzle_temp[32]`, `nozzle_diameter[32]`, `filament_used_g[32]`, etc.) used for validation in
`SET_PRINT_TASK_PARAMETERS`.

`reset_print_info()` (called at construction / new task) resets `extruder_map_table`, `extruders_used`,
`extruders_replenished`, and prefs to identity/defaults — so **each new print starts with an identity map
and all-False `extruders_used` until the screen sets them.** (Verified live: an idle U1 reports
`extruders_used: [False,False,False,False]`, `extruder_map_table[0:8]: [0,1,2,3,0,0,0,0]`.)

The whole struct is exported as a Klipper status object: `printer.print_task_config` (queried via
Moonraker `printer/objects/query?print_task_config`), which is exactly what the macros read.

---

### Tool routing and prestart feed

#### How `Tn` resolves to a physical head
Two distinct paths:

**`T0`–`T3` (physical-range tool commands).** Registered by kinematics/extruder.py as each extruder's
`gcode_id`, dispatching to `cmd_SWITCH_EXTRUDER_ADVANCED`:
```python
def cmd_SWITCH_EXTRUDER_ADVANCED(self, gcmd):
    extruder_map = gcmd.get_int('A', 1, minval=0)        # A defaults to 1 = "apply map"
    if extruder_map != 0 and self.print_config is not None:
        index = int(self.gcode_id.split('T')[1])
        index = self.print_config.get_extruder_map_index(index)   # ← map lookup
        section = 'extruder' if not index else 'extruder%d' % index
        extruder = self.printer.lookup_object(section, None)
        extruder.cmd_SWITCH_EXTRUDER(gcmd)
    else:
        self.cmd_SWITCH_EXTRUDER(gcmd)                    # A=0 → direct, NO map
```
So a bare `T2` **does** route through `extruder_map_table[2]` (because `A` defaults to 1). A `T2 A0`
selects physical head 2 directly, bypassing the map. **The Snapmaker print macros consistently use
`A0`** for deterministic physical addressing (e.g. `SM_PRINT_CHECK_SWITCH_EXTRUDER` emits `T{i} A0`,
preheat emits `M104 ... T{i} A0`).

**`T4`–`T31` (extended/logical tool commands).** These are *macros* — each is literally
`SWITCH_OF_EXTENDED_EXTRUDER INDEX=n`. Implemented in toolhead.py:
```python
def cmd_SWITCH_OF_EXTENDED_EXTRUDER(self, gcmd):
    index = gcmd.get_int('INDEX')                                  # 4..31
    extruder_index = print_task_config.get_extruder_map_index(index)   # ← map lookup
    extruder = lookup('extruder' if extruder_index==0 else 'extruder%d'%extruder_index)
    gcmd._params['A'] = '0'                                        # then direct-select physical
    extruder.cmd_SWITCH_EXTRUDER_ADVANCED(gcmd)
```
So **`T4`–`T31` ALWAYS route through the map** (`get_extruder_map_index`) and then physical-select.

**Net:** `extruder_map_table` is the single routing authority for both ranges (T0–T3 via the default
`A=1`, T4–T31 unconditionally). Remapping with `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=c MAP_EXTRUDER=p`
genuinely redirects body tool-changes to a different physical head.

#### How the prestart feed and preheat are gated
The Orca slicer bakes an **unconditional** prestart block for **all four heads** into every file. From
the real file `lid_PLA_6m28s.gcode`:
```
PRINT_START
T0
SM_PRINT_CHECK_SWITCH_EXTRUDER
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=0
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=2 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=1
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=3 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=2
SM_PRINT_AUTO_FEED EXTRUDER=3
```
The `.gcode` file itself contains **no** `SET_PRINT_USED_EXTRUDERS` / `SET_PRINT_EXTRUDER_MAP` (grep-confirmed).
Those are sent **by the screen/cloud BEFORE the file plays** — and they are what make the baked block selective.

**The feed macro gates on `extruders_used`.** `gcode_macro sm_print_auto_feed`:
```jinja
{% set extruder = params.EXTRUDER | default(999) | int %}
{% if filament_feed_vars != {} and extruder >= 0
      and extruder < printer.configfile.settings.printer.max_physical_extruder_num %}
  {% if printer.print_task_config['extruders_used'][extruder] %}          ← THE GATE
    {% set feed_module_seq  = filament_feed_vars.module_sequence[extruder] %}
    {% set feed_channel_seq = filament_feed_vars.channel_sequence[extruder] %}
    FEED_AUTO MODULE={feed_module_seq} CHANNEL={feed_channel_seq} LOAD=1 PRINTING=1 {rawparams}
  {% endif %}
{% else %} … {% endif %}
```
`sm_print_extruder_preheat` has the **same gate**:
```jinja
{% if extruder >= 0 and extruder < max_physical_extruder_num
      and printer.print_task_config['extruders_used'][extruder] %}
  M104 S{temp} T{extruder} A0
{% endif %}
```
And `sm_print_check_switch_extruder` only switches to heads that are used:
```jinja
{% for i in range(max_physical_extruder_num) %}
  {% if printer.print_task_config['extruders_used'][i] %}
    T{i} A0
  {% endif %}
{% endfor %}
```

#### What gates the feed, in practice
**Yes — `SET_PRINT_USED_EXTRUDERS` (sent before `print_start`) is exactly the command that prevents the
firmware from auto-feeding an unused/empty head.** The baked `SM_PRINT_AUTO_FEED EXTRUDER=n` and
`SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=n` lines are no-ops for any head `n` whose `extruders_used[n]` is
False. For the reporter's case — a file whose body uses heads 0+2 but whose baked prestart feeds 0,1,2,3 —
sending `SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2` before the print causes the macros to **skip** the feed
and preheat for heads 1 and 3, so the empty head never trips a runout.

**Important nuance — which command, and the map interaction:**
- The **physical-head index** in `extruders_used` is what gates the feed. So you must list the *physical*
  heads actually used, after applying any remap.
- `SET_PRINT_EXTRUDER_MAP` alone does **not** set `extruders_used` (it only writes the map table), so a
  remap by itself will not silence the feed of a now-unused head. You need `SET_PRINT_USED_EXTRUDERS` too
  (or the bulk `SET_PRINT_TASK_PARAMETERS`, which derives `extruders_used` from per-tool filament usage
  through the map — §1.5).
- The stock flow uses both: set the map, then set used-extruders to the mapped physical set.

**Confidence:** High from static reading — all three prestart macros explicitly index
`printer.print_task_config['extruders_used'][extruder]`, the gcode file carries the unconditional baked
block, and the file does not self-set the flags. The one thing **not** verified live (read-only constraint
forbids starting a print or sending `SET_*`) is the end-to-end timing: that the screen's
`SET_PRINT_USED_EXTRUDERS` lands and persists into `printer.print_task_config` *before* `PRINT_START`
executes the baked block. This is consistent with the code (the commands are rejected during print, so they
*must* precede it) but should be confirmed on-printer with a 0+2 file before we rely on it in production.

---

### How the stock screen sequences it

The orchestration lives in the **local UI binary `/usr/bin/gui`**, not in snapmakercloud.py (grep of the
cloud component for `extruder_map` / `used_extruder` / `MAP_EXTRUDER` returns nothing — the cloud submits
tasks, the local gui issues the gcode). Exact format strings extracted from `/usr/bin/gui`:

```c
"SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=%d MAP_EXTRUDER=%d\n"
"SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=%u MAP_EXTRUDER=%u"
"SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=%u VENDOR=%s FILAMENT_TYPE=%s FILAMENT_SUBTYPE='%s'"
"SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=%u FILAMENT_COLOR_RGBA=%02X%02X%02X%02X"
"SET_PRINT_USED_EXTRUDERS EXTRUDERS=%s"
"SET_PRINT_PREFERENCES BED_LEVEL=%d FLOW_CALIBRATE=%d SHAPER_CALIBRATE=%d TIME_LAPSE_CAMERA=%d"
"SET_PRINT_PREFERENCES FLOW_CALIBRATE_EXTRUDERS=%s"
```
Plus the JSON-RPC preference mirror (via the `print_task_config/set_print_preferences` webhook):
```
{"jsonrpc":"2.0","method":"printer.print_task_config.set_print_preferences",
 "params":{"auto_replenish_filament":%d}, "id":%u}
   (also: end_led_turn_off, filament_entangle_detect, filament_entangle_sen)
```

**Reconstructed pre-print sequence** (from the command semantics + format strings; the gui sends these
before issuing the print, since all the map/used/params commands are rejected once `print_stats.state`
is `printing`/`paused`):
1. Per edited slot: `SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=<phys> VENDOR=… FILAMENT_TYPE=… FILAMENT_SUBTYPE='…'`
   (and a colour variant) — sets the per-head filament metadata.
2. Per remap: `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<logical> MAP_EXTRUDER=<physical>` — redirect tools.
3. `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv of physical heads in use>` — gate feed/preheat.
4. `SET_PRINT_PREFERENCES …` (bed-level / flow-calib / timelapse / flow-calib-extruders).
5. Start the print (the baked `PRINT_START` block then feeds/preheats only the used heads).

The slicer/cloud path can instead use the single `SET_PRINT_TASK_PARAMETERS MAP_TABLE=… FILAMENT_USED_G=… …`,
which sets the map and derives `extruders_used` from per-tool usage in one command.

---

### Constraints this API puts on callers

1. **Timing is hard.** Every map/used/params command raises an error (`id=531`) if
   `print_stats.state` is `printing` or `paused`. These MUST be sent **before**
   `print_start` / `SDCARD_PRINT_FILE`. Mid-print they are not merely ineffective — they
   error and surface a U1 exception modal.
2. **Indices: mind logical vs physical.** `extruder_map_table` and
   `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER` are **logical** (0–31). `MAP_EXTRUDER`,
   `SET_PRINT_USED_EXTRUDERS EXTRUDERS=`, and `SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER`
   are **physical** (0–3).
3. **Respect `filament_official`.** A slot holding a Snapmaker RFID spool rejects
   `SET_PRINT_FILAMENT_CONFIG` unless `FORCE=1` is passed. Do not blindly overwrite
   official slots.
4. **Read state from `printer.print_task_config`.** A standard Klipper status object,
   subscribed wholesale by the discovery sequence. Fields of interest:
   `extruder_map_table`, `extruders_used`, `filament_exist`,
   `filament_type`/`vendor`/`sub_type`, `filament_color_rgba`, `filament_official`.

### Confirmed on hardware

- **The feed-skip lands in time.** A pre-print `SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2` is
  reflected in `extruders_used` before the slicer-baked `PRINT_START` block runs. Verified
  with a real 2-colour T0/T2 file: the firmware read back
  `extruder_map_table = [2,1,0,3]`, skipped the unused heads, and the print completed.
- **The routing table resets to identity when a print ends** — on normal completion and on
  cancel, both observed directly (idle after either: `extruders_used` `[F,F,F,F]`,
  `extruder_map_table[0:4]` `[0,1,2,3]`).

### Still uncertain

- **Power loss / klippy crash mid-print** are not covered by the reset observation above.
  The read gate described earlier does not depend on the reset, only on a task being
  configured, so this is bounded rather than open.
- **Whether `SM_PRINT_FLOW_CALIBRATE` is defined** in shipping firmware — it appears in the
  slicer-baked block but is **not** present as a `gcode_macro` in this device's live config.
  It may be a no-op / unknown-command-tolerant path, or defined in a build variant. Does not
  affect feed or remap.
- **Exact persistence semantics of `SET_PRINT_EXTRUDER_MAP`** (in-memory only) versus
  whether the stock screen always follows it with a save — a save step is inferred but the
  gui's exact call order was not captured live.
- **Whether `reprint_info.extruder_map_table` is re-applied on a firmware-initiated
  reprint.** `SET_PRINT_EXTRUDER_MAP` writes that mirror as well as the live table, but
  where the mirror is consumed was not read. HelixScreen's own reprint path sends an
  explicit map for every used tool, so it does not depend on the answer.
