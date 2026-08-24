# QIDI Box Heater — Reverse Engineering Reference

Developer reference for the QIDI Box's PTC filament-drying heater. Documents Klipper object schema, G-code command surfaces, firmware variants, and HelixScreen's integration points. Written to preserve findings that required significant research so the next developer doesn't start from scratch.

**See also**: [FILAMENT_MANAGEMENT.md § Dryer / Box-Heater Control](FILAMENT_MANAGEMENT.md#dryer--box-heater-control)

---

## Klipper Object Layout

### Heater

The drying chamber is exposed as a standard Klipper `heater_generic` object:

```
heater_generic heater_box<N>
```

where `N` is typically `1` (first or only Box unit). A second chained unit would be `heater_box2`, and so on.

**Status fields** (readable via `printer.objects.query`):

| Field | Type | Description |
|-------|------|-------------|
| `temperature` | float | Current chamber temperature (°C) |
| `target` | float | Target setpoint (0 = heater off) |
| `power` | float | Heater PWM duty cycle (0.0–1.0) |

Because this is a standard `heater_generic`, the full Klipper heater safety system applies — the firmware will fault and shut down if temperature limits are exceeded.

### Humidity and Temperature Sensors

The Box units ship with an AHT20 combo sensor:

```
aht20_f heater_box<N>
```

**Status fields**:

| Field | Type | Description |
|-------|------|-------------|
| `temperature` | float | Chamber ambient temperature (°C) |
| `humidity` | float | Relative humidity (%) |

Additional backup thermistors are present in some firmware builds:

```
temperature_sensor heater_temp_a_box1
temperature_sensor heater_temp_b_box1
```

These are read-only sensors used for safety monitoring. HelixScreen reads humidity and chamber temp from the `aht20_f` object as part of `DryerInfo`.

### Drying State

The active drying session is tracked in two places — both carry the same data; HelixScreen reads `box_extras` as primary and falls back to `multi_color_controller` if needed:

**Primary** (via `box_extras` Klipper plugin):

```
box_extras.box_drying_state.box<N>
```

**Mirror** (via `multi_color_controller` plugin):

```
multi_color_controller.drying.box<N>
```

**Fields** (identical in both):

| Field | Type | Description |
|-------|------|-------------|
| `dry_state` | int | `1` = drying active, `0` = idle |
| `end_time` | int | Unix epoch (seconds) when the drying session ends |

There is **no native "remaining minutes" field**. Compute remaining time on the client side:

```cpp
int remaining_minutes = static_cast<int>((end_time - now_epoch()) / 60);
```

### Detection

QIDI Box presence is detected by the existence of `box_stepper slot<N>` objects in `printer.objects.list`:

```
box_stepper slot1
box_stepper slot2
box_stepper slot3
box_stepper slot4
```

Presence of `box_stepper slot*` → `AmsType::QIDI_BOX`.

**Important — QIDI Q2 has two heaters**: A QIDI Q2 has both a printer chamber heater (typically `heater_generic chamber`) and a Box drying heater (`heater_generic heater_box1`). Only `heater_box<N>` is the Box dryer. Do not confuse or merge these.

---

## Temperature Limits

There are three distinct temperature ceilings, each enforced at a different layer:

| Tier | Value | Enforcement |
|------|-------|-------------|
| Hardware safety ceiling | **100 °C** | Klipper firmware — heater faults and shuts down above this |
| Settable target ceiling | **90 °C** | `target_max_temp_heater_generic` config key — Klipper rejects `SET_HEATER_TEMPERATURE` calls above this |
| Per-material recommended | **~45–60 °C** | QIDI `drying.conf` per-material table — soft guidance only, not enforced |

### Config Key Variants

The settable target ceiling appears under different key names depending on firmware lineage:

| Firmware | Config section | Key |
|----------|---------------|-----|
| Official QIDI firmware | `[heater_generic heater_box1]` | `max_temp` |
| Community firmware | `[box_config box0]` | `target_max_temp_heater_generic` |

Both are readable via `printer.configfile.settings`. HelixScreen queries both spellings and takes whichever is non-zero, defaulting to 90 °C if neither is present.

### Per-Material Drying Table

The QIDI `drying.conf` defines per-material drying parameters. Columns as shipped:

| Column | Description | Typical values |
|--------|-------------|---------------|
| Type | Filament material name | PLA, PETG, ABS, TPU, PA, … |
| Bed temp | Bed temperature during drying | — |
| Chamber temp | Target drying temperature | 55–60 °C |
| Time | Drying duration | 720 min (12 h) default |

HelixScreen surfaces these as `DryingPreset` entries via `get_drying_presets()` so users can select a material and get sensible defaults without manual entry.

---

## G-code Command Surface

The Box heater exposes multiple command layers. Use them in preference order:

### 1. Standard Klipper heater_generic (always available)

```gcode
SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=<temp>
SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=0    ; stop
```

Works on all firmware variants because `heater_box1` is a standard `heater_generic`. The heater holds the temperature indefinitely — there is no firmware-managed timer when using this command.

### 2. Vendor drying session with firmware timer (box_extras.so)

```gcode
ENABLE_BOX_DRY BOX=<n> TEMP=<temp> END_TIME=<hours>
DISABLE_BOX_DRY BOX=<n>
```

Preferred when the `box_extras` plugin is present. The firmware manages the session timer, updates `box_drying_state.end_time`, and clears `dry_state` when the session ends. `END_TIME` is a duration in hours (e.g., `END_TIME=4` for a 4-hour session).

### 3. Vendor multi-color controller commands

```gcode
MULTI_COLOR_SET_TEMP              ; set box temperature
MULTI_COLOR_DISABLE_HEATER        ; stop heater
MULTI_COLOR_DRY                   ; start drying session
```

Available when the `multi_color_controller` plugin is loaded. These are a higher-level wrapper around the box_extras primitives. Less preferred because parameter syntax is less well-documented.

### 4. Community wrappers (not on stock firmware)

```gcode
TLTG_SET_BOX_TEMP BOX=<n> TARGET=<temp>
OPTIMIZED_DISABLE_BOX_HEATER
```

Present only when the community open-source firmware replacement is installed (see thelegendtubaguy repo below). Do not rely on these being available on user hardware.

---

## Filament Operations (Stock Firmware) — verified from a Q2 + Box config

Source: [`elliotboney/my_qidi_2`](https://github.com/elliotboney/my_qidi_2) `printer_data/config/box1.cfg` — a real **stock** QIDI Q2 + QidiBox install (Camden-Winder's exact hardware class, #1022). The per-tool macros are the entry points the slicer and any UI use:

```ini
[gcode_macro T0]
gcode:
    {% set slot = printer.save_variables.variables.value_t0|default('slot0') %}
    {% if printer.save_variables.variables.enable_box == 1 %}
        EXTRUDER_LOAD SLOT={slot}
    {% endif %}

[gcode_macro UNLOAD_T0]
gcode:
    {% set slot = printer.save_variables.variables.value_t0|default('slot0') %}
    {% if printer.save_variables.variables.enable_box == 1 %}
        EXTRUDER_UNLOAD SLOT={slot}
    {% endif %}
```
`T1`..`T15` / `UNLOAD_T1`..`UNLOAD_T15` repeat the pattern with `value_t1`..`value_t15`.

**Critical gotchas for HelixScreen's load path:**

1. **`T<n>` / `UNLOAD_T<n>` are no-ops unless `save_variables.enable_box == 1`.** If the box is not "enabled" in the firmware's saved state, the macro's `{% if %}` falls through and **nothing happens** — which presents exactly as #1022's "nozzle heats to 250 °C, then nothing." (The 250 °C preheat is driven by HelixScreen / the surrounding tool-change flow, not the `T<n>` macro itself.)
2. **The true primitives are `EXTRUDER_LOAD SLOT=<slotname>` and `EXTRUDER_UNLOAD SLOT=<slotname>`** (implemented in the proprietary `box_stepper.so`). `<slotname>` is a string like `slot0`, resolved per tool from the `value_t<n>` saved variable — **not** the tool index directly.
3. HelixScreen currently sends bare `T<n>` (`ams_backend_qidi.cpp:688`) and `UNLOAD_T<n>` (:720). The macro *names* are correct, but we depend on the firmware's `enable_box` flag and `value_t<n>` mapping being set. Decision for Phase 3 (#1022 de-stub plan): either (a) ensure/SET `enable_box` + `value_t<n>` before issuing `T<n>`, or (b) bypass the wrapper and send `EXTRUDER_LOAD SLOT=slot<n>` directly. (b) is more robust but skips whatever the wrapper guards.

The slicer-level change sequence (`slicer_configs/change_filament.gcode`) confirms the tool-change entry points: it calls `UNLOAD_T[current_extruder]` then `T[next_extruder]` around `CUT_FILAMENT` / flush moves. So the load/unload command *names* HelixScreen emits are right; the gating is the open question.

**Eject:** no discrete "eject one slot back to the box" command is present in this config — `EXTRUDER_UNLOAD SLOT=` is the only unload primitive. This matches HelixScreen leaving `supports_lane_eject()` at its `false` default. Confirm against the user's macro list before implementing an eject affordance.

### Max 4 dialect divergence (`multi_color_controller`) — prestonbrown/helixscreen#1083

The Max 4 box is **not** command-compatible with the Q2/Plus 4 despite sharing the `box_stepper` / `box_extras` / `box_rfid` stack. It adds a closed-source `multi_color_controller.so` state machine that owns the filament ops, and HelixScreen's Q2 eject primitive `FORCE_MOVE STEPPER="box_stepper slotN"` is **rejected on the Max 4 with `Invalid pin value`** (reported on #1068). Reverse-engineered command surface (community RE — the module is binary-only, so these are from symbol dumps + a Moonraker call graph, **not** a device trace):

```gcode
MULTI_COLOR_LOAD       SLOT=slotN   ; load box slot -> extruder
MULTI_COLOR_UNLOAD     SLOT=slotN   ; retract from hotend
MULTI_COLOR_BOX_UNLOAD SLOT=slotN   ; eject material back into the box (Q2 FORCE_MOVE equivalent;
                                    ;   internally drives SLOT_UNLOAD, a box_stepper -3000 move)
```

`slotN` is the same zero-indexed `slot0…` token HelixScreen already uses. Prefer the public `MULTI_COLOR_BOX_UNLOAD` over the internal `SLOT_UNLOAD` (the latter isn't in the public Moonraker `help` surface).

**Dialect discriminator for capability-based dispatch:** presence of the Moonraker **`multi_color_controller`** object. It is Max 4-only — the Q2/Plus 4 expose `box_extras`/`box_stepper`/`box_rfid` but not `multi_color_controller` (confirmed against our presets: `presets/qidi_q2.json` lacks it, `presets/qidi_max4.json` lists it). Dispatch rule: `multi_color_controller` present → Max 4 (`MULTI_COLOR_BOX_UNLOAD SLOT=slotN`); otherwise → Q2/Plus 4 (`FORCE_MOVE STEPPER="box_stepper slotN"`). Sources: `thelegendtubaguy/Qidi-Max-4-Optimized/docs/qidi_box/`, `Wazzup77/Bunny-Box/Max4/`. Not yet verified end-to-end on Max 4 hardware.

---

## Filas List Format (`officiall_filas_list.cfg`, #1030)

> Note the firmware's filename is misspelled **`officiall_filas_list.cfg`** (double-l). Match that spelling.

Plain INI, ~50 `[filaN]` material sections plus a color dictionary and vendor list. Verbatim sample (stock Q2):

```ini
[fila1]
filament                       = PLA Rapido
min_temp                       = 190
max_temp                       = 240
box_min_temp                   = 0
box_max_temp                   = 0
type                           = PLA

[colordict]
1                              = #FAFAFA
2                              = #060606
; … 24 entries, id → hex …

[vendor_list]
0       = Generic
1       = QIDI
2       = eSUN
; … 14 entries, id → vendor name …
```

| Key | Meaning |
|-----|---------|
| `filament` | Display name (e.g. "PLA Rapido", "PLA Matte") |
| `min_temp` / `max_temp` | Nozzle temperature range (°C) |
| `box_min_temp` / `box_max_temp` | Per-material **box drying** range (°C); `0` = unset (e.g. PLA), non-zero for ABS/ASA (≤45), engineering plastics (≤65) |
| `type` | Material family (PLA, ABS, ASA, PC, PA, PET, TPU, PVA, …) |
| `[colordict]` | Numeric id → hex color; per-slot color is stored as a color id referencing this table |
| `[vendor_list]` | Numeric id → vendor name |

`AmsBackendQidi::apply_filas_list()` (declared but unimplemented, `include/ams_backend_qidi.h:164`) should parse this into `fila_profiles_` to seed material presets, nozzle ranges, and per-material drying defaults. The colordict/vendor tables decode the per-slot saved state (see Known Unknowns — where per-slot type/color is persisted is still TBD; `box1.cfg` has **no** `SAVE_VARIABLE` for filament attributes, so it likely lives in `box_rfid.so` state or another saved-variable namespace).

---

## HelixScreen Implementation

### Backend Files

| File | Role |
|------|------|
| `src/printer/ams_backend_qidi.cpp` | `AmsBackendQidi` — dryer virtuals implementation |
| `include/ams_backend_qidi.h` | Class declaration |
| `include/ams_types.h` | `DryerInfo` struct |
| `src/printer/ams_state.cpp` | `AmsState::sync_dryer_from_backend()` — subject bridge |

### DryerInfo Population (`get_dryer_info`)

The `get_dryer_info()` implementation queries three sources:

1. **`heater_generic heater_box<N>`** — `temperature`, `target`, `power`
2. **`box_extras.box_drying_state.box<N>`** — `dry_state`, `end_time` → compute `remaining_minutes`
3. **`printer.configfile.settings`** — `max_temp` / `target_max_temp_heater_generic` → caps field in `DryerInfo`

Humidity comes from `aht20_f heater_box<N>`.

### Command Policy (`start_drying` / `stop_drying`)

```
If box_extras drying state is subscribed and present:
    → prefer ENABLE_BOX_DRY / DISABLE_BOX_DRY  (firmware owns timer)
Else:
    → fall back to SET_HEATER_TEMPERATURE  (manual hold, no timer)
```

This means the countdown display is only available on firmware that exposes `box_extras`. On firmware without `box_extras`, the heater runs until manually stopped.

### Live Subscriptions

`box_extras` and `save_variables` are included in the Moonraker object subscription sequence (in `moonraker_discovery_sequence.cpp`, gated on `AmsType::QIDI_BOX`). This keeps the drying countdown and `dry_state` updating live during a session without polling.

### Write-path logging

The write-path is always enabled. (The former `HELIX_QIDI_BOX_WRITE` field-testing gate was removed once the command syntax was verified against QIDI's open-source firmware — box_stepper.py/box_extras.py, #1030.) Every write op (`start_drying`/`stop_drying`, load/unload/mapping) emits an `info`-level entry log plus the raw G-code via `execute_gcode`, so field behavior stays fully visible while the protocol is confirmed on real hardware.

---

## Firmware Sources and References

| Source | URL | Notes |
|--------|-----|-------|
| Official QIDI Klipper fork | https://github.com/QIDITECH/klipper | Box modules (`box_stepper`, `box_extras`, `box_rfid`, `multi_color_controller`) ship as **compiled/obfuscated `.so`**, not `.py` in `klippy/extras` — see the Plus4-Wiki row for open-source reimplementations |
| Community RE + macros | https://github.com/thelegendtubaguy/Qidi-Max-4-Optimized | `config/box.cfg`, `config/drying.conf`, `docs/qidi_box/` — primary protocol reference |
| Community firmware replacement | https://github.com/qidi-community/Plus4-Wiki/tree/main/content/customisable_qidibox_firmware | Open-source replacements for six obfuscated `.so` modules |

---

## Known Unknowns and Field Validation

> **The exact vendor command and parameter syntax (`ENABLE_BOX_DRY`, `DISABLE_BOX_DRY`, `BOX=` numbering, `END_TIME` unit) is pending validation on real QIDI Q2 + Box hardware.** The syntax documented here is derived from firmware source inspection and community RE, not from a live device.

If firmware behavior differs from what is documented here, adjust the command string in `AmsBackendQidi::start_drying()` and `stop_drying()`. The rest of the pipeline (DryerInfo population, subscription, UI) does not need to change — only the emitted G-code.

**Resolved from the `my_qidi_2` stock config (2026-06-16, #1022):**

- Filament load/unload primitives: `EXTRUDER_LOAD SLOT=<slotname>` / `EXTRUDER_UNLOAD SLOT=<slotname>`, wrapped by `T<n>`/`UNLOAD_T<n>` and gated on `save_variables.enable_box == 1`.
- `officiall_filas_list.cfg` format (INI; `[filaN]` + `[colordict]` + `[vendor_list]`) — see "Filas List Format" above.
- No discrete single-slot eject command in stock config.

Still unconfirmed (needs the user's device dumps):

- **Where per-slot filament type/color is persisted.** `box1.cfg` has no `SAVE_VARIABLE` for it; suspected `box_rfid.so` state or another saved-variable namespace. Need a `save_variables` dump (and a before/after when a slot's filament is set in QIDI's own UI).
- **Whether `enable_box` is set on Camden's box** (the likely cause of load doing nothing) and what the `value_t<n>` → slot mapping is.
- Box numbering when multiple Box units are chained (does `BOX=2` work as `heater_box2`?)
- Whether `END_TIME` is in hours or minutes on the PLUS4 firmware variant
- `MULTI_COLOR_DRY` parameter shapes
- Whether `box_extras` is present on all supported firmware versions or only some
- Why the dryer indicator doesn't render on the stock path despite `dryer.supported==true` forcing it (needs a device `-vv` log; see plan §1).
