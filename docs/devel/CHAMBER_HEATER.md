# Chamber Heaters (Developer Guide)

How HelixScreen models heated printer chambers: the backend abstraction that keeps vendor knowledge in one place, how a chamber is discovered and wired into subjects, the ceiling rules that cap what the UI will send, and the arbitration semantics when something else is driving the heater.

Chamber heaters come in two very different shapes, and the backend interface is what lets the rest of the app treat them identically:

- **Integrated style** (K2 Plus): the chamber is part of the printer — a `heater_generic` + a `temperature_fan` cooling pair, scripted by the printer's own `M141`/`M191` macros. Diagnostics are just the standard Klipper status fields.
- **External appliance** (DragonBreath, Panda Breath): a separate heated-filtration box with its own firmware, exposing a vendor status object (faults, PTC element temp, filter fan) and vendor commands. These need a backend that speaks their schema.

Part of issue #1290.

---

## Architecture Overview

```
Moonraker objects/list
  |
  |  PrinterDiscovery::parse_objects  (include/printer_discovery.h)
  |    try_set_chamber_heater() consults helix::chamber::match()
  v
ChamberHeaterBackend (registry, fixed priority)
  |  generic            — keyword-tier fallback (CHAMBER 100 > ENCLOSURE 90 > CAVITY 85 > BOX 60)
  |  dragonbreath       — appliance, name match at 95
  |  panda_breath       — appliance, name match at 95
  v
PrinterState::set_hardware  (src/printer/printer_state.cpp)
  |  gates on "resolved chamber heater == discovery pick", then:
  |    temperature_state_.set_chamber_diagnostics_source(id, diag_object, filter_pin)
  |    TemperatureController::set_chamber_actions(reset_gcode, filter_pin, conservative_max)
  v
  +-> Subscription builder adds backend surfaces
  |     (src/api/moonraker_discovery_sequence.cpp — chamber::required_status_objects)
  |
  +-> Status frames -> printer_temperature_state.cpp parse block
  |     backend->parse_diagnostics() -> generic ChamberHeaterDiagnostics
  |     -> subjects chamber_heater_* / chamber_filter_fan_*
  |
  +-> TemperatureController::ensure_limits() applies the ceiling
        configfile max_temp  >  backend conservative_max  >  heater default (80)
  |
  v
temp_graph_overlay.xml chamber card (diagnostics in the right column)
  +-> ui_xml/components/chamber_fault_banner.xml (shared one-row banner + Reset)
  +-> ui_xml/components/chamber_diagnostics_card.xml (compact strip, portrait + 480x272)
```

The invariant: **vendor JSON schemas are translated to the generic `ChamberHeaterDiagnostics` struct at the backend border** — subjects, UI, and controllers never see a vendor field name. Adding a brand means one new `.cpp` file and one registry line; nothing else in the tree changes.

---

## Key Files

