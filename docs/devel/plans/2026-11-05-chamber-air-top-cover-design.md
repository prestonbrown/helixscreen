# Chamber air control, first used by the Snapmaker U1 Top Cover

Issue: prestonbrown/helixscreen#2015. Milestone 1.1.

## Why

A user fitted Snapmaker's Top Cover to a U1 running HelixScreen 1.1.0-beta.15 and could not tell whether HelixScreen supported it. HelixScreen detects the cover (`purifier.power_detected`, `include/enclosure_top.h`) and uses that only to pick the printer picture. Nothing else is offered: no way to run the purifier, no fan control, no filter tracking, and no word to the user when the cover is plugged in or pulled out. The stock screen does all four, so a user who switches to HelixScreen loses the accessory they just bought.

The goal is parity with the stock screen for the Top Cover. The build must be generic: "chamber air" (recirculate, exhaust, off, with optional targets, fans the mode owns, a filter that wears) is a shape other enclosed printers share. The Creator 5 Pro on Z-Mod already uses part of it. The U1 must be one provider row, and the next printer with this shape must touch one file (`.claude/rules/vendor-abstraction.md`).

## What the hardware and firmware expose

Source: the stock firmware's `klippy/extras/purifier.py` (U1 2.0.0.205 rootfs). Every U1 has a `[purifier]` object whether or not the cover is fitted, so everything below means something only while `power_detected` is true.

| Item | Detail |
|---|---|
| Presence | `power_detected` (bool): ADC on the cover connector, debounced, hot-pluggable. On removal the firmware forces both fans off and the mode to Idle itself. |
| Mode | `mode` int: 0 Idle, 1 Cool chamber (exhaust fan holds chamber at or below a target), 2 Preheat chamber (inner fan until a target, used with `WAIT_CHAMBER_TEMP`), 3 Hot chamber (inner fan holds heat). Set with `SET_PURIFIER_MODE MODE=n [FAN_SPEED=0..1] [DESIRE_TEMP=c] [ALARM_TEMP=c] [DELAY_OFF=s]`. Cool defaults: 42 °C, 0.6 fan. |
| Targets | `desired_chamber_temp`, `critical_chamber_temp`. |
| Fans | `exhaust_fan{speed 0..1, delay s, speed_threshold}` and `inner_fan{speed, delay, speed_threshold}`, `inner_fan_rpm`. Speed set with `SET_PURIFIER FAN=exhaust|inner SPEED=0..1`. |
| Filter | `inner_fan_work_time`: seconds the inner (filter) fan has run, accrued only in modes 2 and 3, persisted in `purifier.json`. Reset with `SET_PURIFIER FAN=inner WORK=0`. |
| Guards | With no cover, every non-zero speed or mode command is refused with only a klippy log line. Firmware raises `0533-0000-0000` (inner fan RPM 0 in heat modes) and `0533-0000-0001` (chamber above critical in Cool mode) as pausing errors. |

**Three status layouts exist.** 2.x (above, and the reporter's firmware, klipper 2.1.0.157) has `mode` and top-level `inner_fan_work_time`. 1.3.0.168 has `exhaust_fan{}` / `inner_fan{work}` and no `mode`. An older layout recorded in `SNAPMAKER_U1_SUPPORT.md` has flat `fan_speed` / `work_time`. **Decision (builder's-check call):** the provider reads the 2.x layout and keys each feature on its field being present in the status frame: no `mode` means no readback (plain action buttons, as Z-Mod), no `inner_fan_work_time` means no firmware filter counter (the meter keeps print hours), no `exhaust_fan`/`inner_fan` objects means no owned fans. Older firmware degrades to what it reports; nothing is parsed per version string.

Snapmaker U1 Extended Firmware overlay 41 (`41-feature-generic-purifier`) moves the two fans onto real `[fan_generic exhaust_fan]` and `[fan_generic circulation_fan]` objects and routes their writes through the purifier. On that firmware the fans already appear in HelixScreen as ordinary fans.

## What the stock screen does (reverse-engineered from `/usr/bin/gui`, 2.0.0.205)

