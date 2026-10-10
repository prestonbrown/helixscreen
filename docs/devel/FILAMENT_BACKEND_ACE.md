# ACE (Anycubic ACE Pro) Filament Backend

The Anycubic ACE Pro is a 4-slot dryer-equipped multi-material hub (8 slots in Rinkhals
"Combo" setups), surfaced through four different software stacks: native Anycubic
GoKlipper via Rinkhals (primary), community ValgACE/BunnyACE REST, the unverified
Kobra-S1 fork, and multiACE on the Snapmaker U1 (detected, deliberately declined). Topology is
`PathTopology::HUB`; WebSocket on the native path, REST fallback.

## ACE (Anycubic ACE Pro)

The ACE backend supports the Anycubic ACE Pro multi-material hub. The same hardware
shows up behind **several different software stacks**, and they expose *different*
Klipper/Moonraker interfaces. "ACE support" is therefore not one integration — it is
whichever of these the printer is running:

| # | Stack | Klipper / Moonraker surface | Transport | Audience | Backend status |
|---|-------|-----------------------------|-----------|----------|----------------|
| 1 | **Native Anycubic GoKlipper (via Rinkhals)** | `filament_hub` printer object (config `[ace]`) | WebSocket query/subscribe | **Primary real user base** — stock Kobra 3 / 3 V2 / 3 Max / S1 / S1 Max (Combo) flashed with [Rinkhals](https://github.com/jbatonnet/Rinkhals) | ✅ Handled (parses `filament_hub`) |
| 2 | **Community ValgACE / BunnyACE / DuckACE** | `ace` printer object + `ace_status.py` | `/server/ace/*` REST bridge | ACE Pro bolted onto a **non-Anycubic DIY printer** (niche; DuckACE abandoned) | ✅ Handled (REST fallback) |
| 3 | **Mainline-Python Kobra-S1 fork** (`github.com/Kobra-S1/klipper-kobra-s1`) | manager `ace` object + per-unit `ace_instance_N` objects + `ace_status` Moonraker component | WebSocket query/subscribe + `/server/ace/*` REST | KS1 users replacing KobraOS with mainline Klipper (often on an external Pi) | ✅ Handled (object path: instance slots + manager `current_index`; REST: `/status` `ace_manager`) |
| 4 | **multiACE / SnapAce** ([`decay71/multiACE`](https://github.com/decay71/multiACE)) | `ace` printer object, but a **multi-unit** `aces[]` status shape | WebSocket only (no REST bridge) | **Snapmaker U1** with 1-4 ACE Pro / ACE Pro 2 units bolted on | ⚠️ **Declined** - recognised at detection, and the Snapmaker U1 backend keeps the printer. The `aces[]` shape is unread |

**How to think about the four:**

- **Path 1 (native)** is what almost every actual ACE user runs — it ships inside
  Anycubic's own GoKlipper firmware and is surfaced when the printer is reflashed with
  Rinkhals. This is the path the backend is built around.
- **Path 2 (community)** is ACE-on-a-DIY-rig: ValgACE (active), plus the BunnyACE/DuckACE
  forks (DuckACE abandoned). Integrates through Moonraker macros/endpoints rather than a
  native Klipper object.
- **Path 3 (KS1 fork)** is a *full Klipper firmware fork* for the Kobra S1
  — related to the Path 2 driver concept (it too ships an `ace_status.py`) but wrapped in
  KS1-specific cutter/purge/toolchange macros. Its live status surface is confirmed
  (user captures, #1069; the "ACEPRO" driver at `Kobra-S1/ACEPRO` on Kalico) and handled —
  see the Path 3 section below. Control gcode (`ACE_CHANGE_TOOL`,
  `ACE_ENABLE/DISABLE_FEED_ASSIST`) matches what the backend already sends. Full teardown:
  [`printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md`](printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md).
- **Path 4 (multiACE)** is ACE Pro hardware on a **Snapmaker U1**, and it is the one path
  the ACE backend is deliberately kept away from - see the section below.

> The sections below (`filament_hub` schema, REST endpoints, etc.) document Paths 1-3,
> which the backend handles today. Path 4 is documented in its own section; detection
> recognises it and hands the printer back to the U1 backend, so the schema work below
> does not apply to it.

### Path 4: multiACE (Snapmaker U1 + ACE Pro)

> **Recognised, not driven.** A U1 that gains multiACE keeps its Snapmaker backend and its
> four-toolhead UI; what it loses is the ACE-specific affordances, because the `aces[]` slot
> shape is not modelled (prestonbrown/helixscreen#1426). No hardware seen; this is a source
> read of the upstream repo (2026-09-01, v0.99.8b).

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

Net result: a multiACE U1 keeps the four-toolhead Snapmaker UI, and loses only the
ACE-specific affordances.

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

### Path 3: Kobra S1 mainline-Python / ACEPRO

Confirmed from a live rig (#1069: Kobra-S1/ACEPRO driver @ c89fe17 on Kalico, 1x ACE Pro,
fw `V1.3.856`) — real captures, not a source read. The defining property: **the surface is
split in two.** The top-level `ace` Klipper object is a *manager* with no slots; each unit
publishes its own `ace_instance_N` object; and **no slot is ever reported "loaded"** — the
manager's `current_index` is the only seat signal.

**Manager — `ace.get_status()`:**

| Field | Meaning |
|-------|---------|
| `current_index` | **The loaded-tool signal.** Global tool index across every unit (`tool = instance*4 + local_slot` for 4-slot units); `-1` = nothing loaded. This is the fourth and last explicit seat signal `src/printer/ams_backend_ace.cpp#parse_ace_object` arbitrates, via `seat_from_global_index_locked` |
| `target_index` | Tool being changed *toward* mid-swap (`-1` when idle). While it names an unseated tool it is `pending_target_slot` and an idle hub reads as a driver-raised action: LOADING from an empty head; swapping out a seated tool, UNLOADING (path on the outgoing lane) until `rdm_sensor` clears, then LOADING on the target lane. The sensor is made again by the incoming strand, so a per-swap latch, not the sensor alone, holds the phase (`src/printer/ams_backend_ace.cpp#apply_target_index_locked`) |
| `ace_instances` | Unit count (1 on the captured rig) |
| `endless_spool_enabled`, `endless_spool_match_mode` | Endless-spool config (`false` / `"exact"` captured) |
| `ace_pro_enabled` | Master switch; presence is the capability |
| `toolhead_sensor`, `rdm_sensor` | Path sensors (toolhead, hub). With nothing seated, or during a driver-started swap, they place the strand: toolhead made = `TOOLHEAD`, hub only = `OUTPUT` (`#get_filament_segment`). A seated tool outside a swap answers `NOZZLE` once `target_index` is `-1` or the toolhead sensor is made; while the driver still targets it short of the toolhead (a retried feed keeps `target_index` on the current tool) the sensors place the strand. The driver persists `target_index` and can leave it latched on a loaded tool, which is why the toolhead sensor alone ends the retry. A seated tool with both sensors clear is a feed the driver paused on: `current_tool`/`current_slot` keep naming it, but it is published as not loaded and its slot is not stamped LOADED (`#path_empty_under_seat_locked`) |

**Unit — `ace_instance_N.get_status()`** (the captured rig exposes `ace_instance_0`):

| Field | Meaning |
|-------|---------|
| `status` | Unit state string (`"ready"` captured); same loading/unloading/error vocabulary the backend maps to `AmsAction` |
| `dryer_status` | The dryer, under the `dryer_status` key (not `dryer`): `{status, target_temp, duration, remain_time}` — same nested shape as Path 1's `dryer`. Both spellings are accepted by `src/printer/ams_backend_ace.cpp#apply_dryer_state_locked` |
| `temp` | Ambient/unit temperature (top-level; feeds `DryerInfo.current_temp_c`) |
| `slots[4]` | Per-slot inventory; each entry carries `index`, **`tool`** (its own global tool index), `status` (**inventory only** — `"ready"`/`"empty"`, never `"loaded"`), `color` (`[r,g,b]`), **`material`** (not `type`; the backend reads `material` first with `type` as the ValgACE fallback), `temp` (print temp), `rfid` |
| `model`, `firmware`, `boot_firmware`, `protocol` (`"ace1_json"`), `usb_port`, `usb_path`, `connection_state` | Identity/telemetry (`"Anycubic Color Engine Pro"` / `"V1.3.856"` captured) |

**How the backend reads it** (`src/printer/ams_backend_ace.cpp#on_started`,
`#handle_status`): the slot-bearing `ace_instance_N` object is parsed first
(`select_ace_object`), then a manager-shaped `ace` riding the same query response
or notify frame is parsed after it — slots land first, the seat stamps onto them. Any
non-empty `ace` without slots counts as the manager, since its deltas name `current_index`
only when the seat moved (`#manager_ace_object`). A
manager-only notify frame (e.g. `current_index` flipping to `-1` on a TR) is parsed on its
own and clears the seat. Notify frames carry per-object deltas: a frame carrying any
`ace_instance_N` key resolves to the lowest such instance (the one the display is
anchored to), and a frame with no slots array at all falls back to the first non-empty
object rather than being dropped.

**Multi-unit rigs** (`ace_instances > 1`, unobserved — every capture so far has one unit):
the backend displays the lowest instance only. A `current_index` beyond that unit's slots
seats the tool and marks no slot, and the active-material display falls back to the
external-spool profile while printing from an undisplayed unit's lane.

**REST surface** (`ace_status.py`; used when no slot-bearing object answers the initial
query):

| Endpoint | Behaviour |
|----------|-----------|
| `GET /server/ace/status` | `ace_instance_0.get_status()` verbatim **plus** an envelope: `instance_index`, `instances[]` (per-unit get_status), `ace_manager` (the manager fields — **`current_index` seats here**, `#parse_status_response`), `ace_instance_count`. The top-level `status` field is the action alias for the bridge's `action` |
| `GET /server/ace/slots` | `{slots: [...]}` — same slot shape as the instance, `material` key confirmed live |
| `GET /server/ace/info` | **404 — the endpoint does not exist on this fork.** The backend treats /info as optional and identifies the hardware from /status's model + firmware |
| `POST /server/ace/command` | Passes `ACE_*` gcode through — `ACE_CHANGE_TOOL TOOL=`, `ACE_START_DRYING TEMP= DURATION=`, `ACE_STOP_DRYING` are source-confirmed, matching what the backend already sends |

### History

The ACE backend was originally written **blind for ValgACE** (keying on a Klipper object literally named `ace`) and never matched a real Anycubic ACE hub — so Combo printers on Rinkhals got no AMS backend detected at all. Fixed **2026-06-13** to detect `filament_hub` first. The native object name was confirmed in Anycubic GoKlipper `extras_ace.go` and Rinkhals mmu_ace.py. The native `ACE_*` G-code verbs turned out to be exactly what the backend was already sending (ValgACE mirrored them), so the fix was a detection + status-parsing change, not a command-dialect rewrite.

### Detection

ACE is detected in two ways:

1. **Object list detection**: `filament_hub` (native Anycubic/Rinkhals) **or** `ace` (community drivers) in `printer.objects.list`.
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
- **Community fallback path (`ace`):** a background polling thread runs at ~500ms intervals when the backend is active, caching state under mutex protection.

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

On the native path, live dryer state (status, target temp, duration, remaining time) is parsed directly from `filament_hub.dryer`.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