| File | Purpose |
|------|---------|
| `include/chamber_heater_backend.h` | Backend interface + registry API: `ChamberHeaterDiagnostics` (the only shape subjects/UI see), `match()`, `backend_by_id()`, `required_status_objects()` |
| `src/printer/chamber_heater_backend_generic.cpp` | Generic keyword backend + the registry itself (`registry()`, `match()`, `backend_by_id()`) and the keyword-confidence tiers |
| `src/printer/chamber_heater_backend_dragonbreath.cpp` | DragonBreath appliance backend: 24-field `dragonbreath` status parse, `DRAGONBREATH_RESET`, filter pin, 60°C conservative cap. Schema verified live on the U1 rig 2026-08 |
| `src/printer/chamber_heater_backend_panda_breath.cpp` | Panda Breath backend (stock firmware): heater, 60°C fallback ceiling, link state and which control loop holds the heater. No fault surface, no filtration speed, no element temperature |
| `include/chamber_heater_assignment.h` | `chamber::resolve_heater()` and `chamber::resolve_sensor()`: the one rule for which chamber heater and which chamber sensor the printer has, given each role's "auto" / "none" / named-object assignment |
| `include/printer_discovery.h` | `try_set_chamber_heater` lambda in `parse_objects()` — first registry consult, records backend id + diagnostics object + filter pin on discovery |
| `src/api/moonraker_discovery_sequence.cpp` | Subscription-builder block adding the backend's diagnostics object + filter pin (`chamber::required_status_objects`) |
| `src/printer/printer_temperature_state.cpp` | Diagnostics parse block: translates backend output to subjects; also owns all `chamber_heater_*` / `chamber_filter_fan_*` subject registration and display-string formatters |
| `src/printer/printer_state.cpp` | The wiring block: gates both the diagnostics source and the TemperatureController action surface on resolved-heater == discovery-pick |
| `src/ui/temperature_controller.cpp` | `set_chamber_actions()`, `reset_chamber_fault()`, `set_chamber_filter_fan()`, and the `ensure_limits()` ceiling fallback |
| `ui_xml/temp_graph_overlay.xml` | Where the diagnostics render: above the micro/tiny landscape line they live in the right column's `chamber_display_card` (fault banner via the shared component, hairline, element row, filter-fan row with a `ui_switch`); the card's border goes `#danger` while faulted/inhibited/offline. The under-chart `<if cond="printer_has_chamber_heater_diagnostics and temp_graph_mode eq 3 and (ui_is_portrait or ui_breakpoint eq 0)">` builds the compact strip instead on portrait and 480x272, where the right column has no room beside the presets |
| `ui_xml/components/chamber_fault_banner.xml` | Shared one-row banner: reason text (or "Heater offline") plus a compact Reset that hides while the device is offline. Instantiated by both surfaces above, which never coexist |
| `ui_xml/components/chamber_diagnostics_card.xml` | The compact strip for portrait and 480x272: one info row (element icon + value, filter-fan icon + percent, muted External marker, fan switch) sized so 272x480 chamber mode keeps a usable chart with zero scroll; a fault replaces the info row with the banner |
| `src/api/moonraker_client_mock.cpp` | Mock chamber backend shape (`HELIX_MOCK_OBJECTS` dragonbreath trio), registry-based chamber-status key |
| `tests/unit/test_chamber_*.cpp` | Backend match/parse, subjects, ceiling, actions, discovery, mock — tags under `[chamber]` |

---

## The Two Styles

### Integrated style (K2 Plus)

The chamber ships with the printer. Klipper exposes the pair directly:

- `heater_generic chamber_heater` — the heating element (HEATING setpoint)
- `temperature_fan chamber_fan` — chamber cooling (MAINTAINING setpoint)

The printer's own `M141`/`M191` macros split the setpoint across the pair: above 40°C the target lands on the heater, at or below 40°C it parks on the cooling fan with the heater at 0. `M141 S0` resets the fan to its **configured resting target** (`target_temp` in the fan's config section — 35°C on our K2), not to literal 0. HelixScreen mirrors this split in `chamber_effective_setpoint()` (`include/ui_temperature_utils.h`) and routes chamber sets through `M141` when the printer defines the macro (`chamber_uses_m141()`), falling back to raw `SET_HEATER_TEMPERATURE` / `SET_TEMPERATURE_FAN_TARGET` otherwise.

These printers need no diagnostics backend — the generic backend matches by keyword and provides no diagnostics surface; the standard status fields are the diagnostics.

### External appliance style (DragonBreath, Panda Breath)

A separate box with its own firmware bolts onto the printer. The Klipper side is a thin glue module exposing:

- a vendor status object (e.g. `dragonbreath` — 24 fields: `ptc_temp`, `fault`, `inhibited`, `fan_percent`, `fan_reason`, `mode`, `source`, `lease_owned`, ...)
- vendor commands (`DRAGONBREATH_RESET`) and a filter-fan `output_pin`