Proven from strings and disassembly unless marked inferred.

- **Control page Top Cover card**, shown only while `power_detected`. Label "Current Mode": 0 Idle, 1 "External Circulation", 2 and 3 "Internal Circulation". Two sliders, "Exhaust fan speed" and "Recirculation fan speed", in 10 % steps, sent as `printer.control.purifier {"fan":"exhaust"|"inner","speed":<int percent>}`.
  - In Cool mode the exhaust slider cannot reach 0 (floored at 10 %); in modes 2 and 3 the same floor applies to the inner slider.
  - Lowering a fan below its `speed_threshold` asks first: "Reducing the fan speed may affect print quality. Do you want to continue?" (exhaust) / "...may affect filtration performance. Continue?" (inner).
  - Tapping the fan the current mode disables explains why: "With internal insulation mode on, the external circulation fan cannot be enabled" and the reverse.
  - The stock screen offers **no mode buttons and no target editing**.
- **No print-start chamber choice.** Print start sends no purifier command. `SET_PURIFIER_MODE` comes from lines inside the gcode file (inferred: the slicer's start gcode); `virtual_sdcard.py` records the line for power-loss resume. During a print the screen shows "Chamber conditioning..." and a preheat countdown from `machine_state_manager` / `preheat_*` fields.
- **Filter**: the statistics page shows "Air Purifier Runtime Xh" and "Filter working time Xh" from `inner_fan_work_time`, a static "suggest change" label, and a 4-step change tutorial whose confirm sends the work reset. No hours threshold exists anywhere.
- **First fit**: once per install (`gui_config.json` `system.top_cover_guide`), a 3-slide info guide ("New top cover detected" / "View top cover settings on the control page" / "Tap to enter top cover control page"), held off while printing, during power-loss restore, or before the setup wizard has finished. Afterwards, on the next home screen: "After connecting the top cover, we recommend recalibrating vibration compensation to ensure print quality.", buttons "Calibrate Now" (runs `SHAPER_CALIBRATE`, homing first) and cancel. The guide writes no printer state.
- **Removal**: every time, a dialog: "The top cover has been disconnected. To ensure print quality, reconnect the top cover before continuing." with OK.
- **Resume**: in Cool mode, if the chamber is above `critical_chamber_temp`, resuming a paused print asks "The chamber temperature is too high. This may cause extruder clogging. Continue printing?".

## Goals

1. A generic chamber air capability (`helix::chamber_air`) that a provider row fills in, which the U1 Top Cover and the existing Z-Mod row both use.
2. Mode control from the fan overlay's existing "Chamber air" row, showing the live mode where the firmware reports one, offered only while the accessory is present.
3. The accessory's fans as ordinary dials in the fan overlay, with the stock screen's guard rules.
4. Mode targets (temperature, fan speed) editable in a chamber settings row, which the mode buttons send.
5. Filter usage from the firmware's own counter in the existing Maintenance Filter meter, with a reset that clears the firmware counter.
6. First-fit and removal behaviour at parity with the stock screen.

## Non-goals

- Print-start chamber mode selection. The stock screen has none; slicer gcode drives it and already works under HelixScreen. Parity is met by doing nothing here.
- Editing delay-off times. The stock screen never touches them.
- The device self-test "Purifier" check, and the factory `purifier_factory` endpoint.
- A Preheat button. Preheat is a print-start step that pairs with `WAIT_CHAMBER_TEMP`, which slicer gcode issues; as a manual mode it behaves like Hot chamber.

## Anti-goals

- **A control that looks live and does nothing.** The firmware silently drops writes without the cover, so no purifier control may be reachable while presence is false, and controls must follow presence live, not a boot-time read.
- **A fan shown twice.** On Extended Firmware the `fan_generic` objects are the fans; synthetic dials must not appear beside them.
- **Vendor names in generic code.** `purifier`, `U1`, `Snapmaker` appear only in the provider modules.
- **Highlighting a mode the printer is not in.** Providers without mode readback (Z-Mod) keep plain action buttons.

## Design

### 1. `helix::chamber_air` provider contract (extend, do not fork)

`include/chamber_air_modes.h` / `src/printer/chamber_air_modes.cpp` stay the one module. A provider row gains optional fields; the Z-Mod row fills only what it has today and behaves exactly as now.

| Field | Z-Mod row | U1 row |
|---|---|---|
| detect(hw) | the AIR_CIRCULATION macros | `purifier` object plus a U1 marker (reuse `enclosure_top`'s detection; do not duplicate the marker list) |
| presence gate | none (always present) | live: the existing `printer_enclosure_top_fitted` reading |
| gcode per mode | the macros | Recirculate = `SET_PURIFIER_MODE MODE=3 FAN_SPEED=..`, Exhaust = `MODE=1 FAN_SPEED=.. DESIRE_TEMP=.. ALARM_TEMP=..`, Off = `MODE=0` |
| mode readback | none | `purifier.mode`: 0 to Off, 1 to Exhaust, 2 and 3 to Recirculate |
| mode targets | none | Exhaust: target °C + fan %; Recirculate: fan % |
| owned fans | none | exhaust and inner (below) |
| filter counter | none | `inner_fan_work_time` seconds, reset `SET_PURIFIER FAN=inner WORK=0` |
| status objects | none | `purifier` fields: mode, power_detected, desired/critical temp, exhaust_fan, inner_fan, inner_fan_work_time |

The capability questions stay free functions over the table (`supported`, `modes`, `mode_gcode(hw, mode, targets)`, `mode_from_status`, `required_status_objects`, `owned_fans`, `fan_speed_gcode`, `filter_seconds_from_status`, `filter_reset_gcode`). The subscription builder asks `required_status_objects`; it must not name `purifier`. The enclosure-top module already subscribes to `purifier.power_detected`; the builder should merge both modules' field lists into one subscription.

New subjects, named for the capability:
- `chamber_air_available` (0/1): provider matched AND presence holds. The Chamber air row, owned-fan dials and the settings row bind to it instead of the three static `printer_has_chamber_air_*` flags alone.
- `chamber_air_mode` (int, -1 unknown/no readback, else the `AirMode`): drives the checked style on the row's buttons.

### 2. Fan overlay

- The Chamber air row shows while `chamber_air_available`. Buttons send `mode_gcode` with the stored targets. With readback, the active button takes the checked style.
- Owned fans appear as fan dials in the fan overlay only, labelled "Exhaust fan" and "Recirculation fan", and send `fan_speed_gcode`. They do not enter `PrinterFanState`, so home fan widgets, the controls card and Fan settings are unchanged (decision: keeps the change to one overlay; revisit if users ask for them on the home screen).
- When the printer already has `fan_generic` objects the provider names as equivalents (`exhaust_fan`, `circulation_fan`, Extended Firmware), no synthetic dials are added, but those real dials are treated as the provider's fans: hidden while presence is false (their writes go through the purifier and are dropped without the cover) and subject to the guard rules below. One provider hook keyed by fan object name serves both cases.
- In Idle the firmware applies fan speeds directly (only presence is checked), matching the stock screen; guards apply only in modes 1-3. With no readback the guards are off.
- Guard rules from the stock screen, as provider data, not UI branches: a minimum speed for the fan the active mode drives (10 %); a confirm when lowering below `speed_threshold`; a fan the active mode disables shows a short explanation instead of accepting a change. The stock screen's wording is the source; strings go through `lv_tr` and the translation pipeline.
- Builder's call: how the dial presents the floor and the disabled state, within the existing dial component.

### 3. Chamber settings row (mode targets)

A "Chamber air" settings row, shown only while `chamber_air_available` and only for a provider with targets. It edits the targets each mode sends: Exhaust (cool) target temperature and fan %, Recirculate fan %. Defaults are the firmware's (42 °C, 60 %); alarm temperature is sent as target plus the firmware's minimum gap and is not exposed. Values are persistent settings (`SettingsManager`, per printer), not env vars. Builder's call: which settings page hosts the row (the device/fan settings area is the natural home) and the control type, measured at 800x480 and the U1's 480x320.

### 4. Filter meter

The Maintenance overlay's Filter meter gets a second source: when `chamber_air` reports a filter counter, the meter's used hours come from the firmware (`filter_seconds_from_status / 3600`) instead of HelixScreen's print-hour accrual, and Reset sends `filter_reset_gcode` and then clears the local meter. Interval, due notice and status colours keep working unchanged. Without a counter the meter behaves exactly as now. The decision "which source" is one pure function in `maintenance_meters.h` with tests. Decisions:
- While the firmware source is active, local print-hour accrual into the Filter meter stops (the other meters keep accruing). The firmware keeps reporting its counter with the cover off, so the source does not flip on unplug.
- Interval choices and the 200 h default stay; the stock screen has no threshold at all. The due notice fires on the first frame at or past the interval, latched by the existing `notified` flag.
- Reset is offered whenever the source is active (`WORK=` is not presence-gated in firmware). The local meter clears when the gcode succeeds; an error leaves it and reports through the normal gcode error path.

### 5. First fit and removal

Generic, triggered by `chamber_air` presence edges on a provider whose presence can change, with the wording supplied by the provider:
- **First fit**: once per printer (a persisted per-printer flag), on a rising edge while no print is active (or on the first idle moment after one), a notice that the accessory was detected and where its controls are: one modal with a button that opens the fan overlay. Then offer input-shaper recalibration ("After connecting the top cover, we recommend recalibrating vibration compensation...") with "Calibrate now" opening HelixScreen's existing input shaper calibration flow, and "Later". The stock screen's three slides collapse into one modal; parity is the information, not the slide count.
- **Removal**: on every falling edge, a warning modal with the stock wording and OK. During a print it shows immediately (the firmware has already stopped the fans and reset the mode).
- A presence reading at startup is not an edge: a printer that boots with the cover fitted and the flag unset still gets the first-fit notice once; a printer that boots without it gets no removal warning.

### 6. Resume check (parity)

When resuming a paused print and the provider reports Exhaust mode with the chamber temperature HelixScreen already resolves (on the U1, `temperature_sensor cavity`, which is also the purifier's `external_temp_sensor`) above `critical_chamber_temp`, ask "The chamber temperature is too high. This may cause extruder clogging. Continue printing?" before sending resume. Builder: find the existing resume path and its pre-resume checks; add this as a check there, not a second resume button.

### 7. Mock and docs

- Mock U1 purifier: honour `SET_PURIFIER_MODE`, `SET_PURIFIER` (speed, work reset), report `mode`, fan speeds, thresholds and an accruing `inner_fan_work_time` in modes 2 and 3; `HELIX_MOCK_ENCLOSURE_TOP=0/1` keeps working, and a runtime toggle (gcode or env) to unplug and replug the cover exercises the edges.
- `docs/devel/CHAMBER_HEATER.md` § chamber air modes: the extended provider contract and how to add a printer.
- `docs/devel/printers/SNAPMAKER_U1_SUPPORT.md`: replace "purifier NOT SUBSCRIBED" with the real field table and the stock-screen behaviour above. `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` for the mock toggle. This plan is deleted in the change that ships the work.

## Constraints

- Ships whole in a 1.1.x point release (decided 2026-11-05): branch from and land on `release/1.1` in one landing, then port to main.
- Firmware builds compile shared code as gnu++17 with `-fno-exceptions -fno-rtti`; `chamber_air` and `maintenance_meters` are shared code.
- Declarative UI rules and design tokens (`.claude/rules/declarative-ui.md`); threading rules for the status observers (`.claude/rules/threading.md`).
- US English strings through `lv_tr`, added to the translation pipeline.
- Hardware verification is not possible here (no official cover). Verification is the mock plus the Discord reporter on a beta build; the issue keeps `needs-hardware` until they confirm.

## Left to the builder

- Internal shape of the extended provider row and the exact function signatures.
- Dial presentation of the floor and disabled-fan state.
- Which settings page hosts the targets row and its controls.
- Modal layout and copy edits beyond the stock wording.
