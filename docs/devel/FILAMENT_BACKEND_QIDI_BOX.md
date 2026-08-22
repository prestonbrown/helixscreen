# QIDI Box Filament Backend

The QIDI Box is QIDI's RFID-aware multi-material system for the PLUS4, Q2, and MAX4:
4 slots per unit, chainable to 4 units (16 slots), with an active PTC dryer. Topology is
`PathTopology::HUB` - slots converge at a hub inside the Box, like FlashForge IFS or a
Bambu AMS, not a lane-selector MMU.

## QIDI Box (QIDI PLUS4 / Q2 / MAX4)

> **Status: shipped, field-validated only.** `AmsBackendQidi` is a factory-registered `AmsSubscriptionBackend` with a full read path (`save_variables` + per-box heater/humidity objects + the RFID profile list) and an implemented write path (load/unload/tool-change/eject/remap/dryer). No Box sits in the local test fleet: the gcode protocol was verified against Q2 1.1.1 stock firmware through field reports, every write op logs its gcode at info for field validation (#1030), and a few operations still return `not_supported` — `recover()`, `reset()`, `cancel()`, and both bypass entry points. The Max 4 box speaks a different dialect for eject/unload (#1083); see Lane Eject below.

The QIDI Box is QIDI's RFID-aware multi-material system: 4 slots per unit, chainable up to 4 units = 16 colors, with active drying and runout/tangle sensors. It is a **hub-style AMS** (like FlashForge IFS or Bambu AMS), not a lane-selector MMU — the closest in-tree analog is `AmsBackendAd5xIfs`, not Happy Hare or AFC.

### Compatible Hardware

| Printer | Supported | Notes |
|---------|-----------|-------|
| QIDI PLUS4 | Yes (per QIDI) | PLUS4 kit is not interchangeable with Q2/MAX4 — different hub board + data cable |
| QIDI Q2    | Yes (per QIDI) | Same kit as MAX4 |
| QIDI MAX4  | Yes (per QIDI) | Same kit as Q2 |
| Q1 Pro     | **No** | Unsupported by QIDI — different mainboard generation |
| X-Max 3    | **No** | Unsupported by QIDI — older MKSPI board |

The `assets/config/printer_database.json` entries for `qidi_plus_4`, `qidi_q2`, and `qidi_max_4` carry an `"ams_type": "qidi_box"` capability tag. The `qidi_max_4` entry's notes record the Max 4 control divergence (#1083).

### Detection

**Wired.** `PrinterDiscovery::parse_objects()` keys off the `box_stepper slot<N>` Klipper objects the Box's firmware registers — one per physical slot, 4 per box, 1-4 boxes chainable (`include/printer_discovery.h`). Presence of any `box_stepper slot*` object is the unambiguous detection signal, and the per-name count gives the physical slot count (`qidi_box_slot_count()`).

### Firmware Openness

QIDI printers (Q1 Pro and newer) run forks of Klipper and Moonraker from [QIDITECH/klipper](https://github.com/QIDITECH/klipper) and [QIDITECH/moonraker](https://github.com/QIDITECH/moonraker). SSH is open by default (`mks` / `makerbase`), and KIAUH is pre-installed. QIDI discourages upstream Klipper updates because their board requires their fork.

**The Box firmware itself ships as obfuscated `.so` Python extension modules.** A community open-source reimplementation at [qidi-community/Plus4-Wiki customisable_qidibox_firmware](https://github.com/qidi-community/Plus4-Wiki/tree/main/content/customisable_qidibox_firmware) replaces six modules (box_detect.py, box_rfid.py, box_stepper.py, box_extras.py, aht20_f.py, buttons_irq.py) with editable Python. Maintainers label it "strongly WIP." This repo is the primary protocol reference for a HelixScreen integrator.

### Control Surface

All control runs through Klipper gcode macros and `SAVE_VARIABLE` — **no dedicated Moonraker endpoints**, no REST extension. State lives in `save_variables` and printer objects, same shape as AD5X IFS.

**State the backend reads** (`parse_save_variables()` + `handle_status_update()`):

| Source | Keys | Meaning |
|--------|------|---------|
| `save_variables` | `box_count`, `enable_box` | Unit model: 0-4 boxes; connected flag |
| `save_variables` | `slot<N>` | Per-slot state word: 0=empty, 1/3=available, 2=loaded, negative=blocked |
| `save_variables` | `value_t<N>` = `"slot<M>"` | Tool N prints from slot M (the tool map, one direction only) |
| `save_variables` | `last_load_slot` (`"slot-1"` = nothing) | Aggregate loaded pointer |
| `save_variables` | `is_tool_change` | `AmsAction::LOADING` while the box is busy |
| `save_variables` | `filament_slot<N>` / `color_slot<N>` / `vendor_slot<N>` | RFID indices resolved via `officiall_filas_list.cfg` |
| `heater_generic heater_box<N>` | `temperature`, `target`, `power` | Per-box dryer heater |
| `aht20_f heater_box<N>` | `temperature`, `humidity` | Per-box environment |
| `box_extras` | `box_drying_state.box<N>.{dry_state, end_time}` | Drying countdown |
| `printer.configfile.settings` | `heater_generic ... max_temp`, `box_config ... target_max_temp_heater_generic`, `[force_move]`, `[multi_color_controller]` | Dryer ceiling, lane-eject gating, Max 4 dialect marker |

**G-code the backend emits:**

| Command | Used for |
|---------|----------|
| `EXTRUDER_LOAD SLOT=slot<N>` | Load (the backend manages hotend temp itself: `M109` → load → `CLEAR_NOZZLE` → `M104 S0`) |
| `M603 S<temp>` | Unload (verified stock on Q2 1.1.1); falls back to `M109` + `EXTRUDER_UNLOAD SLOT=…` |
| `FORCE_MOVE STEPPER="box_stepper slot<N>" VELOCITY=… DISTANCE=…` | Lane eject on Q2/Plus 4 (#1041), gated on `[force_move] enable_force_move`; distance/velocity user-tunable (defaults 878 mm / 100 mm/s) |
| `MULTI_COLOR_BOX_UNLOAD SLOT=slot<N>` | Lane eject + unload dialect on Max 4 (#1083) — the multi_color_controller rejects FORCE_MOVE |
| `SAVE_VARIABLE VARIABLE=value_t<N> VALUE="slot<M>"` | Tool remap (`RemapStrategy::Native`, the unified remap path) |
| `SAVE_VARIABLE VARIABLE={filament,color,vendor}_slot<N>` | Persisting slot metadata back to the Box's RFID indices |
| `ENABLE_BOX_DRY` / `DISABLE_BOX_DRY BOX=<n>` | Dryer start/stop when the box_extras timer is present |
| `SET_HEATER_TEMPERATURE HEATER=heater_box<N>` | Dryer start/stop fallback |

Firmware capabilities (`M603`, `CLEAR_NOZZLE` presence) are fingerprinted from the macro cache at startup; the 1.1.x → 01.01.02 QIDI refactor changes the macro surface, so the backend branches on capability rather than a version string. Slot metadata display comes from a bounded fetch of `officiall_filas_list.cfg` (`[fila<N>]` temperature profiles, `[colordict]` palette, `[vendor_list]`).

### Path Topology

```
  Slot 1 ──┐
  Slot 2 ──┤
            ├── Hub ── Toolhead
  Slot 3 ──┤
  Slot 4 ──┘
```

`PathTopology::HUB` — slots converge at a hub inside the Box before the toolhead. Chained boxes add units with their own hubs; the unit model follows `save_variables.box_count` (0-4 boxes, each contributing 4 slots), with per-box objects suffixed `…box<N>` (1-indexed).

### RFID

Spools identify via MIFARE Classic RFID tags. Data lives in sector 1 block 0. Third-party read/write tools exist:

- [TinkerBarn/BoxRFID](https://github.com/TinkerBarn/BoxRFID) — Electron desktop app
- [n0cloud/qidi-box-rfid-manager](https://github.com/n0cloud/qidi-box-rfid-manager) — mobile
- [LexyGuru/Qidi_RFID_App](https://github.com/LexyGuru/Qidi_RFID_App)

### Do NOT Confuse With

- **Happy Hare "QuattroBox"** — listed in Happy Hare's supported hardware, but it is an unrelated DIY MMU by [Batalhoti](https://github.com/Batalhoti/QuattroBox). Happy Hare does **not** support the QIDI Box.
- **The `"box"` string alias in `ams_type_from_string()`** — already claimed by `CFS` (Creality K2 "box" terminology). QIDI Box requires the explicit `"qidi_box"` / `"QIDI Box"` / `"qidibox"` spelling.

### Capabilities

| Feature | Supported | Notes |
|---------|-----------|-------|
| Endless Spool | `Unsupported` | No override of `get_endless_spool_capabilities()`; no Box-side auto-backup observed on the wire |
| Tool Mapping | Yes | `RemapStrategy::Native` — `set_tool_mapping()` rewrites `value_t<N>` via `SAVE_VARIABLE`; forward and reverse maps published from one `SlotRegistry` pass so they cannot drift |
| Per-slot loaded authority | Yes | `save_variables slot<N> == 2` is the Box's own per-slot statement; reconciled against the `last_load_slot` aggregate every parse pass (#1199) |
| Bypass Mode | No | `enable_bypass()`/`disable_bypass()` return `not_supported`; [the force override](FILAMENT_MANAGEMENT.md#bypass-visibility-and-the-force-override) shows the external spool for tracking only |
| Spoolman | Optional | Works through standard Moonraker `[spoolman]` |
| Auto-Heat on Load | Yes | The backend drives `EXTRUDER_LOAD` with its own heat → load → wipe → cool envelope (`supports_auto_heat_on_load() = true`); the UI must not run its own preheat |
| Dryer | Yes | Per-box PTC heater + aht20 humidity, 35-90°C (ceiling refined from configfile), up to 720 min, allowed during print (#1019). `ENABLE_BOX_DRY` when the box_extras timer is present, `SET_HEATER_TEMPERATURE` otherwise; countdown from `box_drying_state` end_time |
| Lane Eject | Yes (capability-gated) | `supports_lane_eject()` only when `[force_move]` is enabled or the Max 4 `multi_color_controller` dialect is detected (#1041, #1083) |
| Device Actions | Eject params | `supports_configurable_eject_params() = true` — user-tunable eject distance/velocity surface as device-ops slider rows |
| Error channel | Yes | A blocked slot (`slot<N>` negative) raises a sticky CRITICAL `ErrorEvent` with a dismiss-only affordance (#1172, #1041) |

Not implemented: `recover()` / `reset()` / `cancel()` (return `not_supported`), `clear_slot_override()` (logs a warning), bypass (above), and path visualization (`get_filament_segment()` returns `NONE`). `select_slot()` is deliberately `not_supported` — `load_filament()` is the only path. Filament ops gate at `FilamentOpGate::PrintActiveOnly`, a deliberate narrowing (the box reports `AmsAction::LOADING` itself).

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend_qidi.h` | Backend class declaration, capability overrides, RFID/fila-profile types |
| `src/printer/ams_backend_qidi.cpp` | Full implementation: save_variables parse, gcode builders, dryer, RFID reverse-lookups |
| `include/printer_discovery.h` | `box_stepper slot<N>` detection + slot count |
| `include/ams_types.h` | `AmsType::QIDI_BOX` enum + string converters |
| `tests/unit/test_ams_backend_qidi.cpp` | 102 cases: save_variables parsing, tool map, dryer, eject, remap, filas list |
| `tests/unit/test_ams_qidi_per_slot_loaded.cpp` | 7 cases: the #1199 per-slot loaded authority reconcile |

### Follow-up Work (in order)

1. Hardware validation of the write path — the load/unload/eject/remap gcode is field-verified only through logs (#1030); no Box in the local test fleet.
2. `recover()` / `reset()` / `cancel()` — still `not_supported`; blocked on knowing the stock recovery flow.
3. Bypass — unknown whether the Box has one; needs hardware inspection.
4. Add `assets/images/ams/qidi_box_64.png` (TODO comment exists at the top of the implementation).
5. Path visualization — segments return `NONE` today.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
