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
  (`ams_backend_snapmaker.cpp:258-266`).
- `PathTopology::PARALLEL` on both the unit and `get_topology()`
  (`include/ams_backend_snapmaker.h:100-103`). Because every lane has an independent
  path, `needs_unload_before_load()` is answered by the base class — the serial
  lane rule never applies (`include/ams_backend_snapmaker.h:105-108`).
- `tip_method = TipMethod::NONE` — the U1 has no cutter and forms no discrete tip;
  unload is heat + retract, so the unload stepper renders "Heat nozzle -> Retract"
  (`ams_backend_snapmaker.cpp:242-246`).
- `tool_to_slot_map` is seeded with identity so the 2D gcode viewer applies per-tool
  colors instead of falling back to the slicer's single palette entry
  (`ams_backend_snapmaker.cpp:272-280`).
- A slot select IS a physical tool change: `do_select_slot()` forwards to
  `do_change_tool()`, which emits `T{n}` and moves the carriage
  (`select_slot_moves_toolhead() = true`, `ams_backend_snapmaker.cpp:528-538`).

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
(`ams_backend_snapmaker.cpp:1085-1156`, `1456-1529`).

### RFID (filament_detect.info)

`parse_rfid_info()` reads per-channel tag fields
(`ams_backend_snapmaker.cpp:990-1045`):

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
SUB_TYPE (`ams_backend_snapmaker.cpp:38-48`, `860-869`).

Every row above is code-verified against `parse_rfid_info()` and the apply loop
(`ams_backend_snapmaker.cpp:990-1045`, `1171-1204`): tag identity rides
`filament_detect.info[ch].CARD_UID`, and a `MAIN_TYPE == "NONE"` tag skips the field
apply while its UID is still captured for swap detection (`:1173-1182`). Physical reads
from real RFID spools remain rig-pending; code-verified is not field-verified.

### Commands the Backend Emits

| Command / call | Used for |
|----------------|----------|
| `AUTO_FEEDING EXTRUDER=<n> LOAD=1` | Load (`do_load_filament`, `ams_backend_snapmaker.cpp:427-453`) |
| `AUTO_FEEDING EXTRUDER=<n> UNLOAD=1` | Unload (`do_unload_filament`, `:456-489`) |
| `INNER_FILAMENT_UNLOAD` | Bare unload fallback only when no slot/extruder can be resolved (`:480-482`) |
| `T<n>` | Tool change / slot select (`do_change_tool`, `:532-538`) |
| `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<l> MAP_EXTRUDER=<p>` | Pre-print logical->physical remap (`build_preprint_gcode`, `:1887-1889`) |
| `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv>` | Pre-print feed gating — always sent, remap or not (`:1907`) |
| `POST /printer/filament_detect/set` | Slot-metadata writeback (paxx12 Extended Firmware REST, `:854-924`) |

Why `AUTO_FEEDING ... LOAD=1` and not the obvious alternatives — the source records the
trail (`ams_backend_snapmaker.cpp:443-452`): bare `T{n}` is a no-op when the target tool
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
(`ams_backend_snapmaker.cpp:78-226`). That single classification drives:

- **The operation step bar.** LOAD/manual/preload share a 5-step model
  (Home -> Select -> Heat -> Feed -> Purge); UNLOAD uses 4 steps ending in Retract; the
  Heat step shows a live nozzle temperature. The current index is published through the
  shared `ams_operation_phase` subject, which the sidebar consumes generically
  (`ams_backend_snapmaker.cpp:332-371`).
- **The per-tool "loaded at toolhead" latch.** Set on `load_finish`; cleared on
  `unload_finish` / `wait_insert` / `preload_finish`; left unchanged on transient and
  fail states. This latch — not the motion sensor — is the authority for
  `slot_has_filament_at_toolhead()`, `can_unload_from_toolhead()`, and the NOZZLE path
  segment, because the per-tool encoder fails to drop to false after an unload on
  current firmware (`include/ams_backend_snapmaker.h:289-305`,
  `ams_backend_snapmaker.cpp:491-517`).
