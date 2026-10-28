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

QIDI printers (Q1 Pro and newer) run forks of Klipper and Moonraker from [QIDITECH/klipper](https://github.com/QIDITECH/klipper) and [QIDITECH/moonraker](https://github.com/QIDITECH/moonraker). SSH is open by default, with a login that differs by model (`mks` / `makerbase` on the Q2, `qidi` / `qiditech` on the Max 4; full table in [QIDI_SUPPORT.md#ssh-logins](printers/QIDI_SUPPORT.md#ssh-logins)), and KIAUH is pre-installed. QIDI discourages upstream Klipper updates because their board requires their fork.

**The Box firmware itself ships as obfuscated `.so` Python extension modules.** A community open-source reimplementation at [qidi-community/Plus4-Wiki customisable_qidibox_firmware](https://github.com/qidi-community/Plus4-Wiki/tree/main/content/customisable_qidibox_firmware) replaces six modules (box_detect.py, box_rfid.py, box_stepper.py, box_extras.py, aht20_f.py, buttons_irq.py) with editable Python. Maintainers label it "strongly WIP." This repo is the primary protocol reference for a HelixScreen integrator.

### Control Surface

All control runs through Klipper gcode macros and `SAVE_VARIABLE` — **no dedicated Moonraker endpoints**, no REST extension. State lives in `save_variables` and printer objects, same shape as AD5X IFS.

**State the backend reads** (`parse_save_variables()` + `handle_status()`):

| Source | Keys | Meaning |
|--------|------|---------|
| `save_variables` | `box_count`, `enable_box` | Unit model: 0-4 boxes; connected flag |
| `save_variables` | `slot<N>` | Per-slot state word: 0=empty, 1/3=available, 2=loaded, negative=blocked |
| `save_variables` | `value_t<N>` = `"slot<M>"` | Tool N prints from slot M (the tool map, one direction only) |
| `save_variables` | `last_load_slot` (`"slot-1"` = nothing) | Aggregate loaded pointer |
| `save_variables` | `is_tool_change` | `AmsAction::LOADING` while the box is busy |
| `save_variables` | `filament_slot<N>` / `color_slot<N>` / `vendor_slot<N>` | RFID row indices resolved via `officiall_filas_list.cfg`; a quoted `VALUE="5"` (Qidi Studio's spelling) parses the same as a bare int. Box firmware 02.03.01.21 and later may save a string product id (`'QFA90'`) and a colour list (`['228332']`) instead (§ String product ids) |
| `multi_color_controller` (field `box` only) | `box<B>_filament_info_<I>_{filament_type, vendor_str, min_temp, max_temp}` | The firmware's own resolution of each slot (Box B+1, slot I); the only source for a string product id. Updates carry only the keys that changed |
| `heater_generic heater_box<N>` | `temperature`, `target`, `power` | Per-box dryer heater |
| `aht20_f heater_box<N>` | `temperature`, `humidity` | Per-box environment |
| `box_extras` | `box_drying_state.box<N>.{dry_state, end_time}` | Drying countdown |
| `printer.configfile.settings` | `heater_generic ... max_temp`, `box_config ... target_max_temp_heater_generic`, `[force_move]`, `[multi_color_controller]` | Dryer ceiling, lane-eject gating, controller dialect marker (Max 4 and Q2 01.01.02.x) |

**G-code the backend emits:**

| Command | Used for |
|---------|----------|
| `MULTI_COLOR_LOAD SLOT=slot<N>` | Load on the controller dialect (Max 4, Q2 01.01.02.x): the controller heats, swaps out a seated slot, feeds and wipes, which is what the 01.01.02 stock screen sends |
| `MULTI_COLOR_UNLOAD SLOT=slot<N>` | Unload on the controller dialect |
| `EXTRUDER_LOAD SLOT=slot<N>` | Load without the controller (Q2 1.1.x, Plus 4): the backend manages hotend temp itself: `M109` → load → `CLEAR_NOZZLE` → `M104 S0` |
| `M603 S<temp>` + `ENABLE_ALL_SENSOR` | Unload without the controller (verified stock on Q2 1.1.1). `M603` switches the filament sensor off and nothing else turns it back on before `PRINT_START`, so the backend re-enables it (`filament_macros::restore_after`); falls back to `M109` + `EXTRUDER_UNLOAD SLOT=…` |
| `FORCE_MOVE STEPPER="box_stepper slot<N>" VELOCITY=… DISTANCE=…` | Lane eject on Q2/Plus 4 (#1041), gated on `[force_move] enable_force_move`; distance/velocity user-tunable (defaults 878 mm / 100 mm/s) |
| `MULTI_COLOR_BOX_UNLOAD SLOT=slot<N>` | Lane eject on the controller dialect (#1083): the multi_color_controller rejects FORCE_MOVE |
| `SAVE_VARIABLE VARIABLE=value_t<N> VALUE="slot<M>"` | Tool remap (`RemapStrategy::Native`, the unified remap path) |
| `SAVE_VARIABLE VARIABLE={filament,color,vendor}_slot<N>` | Persisting slot metadata back to the Box's RFID indices, and the metadata clear (`VALUE=0` x3, see Slot Metadata below) |
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

The Box saves what it read off a tag as `filament_slot<N>` / `color_slot<N>` /
`vendor_slot<N>` ids, and `tag_reading()` is those ids resolved through the Box tables
(material, colour, brand) plus the filament profile's nozzle range. HelixScreen's own edit
writes the same variables, so the reading is folded frame by frame
(`helix::ams::fold_tag_frame`): fields the own-write echo guard withholds keep the tag's
value. After a restart the saved ids are the user's and become the reading, and an id set
from the printer's own screen cannot be told from a tag read
(`src/printer/ams_backend_qidi.cpp#parse_save_variables`, `src/printer/ams_tag_reading.cpp#fold_tag_frame_into`).

### Slot Metadata, Swap Detection and the Metadata Clear

Slot identity is the three save_variables row indices `filament_slot<N>` /
`color_slot<N>` / `vendor_slot<N>`, resolved against the Box's own tables in
`officiall_filas_list.cfg` (`[fila<N>]` profiles, `[colordict]` palette,
`[vendor_list]` names). `read_slot_id()` accepts a bare int or a quoted
`VALUE="5"` - Qidi Studio emits the quoted spelling, and Klipper's
save_variables module evaluates the literal before saving, so it lands as a
string - and rejects negatives in either spelling. Zero IS an id: it is the
Box stating the field is unset, and the parse blanks the painted field so the
last reading does not linger.

The Box states no tag UID, so the spool fingerprint is the composite
`fila|color|vendor` (all three ids <= 0 is the empty string, which the tracker
reads as no signal). It runs through the shared `SlotFingerprintTracker`
([FILAMENT_SLOT_METADATA.md](FILAMENT_SLOT_METADATA.md) § Swap fingerprints):
a change no write of ours asked for is a physical spool swap and clears the
user's override. Each field is a separate `SAVE_VARIABLE`, so a poll can land
between writes - `apply_user_edit()` and the clear arm every old/new
cross-product composite as an expected echo before dispatching, and a dispatch
that fails entirely releases only its own claims. The record's persisted
fingerprint is the baseline at startup, so a swap made while HelixScreen was
down clears the stale override on the first frame.

`clear_slot_override()` clears locally (erase + `clear_async` against the
shared `lane_data` namespace) and then erases what the firmware remembers:
three `SAVE_VARIABLE VARIABLE={filament,color,vendor}_slot<N> VALUE=0` writes,
sent mid-print too because `SAVE_VARIABLE` moves nothing. Table row ids start
at 1, so 0 names no row and the slot reads as no identity - the clear erases what the slot remembers,
not what the hardware can read, and a tagged spool re-populates the ids on its
next insert, boot or RFID read. Vendor is the one field where 0 is a real row
(`vendor_list` row 0 = Generic), so user edits write vendor 0 legitimately;
the clear's zeros still read as no identity because all three fields land on
0 together.

### String product ids

Box firmware 02.03.01.21 (shipped with Q2 01.01.03.01, and seen under Plus 5
printer firmware 01.01.02.01) saves what its own screen picks in a second form:
`filament_slot<N> = 'QFA90'`, a product id shaped like Bambu Studio's `GF*` codes
with a `QF` prefix, and `color_slot<N> = ['228332']`, a list of RGB strings with
no `#`. `vendor_slot<N>` is left as it was. Numeric writes still work, and every
write HelixScreen makes stays numeric.

- A colour list paints its RGB directly; the palette is not consulted.
- A product id names no `officiall_filas_list.cfg` row. Material, nozzle range
  and brand come from `multi_color_controller`'s `box` field instead, folded per
  slot as deltas arrive; controller info that lands after the ids repaints the
  slot from the last ids stated. Info naming a different product id is the
  previous spool's and paints nothing. The stale `vendor_slot<N>` is ignored for
  such a slot.
- The swap fingerprint takes either form per component (`QFA90|#228332`,
  `11|2`, `QFA90|2`), so a spool change between product ids clears an override
  and our own numeric write over a product id echoes as our write
  (`src/printer/ams_backend_qidi.cpp#expect_own_write_echoes_locked`). A product
  id decodes to the material the controller paired it with, so the same spool
  restated by product id instead of row id is still the same spool. A pairing
  is learned only from a controller update carrying both the id and the type,
  and a product id no update has described yet defers the swap judgment until
  one does. This rests on the controller's `filament_type` spelling a material
  the way `officiall_filas_list.cfg`'s `type` does (compared ignoring case),
  which no Box of ours has confirmed.
- Only the shape the firmware accepts reads as a product id: two to four
  capital letters, then two or three digits. Anything else is ignored and the
  slot keeps its last ids.
- A numeric id keeps the Box tables as its source even when controller info is
  present.

Field names come from QIDI Studio's `QDSDeviceManager.cpp` and runtime
observation on a Max 4 (01.01.07.01). Not yet seen on our own Box hardware.

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
| Slot Overrides | Yes | Store-backed through the shared `lane_data` namespace (`backend_id` `qidi`); spool-swap detection via the fingerprint tracker (Slot Metadata below) |
| Auto-Heat on Load | Yes | The backend drives `EXTRUDER_LOAD` with its own heat → load → wipe → cool envelope (`supports_auto_heat_on_load() = true`); the UI must not run its own preheat |
| Dryer | Yes | Per-box PTC heater + aht20 humidity, 35-90°C (ceiling refined from configfile), up to 720 min, allowed during print (#1019). `ENABLE_BOX_DRY` when the box_extras timer is present, `SET_HEATER_TEMPERATURE` otherwise; countdown from `box_drying_state` end_time |
| Lane Eject | Yes (capability-gated) | `supports_lane_eject()` only when `[force_move]` is enabled or the Max 4 `multi_color_controller` dialect is detected (#1041, #1083) |
| Device Actions | Eject params | `get_device_actions()` publishes the user-tunable eject distance and speed as two sliders in Filament Control |
| Error channel | Yes | A blocked slot (`slot<N>` negative) raises a sticky CRITICAL `ErrorEvent` with a dismiss-only affordance (#1172, #1041) |

Not implemented: `recover()` / `reset()` / `cancel()` (return `not_supported`), bypass (above), and path visualization (`get_filament_segment()` returns `NONE`). `clear_slot_override()` IS implemented - see Slot Metadata below. `select_slot()` is deliberately `not_supported` — `load_filament()` is the only path. Filament ops gate at `FilamentOpGate::PrintActiveOnly`, a deliberate narrowing (the box reports `AmsAction::LOADING` itself).

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend_qidi.h` | Backend class declaration, capability overrides, RFID/fila-profile types |
| `src/printer/ams_backend_qidi.cpp` | Full implementation: save_variables parse, gcode builders, dryer, RFID reverse-lookups |
| `include/printer_discovery.h` | `box_stepper slot<N>` detection + slot count |
| `include/ams_types.h` | `AmsType::QIDI_BOX` enum + string converters |
| `tests/unit/test_ams_backend_qidi.cpp` | 140 cases: save_variables parsing, tool map, dryer, eject, remap, filas list, fingerprint swap detection, metadata clear |
| `tests/unit/test_ams_qidi_per_slot_loaded.cpp` | 7 cases: the #1199 per-slot loaded authority reconcile |

### Follow-up Work (in order)

1. Hardware validation of the write path — the load/unload/eject/remap gcode is field-verified only through logs (#1030); no Box in the local test fleet.
2. `recover()` / `reset()` / `cancel()` — still `not_supported`; blocked on knowing the stock recovery flow.
3. Bypass — unknown whether the Box has one; needs hardware inspection.
4. Path visualization — segments return `NONE` today.
5. The external spool with a box attached: the stock screen loads it as slot 16 (`MULTI_COLOR_LOAD SLOT=slot16`, or `M604`, whose box branch retracts box filament first with `E_LOAD SLOT=16`); the backend offers no external-spool target, so with a box it cannot be loaded from here. Needs a box to verify.
6. `multi_color_controller.operation.is_waiting_user`: the 01.01.02 stock screen answers it with a confirm popup that sends `USER_CONFIRM_CONTINUE`. Nothing here watches it, so a box operation that stops for the user waits until it is answered from another client. Needs a box to verify.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