The backend translates that schema to the generic struct. Vendor names appear **only** in `chamber_heater_backend_*.cpp` — anywhere else is a vendor-leak bug (see the VENDOR_OK rule in the root `CLAUDE.md`).

---

## Discovery and Arbitration

`chamber::match(object_name)` runs every discovered `heater_generic` / `temperature_fan` bare name past the registry:

- Every backend scores the name; highest confidence wins, ties resolve by registry order.
- Appliance backends claim their own names at **95** — they always beat the generic keyword tiers on the object that is actually theirs.
- The generic backend carries the keyword tiers: `CHAMBER` 100 > `ENCLOSURE` 90 > `CAVITY` 85 > standalone `BOX` 60; -1 for compound names, -40 for air-quality tokens (`TVOC`, `CO2`, `HUMIDITY`, ...), floored at 1.
- `try_set_chamber_heater` additionally breaks ties by object type: a settable `heater_generic` (weight 2) beats a `temperature_fan` (weight 1) at equal keyword confidence. The losing `temperature_fan` is still recorded as the **chamber cooling fan** so the integrated-style Maintaining readout works.
- `try_set_chamber_sensor` scores the sensor pick by keyword alone: a chamber-named `temperature_fan` competes as a sensor candidate — a printer whose only chamber thermistor is such a fan still gets a chamber sensor — and an equal-keyword tie resolves to the passive `temperature_sensor` (weight 2 over the fan's 1) in either iteration order.
- That pick only stands when nothing heats the chamber. Objects are classified in one pass and the three picks cannot consult each other, so a post-pass at the end of `parse_objects()` releases the sensor pick whenever a chamber heater was resolved — the heater measures its own chamber, so it supplies the reading and no probe holds the sensor role beside it. The released probe keeps its own role and stays listed. The reconciliation sits with the AFC and multiACE yield-backs, which revoke in-loop decisions the same way.

### Which heater the printer has

The chamber-heater assignment (`heaters/chamber` in the printer's settings, edited from Sensor
settings and seeded by model presets before the wizard runs) is `"auto"`, `"none"`, or a Klipper
object name. `chamber::resolve_heater()` (`include/chamber_heater_assignment.h`) is the only code
that turns it into a heater:

- `"auto"` takes the discovery pick; `"none"` means no chamber heater.
- A named object counts only while Klipper reports it in its object list. A preset seeds its model
  family's heater name, so a family member without that heater carries a name for hardware it
  lacks; that name falls back to the discovery pick, whatever type it is, including a
  chamber-named `temperature_fan`. A base K2 that reports `temperature_fan chamber_fan` and no
  heater therefore gets a fan-driven chamber control, exactly as a K1C does; a printer with
  neither a chamber heater nor a chamber fan resolves to no chamber heater.

`PrinterState::set_hardware` publishes the result once per discovery as
`temperature_state().chamber_heater_name()` and the `printer_has_chamber_heater` capability.
Before discovery lands the capability is 0, and each later discovery re-resolves it, so it
follows Klipper in both directions. Consumers read what was published and never discovery's pick:
`TemperatureController::resolved_name()` refuses a chamber target when it is empty, the filament
panel builds Cool Down and material chamber targets from it, and the material temps hint reads the
same capability its chamber column binds.

There is no macro-only chamber heater. `M141` is a transport for the resolved heater
(`chamber_uses_m141()`), and the chamber target is read from that heater's own status object, so
every working chamber heater is a `heater_generic` or `temperature_fan` object Klipper reports.
The reading comes from the same object unless a sensor has been assigned to the chamber role by
hand; `chamber_temperature_source()` is the one rule, so every readout and graph series names the
same probe.

The matched backend id survives on `PrinterDiscovery` (`chamber_heater_backend_id()`) and is re-consulted in `PrinterState::set_hardware`: the diagnostics source and the action surface apply **only while the resolved chamber heater is the discovery pick** — a manual override to another heater (or "none") detaches both, and the actions revert to no-ops.

### Which chamber sensor the printer has

The chamber-sensor assignment (`temp_sensors/chamber` in the printer's settings, edited from Sensor
settings) is `"auto"`, `"none"`, or a Klipper object name. `chamber::resolve_sensor()`
(`include/chamber_heater_assignment.h`) is the only code that turns it into a sensor, and it applies
the heater's rule against discovery's sensor pick:

- `"auto"` takes the discovery pick, which is empty whenever a chamber heater was resolved;
  `"none"` means no chamber sensor.
- A named object counts only while Klipper reports it in its object list. A saved name the printer
  does not report, such as one a model preset seeded, falls back to discovery's sensor pick, so the
  chamber reads the sensor the printer does have. The fallback is always discovery's sensor pick,
  never its heater pick; the heater keeps its own type-blind fallback above. A sensor absent for one
  boot (a disconnected MCU, a config being edited) resumes its authority on the discovery that
  reports it again.
- A named object that is not the heater is a deliberate choice of probe, and it outranks the heater
  for the chamber **reading**. The heater goes on supplying the target either way. This is the one
  way a probe wins against the heater that measures its own chamber, and it is what makes the
  assignment meaningful on a printer that has both.

`PrinterState::set_hardware` publishes the result as `temperature_state().chamber_sensor_name()`
and the `printer_has_chamber_sensor` capability, and re-resolves it on every discovery (each klippy
ready and reconnect), so a named sensor that appears or disappears is followed without a restart.
Consumers read what was published. The one reader of discovery's own sensor pick is the chamber
sensor dropdown in Sensor settings (`src/ui/ui_settings_sensors.cpp#populate_chamber_assignment`),
whose "Auto" entry names the sensor `auto` takes.

---

## Subjects

All registered by `PrinterTemperatureState`; display strings are formatter subjects (XML has no deci/percent formatter).
Raw vendor strings — the fault code and the filter-fan reason — are logged at the backend border and classified into the
generic kinds above; they are deliberately not subjects, so nothing can bind a vendor word.

| Subject | Type | Meaning |
|---------|------|---------|
| `chamber_heater_fault` | int 0/1 | Latched fault |
| `chamber_heater_inhibited` | int 0/1 | Heater refusing commands (e.g. post-fault cooldown) |
| `chamber_heater_fault_reason_text` | string | Translated phrase for the backend's generic `FaultReason` kind ("" when none) — what the banner binds |
| `chamber_heater_offline` | int 0/1 | Device unreachable on its own link. 1 only on an engaged "not connected" report; a backend with no link state (generic) leaves it 0 — unknown is not offline. While 1 the card banners the offline message and hides Reset (`DRAGONBREATH_RESET` cannot reach a device that is not answering). The vendor `protocol_error` string behind a link drop is log-only, and so is the raw vendor fault code |
| `chamber_heater_externally_controlled` | int 0/1 | Another controller is driving the heater (display-only, see below) |
| `chamber_heater_element_temp_text` | string | Heating-element temp ("--" = unknown). The number stays a private member: nothing graphs, thresholds or colours the element, so no int subject is registered |
| `chamber_filter_fan_percent_text` | string | Filtration-fan speed ("--" = unknown). The number stays a private member, as with the element temp |
| `chamber_filter_fan_requested` | int | Our output_pin request (-1 unknown / 0 / 1) — what the toggle click inverts |
| `chamber_filter_fan_device_driven` | int 0/1 | Device runs the fan on its own (heater warmup / thermal purge); the card badges the readout and disables the switch |
| `chamber_filter_fan_on` | int | Fan RUNNING state: reported speed when the backend has one, the pin otherwise |
| `printer_has_chamber_heater_diagnostics` | int 0/1 | Capability: the diagnostics surfaces (card block / strip) are built at all |
| `printer_has_chamber_filter_fan` | int 0/1 | Capability: filter-fan toggle and its readout column |
| `printer_has_chamber_element_temp` | int 0/1 | Capability: element readout column, from the backend's `reports_element_temp()` |

Capability setters round-trip through `PrinterCapabilitiesState`; `set_hardware` raises them exactly when the backend provides the corresponding surface. Backends differ in what they publish, so each readout column on the card follows its own capability — a permanently blank row tells the user nothing. The External badge sits in its own Mode column rather than beside the element temperature, so it survives a backend that reports no element.

---

## Ceiling Rules

What the chamber keypad and presets will offer, in precedence order:

1. **`configfile` `max_temp`** for the resolved heater section — the printer's own limit (read via `query_configfile` in `TemperatureController::ensure_limits()`). Example: the K2's `heater_generic chamber_heater` declares `max_temp: 80`.
2. **Backend `conservative_max_temp()`** — used only when configfile is silent, and a declared `max_temp` always wins. DragonBreath and Panda Breath both return 60, the chamber temperature the appliances are sold for. Real configs sit above it (DragonBreath's usually says 75; Snapmaker's shipped Panda Breath heater fragment says 80), so the fallback is deliberately the low end.
3. **Heater default** (`keypad_max_default`, 80 for chamber) — generic backend returns 0 = no clamp, so an unconfigured generic chamber keeps the default.

The fallback snapshot is taken **before** the configfile query fires — the parse callback runs on the WebSocket thread and must not read `this` members (see `THREADING.md`).

---

## Arbitration: Who Is Driving the Heater?

Two distinct mechanisms, both informational in v1 — HelixScreen never fights another controller for the chamber:

- **DragonBreath lease semantics.** SET_HEATER_TEMPERATURE commands round-trip with a lease: while our lease is held the status reports `lease_owned` / `source: klipper`. When another controller (device web UI, physical button) takes over, the firmware invalidates our lease and the status keeps reporting authoritative state. `parse_diagnostics()` derives `externally_controlled = (mode == power_on) && !(lease_owned || source == klipper)` — heating that is neither ours nor Klipper's. The UI shows this as an annotation only.
- **Stock-firmware autonomous mode (Panda Breath).** `device_autonomous_control()` returns true: the device's own Auto mode follows the bed temperature with no host involvement. The binding publishes which loop is holding the heater as `work_mode` (1 = the appliance's own auto cycle, 2 = the target Klipper set, 3 = a filament-drying run) alongside `work_on` for its output stage, so `parse_diagnostics()` derives `externally_controlled = work_on && work_mode != 2`. `work_mode` latches at its last value after the output stops, which is why `work_on` is what makes the answer present-tense. Beyond the badge this is still informational — no policy (e.g. dimming the setpoint UI) acts on it.

