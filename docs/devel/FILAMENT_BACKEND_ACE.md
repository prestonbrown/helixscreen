# ACE (Anycubic ACE Pro) Filament Backend

The Anycubic ACE Pro is a 4-slot dryer-equipped multi-material hub (8 slots in Rinkhals
"Combo" setups), surfaced through three different software stacks: native Anycubic
GoKlipper via Rinkhals (primary), community ValgACE/BunnyACE REST, and the unverified
Kobra-S1 fork. Topology is `PathTopology::HUB`; WebSocket on the native path, REST fallback.

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

**How to think about the three:**

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

> The sections below (`filament_hub` schema, REST endpoints, etc.) document Paths 1 and 2,
> which the backend handles today. Path 3's status schema is still TBD — see the linked
> log-analysis doc for the open items needed to confirm or extend coverage.

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