- **Action lifecycle and errors.** `*_fail` states and `channel_error` tokens surface as
  `AmsAction::ERROR` with a direction-aware message ("No filament in lane N. Load
  filament and retry." for the `no_filament` token), except when the lane is empty,
  idle, and not the active lane — the firmware reports `no_filament` for any empty lane,
  which must not latch a spurious error modal on a deliberately unloaded head in a
  multi-color print (`ams_backend_snapmaker.cpp:56-76`, `1321-1354`).
- **`preload_finish` is terminal-for-latch but does not end the operation** — a re-unload
  of a staged lane keeps that state while the nozzle heats, and dropping to Idle there
  killed the unload step display mid-heat (`ams_backend_snapmaker.cpp:1415-1430`).

A `*_finish` that clears the latch also demotes the slot LOADED -> AVAILABLE and clears
`filament_loaded` for the active tool, but never resets `current_slot`/`current_tool`:
those track the picked-up tool, and resetting them mis-routed a bare unload to T0 for a
user printing TPU without feeders (field report recorded at
`ams_backend_snapmaker.cpp:1378-1392`). Lanes reaching `unload_finish` are reported to
`AmsState::mark_slot_unloaded()` after the mutex is released so `FilamentSensorManager`
suppresses the runout modal during the expected pull-out grace window
(`ams_backend_snapmaker.cpp:1405-1414`, `1694-1699`) — the deferral exists because
calling into `AmsState` under our mutex inverted `add_backend()`'s lock order (TSan,
2026-08-16).

### Runout and Resume

`prepare_for_resume()` (`ams_backend_snapmaker.cpp:588-726`) classifies the pause first:
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
(`ams_backend_snapmaker.cpp:653-666`, `:722-724`). The full chain - runout pause ->
runout dialog -> refeed -> RESUME -> print continues - is **not field-tested**.

Related capability flags: `recovers_filament_on_resume() = true` (Resume re-feeds, so
the runout dialog presents Resume as primary) and
`should_suppress_idle_runout_modal() = true` (the U1 drives load/unload itself, so an
idle lane going empty needs no operator action) (`include/ams_backend_snapmaker.h:179-192`).