---

## Adding a Chamber-Heater Backend

One file + one registry line + one test + one mock shape. The vendor-abstraction test: adding a second appliance must touch exactly the files below — if you find yourself editing the status parser, the subscription builder, *and* the panel, the abstraction is missing something.

1. **Create `src/printer/chamber_heater_backend_<name>.cpp`** — subclass `ChamberHeaterBackend` (see `chamber_heater_backend_panda_breath.cpp` for the minimal shape, `chamber_heater_backend_dragonbreath.cpp` for the full diagnostics parse). Implement `id()`, `discovery_confidence()`, and the capability questions; return `std::nullopt` from `parse_diagnostics()` until the status schema is hardware-verified. Give the file a `// VENDOR_OK:` header comment.

2. **Register it** in the `REG` vector in `chamber_heater_backend_generic.cpp` (`registry()`), plus the `<name>_backend_instance()` free-function pattern the existing backends use. The Makefile picks the new `.cpp` up by wildcard; also add it to `components/helixapp/app_srcs.txt` (ESP32 manifest — the pre-commit gate fails the link otherwise).

3. **Add a `parse_diagnostics` test** in `tests/unit/test_chamber_heater_backend.cpp` — a live nominal payload, a faulted/edge variant, and a foreign-payload rejection (mirror the three DragonBreath cases).

