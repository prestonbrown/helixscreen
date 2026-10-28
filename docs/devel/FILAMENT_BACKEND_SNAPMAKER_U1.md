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

One unit ("SnapSwap"), four Feeders, one per toolhead:

```
  Feeder 1 ── Toolhead 1   (gcode T0)   each feeder has its own path and nozzle
  Feeder 2 ── Toolhead 2   (gcode T1)
  Feeder 3 ── Toolhead 3   (gcode T2)
  Feeder 4 ── Toolhead 4   (gcode T3)
```

Both columns above are what the U1's own firmware UI shows: "Feeder 1".."Feeder 4"
where filament enters, "Toolhead 1".."Toolhead 4" at the printing end, each
numbered from 1. The gcode tool index is the separate, 0-based identity a user
types into a console (`T0`-`T3`, matching `extruder`/`extruder1`-`extruder3`),
which is why it is spelled out per row rather than folded into either column.

- `NUM_TOOLS = 4`, slot `i` carries `extruder_name` `"extruder"` / `"extruder{i}"`
  (`src/printer/ams_backend_snapmaker.cpp#AmsSubscriptionBackend`).
- `PathTopology::PARALLEL` on both the unit and `get_topology()`
  (`include/ams_backend_snapmaker.h#AmsBackendSnapmaker`). Because every feeder has an independent
  path, `needs_unload_before_load()` is answered by the base class — the serial
  feeder rule never applies (`include/ams_backend_snapmaker.h#AmsBackendSnapmaker`).
- `tip_method = TipMethod::NONE` — the U1 has no cutter and forms no discrete tip;
  unload is heat + retract, so the unload stepper renders "Heat nozzle -> Retract"
  (`src/printer/ams_backend_snapmaker.cpp#AmsSubscriptionBackend`).
- `tool_to_slot_map` is seeded with identity because that is the literal truth about
  this machine's **physical attachment**: four heads, each permanently holding its own
  spool (`src/printer/ams_backend_snapmaker.cpp#AmsSubscriptionBackend`). Its consumers are the ones that need
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
  (`select_slot_moves_toolhead() = true`, `src/printer/ams_backend_snapmaker.cpp#do_select_slot`).

### Detection

**Wired to one object.** `PrinterDiscovery::parse_objects()` sets `has_snapmaker_` when
the Klipper object list contains `filament_detect` — unique to U1 firmware
(`include/printer_discovery.h#parse_objects`). Registration order matters: a real aftermarket
MMU (AFC, Happy Hare, …) wins even on U1 hardware that also reports
`filament_detect`; the Snapmaker backend is the fallback for a stock U1 with no MMU,
and a bare `toolchanger` object alone is not enough (`include/printer_discovery.h#parse_objects`).