`is_stuck_motion_sensor_runout()` (motion sensor false, port sensor true = stale encoder)
currently has **no caller in tree** — the auto-recover path that consumed it was pulled
because that signal cannot distinguish "stale encoder" from "preloaded 4 inches short of
the gear"; it is kept as detection infrastructure for a deferred follow-up
(`ams_backend_snapmaker.cpp:560-584`). The active tool's port-present flag it builds on
is still published to `AmsState::set_active_tool_port_present()` on change (#991), which
is what gates Resume in the runout dialog (`ams_backend_snapmaker.cpp:1671-1692`).

### Pre-Print Remap (RemapStrategy::SnapmakerNative)

`SET_PRINT_EXTRUDER_MAP` / `SET_PRINT_USED_EXTRUDERS` error mid-print (firmware id 531),
so the config must land before `PRINT_START`. `requires_preprint_send() = true` is
**always-on, even with no remap**: `SET_PRINT_USED_EXTRUDERS` suppresses the spurious
auto-feed of unused heads baked into every Orca-sliced file, which otherwise feeds an
empty head and cancels the print on runout (`include/ams_backend_snapmaker.h:222-229`,
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
the firmware's default map (`ams_backend_snapmaker.cpp:1869-1918`). Full command
semantics — logical (0-31) vs physical (0-3) index rules, persistence behavior, the
`filament_official` FORCE gate — live in
[SNAPMAKER_U1_PRINT_TASK_CONFIG.md](SNAPMAKER_U1_PRINT_TASK_CONFIG.md).

`set_tool_mapping()` itself returns `not_supported` (`system_info_.supports_tool_mapping
= false`): mapping is fixed 1:1 in the live model, and remaps happen per-print through
the pre-print path above.

### Slot Overrides and lane_data

Per-slot user overrides persist through `FilamentSlotOverrideStore` under the
`"snapmaker"` key style, bulk-loaded in `on_started()` before any status parse
(`ams_backend_snapmaker.cpp:289-310`). Every parse tail runs the shared convergence:
`check_hardware_event_clear()` first (a `CARD_UID` change means the physical spool was
swapped — clear the stale override; empty UID is "no signal" and never clears; first
observation only sets the baseline), then `mirror_firmware_to_lane_data()` under
`OverwriteAlways` so OrcaSlicer's MoonrakerPrinterAgent sees the spool, then
`apply_overrides()` layering the user's fields back over firmware truth
(`ams_backend_snapmaker.cpp:1622-1669`, `1755-1796`).

Because the UID is a hardware identifier the UI cannot write, this backend registers no
expected-echo value with the fingerprint tracker — user edits can never masquerade as a
hardware swap (`include/ams_backend_snapmaker.h:317-334`). Clears preserve
firmware-populated fields (`brand`, `spool_name`, `total_weight_g`) and reset only
override-exclusive ones (`spoolman_*`, `remaining_weight_g`, `color_name`, catalog
identity) (`ams_backend_snapmaker.cpp:1798-1834`).

User edits round-trip to firmware through `POST /printer/filament_detect/set`
(`channel` + `info` with `VENDOR`/`MAIN_TYPE`/`SUB_TYPE`/`RGB_1`/`ALPHA`/temps) — an
Extended Firmware endpoint that 404s on stock firmware; the override still persists to
`lane_data`, so HelixScreen's UI is correct either way
(`ams_backend_snapmaker.cpp:844-924`).

### Capabilities

| Feature | Supported | Notes |
|---------|-----------|-------|
| Endless Spool | `Unsupported` | No `get_endless_spool_capabilities()` override — base default |
| Tool Mapping | Per-print only | Fixed 1:1 live mapping; remaps via `SnapmakerNative` pre-print gcode; `set_tool_mapping()` = `not_supported` |
| Bypass | No | `supports_bypass = false`; both entry points `not_supported` — no external spool on a toolchanger (`ams_backend_snapmaker.cpp:240`, `942-948`) |
| Dryer | No | Not supported |
| Recover / Reset / Cancel | No | All three return `not_supported` (`ams_backend_snapmaker.cpp:544-554`) |
| Operation step bar | Yes | Firmware-driven per-direction steps via `ams_operation_phase`; Heat step live |
| Per-slot loaded authority | Override | `slot_is_actively_loaded()` returns `status == LOADED` verbatim (hub table, `ams_backend_snapmaker.cpp:519-526`) |
| Path visualization | Yes | NOZZLE when the latch is set, OUTPUT when port/motion sensor still sees filament, NONE otherwise (`ams_backend_snapmaker.cpp:385-417`) |
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
| `docs/devel/SNAPMAKER_U1_PRINT_TASK_CONFIG.md` | The firmware-native remap/feed-gate API this backend's pre-print path emits |
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
   "filament at the gear" signal exists (`ams_backend_snapmaker.cpp:560-584`). Checked
   2026-08-21: the status model carries **no dedicated feeder/gear-presence field** -
   the three presence signals are `filament_detect.state` (per channel),
   `filament_feed` per-extruder `filament_detected` (port), and the per-tool motion
   sensor. The code's own candidate is `filament_feed.channel_state`: `load_finish`
   (fed to nozzle) vs `preload_finish` (firmware assist stops short of the gear) -
   both already parsed into the channel-state machine
   (`ams_backend_snapmaker.cpp:134-137`, `:560-567`). What is missing is rig
   confirmation that the state reliably means "filament at the gear" before the gate
   is revived.
3. End-to-end timing of the pre-print `SET_PRINT_USED_EXTRUDERS` is unverified live.
   Code-side ordering is established (see Pre-Print Remap above): the print-start
   request is issued only after the config gcode's JSON-RPC ack. Still open,
   firmware-side: whether the ack means the config is mutated and persists by the time
   the baked `PRINT_START` block runs — flagged in
   [SNAPMAKER_U1_PRINT_TASK_CONFIG.md](SNAPMAKER_U1_PRINT_TASK_CONFIG.md) § "Still
   UNCERTAIN".
4. The `prepare_for_resume` doc comment in `include/ams_backend_snapmaker.h:162-169`
   still describes the retired sensor-disable chain; the implementation drives
   `AUTO_FEEDING` (see Runout and Resume above). Comment is stale, code is right.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