4. **Mock shape** — for appliances with a status object, extend `append_chamber_backend_status()` in `moonraker_client_mock.cpp` with a branch on your backend id, and add any `HELIX_MOCK_*` hooks the interesting states need (`MOCK_ENVIRONMENT_VARIABLES.md`). Two references, both in `test_chamber_mock_appliances.cpp`: the dragonbreath trio `HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter"` (heater, status object, filter pin) and the stock pair `HELIX_MOCK_OBJECTS="heater_generic panda_breath panda_breath"` (heater, status object, no pin). Synthesize only fields the real binding publishes — inventing one exercises a parse path no device can reach.

Verify with `./build/bin/helix-tests "[chamber]"` and a mock run:

```bash
HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter" \
  ./build/bin/helix-screen --test -vv
```

---

## Testing

```bash
./build/bin/helix-tests "[chamber]"          # the whole feature
./build/bin/helix-tests "[chamber][backend]" # registry match + per-backend parse
./build/bin/helix-tests "[chamber][subjects]"# diagnostics subjects + pin mapping
./build/bin/helix-tests "[chamber][ceiling]" # configfile > conservative > default
./build/bin/helix-tests "[chamber][actions]" # fault reset + filter fan gcode
```

Per-file: `test_chamber_heater_backend.cpp` (match/parse), `test_chamber_heater_discovery.cpp` (discovery hook), `test_chamber_diagnostics_subjects.cpp`, `test_chamber_ceiling_actions.cpp`, `test_chamber_mock_appliances.cpp`, `test_chamber_panel_diagnostics.cpp`, `test_chamber_temperature.cpp`, `test_chamber_mode_icon_label_parity.cpp`.