`AmsType::ACE` is the one exception to that precedence. An `ace` object beside
`filament_detect` is [multiACE](FILAMENT_BACKEND_ACE.md), whose slots are nested per unit
under `aces[]` rather than in the top-level `slots` array `AmsBackendAce` reads, so
detection declines it and leaves the printer here (prestonbrown/helixscreen#1426). The other
U1 + ACE Pro mod, DnG-Crafts/U1-Ace, registers `ace_device`, matches no ACE pattern at all,
and reaches this backend without the exception being involved.

`AmsType::AFC` has the same exception for one stub. PAXX ships **AFC-Lite**, a status-only
layer that impersonates [AFC](FILAMENT_BACKEND_AFC.md) so Fluidd and Mainsail will draw their
AFC panel for the U1's four extruders; its own docs call it "a status reporting stub" that
"does not implement actual AFC hardware control", and every macro it defines wraps the U1's
native `AUTO_FEEDING` / `SET_PRINT_FILAMENT_CONFIG`. Its unit reports empty `extruders` and
`hubs`, so AFC infers the unit as `HUB` and the path draws one nozzle behind a hub for a
four-toolhead machine. An `AFC_unit` object beside `filament_detect` is therefore declined and
the printer stays here. The discriminator is exact: real AFC's AFC_unit.py is a base class
with no `load_config_prefix`, so every real unit registers its own hardware type
(`AFC_BoxTurtle`, `AFC_OpenAMS`, `AFC_HTLF`, …) and a Box Turtle genuinely wired to a U1 keeps
its AFC backend.

### Status the Backend Reads

The subscription is the standing whole-frame `notify_status_update` hook every
`AmsSubscriptionBackend` gets (`src/printer/ams_subscription_backend.cpp#start`); the
parse keys off whatever objects the frame carries:

| Klipper object | Fields used | Meaning |
|----------------|-------------|---------|
| `extruder`, `extruder1`-`extruder3` | `state`, `park_pin`, `active_pin`, `activating_move`, `extruder_offset`, `switch_count`, `retry_count`, `error_count`, `nozzle_diameter`, `nozzle_volume_type` | Per-tool toolchanger state and fitted nozzle (`ExtruderToolState`) |
| `toolhead` | `extruder` | Which extruder the carriage holds — authority for the active tool |
| `filament_detect` | `info` (per-channel RFID array), `state` (`[int x4]`) | Spool metadata per channel; `state` is the entrance/tag reading, which reads 0 once filament is fed through to the toolhead — it is not a lane-presence signal |
| `filament_feed left` / `filament_feed right` | per-`extruder{N}`: `filament_detected`, `channel_state`, `channel_error` | Port/buffer presence, the feed state machine, per-channel errors |
| `print_task_config` | `filament_exist`, `filament_type`, `filament_vendor`, `filament_color_rgba` | Firmware-authoritative filament metadata (see the native API doc) |
| `filament_motion_sensor e{N}_filament` (and the `filament_switch_sensor` form) | `filament_detected` | Per-tool runout encoder |

Slot status arbitration across those sources, in parse order: extruder pins
(`active_pin` -> LOADED, `park_pin` -> AVAILABLE), then `filament_detect.state` only
when status is still UNKNOWN, then the port sensor, then `print_task_config.filament_exist`.
The active tool is detected from extruder pin state or `toolhead.extruder`, and
`current_slot`/`current_tool` track the picked-up tool 1:1
(`src/printer/ams_backend_snapmaker.cpp#handle_status`, `1456-1529`).

### RFID (filament_detect.info)

`parse_rfid_info()` reads per-channel tag fields
(`src/printer/snapmaker_status_parse.cpp#parse_rfid_info`):

| Tag field | Maps to | Notes |
|-----------|---------|-------|
| `MAIN_TYPE` | `material` | e.g. "PLA", "PETG"; `"NONE"` skips the whole slot's RFID apply, and with `SUB_TYPE` also absent or `NONE` drops the lane's vendor-cache record |
| `SUB_TYPE` | `spool_name` | Snapmaker product line, e.g. "SnapSpeed"; blank beside a brand other than Snapmaker or empty, where it is firmware's placeholder |
| `MANUFACTURER` (fallback `VENDOR`) | `brand` | |
| `ARGB_COLOR` | `color_rgb` | masked to the low 24 bits |
| `HOTEND_MIN_TEMP` / `HOTEND_MAX_TEMP` / `BED_TEMP` | `nozzle_temp_min` / `nozzle_temp_max` / `bed_temp` | |
| `WEIGHT` | `total_weight_g` | |
| `CARD_UID` | fingerprint string | 4-byte array canonicalized to `"144,32,196,2"`; drives the override-clear hardware event |

The RFID tag exposes no color *name* — `color_name` stays firmware-unset and is
user-editable only. `SUB_TYPE` is recognized as a product line only when it matches one
of eight known literals ("Basic", "Matte", "SnapSpeed", "Silk", "Support", "HF", "95A",
"95A HF"), and those lines name Snapmaker spools: a free-form user `spool_name` is never
round-tripped to firmware as a SUB_TYPE, and neither is a product line beside another
vendor's brand: `build_filament_config_gcode` writes `Basic` instead
(`src/printer/ams_backend_snapmaker.cpp#build_filament_config_gcode`). Nothing this backend
sends reaches `filament_detect.info`, so whatever the reader reports there is the tag's
(`src/printer/ams_backend_snapmaker.cpp#apply_rfid_entry_locked`). Every path that names
the spool from the tag (the read itself, a user clear, a swap-detected clear) goes
through `tag_spool_name`, which blanks a known product line beside a non-Snapmaker brand:
`build_filament_config_gcode` writes `Basic` for any slot that names no product line, so
"Generic" + "Basic" names no line.
On load, a stored product line the record does not declare, beside a non-Snapmaker
brand, is dropped and the record saved back (`src/printer/ams_backend_snapmaker.cpp#on_started`). A channel whose firmware states no
identity at all (tag `MAIN_TYPE`/`SUB_TYPE` NONE, `print_task_config` vendor NONE and not
official) retracts a Snapmaker product line its stored record only remembers, and the
Snapmaker brand beside it, and saves the record back. Nothing else is touched: declared
fields, any other remembered name or brand, a brand beside a catalog pick, and a record
bound to a Spoolman spool all stay
(`src/printer/ams_backend_snapmaker.cpp#retract_unbacked_identity_locked`).

The vendor-cache record mirrors `filament_detect.info`, so an entry that names no tag
(`MAIN_TYPE` NONE, `SUB_TYPE` absent or NONE) drops it: a clear then cannot restate the
product line, material, brand or colour of a tag the reader no longer reports. A slot
field still holding the dropped record's value falls back to what `print_task_config`
last said for that head, or blank/default where it never spoke; the tag's nozzle and
bed temperatures and spool weight go back to SlotInfo's defaults, unless the lane holds a
user's declaration or a bound Spoolman spool. A field something else has written since stays. A `print_task_config` `filament_type` of `NONE` or empty is no
material, never the literal. A repeat of the same silence finds nothing to drop and reports
no change (`src/printer/ams_backend_snapmaker.cpp#retract_tag_reading_locked`).

Every row above is code-verified against `parse_rfid_info()` and the apply loop
(`src/printer/snapmaker_status_parse.cpp#parse_rfid_info`, `src/printer/ams_backend_snapmaker.cpp#apply_rfid_entry_locked`): tag identity rides
`filament_detect.info[ch].CARD_UID`, and a `MAIN_TYPE == "NONE"` tag skips the field
apply (dropping the cached reading, above) while its UID is still captured for swap
detection (`src/printer/ams_backend_snapmaker.cpp#apply_rfid_entry_locked`). Physical
reads from real RFID spools remain rig-pending; code-verified is not field-verified. In
particular, a reader that is disabled, or a restart that does not re-read while a tagged
spool stays loaded, reports `NONE` for that spool: the lane then drops its spool name,
temperatures and weight, and material, brand and colour fall back to
`print_task_config`.

**Tag reading.** `tag_reading()` answers the channel's last entry that a real card stands
behind (`has_card`, a nonzero `CARD_UID`): the vendor-cache fields, the temperatures and
weight the tag states (a `0` is one it left out), and the multi-colour from `COLOR_NUMS`
and `RGB_1..RGB_5` as `#RRGGBB,#RRGGBB`. A filled entry with no card was written over the
API, not read off a spool, and is no reading. The multi-colour is laid onto
`SlotInfo::multi_color_hexes` only while the lane declares no colour, so a colour the user
picks survives the next read (`src/printer/ams_backend_snapmaker.cpp#record_tag_reading_locked`).

### Commands the Backend Emits

| Command / call | Used for |
|----------------|----------|
| `AUTO_FEEDING EXTRUDER=<n> LOAD=1` | Load (`do_load_filament`, `src/printer/ams_backend_snapmaker.cpp#do_load_filament`) |
| `AUTO_FEEDING EXTRUDER=<n> UNLOAD=1` | Unload (`do_unload_filament`, `:456-489`) |
| `INNER_FILAMENT_UNLOAD` | Bare unload fallback only when no slot/extruder can be resolved (`:480-482`) |
| `T<n>` | Tool change / slot select (`do_change_tool`, `:532-538`) |
| `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<l> MAP_EXTRUDER=<p>` | Pre-print logical->physical remap (`build_preprint_gcode`, `:1887-1889`) |
| `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv>` | Pre-print feed gating — always sent, remap or not (`src/printer/ams_backend_snapmaker.cpp#build_preprint_gcode`) |
| `SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[...]` | The saved unload-at-end choice, ahead of every print start (`src/printer/ams_backend_snapmaker.cpp#end_unload_gcode`; see [`SET_PRINT_PREFERENCES`](#set_print_preferences)) |
| `AUTO_FEEDING_BATCH ACTION=START / DOING EXTRUDER=<n> / END` | Batch load/unload when the firmware macro is present (see Batch Load/Unload) |
| `SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=<head> VENDOR=… FILAMENT_TYPE=… FILAMENT_SUBTYPE=…` + `FILAMENT_COLOR_RGBA=…` | The head's `print_task_config` record, which RESUME requires (below) |
| JSON-RPC `printer.control.nozzle_properties` `{extruder, diameter \| volume_type}` | A head's fitted nozzle, from the Nozzle rows (`src/printer/ams_backend_snapmaker.cpp#send_nozzle_change`; see [Fitted Nozzles](#fitted-nozzles)) |

The firmware refuses RESUME with `e<N> not edit filament` while a head the task uses has
no `print_task_config.filament_type`. An RFID spool or `SET_PRINT_FILAMENT_CONFIG` fills it.

A user edit writes the head's record with `SET_PRINT_FILAMENT_CONFIG`, the command the stock
screen sends, which leaves `filament_official` False. Only the lines that differ go out:
the identity line when the record names a different type, vendor or subtype, the colour
line when it names a different colour, both when the record has not been seen and the edit
declared a material, nothing when it already agrees, since every send runs `FLOW_RESET_K`
on the head. VENDOR is `Snapmaker` or `Generic`, FILAMENT_SUBTYPE a known product line or
`Basic` (`src/printer/ams_backend_snapmaker.cpp#write_task_filament_record`).

The firmware forgets a head's record whenever filament leaves its toolhead. The
toolhead switch's remove event (an unload, unload-at-end, a runout), and a feeder port
that empties while the toolhead holds nothing, call `filament_detect`'s
`request_clear_filament_info`, which resets the head's `print_task_config` record to
`NONE` / `FFFFFFFF` without a log line. A
tagless spool resting in the feeder after an unload-at-end therefore reads `NONE` to
every other client while its lane still declares it. The backend writes the lane's
declaration back when the record reads unset, the lane declares a material, the port or
the nozzle latch holds filament, no spool has gone into the port since the declaration,
no feed is moving the head, and the heads are free (the same hold as an edit). It sends
once per drop: the record naming a type re-arms it, and an edit's own write counts as the
restore until the firmware echoes it. A spool inserted after the declaration is one
nobody has named, so it waits for the next edit
(`src/printer/ams_backend_snapmaker.cpp#record_restores_locked`).

The Extended Firmware's `POST /printer/filament_detect/set` is not used. It fills the same
record, but it also sets `filament_official` True for the head (measured on a rig: the flag
rises within a second of the HTTP 200, a plain `SET_PRINT_FILAMENT_CONFIG` is then refused
with `official filament, not configurable!`, and `FORCE=1` clears it), and nothing the
endpoint accepts clears the flag. The `filament_detect.info` entry it also writes is read by
nothing on the machine but the endpoint itself and spoollink (which looks only at
`CARD_UID` and `CARD_EVENT_TIME`); the stock screen and OrcaSlicer read identity from
`print_task_config`, and OrcaSlicer takes only `BED_TEMP` and `FIRST_LAYER_TEMP` from
`filament_detect`. Temperatures from an edit live in `lane_data`.

Every record write runs `FLOW_RESET_K` on its head, so none lands under a running print.
A save made while the backend's own `print_stats.state` reads `printing` is kept and
written by the first frame that reads neither `printing` nor `paused`, since a pause the
print raised (a runout, an M600) is no moment to reset a head's flow calibration. A save
made while already paused goes straight out, as the resume path writes then too, and a
held save is dropped with the head's override (a Clear Spool, a swap) and at a restart;
before the backend's frames name a state, the app's print job
state decides (`src/printer/ams_backend_snapmaker.cpp#record_writes_held_locked`). A save
the record already agrees with sends nothing, forced head or not; FORCE=1 only decides
the flag and that both lines go.

A head an earlier endpoint write left official is repaired once per session: official,
its `filament_detect` entry filled with `CARD_UID` known to be no card, no card seen on
the channel this session, and a record whose type (and colour, when both are known)
matches the stored edit gets a `FORCE=1` rewrite of that edit. It waits for a
`print_stats.state` the backend has read itself that is neither `printing` nor `paused`.
An empty entry (what a reader that is off reports for a real spool), a real card, a
record naming anything else, or a head with no stored edit is never touched
(`src/printer/ams_backend_snapmaker.cpp#official_flag_repairs_locked`).

A filled entry with no card behind it was written over the API, not read off a spool, so
it files no vendor-cache reading and retracts one an earlier tag left
(`src/printer/ams_backend_snapmaker.cpp#apply_rfid_entry_locked`).

**hw-verify:** what a `filament_detect/set` written by a 1.0.x build leaves on a head
holding a real Snapmaker RFID spool is unmeasured. The endpoint builds the entry fresh
from the POST body, which carried no `CARD_UID`, so the entry may read as filled with no
card until the reader next reads the spool: a vendor-cache retraction, and an
official-flag repair if the record matches the stored edit and no card was seen this
session. Needs a real RFID spool to check. A `SET_PRINT_FILAMENT_CONFIG` was measured on
the rig to change `print_task_config` only, leaving `filament_detect.info` and
`filament_official` untouched.

`filament_official` therefore does not mean an RFID tag. The backend decides that from
`filament_detect.info[ch].CARD_UID`: the firmware reports it either as a plain integer
(`0` for no card, on every head of a rig with its reader off) or as a byte array. A nonzero
integer or a nonzero byte is a real card; `0` or an all-zero array is known to be none (an
untagged channel, an endpoint write, a reader that is off); an entry with no `CARD_UID` is
unknown (`include/snapmaker_status_parse.h#SnapmakerRfidInfo`). A head with a real card is
never written. A head that is official with no card is written with `FORCE=1`, which the
firmware requires there; with the reader off a real Snapmaker spool also reads `0`, so its
record is overwritten too. A head that is official with no card reading is left alone (`src/printer/ams_backend_snapmaker.cpp#record_write_needs_force_locked`).

A record filled once can still be blank by the time a print pauses (a later firmware
filament operation can clear it). `prepare_for_resume` classifies the pause first on every
path, so a Terminal pause writes nothing. A pause that is not a runout then writes the same
record, under the same card rule, for every used head whose record is blank while the slot
knows its material and the record has been seen; each head goes out as its own request, in
order, and a refused one does not stop the rest. The used heads are the union of
`extruders_used` (which can read all false mid-print), the heads the task's snapshotted
extruder map routes to, and the head paused on. A runout pause writes nothing: the user may
have loaded a different spool, so the material remembered for the old one would be recorded
against it, and the refeed runs alone. A head left blank gets RESUME's refusal, which
`classify_error` explains by naming the head and saying it needs a material
(`src/printer/ams_backend_snapmaker.cpp#resume_record_repairs_locked`).

Why `AUTO_FEEDING ... LOAD=1` and not the obvious alternatives — the source records the
trail (`src/printer/ams_backend_snapmaker.cpp#do_load_filament`): bare `T{n}` is a no-op when the target tool
is already active; `AUTO_FEEDING EXTRUDER={n}` alone is a silent no-op because
`FEED_AUTO` falls through without a LOAD/UNLOAD parameter; `SM_PRINT_AUTO_FEED` is gated
on `print_task_config.extruders_used`, which sits all-false during a paused print. The
same comment names the firmware reference (the cmd_FEED_AUTO handler in Snapmaker's
filament_feed extension, ~line 1681). Unload
must use the same envelope: the bare `INNER_FILAMENT_UNLOAD` leaf skips the feed state
machine and breaks aftermarket feeders that hook `unload_finish` (the DnG-Crafts U1-Ace
ACE-Pro adapter, #974).

### Batch Load/Unload

The U1 answers `max_heads_per_filament_op()` 4 (`include/ams_backend_snapmaker.h#AmsBackendSnapmaker`;
the default is 1, and the mock's Snapmaker persona answers the same). The sidebar's Load and
Unload open the head picker (`BatchFilamentModal`, `include/ui_batch_filament_modal.h`) with
up to four heads tickable; several ticked run as one call, where `load_filament_batch()` /
`unload_filament_batch()` route through the shared `run_filament_op()` executor, so the
operation bar, error surfacing and skip handling are the same as a single-slot op. One
ticked head takes the per-slot path.

`do_filament_batch()` (`src/printer/ams_backend_snapmaker.cpp#do_filament_batch`) builds one gcode
script per batch in two shapes:

- **Legacy firmware:** a chain of per-head `AUTO_FEEDING EXTRUDER=<n> LOAD=1` (or `UNLOAD=1`)
  lines, one per selected head.
- **Firmware with the batch macro**, detected at connect via
  `discovery.macro_config_name(helix::macro_patterns::AUTO_FEEDING_BATCH)`: a single
  `AUTO_FEEDING_BATCH ACTION=START` / `ACTION=DOING EXTRUDER=<n> <LOAD|UNLOAD>=1
  [NEXT_EXTRUDER=<next>]` / `ACTION=END` script. START snapshots the current hotend targets and
  raises the firmware-side `doing` interlock; `PRINT_PRESTART_CHECK` refuses to start a print
  while it is set; END restores the snapshot.

Two recovery paths keep a firmware batch from being stranded half-done:

- **RPC failure mid-batch:** `end_firmware_batch()` re-sends `ACTION=END` only when the dispatch
  id still matches the live plan, so a second batch already started is never ended out from
  under itself.
- **Reconnect:** `batch_feeding::reconcile_on_connect()` (`include/batch_feed_reconcile.h`) sends
  `ACTION=END` at connect when the `doing` interlock is set, no print is in flight, and no local
  batch is active (`filament_batch_in_flight() == false`).

The batch's single RPC gets `slots.size() * kBatchFeedOpTimeoutMs` (150 s per head, the same
headroom one feed gets): a four-head cold batch heats every nozzle from ambient. The backend
tracks the dispatched heads in its `BatchPlan`, whose cursor advances only as a head reaches the
direction's terminal channel state; a head that hits a `*_fail` state clears the firmware batch
interlock and is reported, not retried silently.

Not yet verified on hardware; both script shapes and both recovery paths are source-derived.

### Per-Filament Load/Unload Temperatures

The firmware keeps load and unload temperatures per spool identity. The provider table has
one row for the U1: detect object `filament_parameters`, files under the `config` root
(`include/filament_temperature_source.h`). The firmware's `filament_parameters` module writes
one JSON file per nozzle config into its snapmaker config dir, `snapmaker/filament_para_<nozzle>.json`
for `standard_02`, `standard_04`, `standard_06`, `standard_08` and `high_flow_04`, once Klipper
is ready, each a flat object keyed
`{vendor}_{main_type}_{sub_type}_{field}`. `capture_on_connect()` runs as the
`filament_temperatures` discovery step, downloads every file through `ITransfersAPI::download_file`,
and publishes the merge of the files that parse as `FilamentTemperatures{load_c, unload_c}`;
the 0.4 standard file leads, so its values win a leaf several files list. A missing or
unparseable file is skipped, and with none readable nothing is published. It never sends
`FILAMENT_PARA_GET_ALL_INFO`: that prints the same tables, about 43 KB, into every client's
console. `lookup_filament_temperatures()` feeds the result into
`ActiveMaterialProvider` as `result.firmware_temps`
(`src/printer/active_material_provider.cpp`). The load temperature wins over the
database-derived preheat when the firmware publishes one
(`include/filament_op_slot_resolver.h`): a firmware that measured a load temperature measured
it for the load, so it outranks a print-range midpoint standing in for one. The unload
temperature is captured and parsed but has no consumer yet. These are load/unload
temperatures only; they are never folded into `nozzle_temp_min`/`max`, which keep describing
the printing range.

### The channel_state Feed Machine

The firmware exposes 39 distinct `filament_feed` channel states (captured live from
firmware 20260608); `classify_channel_state()` maps each to
`{action, phase, terminal, fail, sets_loaded, clears_loaded}` off one table, with a
conservative prefix/suffix fallback for unknown future states
(`src/printer/ams_backend_snapmaker.cpp`). That single classification drives:

- **The operation step bar.** LOAD/manual/preload share a 6-step model in
  the firmware filament_feed.py's own order (Home -> Select -> Feed -> Heat -> Extrude -> Purge:
  the module pushes filament to the cold toolhead, then the nozzle heats and the extruder
  pulls it in). A load whose head is already picked skips Select; manual feed has no
  Feed and preload stops at Feed. UNLOAD uses 4 steps (Home -> Select -> Heat ->
  Retract). The Heat step shows a live nozzle temperature. The current index is published through the
  shared `ams_operation_phase` subject, which the sidebar consumes generically
  (`src/printer/ams_backend_snapmaker.cpp`). Two traits keep the bar on the right list:
  `holds_optimistic_action`, because `channel_state` stays idle between the firmware
  accepting `AUTO_FEEDING` and its first homing step, and `unloads_stand_alone`, because
  an unload started anywhere else (Filament panel, batch picker, a macro) would otherwise
  read as the first half of a swap and draw the 6-step load list. A single-head
  `AUTO_FEEDING` blocks until the feed is over, so its answer ends the held action: OK
  raises `kEventLoadComplete` / `kEventUnloadComplete`, an error raises `kEventError`,
  and a timeout raises nothing (`src/printer/ams_backend_snapmaker.cpp#dispatch_feed`).
  That covers the loads `FEED_AUTO` declines with an OK and no frame; the ones visible in
  the channel snapshot (already loaded, feeder not in automatic mode, motion sensor
  disabled) are refused before sending
  (`src/printer/ams_backend_snapmaker.cpp#do_load_filament`).
- **The per-tool "loaded at toolhead" latch.** Set on `load_finish`; cleared on
  `unload_finish` / `wait_insert` / `preload_finish`; left unchanged on transient and
  fail states. This latch — not the motion sensor — is the authority for
  `slot_has_filament_at_toolhead()`, `can_unload_from_toolhead()`, and the NOZZLE path
  segment, because the per-tool encoder fails to drop to false after an unload on
  current firmware. That switch is still true after an unload because the cut tip stays
  in the toolhead, so every Unload gate also ORs `slot_filament_parked_in_toolhead()`:
  Unload stays offered on a just-unloaded head, since a heated unload still has filament
  to act on (`include/ams_backend_snapmaker.h#AmsBackendSnapmaker`,
  `src/printer/ams_backend_snapmaker.cpp#can_unload_from_toolhead`).
- **Action lifecycle and errors.** `*_fail` states and `channel_error` tokens surface as
  `AmsAction::ERROR` with a direction-aware message ("No filament in Feeder N. Load
  filament and retry." for the `no_filament` token), except when the feeder is empty,
  idle, and not the active feeder — the firmware reports `no_filament` for any empty
  feeder, which must not latch a spurious error modal on a deliberately unloaded head in a
  multi-color print (`src/printer/ams_backend_snapmaker.cpp#friendly_channel_error`, `1321-1354`).
- **Resume refusal.** RESUME is refused while a used extruder's `filament_type` in
  `print_task_config` is empty or `NONE` (a tagless third-party spool), with
  `!! e<N> not edit filament`. `classify_error()` turns it, while paused or printing, into a
  "Filament not set" modal naming the 1-based slot, with a lone OK: no single gcode fixes it,
  and offering Resume would loop. It reaches the user only for a head whose material
  HelixScreen does not know either, or after a runout: on any other pause
  `prepare_for_resume` first writes the record of every used head the slot can answer for
  (see "Commands the Backend Emits"). The event sets
  `command_refusal`, so the resume caller's raw
  "Failed to resume:" toast stands down for it and runs only if the explanation never
  arrives (`RPC_ERROR_OWNERSHIP.md` § "A refusal the classifier explains";
  `src/printer/ams_backend_snapmaker.cpp#classify_error`).
- **`preload_finish` is terminal-for-latch but does not end the operation** — a re-unload
  of a staged feeder keeps that state while the nozzle heats, and dropping to Idle there
  killed the unload step display mid-heat (`src/printer/ams_backend_snapmaker.cpp#handle_status`).

A `*_finish` that clears the latch also demotes the slot LOADED -> AVAILABLE and clears
`filament_loaded` for the active tool, but never resets `current_slot`/`current_tool`:
those track the picked-up tool, and resetting them mis-routed a bare unload to T0 for a
user printing TPU without feeders (field report recorded at
`src/printer/ams_backend_snapmaker.cpp#handle_status`). Feeders reaching `unload_finish` are reported to
`AmsState::mark_slot_unloaded()` after the mutex is released so `FilamentSensorManager`
suppresses the runout modal during the expected pull-out grace window
(`src/printer/ams_backend_snapmaker.cpp#handle_status`, `src/printer/ams_backend_snapmaker.cpp#handle_status`) — calls into
`AmsState` stay outside the backend mutex because the backend registry takes its own lock
before a backend's (`include/ams_backend_registry.h#AmsBackendRegistry`).

### Runout and Resume

`prepare_for_resume()` (`src/printer/ams_backend_snapmaker.cpp#prepare_for_resume`) classifies the pause first:
dirty-bed exceptions (`{id:532, code:1}`, or the message text) are Terminal and surface
the restart UX; runout (`{id:523, code:0}`, "e{N}_filament runout") is Recoverable
(`include/snapmaker_resume.h#helix` — `sdcard` state deliberately not consulted because
runout also clears it). With the motion sensor reading filament, RESUME needs no prep.
With runout latched, the backend drives
`AUTO_FEEDING EXTRUDER=<n> LOAD=1 PRINTING=1` itself — the firmware's own
`INNER_RESUME` auto-feed is gated on `extruders_used`, which stays false mid-print, so a
plain RESUME re-pauses immediately. The chain heats, feeds, and flushes (~86 s measured;
150 s timeout, silent send), raises an info toast for the wait, and hands control back so
the caller dispatches RESUME; `on_ready` always fires on the main thread. What is
recorded as live-verified (#991) is the `AUTO_FEEDING` command itself - it blocks until
`load_finish`, is idempotent, and the ~86 s figure was measured live
(`src/printer/ams_backend_snapmaker.cpp#prepare_for_resume`, `:722-724`). The full chain - runout pause ->
runout dialog -> refeed -> RESUME -> print continues - is **not field-tested**.

**Resume waits for a confirmed load.** `FEED_AUTO` answers OK without feeding when the
port reads empty under `PRINTING=1`, and raises 525 (`extruder[N]: state: ..., error: ...!`)
when its own test, `channel_state == load_finish` with `channel_error == ok`, fails at the
end. The head presence switch reads present for filament that reached the toolhead and
stopped short of the nozzle, so it vouches for nothing. A load that errors, times out, or
answers OK with the feeder short of that test (the refeed, a Load from the filament panel),
and a feeder frame naming a `*_fail` state or a channel_error fault from any source, mark
the head's load unconfirmed for the current print. The refeed's OK can arrive before the
frame naming `load_finish`, so it is judged on the head's next decisive frame (loaded with
no error, or a failure; `load_finish` can land a frame before its error reads `ok`), or
after 3 s without one. One refeed runs at a time: another Resume meanwhile is answered busy.
A feeder still mid-load or mid-hand-feed on the head (a `load_*` or `manual_sta_*` state in
progress, e.g. started from the printer's own screen) refuses Resume outright. While a head is marked, `prepare_for_resume` never reaches RESUME: on the head
it resumes on it retries the refeed, and on another head the print uses (`extruders_used`)
it refuses, since loading that head would leave it picked up when RESUME runs; a head the
print does not use holds nothing. A Resume after a failed load is the
dangerous action: `INNER_RESUME` extrudes 20 mm (`_CLIENT_EXTRUDE LENGTH=20`) into
whatever jammed. The refusal names the head and points at the hardware ("Toolhead 2 (T1)
didn't load. Check that its hotend is seated and do a test extrude before resuming."),
with tapping Resume to retry the load as the way forward. Even with nothing marked, a head
an automatic feeder serves whose feeder reports a failure runs the refeed first, the motion
sensor's present reading notwithstanding. Settled states that are no failure resume as they
are: `load_finish`, `unload_finish` (filament back at the feeder), `manual_sta_finish` (a hand
load), `preload_finish`. The mark clears when that feeder reaches `load_finish`, or finishes a
hand feed (`manual_sta_finish`), with no error, and when the print leaves printing/paused.
A bare sensor reading never clears it: `FEED_AUTO` returns OK without feeding on an empty
port, and filament stopped at the head switch reads present. For a head already loaded,
`<klipper>/klippy/extras/filament_feed.py`'s `cmd_FEED_AUTO` returns OK at once when
`channel_state` is `load_finish` and `channel_error` is `ok`, changing nothing and sending
no frame, so the refeed judges it on the snapshot it already holds; a hand-loaded head
(`manual_sta_finish`) is not that early return, and its own frame clears the mark before
Resume refeeds anything. The marks live in the app: after the app restarts (not a websocket
drop), a mark from an `AUTO_FEEDING` error or an OK with no `load_finish` is gone, and only
a feeder still reporting a `*_fail` state or an error fault marks the head again. A restart
from the beginning reads the file's tools from Moonraker's metadata, the tools cache and at
most the footer's one range read (`FileRead::Footer`), never downloading the file: Orca's U1
profile leaves `filament_weights` out of the metadata but writes `; filament used [g] = ...`
per filament in the footer. A printer switch during that read drops the restart It survives a reconnect to a print still
running, since the first print state a connection reads is no change of print, and the
subscription's first frame marks any head whose feeder still reports a failure
(`src/printer/ams_backend_snapmaker.cpp#mark_load_unconfirmed_locked`).

**A print started without the task setup.** Orca's upload-and-print (Moonraker's
`/server/files/upload` with `print=true`) and a Fluidd start reach the firmware as a plain
`SDCARD_PRINT_FILE` with no parameters, so `extruders_used` stays all false: the firmware's
prestart feed (`sm_print_auto_feed`), pre-extrude (`SM_PRINT_PREEXTRUDE_FILAMENT`), switch
check and end-of-print unload (`SM_PRINT_END_AUTO_UNLOAD_FILAMENT`) all skip every head.
`SET_PRINT_USED_EXTRUDERS` is refused while printing or paused (`id=531, code=16`), so it
cannot be repaired mid-print. The stock screen says nothing about it, and with its heads
loaded such a print runs fine. What fails is a head the print uses that is not loaded: the
print pauses when it first needs it. So on a print's first printing frame the backend reads
the file's tools (`FileStartFactsLoader`, footer at most) and, for each head they route to
whose feeder reports it not loaded (`feeder_reports_loaded()` false, no load under way) and
that the task does not name in `extruders_used` (the firmware loads those itself), raises
one warning naming the head ("Toolhead 3 (T2) isn't loaded. The print will pause when it
needs it."). Once per print per connection, on the
first frame naming the print's state or file; after a restart that never left the print it
waits for the frame naming the new file, so it reads the file that starts (a restart of the
same file is not checked again); silent when every used head is loaded or the tools cannot be read (`src/printer/ams_backend_snapmaker.cpp#warn_unloaded_used_heads`).

Related capability flags: `recovers_filament_on_resume() = true` (Resume re-feeds, so
the runout dialog presents Resume as primary) and
`should_suppress_idle_runout_modal() = true` (the U1 drives load/unload itself, so an
idle feeder going empty needs no operator action) (`include/ams_backend_snapmaker.h#AmsBackendSnapmaker`).

`is_stuck_motion_sensor_runout()` (motion sensor false, port sensor true = stale encoder)
currently has **no caller in tree** — the auto-recover path that consumed it was pulled
because that signal cannot distinguish "stale encoder" from "preloaded 4 inches short of
the gear"; it is kept as detection infrastructure for a deferred follow-up
(`src/printer/ams_backend_snapmaker.cpp`). The active tool's port-present flag it builds on
is still published to `AmsState::set_active_tool_port_present()` on change (#991), which
is what gates Resume in the runout dialog (`src/printer/ams_backend_snapmaker.cpp#handle_status`).

### Pre-Print Remap (RemapStrategy::PrePrintSend)

`SET_PRINT_EXTRUDER_MAP` / `SET_PRINT_USED_EXTRUDERS` error mid-print (firmware id 531),
so the config must land before `PRINT_START`. `requires_preprint_send() = true` is
**always-on, even with no remap**: `SET_PRINT_USED_EXTRUDERS` suppresses the spurious
auto-feed of unused heads baked into every Orca-sliced file, which otherwise feeds an
empty head and cancels the print on runout (`include/ams_backend_snapmaker.h#AmsBackendSnapmaker`,
`src/ui/ui_print_start_controller.cpp#execute_print_start`).

Send ordering is guaranteed on our side of the wire. Both start paths gate on
`requires_preprint_send()` and hand the real start step to
`send_backend_preprint_then()` as its completion continuation, then `return` - the
start cannot fire first (`src/ui/ui_print_start_controller.cpp#execute_print_start`, reprint
`:454-471`). The built gcode goes out as a single `printer.gcode.script` JSON-RPC with a
15 s timeout (`:355-404`, dispatched at `src/api/moonraker_api_controls.cpp#execute_gcode`); the
print-start request is issued only from that request's success callback, and an error or
timeout aborts with a modal so the print never starts half-configured (`:388-403`). The
upload/prep window that follows only widens the gap. What this does NOT settle is
firmware-side: whether the ack means `print_task_config` is already mutated - and stays
mutated - by the time the baked `PRINT_START` block executes. That stays in the config
doc's "Still UNCERTAIN" list.

The routing half is the static `preprint_gcode(tools_used, remap)`, which is pure (no API
access; unit-tested directly): one `SET_PRINT_EXTRUDER_MAP` per user remap entry, then one
`SET_PRINT_USED_EXTRUDERS` with the deduplicated, ascending physical-head CSV resolved
through the remap. Logical tools 4-31 without an explicit remap fall to head 0, matching
the firmware's default map (`src/printer/ams_backend_snapmaker.cpp#preprint_gcode`).
`build_preprint_gcode(tools_used, remap)` appends the per-print preferences to it, today
the unload-at-end list (`end_unload_gcode()`), which needs no routing. So an empty
`tools_used` still sends the preferences, and the two callers that have no routing use
exactly that: a reprint with no recorded task map
(`src/ui/ui_print_start_controller.cpp#reprint_impl`) and a restart from the beginning,
which appends them after its `CANCEL_PRINT_BASE`
(`src/ui/ui_resume_dispatch.cpp#restart_print_from_beginning`). Full command
semantics — logical (0-31) vs physical (0-3) index rules, persistence behavior, the
`filament_official` FORCE gate — live in
[Firmware API: `print_task_config`](#firmware-api-print_task_config) below.

`set_tool_mapping()` itself returns `not_supported`, and the backend declares
`owns_tool_mapping_table() == false`: the physical head-to-spool attachment is fixed 1:1
and cannot be edited, and
remaps happen per-print through the pre-print path above. Routing is not fixed — the
firmware's `extruder_map_table` holds the live logical-to-physical answer for the current
print, and `get_tool_mapping()` publishes it (read-only; the write side is the pre-print
gcode).

**A reprint with no known routing.** A reprint replays the routing the backend saw the
last print configured with (`last_print_tool_mapping()`). When it saw none (a slicer or
Fluidd start, or the app restarted since), no map is written: an identity map is a wrong
answer for a crossover print. The used heads still go out. `<klipper>/klippy/extras/print_task_config.py`'s
`reset_print_info()` sets `extruder_map_table` back to its default at every print end
(complete, cancel, error), so a print whose start writes no map runs tool n on head n, and
`build_unrouted_preprint_gcode()` sends `SET_PRINT_USED_EXTRUDERS` for the file's tools at
those heads, plus the unload choice (`print_routing_resets_to_identity()`). The file's tools
come from `file_tools_used()` (scanned tools, else per-tool weights). Nothing goes when the
tools cannot be read, or when the firmware's live map reads other than its default for a
used tool (a map a start that never printed left behind). A file cannot set its own
routing: `SET_PRINT_EXTRUDER_MAP`, `SET_PRINT_USED_EXTRUDERS` and
`SET_PRINT_TASK_PARAMETERS` all refuse while `print_stats.state` is printing or paused
(`id=531`), which is when a file's lines run
(`src/ui/ui_print_start_controller.cpp#reprint_impl`).

### Slot Overrides and lane_data

Per-slot user overrides persist through `FilamentSlotOverrideStore` under the
`"snapmaker"` key style, bulk-loaded in `on_started()` before any status parse
(`src/printer/ams_backend_snapmaker.cpp#on_started`). Every parse tail runs the shared convergence:
`check_hardware_event_clear()` first (a `CARD_UID` change means the physical spool was
swapped — clear the stale override; empty UID is "no signal" and never clears; first
observation only sets the baseline), then `mirror_firmware_to_lane_data()` under
`OverwriteAlways` so OrcaSlicer's MoonrakerPrinterAgent sees the spool, then
`apply_resolved_lane()` laying the lane's resolved identity back over firmware truth
(`src/printer/ams_backend_snapmaker.cpp#handle_status`, `include/ams_backend.h#AmsBackend/apply_resolved_lane`).

Because the UID is a hardware identifier the UI cannot write, this backend registers no
expected-echo value with the fingerprint tracker — user edits can never masquerade as a
hardware swap (`include/ams_backend_snapmaker.h#AmsBackendSnapmaker`). Clears preserve
firmware-populated fields (`brand`, `total_weight_g`) and reset override-exclusive ones
(`spoolman_*`, `remaining_weight_g`, `color_name`, catalog identity)
(`src/printer/ams_backend_snapmaker.cpp#clear_override_fields`); `spool_name` is reset to
the product line the tag itself states, or blank.

User edits round-trip to firmware through `SET_PRINT_FILAMENT_CONFIG` (see "Commands the
Backend Emits"); the override persists to `lane_data` whatever the firmware takes
(`src/printer/ams_backend_snapmaker.cpp#apply_user_edit`), and a record the firmware
later drops to `NONE` is written back from it while the spool stays in the lane.

### Capabilities

| Feature | Supported | Notes |
|---------|-----------|-------|
| Endless Spool | `Unsupported` | No `get_endless_spool_capabilities()` override — base default |
| Tool Mapping | Per-print only | Physical attachment fixed 1:1 and non-editable (`set_tool_mapping()` = `not_supported`); per-print ROUTING is set via `PrePrintSend` pre-print gcode and read back from `extruder_map_table` by `get_tool_mapping()` |
| Bypass | No | `supports_bypass = false`; both entry points `not_supported` — no external spool on a toolchanger (`src/printer/ams_backend_snapmaker.cpp#AmsSubscriptionBackend`, `src/printer/ams_backend_snapmaker.cpp#enable_bypass`) |
| Dryer | No | Not supported |
| Recover / Reset / Cancel | No | All three return `not_supported` (`src/printer/ams_backend_snapmaker.cpp#recover`) |
| Operation step bar | Yes | Firmware-driven per-direction steps via `ams_operation_phase`; Heat step live |
| Homing and heating | Firmware | `FEED_AUTO` homes and sets its own nozzle target (`load_homing` / `load_heating`, `unload_homing` / `unload_heating`), so `delegates_homing_to_printer()` and `supports_auto_heat_on_load()` are true: no G28 and no UI preheat from us |
| Batch load/unload | Yes | `max_heads_per_filament_op()` = 4; see Batch Load/Unload above |
| Per-slot loaded authority | Override | `slot_is_actively_loaded()` returns `status == LOADED` verbatim (hub table, `src/printer/ams_backend_snapmaker.cpp#slot_is_actively_loaded`) |
| Path visualization | Yes | NOZZLE when the latch is set or the toolhead switch reads filament, PREP when only the feeder's port sensor does (it is each lane's entry sensor, `slot_has_prep_sensor()`), OUTPUT when only an unreported toolhead switch's default says so, NONE otherwise (`src/printer/ams_backend_snapmaker.cpp#slot_segment_locked`). The system segment is the mounted tool's own lane, NONE with no tool on the carriage |
| RFID | Yes | Per-channel tag read; UID change clears the slot override |
| Spoolman | Fields only | `spoolman_id`/`spoolman_vendor_id` persist in slot overrides; no Snapmaker-specific Spoolman wiring exists in the backend |
| Active spool switching | Firmware with SpoolLink, else HelixPrint | See below |
| Mock mode | Yes | `HELIX_MOCK_AMS=snapmaker` (aliases `snapswap`, `u1`): 4 slots, PARALLEL, non-editable mapping ([MOCK_ENVIRONMENT_VARIABLES.md](MOCK_ENVIRONMENT_VARIABLES.md)) |

### Active Spool and SpoolLink

Moonraker charges filament to one active spool, so a four-head print is right only when the
active spool follows the extruder. The paxx extended firmware's SpoolLink feature
(`overlays/firmware-extended/38-feature-spoollink` in paxx12/SnapmakerU1-Extended-Firmware)
does that itself: a Moonraker component `[spoollink]` plus a Klipper extra `[spoollink]`
(webhook `spoollink/set`, macro `SET_SPOOL_ID`) that sets Moonraker's active spool on every
extruder change. It is installed only while Spoolman is configured.

HelixScreen reads a `spoollink` object in `printer.objects.list`, or a `[spoollink]` section in
`configfile.settings`, as "the printer already switches spools"
(`include/filament_accounting.h#printer_switches_active_spool`, from
`PrinterDiscovery::has_spoollink()`). The filament accounting ownership rule then answers
`printer`, and no HelixScreen path switches it: not the plugin, not the app's fallback, and not the lane-load sync (`active_spool_owner_now()`). On stock
firmware the owner is `helix`: the plugin switches the active spool from `toolhead.extruder`
with the spools linked to each head, when the user opted in (`HELIXPRINT_PLUGIN.md`, filament
accounting). Either way the plugin's per-job spool record still counts what Moonraker charged.

### Toolhead Offset Calibration

The stock firmware's multi-toolhead offset calibration is the `snapmaker_nozzle_offset` guided flow, started from Controls > Calibration > Tool Offsets: `helix::tool_offset_calibration::resolve_procedure()` returns it as a `GuidedFlow` for a U1, so the generic entry opens it instead of the macro panel, with no beta gate. It is offered only when `snapmaker::nozzle_offset::supported()` finds `_EXTRUDER_OFFSET_ACTION_PRESTART`, `_EXTRUDER_OFFSET_ACTION_EXIT` and the five per-head hook macros for T0..T3 in the objects list (`include/snapmaker_nozzle_offset.h`); the Calibration tile is `moves_machine`, so it greys out while a print, running or paused, holds the printer.

The commands are registered in Python by `ExtruderOffsetCalibration` in `<klipper>/klippy/extras/probe_inductance_coil.py` (status object `extruder_offset_calibration`), with no `desc=`, so they never appear in `printer.gcode.help`. Each `EXTRUDER_OFFSET_ACTION_<STEP>` is one blocking step that runs the matching `_EXTRUDER_OFFSET_ACTION_<STEP>[_Tn]` macro from `xyz_offset_calibration.cfg`. Every step but PRESTART refuses with coded fault `0003-0530-0000-0009` unless `machine_state_manager.main_state` is `XYZ_OFFSET_CALIBRATE`, which PRESTART enters and EXIT leaves. The stock screen (`/usr/bin/gui`, view source cali_extruders_offset.c) sequences them; its step table and its Next-button handler fix this order:

| # | Command | Who waits |
|---|---------|-----------|
| 1 | `EXTRUDER_OFFSET_ACTION_PRESTART` | enters the state, cavity LED on, bed 55 C and all nozzles 140 C, `G28` |
| 2 | per head n: `HEAT TOOL_ID=Tn` | heats to the loaded filament's print temperature minus 20 C (`HEAT_TEMP` overrides) |
| 3 | `AUTO_CLEAN TOOL_ID=Tn SAFE_MOVE_Y_POS=250` | purges 30 mm (5 mm for soft filament) and wipes at the discard position |
| 4 | `MANUAL_CLEAN TOOL_ID=Tn` | parks the head at X135 Y5 Z150 |
| 5 | user brushes the hot nozzle, taps Next | stock text: "Clean the nozzle with the provided wire brush and maintain a clean docking position." |
| 6 | `WAIT_COOL TOOL_ID=Tn TEMP=140` | fans on until the nozzle reaches probing temperature |
| 7 | user removes the PEI sheet, taps Next | |
| 8 | `DETECT_PLATE PRESENCE=0` | probes a solid and a hole point; fault `0003-0530-0000-0011` while the sheet is on (`screws_tilt::plate_still_on_bed`) |
| 9 | per head n: `PROBE_CALIBRATE TOOL_ID=Tn` | `XYZ_OFFSET_CALIBRATE` against the bed's reference hole; the step that completes the fourth result runs `SAVE_RESULT FORCE_SAVE=0` itself |
| 10 | `EXTRUDER_OFFSET_ACTION_EXIT` | heaters off, idle timeout restored, Z to 150, state back to IDLE |

`snapmaker::nozzle_offset::Runner` walks that plan one RPC at a time. A plate refusal asks for the sheet again; any other failure sends EXIT and stops. A cancel sends EXIT once, after the webhook `printer.extruder_offset_calibration.probe_abort` when a probe pass is in flight, because a Klipper command blocks the queue until it ends and EXIT would wait behind it. The overlay (`ui_snapmaker_nozzle_offset_overlay`) cancels on `NavigateAway` only: a screensaver must not end a 20-minute run. After EXIT it reads each head's `extruder_offset` back for the result screen.

**A run whose driver died.** The state lives in `machine_state_manager.main_state` (`MachineMainState.XYZ_OFFSET_CALIBRATE` = 2 in `<klipper>/klippy/extras/machine_state_manager.py`; IDLE is 0, SCREWS_TILT_ADJUST is 8). If HelixScreen exits mid-run, the firmware keeps the state, the heaters at the run's targets and the idle timeout at its pause value. `AmsBackendSnapmaker::on_started()` therefore calls `snapmaker::nozzle_offset::reconcile_on_connect()`: one fresh `objects.query` of `main_state` (the discovery subscription does not always carry the object), then one EXIT, logged, when the state is held and `run_live()` says no Runner in this process owns it. A websocket reconnect during a live run restarts the backend too, and the live check is what keeps the reconcile from ending that run. The decision is made when the query answers, not when it is sent. A firmware restart clears the state on its own, since `main_state` is not persisted.

**A head with no filament.** Stock runs every step on all four heads regardless: the screen's controller calls HEAT and AUTO_CLEAN with fixed tool indices 0 to 3 and reads no filament sensor or `filament_detect` field anywhere in the offset calibration, and the macro purges `G1 E30` unconditionally (5 mm for soft filament). An empty head just turns its extruder over air. HelixScreen does the same. The heat target comes from the head's `print_task_config` filament record minus 20 C; how the firmware resolves an empty record there has not been checked on hardware.

`EXTRUDER_OFFSET_ACTION_CHECK_TARGET_TEMP` is also called by the stock screen; where in the sequence is not established, and the flow works without it.

Mock: the `snapmaker_u1` persona lists the hook macros, and `MoonrakerClientMock::simulate_nozzle_offset_calibration` answers each step after a beat, models the state gate, and moves the probed head's `extruder_offset`. `HELIX_MOCK_NOZZLE_CAL_PLATE_REFUSE=<n>` refuses the first n plate checks; `HELIX_MOCK_NOZZLE_CAL_FAIL=<n>` fails head n's probe.

### Fitted Nozzles

The U1's heads take swappable nozzles, and the firmware keeps each head's fitted nozzle outside `printer.cfg`: `PrinterExtruder.__init__` in `<klipper>/klippy/kinematics/extruder.py` loads a per-extruder file from the persistent Snapmaker config directory (`diameter`, `volume_type`; default 0.4 and `standard`) and lets it replace the config's `nozzle_diameter`. `get_status()` reports both as `nozzle_diameter` and `nozzle_volume_type` (`standard` or `high_flow`), while `configfile.settings` keeps the config's value, so a printer with a 0.2, two 0.4 High Flow and a 0.6 nozzle reads 0.4 four times there. The firmware's own check (`<klipper>/klippy/extras/print_task_config.py`, run only for the `SET_PRINT_TASK_PARAMETERS` Snapmaker's Orca writes) refuses a used head whose diameter differs (code 14) or whose file flow type differs while the head is not `high_flow` (code 19).

HelixScreen subscribes both extruder fields for every printer (stock Klipper has neither and answers null), and the fit check prefers them to the config's (`src/printer/print_start_inputs.cpp#current_fit_machine`, `fit_machine_with_live_nozzles()`); the plugin queries the same fields before it checks a slicer's upload-and-print. The file's flow types come from Orca's `; nozzle_volume_type = Standard,High Flow,...` config comment, else from the `filament_volume_type` list Snapmaker's Moonraker adds to the metadata, beside `nozzle_diameter_list`. File tool i is compared with head i; a Filament Mapping remap is not followed. The mock persona reports 0.4 mm `standard` on all four heads.

#### Changing a Head's Nozzle

The firmware takes a nozzle change through the webhook `control/nozzle_properties` (`_handle_control_nozzle_properties` in `<klipper>/klippy/kinematics/extruder.py`), which Moonraker proxies as `printer.control.nozzle_properties` with `{"extruder": <head>, "diameter": <mm>, "volume_type": "standard" | "high_flow"}`, either field optional. It refuses while `print_stats.state` is `printing` or `paused`, and accepts only the diameters 0.2, 0.4, 0.6 and 0.8 and the two flow types, compared exactly. A refusal is not a JSON-RPC error: the reply is a successful one whose result is `{"state": "error", "message": "..."}`, so `{"state": "success"}` is the only answer that means the change was made. Setting 0.2 mm on a head whose loaded filament the firmware will not print through that nozzle clears the head's `print_task_config` filament info (`_set_nozzle_diameter`, `FilamentParameters.is_allow_to_print` in `<klipper>/klippy/extras/filament_parameters.py`: a type starting `PA` or `PC`, TPU, PVA, PEBA, any `-CF` or `-GF` type, or the Glow, Marble and Wood sub-types).

The Multi-Filament settings show a **Toolhead** section (`toolhead`, described as "Nozzle size and flow type per head") once any head reports either field, with a **Nozzle diameter** and a **Nozzle flow** dropdown per head for each field that head reports (`src/printer/ams_backend_snapmaker.cpp#get_device_actions`). The rows are disabled with "Finish or cancel the print first" while a print is printing or paused. A pick sends only the field that differs from what the head reports, and a pick of the reported value sends nothing (`snapmaker::nozzle::change_params` in `src/printer/snapmaker_nozzle_properties.cpp`). The 0.2 mm option asks for confirmation first (`DeviceAction::with_option_confirm`). The reply is read by `snapmaker::nozzle::read_reply`; a refusal, an unrecognised reply or a failed send shows "Nozzle not changed: ..." and emits `EVENT_STATE_CHANGED`, so the open section redraws from what the head still reports. An accepted change moves the row when the firmware's extruder status update arrives.

Mock: `MoonrakerClientMock::set_nozzle_properties` keeps the firmware's checks and their order (a bad flow type is refused after a good diameter in the same call is already applied, as the firmware does in memory), stores the result per head, pushes it as an extruder status update and reports it in the objects query. It does not model the 0.2 mm filament-info reset. Other personas answer "Method not found".

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend_snapmaker.h` | Backend class, `ExtruderToolState` / `SnapmakerRfidInfo`, capability overrides, per-slot state arrays |
| `src/printer/ams_backend_snapmaker.cpp` | Full implementation: status parse, channel_state table, gcode, overrides, pre-print builder |
| `include/printer_discovery.h` | `filament_detect` detection + registration order (MMU wins over stock U1) |
| `include/snapmaker_resume.h` + `src/printer/snapmaker_resume.cpp` | Terminal-pause matchers (dirty bed vs runout) and coded-error `msg` extraction |
| `include/snapmaker_nozzle_offset.h` + `src/printer/snapmaker_nozzle_offset.cpp` | Toolhead offset calibration: capability check, step plan, `Runner` |
| `include/ui_snapmaker_nozzle_offset_overlay.h` + `src/ui/ui_snapmaker_nozzle_offset_overlay.cpp` | The guided flow's overlay, registered by `src/ui/ui_snapmaker_device_flows.cpp` |
| `include/batch_feed_reconcile.h` | Connect-time cleanup of a firmware batch left in the `doing` state |
| `include/snapmaker_print_preferences.h` | `PrintPreferences` read/write model for the firmware prefs surfaced as device actions, plus each head's `filament_soft` |
| `include/snapmaker_nozzle_properties.h` + `src/printer/snapmaker_nozzle_properties.cpp` | The Nozzle rows' options, the `nozzle_properties` params a pick sends, and the reading of its reply |
| `include/filament_temperature_source.h` + `src/printer/filament_temperature_source.cpp` | Per-filament load/unload temperature capture (see above) |
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
   Open checks for the filament record: does the official-flag repair's log line
   ("flagged official with no card behind it") appear at an idle start? It depends on
   the backend seeing `print_stats` in the initial status frame; without it the repair
   waits for the next print-state change. And a save held across a print should
   write once at `complete`/`standby`, not at a runout pause.
2. `is_stuck_motion_sensor_runout()` has no caller — revive when a verifiable
   "filament at the gear" signal exists (`src/printer/ams_backend_snapmaker.cpp`). Checked
   2026-08-21: the status model carries **no dedicated feeder/gear-presence field** -
   lane presence resolves from two signals: the `filament_feed` per-extruder
   `filament_detected` port sensor OR the `filament_feed.channel_state`
   loaded-at-toolhead latch (`load_finish` sets it; `unload_finish` /
   `wait_insert` / `preload_finish` clear it). `filament_detect.state` is the
   entrance/tag reading and reads 0 once filament is fed through, so it backs
   no presence claim; the per-tool motion sensor is a runout signal for the
   active tool, not lane presence. The gear-presence candidate is that same
   channel_state pair: `load_finish`
   (fed to nozzle) vs `preload_finish` (firmware assist stops short of the gear) -
   both already parsed into the channel-state machine
   (`src/printer/snapmaker_channel_state.cpp#classify_channel_state`). What is missing is rig
   confirmation that the state reliably means "filament at the gear" before the gate
   is revived.
3. End-to-end timing of the pre-print `SET_PRINT_USED_EXTRUDERS` is unverified live.
   Code-side ordering is established (see Pre-Print Remap above): the print-start
   request is issued only after the config gcode's JSON-RPC ack. Still open,
   firmware-side: whether the ack means the config is mutated and persists by the time
   the baked `PRINT_START` block runs — flagged in
   [Firmware API: `print_task_config`](#firmware-api-print_task_config) § "Still
   UNCERTAIN".
4. The `prepare_for_resume` doc comment in `include/ams_backend_snapmaker.h#AmsBackendSnapmaker`
   still describes the retired sensor-disable chain; the implementation drives
   `AUTO_FEEDING` (see Runout and Resume above). Comment is stale, code is right.
5. Batch load/unload (both script shapes, the dispatch-id END recovery, and
   `batch_feed_reconcile`) and the per-filament temperature capture are source-derived only -
   none of it has run on the rig yet.
6. The Nozzle rows are source-derived and mock-tested only: the reply shape through
   Moonraker's proxy, the refusal while printing, and the extruder status update after a
   change have not been checked on the rig.
7. The discovery subscription lists only `temperature`, `target`, `power`,
   `pressure_advance`, `nozzle_diameter` and `nozzle_volume_type` for each extruder
   (`src/api/moonraker_discovery_sequence.cpp`), so the toolchanger fields in the status
   table above (`state`, `park_pin`, `active_pin`, `activating_move`, `extruder_offset`,
   `switch_count`, `retry_count`, `error_count`) are not subscribed. Which frames still
   carry them has not been checked, and the subscription is not changed here.

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
logical-to-physical routing, and the live gcode preview colors each tool by the toolhead
that will actually print it. Three gating rules, all from observation on a real U1:

- The firmware restores identity when the print **completes** and when it is
  **cancelled** — both observed directly (idle after either: `extruders_used`
  `[F,F,F,F]`, `extruder_map_table[0:4]` `[0,1,2,3]`).
- Idle therefore reads `[0,1,2,3]`, which is **indistinguishable from "this print needs
  no remap"** and is flatly wrong for a file whose tools do not line up with the toolheads.
  Observed concretely: after cancelling a print whose correct routing was `[2,1,0,3]`,
  the table read identity. So the read is gated on `extruders_used` having at least one
  true — the firmware's own "a task is configured" signal — and answers "no opinion"
  otherwise.
- Power loss and a klippy crash mid-print are **not** covered by those observations. The
  gate above is what makes the read safe on those paths too: it does not depend on the
  reset happening, only on a task being configured.

**Reprint.** That same gate is what makes the table unreadable at the one moment a reprint
needs it: the print has ended, so `extruders_used` is all-false and the table has been reset
to identity. Nothing else on the reprint path can reconstruct the routing either - there is
no detail view, no swatch card and no picker, so there is no colour/type match to recompute.
`AmsBackendSnapmaker::last_print_tool_mapping()` is therefore a snapshot of the table taken
on every `print_task_config` frame that arrives with the gate **open**, i.e. the routing the
most recent print actually ran with. `PrintStartController::initiate_reprint()` runs it
through `FilamentMapper::reprint_remap()`, which answers `nullopt` when no configured task
was ever observed - and the reprint then sends **nothing** rather than defaulting, because an
empty remap makes `build_preprint_gcode()` write every used tool to its firmware-default head
and erase the crossover the print used. Provenance is the point: a value in the snapshot came
from a frame where the firmware said a task was configured, which is what `reprint_info`
below cannot say about any of its own entries.

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
- The values are stored in `<printer_data>/config/snapmaker/print_task.json` on the printer and survive a
  reboot. When a print ends, `reset_print_info()` clears `flow_calibrate`, `auto_bed_leveling`,
  `time_lapse_camera` and `end_unload_filament`, but **not** `shaper_calibrate`: a 1 written once
  stays stored for every print after it.

**Input shaping is never a pre-print option in HelixScreen**, and HelixScreen never sends
`SHAPER_CALIBRATE`. The firmware stores the field and keeps it across prints, but nothing on the
printer consumes it: no macro (`PRINT_START` included) and no klippy extra other than
the `print_task_config` module, which only sets it, reads `shaper_calibrate`. Bed leveling, flow
calibration and timelapse are the U1's pre-print toggles, each sending its own
`SET_PRINT_PREFERENCES` line whether on or off.

HelixScreen surfaces the filament-side subset of these prefs as device actions in the printer's
settings: `get_device_sections()` publishes one "Print Behavior" section (`print_prefs`) and
`get_device_actions()` one row per pref the firmware has actually reported
(`src/printer/ams_backend_snapmaker.cpp#get_device_actions`) - auto-replenish, replenish-ignores-colour,
tangle detect, tangle sensitivity (dropdown low/medium/high), end-of-print LED, and one
"Unload at end" toggle per toolhead, carrying its `tool_index` so the UI names it ("Unload at end - Toolhead 1 (T0)"). A pref with no reported value
renders no row at all: a toggle whose state is unknown would render off and invite "changing" it
to its own value. Toggles are written back through `build_preference_gcode()`, which emits
`SET_PRINT_PREFERENCES` with only the changed parameter (omitted parameters keep their firmware
value).

**Unload at end is HelixScreen's setting, not the firmware's.** The firmware clears
`end_unload_filament` when every print ends, and its echo of the list lags a tap behind, so
neither can be the record of the user's choice. The record is the per-printer setting
`ams/end_of_print_unload_tools`, a bitmask with bit N for tool N
(`include/settings_manager.h#get_end_of_print_unload_tools`). The toggles read it, and a toggle
writes it first and then sends the whole `END_UNLOAD_FILAMENT` list built from it
(`src/printer/ams_backend_snapmaker.cpp#do_execute_device_action`). While printing or paused the
firmware refuses that list without `FORCE=1`, which a toggle never passes, so the send is skipped and the
saved choice goes out with the next start. Every print start from this screen carries the list through
`build_preprint_gcode()` (above). A print some other client started gets it through
`standing_print_gcode()`, the same list with `FORCE=1`, sent just after the job starts by the
HelixPrint plugin's `print_hooks` feature or by the screen (`include/standing_print_options.h`).
That forced write mid-print has not been checked on hardware.

A head whose `filament_soft` is set (TPU and the like) is never unloaded: the firmware's
`SM_PRINT_END_AUTO_UNLOAD_FILAMENT` skips it, as the stock screen does. Its toggle is disabled
with "Soft filament is never unloaded", reads off whatever is saved, and refuses to be turned on.
The list still carries the saved bit for that head and leaves the skip to the firmware, so the
choice reads on again once the head holds rigid filament.

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
| `filament_official` | `bool[4]` | true = Snapmaker RFID spool, or any Extended Firmware `filament_detect/set` write (locks `SET_PRINT_FILAMENT_CONFIG` w/o FORCE) |
| `filament_sku`, `filament_soft`, `filament_edit` | per-head | SKU id; "soft" flag (the end-of-print unload skips that head); UI-editable flag |
| `flow_calibrate`, `flow_calib_extruders[4]`, `auto_bed_leveling`, `time_lapse_camera`, `shaper_calibrate` | prefs | task prefs |
| `auto_replenish_filament`, `replenish_ignore_color` | prefs | runout auto-replenish behaviour |
| `filament_entangle_detect`, `filament_entangle_sen` | prefs | tangle detection |
| `end_led_turn_off`, `end_unload_filament[4]` | prefs | end-of-print actions. `end_unload_filament` is reset when each print ends |
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
   official slots. The flag is also set by any Extended Firmware `filament_detect/set`
   write and never cleared by it, so tell a real tag by its nonzero `CARD_UID` (see
   "Commands the Backend Emits").
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
- **Exact persistence semantics of `SET_PRINT_EXTRUDER_MAP`** (in-memory only) versus
  whether the stock screen always follows it with a save — a save step is inferred but the
  gui's exact call order was not captured live.
- **Whether `reprint_info.extruder_map_table` is re-applied on a firmware-initiated
  reprint**, and whether it appears in the exported status object at all - no live payload
  carrying it has been captured. `SET_PRINT_EXTRUDER_MAP` writes that mirror as well as the
  live table, but where the mirror is consumed was not read. It is the obvious candidate for
  making HelixScreen's reprint routing survive an app restart (the in-app snapshot above does
  not), with one hazard to settle first: both writers only ever set entries and neither
  clears, so the mirror accumulates across prints and an entry carries no evidence of which
  print wrote it. `SET_PRINT_TASK_PARAMETERS` - the bulk path the stock screen and the cloud
  use - is not documented as writing the mirror at all, so a screen-started print would leave
  a previous job's entries in place looking exactly like fresh ones.
