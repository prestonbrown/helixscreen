# ACE (Anycubic ACE Pro) Filament Backend

The Anycubic ACE Pro is a 4-slot dryer-equipped multi-material hub (8 slots in Rinkhals
"Combo" setups), surfaced through four different software stacks: native Anycubic
GoKlipper via Rinkhals (primary), community ValgACE/BunnyACE REST, the ACEPRO Klipper
driver (one unit per daisy-chained ACE), and multiACE on the Snapmaker U1 (driven by its
own backend, not this one). Topology is `PathTopology::HUB`; WebSocket on the native and
ACEPRO paths, REST fallback.

## ACE (Anycubic ACE Pro)

The ACE backend supports the Anycubic ACE Pro multi-material hub. The same hardware
shows up behind **several different software stacks**, and they expose *different*
Klipper/Moonraker interfaces. "ACE support" is therefore not one integration — it is
whichever of these the printer is running:

| # | Stack | Klipper / Moonraker surface | Transport | Audience | Backend status |
|---|-------|-----------------------------|-----------|----------|----------------|
| 1 | **Native Anycubic GoKlipper (via Rinkhals)** | `filament_hub` printer object (config `[ace]`) | WebSocket query/subscribe | **Primary real user base** — stock Kobra 3 / 3 V2 / 3 Max / S1 / S1 Max (Combo) flashed with [Rinkhals](https://github.com/jbatonnet/Rinkhals) | ✅ Handled (parses `filament_hub`) |
| 2 | **Community ValgACE / BunnyACE / DuckACE** | `ace` printer object + `ace_status.py` | `/server/ace/*` REST bridge | ACE Pro bolted onto a **non-Anycubic DIY printer** (niche; DuckACE abandoned) | ✅ Handled (REST fallback) |
| 3 | **ACEPRO driver** ([`Kobra-S1/ACEPRO`](https://github.com/Kobra-S1/ACEPRO), a Klipper extras module) | manager `ace` object (also registered as `ace_state`) + one `ace_instance_N` object per unit + `ace_status` Moonraker component | WebSocket query/subscribe + `/server/ace/*` REST | Kobra S1 / Kobra X / Kobra 3 family on vanilla Klipper or Kalico (often on an external Pi), one or more ACE Pro / ACE 2 units | ✅ Handled (object path: one unit per instance + manager `current_index`; REST: one unit, `/status` `ace_manager`) |
| 4 | **multiACE / SnapAce** ([`decay71/multiACE`](https://github.com/decay71/multiACE)) | `ace` printer object, but a **multi-unit** `aces[]` status shape | WebSocket only (no REST bridge) | **Snapmaker U1** with 1-4 ACE Pro / ACE Pro 2 units bolted on | ➡️ **Another backend** - recognised at detection; the Snapmaker U1 backend keeps the printer and `AmsBackendMultiAce` reads the `aces[]` shape ([FILAMENT_BACKEND_MULTIACE.md](FILAMENT_BACKEND_MULTIACE.md)) |

**How to think about the four:**

- **Path 1 (native)** is what almost every actual ACE user runs — it ships inside
  Anycubic's own GoKlipper firmware and is surfaced when the printer is reflashed with
  Rinkhals. This is the path the backend is built around.
- **Path 2 (community)** is ACE-on-a-DIY-rig: ValgACE (active), plus the BunnyACE/DuckACE
  forks (DuckACE abandoned). Integrates through Moonraker macros/endpoints rather than a
  native Klipper object.
- **Path 3 (ACEPRO)** is a Klipper extras driver, not a firmware fork: it runs on the
  Kobra S1 / Kobra X / Kobra 3 family under vanilla Klipper (or Kalico), drives one or
  more daisy-chained ACE Pro / ACE 2 units, and ships its own `ace_status.py` bridge
  beside its toolchange macros. Its live status surface is confirmed (user captures,
  #1069, captured on the `klipper-kobra-s1` firmware) and handled — see the Path 3
  section below. `ACE_CHANGE_TOOL` matches what the backend sends; its slot commands
  (`ACE_FEED`, `ACE_RETRACT`, `ACE_ENABLE/DISABLE_FEED_ASSIST`) need `INSTANCE=` beside
  `INDEX=` (see G-code Commands below). Full teardown:
  [`printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md`](printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md).
- **Path 4 (multiACE)** is ACE Pro hardware on a **Snapmaker U1**, and it is the one path
  the ACE backend is deliberately kept away from - see the section below.

> The sections below (`filament_hub` schema, REST endpoints, etc.) document Paths 1-3,
> which the backend handles today. Path 4 is documented in its own section; detection
> recognises it and hands the printer back to the U1 backend, so the schema work below
> does not apply to it.

### Path 4: multiACE (Snapmaker U1 + ACE Pro)

> **Not this backend's.** A U1 that gains multiACE keeps its Snapmaker backend and its
> four-toolhead UI, and `AmsBackendMultiAce` adds the ACE units upstream of the heads:
> [FILAMENT_BACKEND_MULTIACE.md](FILAMENT_BACKEND_MULTIACE.md). The detection rule below is
> what keeps `AmsBackendAce` away from it (prestonbrown/helixscreen#1426). Source read of
> the upstream repo (2026-09-01, v0.99.8b).

[multiACE](https://github.com/decay71/multiACE) hangs **1-4 Anycubic ACE Pro / ACE Pro 2
units off a Snapmaker U1** — the U1's four toolheads each get fed from an ACE slot through
a 1-to-N PTFE splitter, with the ACE units daisy-chained over USB. GPL-3.0, beta,
reverse-engineered, no custom printer firmware required (SSH root via `/oem/.debug`, then a
bash installer). Fork chain: [`BlackFrogKok/SnapAce`](https://github.com/BlackFrogKok/SnapAce)
→ [`decay71/multiACE`](https://github.com/decay71/multiACE) (upstream, the one to track) →
`physicsG/multiACE` (a stale personal fork — same project, do not cite it as the source).

**What it installs**, all under `klippy/extras/`:

| File | Role |
|------|------|
| ace.py (~14.5k lines) | The `[ace]` extra: multi-unit state, ~43 `ACE_*` G-code verbs, `get_status()` |
| ace_protocol{,_v1,_v2}.py | ACE Pro v1 / ACE Pro 2 serial dialects |
| ace_bg_swap.py, ace_tipform.py | Parked-position background swaps, tip forming (no cutter) |
| filament_feed_ace.py, filament_switch_sensor_ace.py, kinematics/extruder_ace.py | **Shadow replacements for the U1's own stock extras** — installed over filament_feed.py / filament_switch_sensor.py / extruder.py, with *_pre_multiace.py backups |

Config is `[ace]` (from `config/extended/ace.cfg`). The web UI at
`https://<printer>/multiace/` is multiACE's own FastAPI backend, **not** a Moonraker
component — so there is no `/server/ace/*` REST bridge on this path.

**The detection collision — this is the part that matters to us.** A U1 running multiACE
reports *both* marker objects. Left to the general precedence rule, they would resolve the
wrong way round:

1. `filament_detect` (stock U1) sets `has_snapmaker_` (`include/printer_discovery.h#parse_objects`).
2. `ace` (multiACE) sets `has_mmu_` + `mmu_type_ = ACE` (`include/printer_discovery.h#parse_objects`).
3. The registration chain tests `has_mmu_` **first**, which would leave `has_snapmaker_`
   unreached (`include/printer_discovery.h#parse_objects`) — by design, since a real aftermarket MMU should beat
   the U1 fallback. Here that design would fire on a stack we cannot actually read.
4. `AmsBackendAce` requires a **top-level non-empty `slots` array** to accept the
   object (`src/printer/ams_backend_ace.cpp#select_ace_object`). multiACE has none — its slots are nested
   one level down, per unit, under `aces[]`. So `select_ace_object()` would return null,
   the backend would log "no status data — trying REST bridge fallback"
   (`src/printer/ams_backend_ace.cpp#on_started`), and the REST bridge does not exist on a U1.

**What detection does about it.** `parse_objects()` resolves the collision before it
registers a backend: ACE plus `filament_detect` on one printer is multiACE, so it clears
`has_mmu_` and the chain falls through to the Snapmaker U1 backend
(`include/printer_discovery.h#parse_objects`). The ACE object names stay recorded - the
hardware really does carry them, and they reach the hardware fingerprint - but nothing
subscribes them once the type is no longer `ACE`.

The discriminator is co-presence rather than multiACE's own `aces[]`/`device_count` markers,
because `printer.objects.list` carries object names without status: the backend decision is
made at discovery step 3, and status content does not arrive until the subscribe at step 7
(the timeline in `include/moonraker_discovery_sequence.h`). Co-presence is sound for the
same reason it is cheap: `filament_detect` is published by U1 firmware alone, and no ACE
stack this chain matches runs on an unmodded U1. The yield is scoped to `AmsType::ACE`, so
a U1 carrying a filament system we *can* read - Happy Hare, AFC - still hands that system
the printer.

Net result: a multiACE U1 keeps the four-toolhead Snapmaker UI, and the factory builds
`AmsBackendMultiAce` for it, which reads the `aces[]` shape this backend cannot.

> **Do not confuse multiACE with the other U1 + ACE Pro mod.**
> [DnG-Crafts/U1-Ace](https://github.com/DnG-Crafts/U1-Ace) registers `ace_device`, which
> matches none of our ACE patterns — so `has_mmu_` stays false and the Snapmaker backend
> correctly keeps that printer (its bug was the unload path, #974, fixed in 0.99.72; see
> `src/printer/ams_backend_snapmaker.cpp#do_unload_filament`). multiACE registers plain `ace`, which is exactly what
> we match, and reaches the same Snapmaker backend only because detection then declines it.
> Same hardware category, same outcome by two different routes - worth checking which mod a
> U1 reporter actually has, because their ACE-side symptoms differ.

**Supporting it is schema work, and it is not started.** Detection only declines the
printer; reading multiACE would mean modelling the shape below.

**What its `ace.get_status()` actually publishes** (ace.py, `get_status()` — multi-unit,
head-centric, nothing like Path 1's flat single hub):

| Key | Meaning |
|-----|---------|
| `aces[]` | Per-unit array: `idx`, `connected`, `protocol`, `model`, `firmware`, `status`, `temp`, `humidity`, `dryer_status`, `gate_status`, `feed_assist`, `serial_path`, and the unit's own `slots[]` (`index`/`status`/`sku`/`material`/`subtype`/`rfid`/`brand`/`color`) |
| `device_count`, `active_device` | How many ACE units, which one is selected |
| `head_ace{}`, `head_source{}`, `head_manual{}`, `head_feeder{}` | **Head → ACE unit routing** — the U1 toolhead's filament source. This is the model our slot/tool abstractions would have to grow to represent it |
| `ace_head` / `ace_heads[]` | Which of the four U1 heads are currently ACE-fed vs stock-fed |
| `mode` | `normal` (stock U1 behaviour) vs multi — the user can toggle the whole system off |
| `swap_phase`, `swap_in_progress`, `last_swap_result`, `event_seq` | In-print swap state machine |
| `spools{}`, `spool_binding{}`, `spool_mode`, `spoollink*` | Spoolman / SpoolLink integration |

**Command surface.** ~43 verbs. Five overlap exactly with what we already emit on the
native path — `ACE_FEED`, `ACE_RETRACT`, `ACE_ENABLE_FEED_ASSIST`,
`ACE_DISABLE_FEED_ASSIST`, `ACE_START_DRYING` / `ACE_STOP_DRYING`. The rest are its own
vocabulary, and the central one has no analogue anywhere else in this doc:

- `ACE_SWAP_HEAD HEAD=<0..3> ACE=<0..3>` — mid-print swap of which ACE unit feeds a head.
- `ACE_LOAD_HEAD` / `ACE_UNLOAD_HEAD` / `ACE_UNLOAD_ALL_HEADS`, `ACE_CLEAR_HEADS`.
- `ACE_SET_HEAD_ACE` / `ACE_SET_HEAD_FEEDER` / `ACE_SET_HEAD_MANUAL` — routing config.
- `ACE_SWITCH`, `ACE_PRELOAD`, `ACE_DRY`, `ACE_SET_AUTO_DRY`, `ACE_LIST`, `ACE_HEAD_STATUS`.
- Note `ACE_CHANGE_TOOL` — the verb our backend drives Path 1 with — is **absent**. Tool
  changes stay the U1's own `T<n>`; multiACE only changes what is *behind* a head.

Also worth knowing: a Fluidd macro layer (`ACEA__Switch_*`, `ACEB__Load_*`, `ACEC__Unload_*`,
`ACED__Dry_*`, `ACEF__Mode_*`, `ACEG__Status`) wraps those verbs, so a real installation
shows a large alphabetised macro list — the same "macro soup" tell as AFC and Happy Hare.

**If we ever support it**, the topology is neither `HUB` nor the U1's `PARALLEL`: four
parallel toolheads, each with a *switchable* upstream source among N hubs. That is closer to
a per-lane multiplexer than to anything currently modelled — see
[FILAMENT_BACKEND_SNAPMAKER_U1.md](FILAMENT_BACKEND_SNAPMAKER_U1.md) for the stock model it
replaces.

### Path 3: the ACEPRO driver

Confirmed from a live rig (#1069: Kobra-S1/ACEPRO driver @ c89fe17 on Kalico, 1x ACE Pro,
fw `V1.3.856`) — real captures — with the multi-unit surface read from the driver's source
(its `extras/ace` manager and instance modules). The defining property: **the surface
is split in two.** The top-level `ace` Klipper object is a *manager* with no slots (the
driver registers the same object a second time as `ace_state`); each unit publishes its
own `ace_instance_N` object, a separate Klipper object, so a notify frame carries any
subset of them; and **no slot is ever reported "loaded"** — the manager's `current_index`
is the only seat signal.

**Manager — `ace.get_status()`:**

| Field | Meaning |
|-------|---------|
| `current_index` | **The loaded-tool signal.** The driver's tool number across every unit (`tool = instance*4 + local_slot`; each slot also publishes its own `tool`); `-1` = nothing loaded. This is the fourth and last explicit seat signal `src/printer/ams_backend_ace.cpp#parse_ace_object` arbitrates, via `seat_from_tool_locked`, which finds the slot by the `tool` each slot publishes |
| `target_index` | Tool number being changed *toward* mid-swap (`-1` when idle). While it names an unseated tool it is `pending_target_slot` and an idle hub reads as a driver-raised action: LOADING from an empty head; swapping out a seated tool, UNLOADING (path on the outgoing lane) until `rdm_sensor` clears, then LOADING on the target lane. The sensor is made again by the incoming strand, so a per-swap latch, not the sensor alone, holds the phase (`src/printer/ams_backend_ace.cpp#apply_target_index_locked`). |
| `ace_instances` | Unit count |
| `endless_spool_enabled`, `endless_spool_match_mode` | Endless-spool config (`false` / `"exact"` captured) |
| `ace_pro_enabled` | Master switch; presence is the capability |
| `toolhead_sensor`, `rdm_sensor` | Path sensors (toolhead, hub). With nothing seated, or during a driver-started swap, they place the strand: toolhead made = `TOOLHEAD`, hub only = `OUTPUT` (`#get_filament_segment`). A seated tool outside a swap answers `NOZZLE` once `target_index` is `-1` or the toolhead sensor is made; while the driver still targets it short of the toolhead (a retried feed keeps `target_index` on the current tool) the sensors place the strand. The driver persists `target_index` and can leave it latched on a loaded tool, which is why the toolhead sensor alone ends the retry. A seated tool with both sensors clear is a feed the driver paused on: `current_tool`/`current_slot` keep naming it, but it is published as not loaded and its slot is not stamped LOADED (`#path_empty_under_seat_locked`) |

**Unit — `ace_instance_N.get_status()`** (the captured rig exposes `ace_instance_0`; an ACE 2 publishes the same keys plus the ones marked ACE 2):

| Field | Meaning |
|-------|---------|
| `status` | Unit state string (`"ready"` captured); same loading/unloading/error vocabulary the backend maps to `AmsAction` |
| `dryer_status` / `dryer` | The dryer, `{status, target_temp, duration, remain_time}` — same nested shape as Path 1's `dryer`. A unit that has answered a status poll states it as `dryer_status`; before its first answer the driver's default carries `dryer`. Both spellings are accepted (`src/printer/ace_slot_parse.cpp#apply_dryer_state`) |
| `humidity` (ACE 2) | The unit's humidity sensor; an ACE Pro states none. Kept on the unit's environment through frames that omit it |
| `status_code` (ACE 2), `slots[].status_code` / `slots[].status_detail` | Device fault detail; not read |
| `temp` | Ambient/unit temperature (top-level; feeds `DryerInfo.current_temp_c`) |
| `slots[4]` | Per-slot inventory; each entry carries `index`, **`tool`** (its own global tool index), `status` (**inventory only** — `"ready"`/`"empty"`, never `"loaded"`), `color` (`[r,g,b]`), **`material`** (not `type`; the backend reads `material` first with `type` as the ValgACE fallback), `temp` (print temp), `rfid` |
| `slots[].park_state` | `"unknown"` / `"parked"` / `"loaded"`, not persisted: every slot reads `"unknown"` after a driver restart. Parsed on both the object path and `/server/ace/slots` (`src/printer/ams_backend_ace.cpp#note_park_state_locked`). An absent field or any other string is `unknown`, which draws as before. An idle slot (not the seat, `target_index` `-1`, action idle) with filament present and `park_state` `"parked"` draws its strand at `OUTPUT`, the segment the hub sensor uses (`#slot_parked_at_hub_locked`). `"parked"` covers an RDM-verified park just short of the hub and a fixed-length toolhead-only unload that need not reach it; the driver does not tell them apart yet, so both draw at the hub. `"loaded"` adds nothing: the seat and the sensors place that strand. The sensors, the seat and the toolchange phase rules always win |
| `instance` | N, the unit's place in the chain |
| `protocol` | `"ace1_json"` (ACE Pro) or `"ace2_proto"` (ACE 2) |
| `model`, `firmware`, `boot_firmware`, `usb_port`, `usb_path` | Identity/telemetry (`"Anycubic Color Engine Pro"` / `"V1.3.856"` captured). An ACE 2 whose device reports no model gets the driver's own label (`"ACE2 (Shared Bus)"` or `"ACE2 (<port>)"`) |
| `connection_state` | `connected`, or `initializing` / `connecting` / `reconnecting` / `disabled`; read as the unit's link |
| `feed_assist_slot` | The unit-local slot the driver is feed-assisting, `-1` for none. The driver turns it on at the end of a load and keeps it on through a print. The Feed Assist toggle shows it for the loaded slot (`src/printer/ams_backend_ace.cpp#feed_assist_action_locked`), carries no value until the unit has stated it, and is disabled with nothing loaded, where `ACE_ENABLE/DISABLE_FEED_ASSIST` would have no slot to mean |

**How the backend reads it** (`src/printer/ams_backend_ace.cpp#on_started`,
`#handle_status`): the bootstrap queries every `ace_instance_N` discovery found
(`ace_instance_0`-`3` when discovery lists none) and commits to the object path when one
carries slots (`select_ace_object`). Every `ace_instance_N` is then unit N, parsed in
ascending N, and a manager-shaped `ace` riding the same query response or notify frame
is parsed after them — slots land first, the seat stamps onto them. Any
non-empty `ace` without slots counts as the manager, since its deltas name `current_index`
only when the seat moved (`#manager_ace_object`). A
manager-only notify frame (e.g. `current_index` flipping to `-1` on a TR) is parsed on its
own and clears the seat. Notify frames carry per-object deltas, so a frame holds only
the units that changed, each applied to its own unit; on the other drivers a frame with
no slots array at all falls back to the first non-empty object rather than being dropped.

**Units** (`ace_instances` of them; observed with an ACE Pro and an ACE 2). The driver
numbers its instances in USB daisy-chain order, ACE Pro units first and ACE 2 units on the
shared bus after them, and `ace_instance_N` is unit N here, its slots numbered globally
after the units before it (`#index_units_locked`). A delta for one instance updates that
unit only, looked up by its number (`#ace_instance_objects`, numeric order, instances
below `kMaxUnits`). Only an instance that carries `slots`, or the bootstrap's pick, puts
the backend on this path: a slotless delta on a rig the REST bridge serves is ignored. A
unit named before the ones below it have reported leaves them as four disconnected
placeholder slots, so numbering never shifts; a placeholder no instance has reported is
`absent` and takes no dryer or slot command, since no instance answers to its
`INSTANCE=`. The bootstrap answer names every unit there is, so units it does not name
are dropped (`#keep_units`), as on a restart onto a shorter chain or another driver.
A unit created after the manager's last sensor frame takes the sensor readings already
in hand. Every ACEPRO lane names the `extruder` and the hub `ace`: all units feed one
toolhead through the one hub `rdm_sensor` watches.

- **Names** (`#apply_unit_identities_locked`): a unit shows the short product name for
  its `protocol`, `ACE Pro` (`ace1_json`) or `ACE Pro 2` (`ace2_proto`; anything else
  reads `ACE Pro`), numbered `#2`, `#3` when a name repeats. Product names, so not
  translated. The `model` string stays the unit's `name`, never its display name, and the
  protocol words never reach the screen. Model, protocol and
  firmware are kept per unit, apart from the units, so identity a frame states before the
  unit's slots arrive is not lost; each unit carries its own `firmware_version`, and the
  system version is unit 0's.
- **Tools**: a slot's tool is the `tool` it publishes, taken only as an integer below
  `kMaxUnits * kSlotsPerUnit` (`src/printer/ace_slot_parse.cpp#read_slot_tool`) and kept
  as the slot's `mapped_tool`, which `sync_tool_map_from_slots` publishes in both
  directions (`#slot_for_tool_locked`, `#tool_for_slot_locked` read it). `ACE_CHANGE_TOOL
  TOOL=` sends that number. A slot the
  driver has named no tool for refuses a load. A `current_index` naming a tool whose unit
  has not reported is stated with no slot marked, and every instance frame resolves the
  seat again, so it lands once that unit's slots arrive.
- **Status**: each unit's `status` word is kept apart, so one unit reading `ready` does
  not end another's `loading` (`#set_unit_action_locked`), and an IDLE resolved from them
  (`busy` or `ready` while the driver feeds) leaves a load sent from the screen LOADING
  until its own ack; `connection_state` sets the unit's link.
- **Dryer**: each unit keeps its own dryer and environment, and the dryer commands name
  it: `ACE_START_DRYING INSTANCE=N TEMP= DURATION=` and `ACE_STOP_DRYING INSTANCE=N`
  (`#dryer_instance_argument`). **Without `INSTANCE=` the driver acts on every unit**, so
  the backend never omits it once it has seen this driver. The limits are the ACE Pro
  defaults (`src/printer/ace_slot_parse.cpp#ace_pro_dryer_info`): the driver checks each
  unit against its configured `max_dryer_temperature` (60 by default) but does not
  publish it.

**REST surface** (`ace_status.py`; used when no slot-bearing object answers the initial
query). **The REST path shows one unit only**: instance 0, at unit 0. The backend polls
`/status?instance=0` and `/slots?instance=0` (`#kRestStatusPath`; the ValgACE bridge
ignores the argument), reads each REST slot's `tool` as the object path does, and names
the instance `/status` reports serving in its slot and dryer commands
(`#ace_instance_for_unit_locked`). A chain with no instance 0 answers those polls with HTTP 200 and a
result carrying only `error`; that counts as a failed poll (`#rest_poll_answered`): the
inventory stands, and with no data ever the missing-bridge error surfaces.

**Pushed by the HelixPrint plugin.** Where the plugin's `vendor_ace` feature is ready
(`moonraker-plugin/helix_vendor_ace.py`), the plugin calls the same two endpoints in-process
and broadcasts their answers as `notify_helix_vendor_state` (vendor `ace`, payload
`{"status": ..., "slots": ...}`, `error` answers included) only when they changed. The
plugin reports the feature ready only after a read returned data, and unavailable again
after three failed reads in a row. While it pushes, the polling thread drops to a 30 s
backstop (`#rest_data_poll_due`, `kVendorPushBackstop`), and `#on_vendor_state` hands each
answer where a poll's would go: data to `#apply_rest_status` / `#apply_rest_slots`, a status
error to `#note_rest_status_failure` (the failure count and the missing-bridge toast).
Status frames are the tick that re-reads the plugin's state (`#refresh_vendor_push`): a
probe on the current connection that finds the feature ready owes one poll of both
endpoints, and anything else (no probe since a reconnect, the feature gone) puts the thread
back on its 500 ms cadence. `on_started` / `on_stopping` reset the tracker. The rule is `vendor_state_source()` (`include/vendor_state_push.h`),
shared with the AD5X IFS backend; the plugin side is in
[HELIXPRINT_PLUGIN.md](HELIXPRINT_PLUGIN.md).

| Endpoint | Behaviour |
|----------|-----------|
| `GET /server/ace/status[?instance=N]` | One instance's `get_status()` verbatim **plus** an envelope: `instance_index` (the instance served), `instances[]` (every unit's get_status), `ace_manager` (the manager fields — **`current_index` seats here**, `#parse_status_response`), `ace_instance_count`. With `?instance=N` it serves instance N. **Without it the bridge picks the instance whose number equals `current_index`** (a tool number, not an instance), else instance 0, so the served unit would change with the seat, which is why the backend always asks for instance 0. The top-level `status` field is the action alias for the bridge's `action` |
| `GET /server/ace/slots[?instance=N]` | `{slots: [...]}` — the slots of the instance `/status` would serve for the same arguments, each with its `tool`; `material` key confirmed live |
| `GET /server/ace/info` | **404 — this driver's bridge has no such endpoint.** The backend treats /info as optional and identifies the hardware from /status's model + firmware |
| `POST /server/ace/command` | Passes `ACE_*` gcode through — `ACE_CHANGE_TOOL TOOL=`, `ACE_START_DRYING [INSTANCE=] TEMP= [DURATION=]`, `ACE_STOP_DRYING [INSTANCE=]` are source-confirmed, matching what the backend sends |

### History

The ACE backend was originally written **blind for ValgACE** (keying on a Klipper object literally named `ace`) and never matched a real Anycubic ACE hub — so Combo printers on Rinkhals got no AMS backend detected at all. Fixed **2026-06-13** to detect `filament_hub` first. The native object name was confirmed in Anycubic GoKlipper `extras_ace.go` and Rinkhals mmu_ace.py. The native `ACE_*` G-code verbs turned out to be exactly what the backend was already sending (ValgACE mirrored them), so the fix was a detection + status-parsing change, not a command-dialect rewrite.

### Detection

ACE is detected in two ways:

1. **Object list detection**: `filament_hub` (native Anycubic/Rinkhals), `ace` (community drivers, and the ACEPRO manager) **or** any `ace_instance_N` (ACEPRO units) in `printer.objects.list`.
2. **REST probe fallback**: A probe to `/server/ace/info` via `AmsState::probe_ace()` catches **community** setups where the object list is unavailable. (The native path never needs this — `filament_hub` is always in `objects.list`.)

### Native `filament_hub` Status Schema

The native GoKlipper `filament_hub.get_status()` is **flat and single-hub** (one hub, 4 slots). Multi-unit "Combo" configurations (8 slots) are a Rinkhals-layer abstraction stacked above this single-hub GoKlipper object.

| Field | Type | Meaning |
|-------|------|---------|
| `status` | string | Overall hub status |
| `dryer.status` | string | Dryer running/idle |
| `dryer.target_temp` | int | Dryer target temperature |
| `dryer.duration` | int | Configured drying duration |
| `dryer.remain_time` | int | Remaining drying time |
| `temp` | int | Hub temperature |
| `slots[]` | array | Per-slot state (4 entries) |
| `slots[].index` | int | Slot index |
| `slots[].status` | string | `empty` / `ready` / `preload` / `running` / `runout` |
| `slots[].sku` | string | Filament SKU |
| `slots[].type` | string | Filament material type |
| `slots[].color` | `[r, g, b]` | Slot color |
| `current_filament` | string | Loaded slot as `"<unitId>-<localIndex>"` (e.g. `"0-2"`); empty/absent = nothing loaded |

### G-code Commands (native `ACE_*`)

These are the real native verbs from GoKlipper `extras_ace.go` — the backend drives the native path with exactly these:

| Command | Action |
|---------|--------|
| `ACE_CHANGE_TOOL TOOL={n}` | Load slot (or `-1` to unload) |
| `ACE_FEED INDEX={i} LENGTH={mm} SPEED={s}` | Feed filament from a slot |
| `ACE_RETRACT INDEX={i} LENGTH={mm} SPEED={s}` | Retract filament to a slot |
| `ACE_ENABLE_FEED_ASSIST INDEX={i}` | Enable feed assist on a slot |
| `ACE_DISABLE_FEED_ASSIST INDEX={i}` | Disable feed assist on a slot |
| `ACE_START_DRYING TEMP={t} DURATION={m}` | Start drying |
| `ACE_STOP_DRYING` | Stop drying |

> The ACEPRO driver resolves the unit for `ACE_FEED`, `ACE_RETRACT` and
> `ACE_ENABLE/DISABLE_FEED_ASSIST` (and `ACE_SET_SLOT` and the other slot verbs) from
> `T=<tool>` or `INSTANCE=<unit> INDEX=<local slot>`, and rejects `INDEX=` alone ("Must
> specify either T=<tool> or INSTANCE=<inst> INDEX=<slot>", prestonbrown/helixscreen#1821).
> Once the backend has seen an `ace_instance_N` object or the REST bridge's `ace_manager`,
> `src/printer/ams_backend_ace.cpp#slot_command` sends the `INSTANCE=` form with the slot's
> own unit's instance and its local index (on the REST path, the instance `/status`
> serves); GoKlipper and ValgACE keep `INDEX=`. With nothing seated the commands address
> the first slot of the first unit present, as slot 0 is on every driver; a slot on an
> `absent` placeholder refuses them. A seated tool whose unit has not reported
> its slots refuses them, since slot 0 would feed a second strand into its path.

> Note: `ACE_RECOVER` and `ACE_RESET` are **not** native GoKlipper commands — do not send them on the native path.

### REST Endpoints (community fallback only)

These belong to ValgACE's Moonraker component (`ace_status.py`) and are used **only** on the community fallback path; the native Rinkhals deployment never uses them. BunnyACE/DuckACE users must install ValgACE's `ace_status.py` separately to get this bridge.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/server/ace/info` | GET | System information (model, version, slot count) |
| `/server/ace/status` | GET | Current state (dryer, loaded slot, action) |
| `/server/ace/slots` | GET | Slot information (colors, materials, status) |

### Threading

- **Native path (`filament_hub`):** WebSocket query + subscription. State is held under `mutex_`; updates arriving on the WebSocket background thread are deferred to the main thread via `token.defer(...)` (L081-safe — never mutate UI state directly from the WS callback).
- **Community fallback path (`ace`):** a background polling thread runs at ~500ms intervals when the backend is active, caching state under mutex protection. While the HelixPrint plugin pushes the bridge's state, the thread keeps running but polls `/status` and `/slots` only every 30 s (`VendorStateTracker::pushing()` is atomic); pushes arrive on the WebSocket thread and are deferred to the main thread like every other callback.

### Tag reading

`tag_reading()` keeps a bay's last frame whose `rfid` says identified (`true`, or `2` in
the integer form): material, colour, and `temp_min`/`temp_max` when the frame carries them.
A finished read that found no tag (`false`, or `1`) or an emptied bay drops it; `0` and
`3` are a read not finished yet and leave it standing. Both parse paths, the subscription
and the REST bridge, record it (`src/printer/ams_backend_ace.cpp#note_tag_frame_locked`).

### Slot edits

Where an edit goes depends on the driver:

- **ACEPRO** (an `ace_instance_N` object, or the REST bridge's `ace_manager`): the driver keeps
  each slot's colour, material and temperature in its inventory, so `apply_user_edit()` writes
  them with `ACE_SET_SLOT INSTANCE=<unit> INDEX=<local> COLOR=R,G,B MATERIAL="<m>" TEMP=<t>`
  (all three required by the driver; TEMP is the edit's nozzle midpoint, else the material's
  default, 1-300), and the lane shows the edit as Pending until the hub restates it. An edit
  naming no material is refused through the editor's inline error, except one that also clears
  the colour, which is `ACE_SET_SLOT ... EMPTY=1`: Clear Spool's cleared edit sends that, once
  (`clear_slot_override()` is the lane half); the slot's presence comes back from the hardware
  on the next heartbeat. The lane record is saved once the driver has run the write, and one it
  rejects saves nothing. The driver's single per-slot `temp` is read as the slot's
  temperature, so an edit that does not move it re-sends what the hub holds. The hub's colour
  and material outrank the lane record. Parameters read from the driver source
  (Kobra-S1/ACEPRO, `cmd_ACE_SET_SLOT` in its ace commands module, commit 92ecc1ca); not run on hardware.
- **Every other driver** (GoKlipper's `filament_hub`, ValgACE/BunnyACE `ace`, the REST bridge
  without `ace_manager`): nothing writes a slot, so the lane record is the edit. The colour,
  material and brand the record states stand over the hub's memory
  (`src/printer/ams_backend_ace.cpp#file_hub_reading_locked`, `helix::ace::yield_to_record`);
  a different spool going in clears the record and the hub's reading shows again.

Either way the lane record (`lane_data`) carries the rest: brand, spool, weights, temperatures.
A write that does not reach the printer refuses the edit and saves nothing. The REST bridge also reports `temp_min`/`temp_max`;
they stand unless a user edit moved the slot's temperatures this session. The stored
record keeps a nozzle temperature for every edit, the hub's own included, so it cannot
say which were the user's: after a restart the hub's temperatures apply again
(`src/printer/ams_backend_ace.cpp#apply_hub_temps_locked`).

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | `Unsupported` | No override; inherits the base default |
| Tool Mapping | No | Fixed 1:1 mapping |
| Bypass Mode | No | `enable_bypass()` returns `not_supported`; [the force override](FILAMENT_MANAGEMENT.md#bypass-visibility-and-the-force-override) shows the external spool for tracking only |
| Spoolman | No | -- |
| Auto-Heat on Load | No | -- |
| Dryer | Yes | Built-in hardware dryer |

### Dryer Control

ACE is the primary backend with integrated dryer support. The `DryerInfo` struct provides:

- Current/target temperature
- Duration and remaining time
- Fan speed control
- Hardware capability limits (min/max temp, max duration)

Drying presets are derived from the filament database via `get_default_drying_presets()`.

On the native path, live dryer state (status, target temp, duration, remaining time) is parsed directly from `filament_hub.dryer`. On the ACEPRO driver each unit has its own dryer, addressed by `INSTANCE=` (Path 3 above).

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
