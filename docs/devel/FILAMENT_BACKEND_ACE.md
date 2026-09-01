# ACE (Anycubic ACE Pro) Filament Backend

The Anycubic ACE Pro is a 4-slot dryer-equipped multi-material hub (8 slots in Rinkhals
"Combo" setups), surfaced through four different software stacks: native Anycubic
GoKlipper via Rinkhals (primary), community ValgACE/BunnyACE REST, the unverified
Kobra-S1 fork, and multiACE on the Snapmaker U1 (misdetected today). Topology is
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
| 3 | **Mainline-Python Kobra-S1 fork** (`github.com/Kobra-S1/klipper-kobra-s1`) | custom `[ace]` extra + `[ace_status]` Moonraker component | **unconfirmed** (`ace_status.py` JSON/REST) | KS1 users replacing KobraOS with mainline Klipper (often on an external Pi) | ❓ **Unverified** — status surface not yet inspected |
| 4 | **multiACE / SnapAce** ([`decay71/multiACE`](https://github.com/decay71/multiACE)) | `ace` printer object, but a **multi-unit** `aces[]` status shape | WebSocket only (no REST bridge) | **Snapmaker U1** with 1-4 ACE Pro / ACE Pro 2 units bolted on | ⚠️ **Misdetects** — steals the U1 backend, then parses nothing |

**How to think about the four:**

- **Path 1 (native)** is what almost every actual ACE user runs — it ships inside
  Anycubic's own GoKlipper firmware and is surfaced when the printer is reflashed with
  Rinkhals. This is the path the backend is built around.
- **Path 2 (community)** is ACE-on-a-DIY-rig: ValgACE (active), plus the BunnyACE/DuckACE
  forks (DuckACE abandoned). Integrates through Moonraker macros/endpoints rather than a
  native Klipper object.
- **Path 3 (KS1 fork)** is newly observed in real logs (2026-06-24) and **not yet
  validated against our backend.** It is a *full Klipper firmware fork* for the Kobra S1
  — related to the Path 2 driver concept (it too ships an `ace_status.py`) but wrapped in
  KS1-specific cutter/purge/toolchange macros. Whether its `[ace_status]` surface matches
  Path 1's `filament_hub`, Path 2's `ace`/REST, or neither is an **open question**.
  Control gcode (`ACE_CHANGE_TOOL`, `ACE_ENABLE/DISABLE_FEED_ASSIST`) does match what the
  backend already sends. Full teardown:
  [`printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md`](printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md).
- **Path 4 (multiACE)** is ACE Pro hardware on a **Snapmaker U1**, and it is the one path
  that actively *breaks* an otherwise-working printer for us — see the section below.

> The sections below (`filament_hub` schema, REST endpoints, etc.) document Paths 1 and 2,
> which the backend handles today. Path 3's status schema is still TBD — see the linked
> log-analysis doc for the open items needed to confirm or extend coverage. Path 4 is
> documented in its own section immediately below; it needs a detection fix before any of
> the schema work matters.

### Path 4: multiACE (Snapmaker U1 + ACE Pro)

> **Not handled. Actively harmful today** — a U1 that gains multiACE *loses* its working
> Snapmaker backend and gets an ACE backend that parses nothing
> (prestonbrown/helixscreen#1426). No hardware seen; this is a source read of the upstream
> repo (2026-09-01, v0.99.8b).

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
reports *both* marker objects, and our chain resolves them the wrong way round:

1. `filament_detect` (stock U1) sets `has_snapmaker_` (`include/printer_discovery.h:440`).
2. `ace` (multiACE) sets `has_mmu_` + `mmu_type_ = ACE` (`include/printer_discovery.h:333`).
3. `has_mmu_` is checked **first**, so `has_snapmaker_` never runs
   (`include/printer_discovery.h:642`) — by design, since a real aftermarket MMU should beat
   the U1 fallback. Here that design fires on a stack we cannot actually read.
4. `AmsBackendAce` then requires a **top-level non-empty `slots` array** to accept the
   object (`src/printer/ams_backend_ace.cpp:980`). multiACE has none — its slots are nested
   one level down, per unit, under `aces[]`. So `select_slot_bearing_object()` returns null,
   the backend logs "no status data — trying REST bridge fallback"
   (`src/printer/ams_backend_ace.cpp:141`), and the REST bridge does not exist on a U1.

Net result: the ACE backend attaches and stays empty, and the Snapmaker U1 backend — which
would have worked for the four toolheads — is suppressed.

> **Do not confuse multiACE with the other U1 + ACE Pro mod.**
> [DnG-Crafts/U1-Ace](https://github.com/DnG-Crafts/U1-Ace) registers `ace_device`, which
> matches none of our ACE patterns — so `has_mmu_` stays false and the Snapmaker backend
> correctly keeps that printer (its bug was the unload path, #974, fixed in 0.99.72; see
> `ams_backend_snapmaker.cpp:475`). multiACE registers plain `ace`, which is exactly what
> we match. Same hardware category, opposite outcome — worth checking which mod a U1
> reporter actually has.

**The fix is detection-side, not schema-side:** an `ace` object carrying `aces[]`/`device_count` on a printer that also
reports `filament_detect` is multiACE, and until the shape is supported it should leave the
U1 backend in place rather than claim the printer.

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