---

## Verification Log

### DragonBreath on the U1 rig (2026-08, live)

- `SET_HEATER_TEMPERATURE HEATER=<bare> TARGET=...` round-trips with **lease semantics**: the `dragonbreath` status object reports our lease while we hold it; another controller taking over invalidates it and the object keeps reporting authoritative state.
- The glue module's `M141` is **module-registered, not a macro** — invisible to Moonraker's `gcode_macro` object list. Chamber routing therefore falls back to raw `SET_HEATER_TEMPERATURE` (`chamber_uses_m141()` returns false because no `gcode_macro M141` object exists).
- `configfile` exposes `max_temp: 75` for the heater section — readable, and it wins over the backend's conservative 60.
- The `dragonbreath` status object carries **24 fields**; the backend parse (`chamber_heater_backend_dragonbreath.cpp`) is written against a captured live payload.

### Stock Panda Breath on the U1 rig (2026-09-16, live)

Captured with the appliance flipped to its stock OTA slot and the U1's Klipper
config switched from the DragonBreath fragment to Snapmaker's shipped
`[panda_breath]` one (`firmware: stock`, a WebSocket to `ws://<host>:80/ws`).

- Objects: `panda_breath` (status, 15 fields) and `heater_generic panda_breath`.
  **No filter-fan `output_pin`** — the appliance runs its filter from its own
  auto settings and publishes no speed. `temperature_sensor cavity` is the
  printer's own probe and is unrelated.
- Status fields: `temperature`, `target`, `smoothed_temp`, `connected`,
  `work_mode`, `work_on`, `device_target`, `auto_enabled`, `auto_target`,
  `auto_filtertemp`, `auto_hotbedtemp`, `filament_temp`, `filament_timer`,
  `remaining_seconds`, `filament_drying_active`. No fault, inhibit or element
  temperature anywhere in the schema.
- Chamber temperature arrives in **whole degrees** (23.0 → 24.0 → 25.0).
- Mode transition, measured: at rest the appliance sat in its own auto cycle
  (`work_mode: 1`, `work_on: true`, `auto_enabled: true`, `device_target: 60`)
  while the Klipper target read 0. `SET_HEATER_TEMPERATURE HEATER=panda_breath
  TARGET=30` flipped it to `work_mode: 2`, `auto_enabled: false`,
  `device_target: 30`, and the chamber rose 23 → 25 °C in 16 s at `power: 1.00`.
  Returning to 0 cleared `work_on` but **left `work_mode` at 2** — the mode
  latches, so `work_on` is what makes `externally_controlled` present-tense.
- The binding registers `PANDA_BREATH_AUTO`, `PANDA_BREATH_DRY_START` and
  `PANDA_BREATH_DRY_STOP` without a help description, so **none of them appear
  in `/printer/gcode/help`** — capability detection has to come from the status
  object's presence, not the command list.
- No `gcode_macro M141` with this config fragment, so chamber routing falls
  through to raw `SET_HEATER_TEMPERATURE`, as it does for DragonBreath.
  Snapmaker's optional `panda_breath_heater_auto.cfg` *does* define `M141` (and
  routes it through `PANDA_BREATH_AUTO`), so both shapes occur in the field and
  the existing `chamber_uses_m141()` check picks the right one.

### K2 Plus, integrated style (2026-08-20, live, non-invasive)

Printer idle-checked first (`print_stats.state: complete`) before any gcode was sent.

| Check | Result |
|-------|--------|
| Chamber objects present | **PASS** — verbatim: `heater_generic chamber_heater`, `temperature_fan chamber_fan`; also `heater_fan chamber_fan` (PTC fan on the heater itself) and `temperature_sensor chamber_temp` |
| `M141`/`M191` macro visibility | **PASS** — `gcode_macro M141` and `gcode_macro M191` are listed objects (macro bodies not exposed via `configfile`; `gcode_macro SET_CHAMBER_FAN` and `gcode_macro CANCEL_CHAMBER_FAN_SWITCH` also present) |
| `M141 S35` readback (cooling range) | **PASS** — heater target stayed 0.0; the setpoint parks on `temperature_fan chamber_fan` (target 35.0). Note: 35 equals this unit's configured resting `target_temp`, so the parking is confirmed by the mode model plus the `S0` behavior below rather than a visible target change |
| `M141 S0` reset | **PASS** (with nuance) — heater 0; the cooling fan returns to its configured resting target (35.0), **not** literal 0. This is exactly the resting semantics `chamber_effective_setpoint()` implements; a literal `SET_TEMPERATURE_FAN_TARGET ... TARGET=0` is a setpoint *below* resting, not the firmware's off state |
| Left clean | **PASS** — restored to the config-defined idle (heater target 0 / power 0, fan target at resting 35.0, fan speed 0) |

Config facts captured: `heater_generic chamber_heater` — `max_temp: 80`, watermark control, `verify_heater`; `temperature_fan chamber_fan` — `max_temp: 80`, `target_temp: 35` (resting), watermark, `max_delta: 0.5`.

---

## Deferred: Dryer Mode

Chamber dryer mode (Panda Breath's `PANDA_BREATH_DRY_START`/`STOP` passthrough, DragonBreath's hardware drying with no Klipper surface, a generic hold-N°C-for-M-hours loop) is **deliberately out of scope** for v1 — tracked in [#1299](https://github.com/prestonbrown/helixscreen/issues/1299). It should land as a generic backend capability question reusing the existing dryer UX (Happy Hare dryer panels, AMS environment overlay), not as per-vendor UI.

The two appliance firmwares are not equally blocked, which matters when #1299 is picked up. Stock Panda Breath has **both halves**: the commands above plus live status in the same object the backend already parses — `filament_drying_active`, `filament_temp`, `filament_timer` and a `remaining_seconds` countdown (`work_mode: 3` while a cycle runs). DragonBreath's glue exposes neither a drying command nor drying status through Klipper, even though the appliance itself advertises a `drying` capability on its own HTTP API. So a #1299 implementation can be verified end to end on stock firmware and only stubbed for DragonBreath.
