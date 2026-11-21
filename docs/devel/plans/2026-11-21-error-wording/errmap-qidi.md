# QIDI firmware error map

Scope: QIDI-specific user-facing errors on Q1 Pro, Plus 4, Q2, Max 4, Plus 5 and the QIDI Box. Stock Klipper errors are a separate effort; only places where QIDI changed stock wording are listed here. Everything below was read from source or from string tables of QIDI's compiled modules, unless marked **unverified**. No code from the cloned trees was run; no printer was touched.

Source root: `/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/71793b24-69aa-4392-a9a2-57deecc9232b/scratchpad/src-qidi/` (called `src-qidi/` below). Extracted string tables: `scratchpad/strings/`.

## 1. Firmware lineage per model, sources read

| Model | Published source | Klipper base | Box code available as | Notes |
|---|---|---|---|---|
| Plus 4 | `QIDITECH/QIDI_PLUS4` (README says V2.1.1) pins `QIDITECH/klipper@PLUS4` 4bb7c63 "V1.7.1" (2025-08-06) and `QIDITECH/moonraker@PLUS4` | ~v0.12 era, MKS PI host, TJC serial panel | Cython `.so` (`box_extras`, `box_stepper`, `box_rfid`, `box_detect`, `aht20_f`) in `klipper-PLUS4/`; plain Python of an older Plus 4 build from issue #954 attachments in `src-qidi/issue954-plus4-py/` | Published Klipper lags the shipped firmware (1.7.1 vs 2.1.1). Panel firmware `UI/xindi_800_480.tft` has its own English fault wording (strings only). |
| Q1 Pro | `QIDITECH/QIDI_Q1_Pro` (README V6.1.1) pins `QIDITECH/klipper` 5a41ce0 "V4.4.21" (2024-07-19), cloned to `klipper-Q1pin/`; branch tip in `klipper-Q1_Pro/` | pre-v0.12 | none | README for 6.1.1 mentions the Box and `QDC_100_005_000_001`, but no Box code for Q1 Pro is published. Q1 Pro Box behavior **unverified** (presumably the Plus 4 modules). |
| Q2 | `QIDITECH/QIDI_Q2` main 3f6bcc8 (2026-01-05), klipper + moonraker vendored in tree | newer upstream (has `error_mcu.py`, `ldc1612`, `load_cell`), new-gen board, RK host, `qidi-client` LVGL screen | Cython `.so` in `QIDI_Q2/klipper/klippy/extras/` | Most complete coded set (QDE_002, QDE_004, QDE_005). |
| Max 4 | `QIDITECH/QIDI_MAX4` main 47fc14c (2026-08-06) | same new-gen base as Q2 plus `closed_loop.so`, `cl_interface.so`, `upgrade_motor.so`, D-Bus IPC to `qidi-client` | **not published**: `box_config.py` imports `box_stepper`/`box_rfid`, and `printer.cfg` declares `[multi_color_controller]`, but neither module is in the repo | QDE_002/QDE_005 present in Python. Box fault codes **unverified** on Max 4 (likely the QDE_004 family, since `box_config.py` builds the same `BoxExtruderStepper` class). Macros are a fork of klipper-macros (`config/klipper-macros-qd/`), whose errors are not QIDI's own. |
| Plus 5 | **no QIDITECH repo** | **unverified**: same board generation as Q2/Max 4 per `docs/devel/printers/QIDI_SUPPORT.md` | none | Treat as Q2/Max 4 family until a source or a debug bundle says otherwise. |

Also read: `QIDITECH/QIDIStudio` (sparse: `src/slic3r/GUI`), which has no QDE/QDC table (its HMS code is Bambu's, unused for QIDI Moonraker printers). Upstream Klipper v0.12.0 and v0.13.0 were cloned only as a diff baseline. HelixScreen files read: `docs/devel/printers/QIDI_SUPPORT.md`, `src/printer/ams_backend_qidi.cpp`, `src/application/gcode_error_router.cpp`, `src/application/error_classify.cpp`, `include/error_event.h`, `include/firmware_fault_codes.h`, `src/printer/firmware_fault_codes.cpp`, `src/printer/klipper_error_table.cpp`, `include/snapmaker_exceptions.h`.

## 2. How errors reach a client

QIDI has **one numeric scheme in Klipper**: `QD<k>_<module>_<index>`, three-digit groups.

- `QDE_` = error raised by Klipper extras. Modules seen: `002` fans, `004` QIDI Box / filament path, `005` heater verification.
- `QDC_` = codes from QIDI's closed components (`qidi-client`, firmware-only Moonraker components such as the MQTT link), with four groups (`QDC_100_005_000_001`, `QDC_400_004_010_001`). They appear in QIDI READMEs and the Moonraker log, never in published Klipper source. Whether any reaches a Moonraker websocket client is **unverified** (believed log/screen only).

Wire format: `Code:QDE_004_001; Message:<English text>`. Variants seen in source: no space after `;` (`Code:QDE_002_002;Message:`), and one bare form `!! QDE_004_010 The current feeding status ...` (no `Code:`/`Message:` labels, `box_extras.handle_connect`).

Channels, per raise site:

1. **`respond_raw("!!Code:...")`**: a `notify_gcode_response` line starting `!!` with **no space** before `Code:`. HelixScreen's `parse_gcode_error_line` accepts this as a Bang line.
2. **`respond_info("Code:...")`**: the console line is `// Code:QDE_...`. This is the most common box channel (QDE_004_005/006/007/009/011/013/015/016/017/018/022/023, QDE_002_001, QDE_005_001). HelixScreen treats `//` as a comment and **never classifies these lines**. They are the fault codes most likely to come with a PAUSE.
3. **`respond_raw("Code:...")` with no prefix**: `controller_fan` QDE_002_002 (chamber). Also ignored by HelixScreen today.
4. **`invoke_shutdown(msg)`**: `heater_fan` QDE_002_002 (hotend fan) shuts Klipper down; the coded text becomes Klipper's `state_message` (`printer.info`, `webhooks.state_message`, `notify_klippy_shutdown`). The stock shutdown path carries it.
5. **Standing state in `save_variables.variables`** (already subscribed by `AmsBackendQidi`):
   - `retry_step`: set alongside nearly every QDE_004 raise to `QDE_004_<idx>_<slot#>` (slot-scoped: 001-007, 009) or `QDE_004_<idx>_00` (global). Cleared to `None` by `TRY_MOVE_AGAIN` and, on Max 4 only, at print start (`klipper-macros-qd/start_end.cfg:89`). **Persisted to disk**: it survives restarts and can stand stale on Q2/Plus 4 because nothing else clears it. Note one source bug: QDE_004_020 stores `QDE_004_019_00`.
   - `slot<N>`: set to `-1` on slot load/unload failures (QDE_004_001/002/003, and 007) and `-2` on extruder load/unload failures (QDE_004_006/009). HelixScreen maps any negative value to `SlotStatus::BLOCKED`.
   - `qdc_ai_error_code`: `foreign=1 status=1 time=...` from M4050 foreign-object detection; the detection body is dead code in Q2/Max 4 published source (early `return`), so **unverified** in the field.
6. **`print_stats` error**: `M4050 execution failed[: <e>]` via `note_error` (Q2, Max 4) ends the job with `print_stats.state = error`.

Pause behavior is decided at the raise site: several QDE_004 raises call `PAUSE` only when `print_stats.state == "printing"`; outside a print they leave the box idle with the slot negative. QIDI's own recovery is `TRY_MOVE_AGAIN` (re-runs the failed step from `retry_step`, then `RESUME_PRINT TOOL_CHANGE=1` if nothing new failed). It exists in both Plus 4 and Q2 `box_extras.so`.

## 3. Error tables

Columns: **Ch** = channel from section 2 (`!!` raw bang, `//` respond_info, `raw` unprefixed, `SD` shutdown, `var` save_variables). **Models**: P4 = Plus 4 (`.so` v1.7.1 plus issue #954 source), Q2, M4 = Max 4, Q1 = Q1 Pro, P5 = Plus 5. **HS** = does HelixScreen word it today. `%s` slot labels are `1A`..`4D` (box 1-4, lane A-D).

### 3a. QIDI Box: QDE_004 (box_stepper / box_extras)

| Code | Firmware text | Raised in | Ch | Print effect | Models | Plain cause | User fix | HS |
|---|---|---|---|---|---|---|---|---|
| QDE_004_001 | Slot loading failure, please check the trigger, please reload %s. | `box_stepper.slot_load` | `!!` + var `_<n>`, slot -1 | Refused while printing (op only runs idle) | P4, Q2, M4 unverified | Filament fed into the slot never reached the Box's feed sensor (endstop), or it triggered unreliably over 5 tries. | Re-insert the filament into slot %s until the feeder grabs it, check that the end is cut clean and the lane sensor is not stuck; then retry. | Partial: generic "slot blocked, manual intervention required" from the -1 state, and the raw `!!` text as "Printer Error" |
| QDE_004_002 | Extruder has been loaded, cannot load %s. | `box_stepper.slot_load` | `!!` + var, slot -1 | idle only | P4, Q2 | Another filament is already in the toolhead, so the Box refuses to feed a slot. | Unload the current filament first (Unload), then load %s. Retry unloads then loads. | Partial (as above) |
| QDE_004_003 | Slot unloading failure, please unload %s again. | `box_stepper.cmd_SLOT_UNLOAD` | `!!` in .so (`!Code:` single bang in #954 source, which no client treats as an error) + var, slot -1 | idle only | P4, Q2 | Retracting the filament out of the Box did not clear the slot sensor: tangled spool, stuck or snapped filament. | Free the spool and pull the filament out of slot %s by hand, then retry. | Partial |
| QDE_004_004 | Please unload extruder first. | `cmd_SLOT_UNLOAD` | `!!` + var | idle only | P4, Q2 | Asked to eject the slot that is still loaded into the toolhead. | Unload from the toolhead first; Retry does both. | No |
| QDE_004_005 | Please load the filament to %s first. | `cmd_EXTRUDER_LOAD` | `//` + var | **PAUSE** (always) | P4, Q2 | The slot chosen for a tool change has no filament at its inlet sensor. | Load filament into slot %s, then Retry / Resume. | No (line ignored) |
| QDE_004_006 | Extruder loading failure. | `cmd_EXTRUDER_LOAD` | `//` + var, slot -2 | PAUSE if printing | P4, Q2 | Filament left the Box but never reached the toolhead's filament sensor: PTFE gap, clog at the toolhead entry, cutter fragment. | Check the PTFE tube from the Box to the toolhead and the toolhead entry, clear any cut tip, then Retry (reheats and reloads). | Partial (-2 shows "blocked") |
| QDE_004_007 | Extruder not loaded. | `cmd_EXTRUDER_UNLOAD` | `//` + var, slot -1 | PAUSE if printing | P4, Q2 | Asked to unload but the Box's buffer sensor says no filament is in the path. | Usually informational; check the toolhead is really empty, then Resume. | Partial |
| QDE_004_008 | Extruder unloading failure. | `box_stepper` (Q2 .so only; site unknown) | `//` (assumed) | **unverified** | Q2 | Same text as 009; context **unverified**. | As 009. | No |
| QDE_004_009 | Extruder unloading failure. | `cmd_EXTRUDER_UNLOAD` | `//` + var, slot -2 | PAUSE if printing | P4, Q2 | Retracting from the toolhead to the Box never cleared the buffer sensor (after up to 3 slowed retries): filament not cut through, swollen tip, tangle. | Check the cutter cut fully, pull the filament back by hand if needed, then Retry. | Partial |
| QDE_004_010 | The current feeding status is incorrect. Please exit the filament from the extruder. | `box_extras.handle_connect` / `delayed_init_error_raw` | `!!` twice (bare `!! QDE_004_010 ...` form, then `!!Code:` form 3 s later) + var `_00` | At startup | P4, Q2 | After a restart the saved "synced slot" names a Box lane that no longer exists (Box removed or count changed). | Unload the filament from the toolhead by hand, then reconnect the Box or restart. | No |
| QDE_004_011 | Detected that filament have been loaded, please unload filament first | `cmd_SLOT_RFID_READ` | `//` + var `_00` | none | P4, Q2 | An RFID read asks to move filament but the toolhead is still loaded. | Unload, then rescan the tag. | No |
| QDE_004_013 | Detected wrapping filament,please check the filament. | `box_extras.motion_tips` (P4 `MOTION_TIPS`), `box_stepper` (Q2) | `//` + var `_00` | Pause comes from the motion sensor's own runout (**unverified** wiring) | P4, Q2 | The Box motion sensor saw no movement while feeding: spool tangled or wound over itself. | Untangle the spool or rewind it, then Resume. | No |
| QDE_004_014 | Parameter setting error, please reset. | `box_extras.cmd_ENABLE_BOX_DRY` (uncoded in #954 source) | `//` | none | Q2 (coded), P4 (uncoded text) | Drying started with zero time or zero temperature. | Set a drying temperature and duration, then start again. | No |
| QDE_004_015 | Box self-inspection failed, please try manually withdrawing the fed filament. | `cmd_SELF_INSPECTION` | `//` + var `_00` | none (runs at boot) | P4 (not in Q2 .so) | At startup the toolhead has filament but the Box could not work out which slot it came from. | Pull the filament out of the toolhead by hand (or set the loaded slot), then reload. | No |
| QDE_004_016 | The filament has been exhausted, please load the filament to %s. | `cmd_EXTRUDER_UNLOAD` (runout while printing) | `//` + var `_00` | **PAUSE** | P4, Q2 | Slot %s ran out mid-print and auto-refill is off. | Load a new spool into %s (or enable auto refill with a matching spool), then Resume. | No (runout may also show via the stock sensor path) |
| QDE_004_017 | Filament flush failed, please clean and then load the filament in %s. | `flush_all_filament` | `//` + var `_00` | **PAUSE** | P4, Q2 | During auto-refill the old filament would not purge out of the toolhead after 50 tries. | Clear the nozzle and purge chute, load %s, then Resume. | No |
| QDE_004_018 | No filament specified, %s cannot be automatically replaced. | `switch_next_slot` | `//` + var `_00` | Run-out pause follows (**unverified** ordering) | P4, Q2 | Auto-refill needs vendor, color and type set for the empty slot, and they are not. | Set the slot's filament info, or load the same slot by hand. | No |
| QDE_004_019 | Please check if your PTFE Tube is bent | `cmd_SLOT_RFID_READ` | `!!` + var `_00` | none | P4, Q2 | During an RFID scan feed, the filament stalled before the sensor while still present at the inlet: bent or kinked tube. | Straighten the PTFE tube for that slot and rescan. | No |
| QDE_004_020 | Detected that the filament has been unloaded, please reload. | `cmd_SLOT_RFID_READ` | `!!` + var (stored as `QDE_004_019_00`) | none | P4, Q2 | During an RFID scan the filament left the inlet sensor. | Re-insert the filament and rescan. | No |
| QDE_004_021 | Unable to recognize loaded filament. | `button_extruder_load` (`E_LOAD`) | `!!` + var `_00` | Load aborted, nozzle heat off | P4, Q2 | A load was requested while the toolhead holds filament the Box has no record of. | Unload the toolhead by hand, then load again. | No |
| QDE_004_022 | No replaceable slot found. | `switch_next_slot` | `//` + var `_00` | Run-out pause stands | P4, Q2 | Auto-refill found no other slot with the same vendor, color and type. | Load a matching spool, or load any slot by hand and Resume. | No |
| QDE_004_023 | Auto reload failed. | `cmd_AUTO_RELOAD_FILAMENT` | `//` + var `_00` | Run-out pause stands | P4, Q2 | Auto-refill could not start (buffer sensor empty or no next slot). | Load filament by hand and Resume. | No |
| QDE_004_025 | Extruder unloading failure. | `box_stepper` (Q2 .so only) | `//` (assumed) | **unverified** | Q2 | Context **unverified**. | As 009. | No |
| (gaps) | QDE_004_012, 024 | not found in any source | | | | | | |

Uncoded Box messages (no `Code:`), same files:

| Text | Raised in | Ch | Models | Cause / fix | HS |
|---|---|---|---|---|---|
| `%s did not recognize the filament` / `Unrecognized label read in %s` | `box_rfid` | `//` | P4 (first), P4+Q2 (second) | RFID tag missing, unreadable or not a QIDI tag. Set the filament by hand. | No |
| `Extruder unloading failure.Please try again.` | `button_box_unload` (`E_BOX`) | raw | P4 | Eject from toolhead failed before ejecting the slot. Retry. | No |
| `!!Invalid BOX number. Available: 1-N` | `ENABLE_BOX_DRY` | `!!` | P4, Q2 | Drying asked for a Box that is not connected. | Raw text only |
| `!!Error in heating: <e>` / `!!Error setting slot<n>: <e>` | drying timer / setter | `!!` | P4, Q2 | Internal drying fault; dryer stops. Restart drying; report if repeated. | Raw text only |
| `buffer state error, auto reset buffer`, `Box disabled, cannot set buffer state.`, `No slot selected, cannot init buffer state.` | `box_extras` (Q2 buffer) | unknown (**unverified**) | Q2 | Buffer sensor disagreement; firmware self-resets. | No |
| `filament num error.` / `officiall_filas_list file error.` | `get_temp_by_*` | raw | P4, Q2 | Slot's filament id is not in `officiall_filas_list.cfg`, or that file is damaged (the README's QDC_100_005_000_001 advice: restore factory settings, reconfigure Box filament in QIDI Studio, never edit Box filament from Fluidd). | No |
| `Unable to save variable <k> as <v>` | `box_extras.save_variable` | command error | P4, Q2 | `saved_variables.cfg` not writable (disk full or permissions). | Raw |
| Box plug or unplug | `box_detect` rewrites `box.cfg` and calls `request_exit('error_exit')` | Klipper restarts | P4, Q2 | Expected behavior, not a fault; mid-print it ends the print through the stock "lost communication with MCU 'mcu_box<N>'" path (**unverified** wording on QIDI). | stock path |

### 3b. Fans and heaters: QDE_002, QDE_005

| Code | Firmware text | Raised in | Ch | Print effect | Models | Plain cause | User fix | HS |
|---|---|---|---|---|---|---|---|---|
| QDE_002_001 | `{"fan_name":"<fan>"} speed{"<rpm>"} may be error, please check!` | `fan.py._rpm_watchdog_callback` | `//` | none (warning) | Q2, M4 (not P4 1.7.1) | A fan was commanded on but its tachometer read 0 RPM for `rpm_timeout` seconds. | Check that fan for a loose plug, debris or a dead motor. | No |
| QDE_002_002 (hotend) | `{"fan_name":"<fan>"} speed check error ,turn off {"heater":"[...]"}` | `heater_fan.py.callback` | **SD** (`invoke_shutdown`), heater set to 0 | **Print stops; Klipper shuts down** | Q2, M4 | The hotend cooling fan stopped while the hotend was hot, so the firmware killed heat to protect the heatbreak. | Check the hotend fan and its connector, then FIRMWARE_RESTART. | Shown only as the stock shutdown text (raw code visible) |
| QDE_002_002 (chamber) | same, `;Message:` without space | `controller_fan.py` | raw (unprefixed) | chamber heater off, print continues | Q2, M4 (and Box board fan on M4 via `box_config`) | The chamber or Box-board fan stopped; heater turned off. | Check that fan; re-enable chamber heat afterwards. | No (line ignored) |
| QDE_005_001 | `Heater heater_box<N> not heating at expected rate` | `verify_heater.box_heater_fault` | `//` | Box heater set to 0; print continues | P4, Q2, M4 | The QIDI Box dryer heater could not reach its target (open lid, failed PTC, or cold room). Stock Klipper would shut the printer down here; QIDI only stops the dryer. | Close the Box lid, check the Box power, restart drying; repeated failures mean a heater or sensor fault. | No |

### 3c. Other QIDI-only Klipper errors

| Text | Raised in | Ch | Models | Cause / fix | HS |
|---|---|---|---|---|---|
| `!! Homing failed after 5 retries: <stock reason>` (after raw lines `Retrying homing (n/5) after error: ...`) | `probe.py` probe loop, `kinematics/corexy.py` home | `!!` then the original command error | Q2, M4 | QIDI wraps stock "No trigger on ... after full movement" with 5 silent retries. Same fix as the stock message; the prefix means the retries were already spent. | Stock wording only if the stock table matches the substring |
| `SECOND_HOME: retries over limit[N], fail` | `extras/homing.py` | command error | M4 | Second (fine) homing touch kept failing. Check the axis for obstruction, then home again. | No |
| `Homing failed due to printer shutdown` | `stepper.cmd_REVERSE_HOMING` | command error | P4, Q2, Q1 | Shutdown happened during reverse Z homing; fix the shutdown cause. | Stock-like |
| `pei detecting error` | `probe.cmd_TESTING_PEI` | command error | Q2, M4 | Build plate (PEI sheet) presence probe did not trigger: plate missing or misplaced. Seat the plate. | No |
| `Toolhead moved during probe activate_gcode script` (and deactivate) | `qdprobe.py` | command error | P4, Q1 | QIDI probe macro moved the head. Stock-like wording. | stock |
| `Requested temperature (T) out of range (min:max)` for `heater_generic chamber` | `heaters.py` | command error | Q2, M4 | Chamber limits come from QIDI's `target_min/max_temp`. Stock wording. | stock |
| `The hotbed temperature is too low, and it will automatically heat up to N` | `heaters.py` | `//` | Q2, M4 | Informational: chamber heating raises the bed. | n/a |
| `M4050 execution failed[: <e>]` | `virtual_sdcard` print start | `print_stats` error, `on_error_gcode` | Q2, M4 | Pre-print foreign-object check (AI camera) errored and the print was cancelled. Retry; disable the pre-print check if it repeats. | No |
| `weight_error` MCU shutdown hint: `CS传感器有问题，请检查一下接线！` ("CS sensor problem, check the wiring") | `error_mcu.py` added hint | SD message tail | Q2, M4 | Load-cell (CS1237) bed probe fault. Check the probe/load-cell cable. Hint is Chinese only. | No |
| `probe_weigh sensor outage`, `Failed calibration - ...` | `air.cpython-39-*.so` | unknown (**unverified**) | Q2, M4 | Weigh-probe sensor lost / calibration failed. | No |
| Closed-loop stepper alarms: `Close loop stepper '<x>' ... encoder alarm, please check encoder connection or status`, `stepper overcurrent alarm ...`, `stepper overvoltage alarm ...`, `stepper undervoltage alarm ...`, `stepper phase loss alarm ...`, `stepper out-of-tolerance alarm ...`, `high temp alarm ...`, `motor temperature higher than 100/110/120 C, please check the motor status`, `status request timeout, no response received`, `Cannot get verson of Close loop stepper '%s' after multiple attempts, check the stepper connection and reboot the machine` | `closed_loop.so` | **unverified** (compiled; fragments are concatenated at run time) | M4 | Max 4 X/Y closed-loop drivers. Each names its own fix; the plain version is: check that motor's cable and power, let it cool, reboot. | No |
| QDC_100_005_000_001 | (no text published) | closed component | P4 2.1.1, Q1 6.1.1 | QIDI README: Box filament config corrupted. Unload, restore factory settings, reconfigure Box filament in QIDI Studio. Client visibility **unverified**. | No |
| QDC_400_004_010_001 `MQTT account/password/ip decryption returned EMPTY result` | closed Moonraker component | Moonraker log | Q2, M4 (field-observed) | `qidi-client` disabled. Already documented in QIDI_SUPPORT.md; not a Moonraker client error. | Doc only |

Stock-wording changes worth telling the Klipper effort about: heater verification on Box heaters no longer shuts down (QDE_005_001 instead); `verify_heater` ignores the error above a configured `position_z`; hotend fan failure is a new shutdown cause; probe and CoreXY homing retry 5 times before raising the stock message.

## 4. Recommended rows to word first

1. **Make `// Code:QDE_...` lines classify.** The highest-value fix is not a row: 13 of the 23 Box codes arrive as `//` lines that HelixScreen drops, and that set includes every Box code that pauses a print (005, 006, 007, 009, 016, 017). Without this, the user sees a paused print with no reason.
2. QDE_004_005, 006, 009, 016, 017 (the mid-print pause causes, in roughly that order of field frequency, **unverified** frequency).
3. QDE_004_001, 002, 003 (idle load/unload faults that today read "slot blocked, manual intervention required" with no cause).
4. QDE_002_002 hotend (a shutdown; the user needs "hotend fan stopped" not a JSON fragment) and QDE_005_001 (dryer stopped quietly).
5. QDE_004_010 / 015 / 021 (startup and manual-load states that block every later operation).
6. Offer QIDI's own `TRY_MOVE_AGAIN` as the Retry action on QDE_004_001-007/009 when `retry_step` names that code. It reheats where needed and resumes the print itself.

## 5. Matching strategy

- Follow the Snapmaker model (`include/snapmaker_exceptions.h`): a `qidi_fault_codes` module with decode, format and per-code summary + cause + fix, plugged in as a provider in `src/printer/firmware_fault_codes.cpp`. The code is the identity; the English text changes between releases (`!Code` vs `!!Code`, `;Message` spacing, QDE_004_014 gaining a code).
- Decode with one pattern that ignores prefix and labels: `QD([A-Z])_(\d{3})_(\d{3})` anywhere in the line, optional `Code:` before, optional `;\s*Message:` after. This covers `!!Code:`, `// Code:`, the bare `!! QDE_004_010 ...` form, the unprefixed chamber line and a shutdown `state_message`. Use `helix::Regex` or a hand scanner (no `std::regex`).
- Accept the `//` and unprefixed channels **only for lines carrying a QD code**, so ordinary comments still never classify. That means the provider must see lines before `parse_gcode_error_line` rejects them; today `faultcodes::classify` runs after that rule (`src/printer/firmware_fault_codes.cpp`, line using `parse_gcode_error_line`).
- Severity: derive from print state plus a per-code table (pause codes CRITICAL while paused/printing; QDE_002_001 WARNING; QDE_002_002 hotend is a shutdown and goes through the shutdown presenter). There is no level field in QIDI codes, unlike the U1.
- Slot locator: the `%s` slot label (`1A`..`4D`) is in the message; for slot-scoped codes `retry_step`'s suffix is the global slot number (`_00` means none). Map either to `lane_label()`.
- Standing faults: `read_standing_faults` can read `save_variables.variables.retry_step`, but only surface it when it agrees with live state (a negative `slot<N>`, or `print_stats.state == paused`), because it is persisted and is not cleared on Q2/Plus 4 except by `TRY_MOVE_AGAIN`. A stale value after a reboot must not open a modal.
- Dedup: one raise produces the console line, the `retry_step` change and a negative slot state. `AmsBackendQidi::current_error()` (the "blocked" modal) should defer to the coded event when one names the same slot, or take its wording from it.
- Firmware gate: `firmware_reports_fault_codes()` for QIDI = `AmsType::QIDI_BOX` or any QIDI printer detection, since QDE_002/005 fire without a Box.

## 6. Gaps and open questions

- Max 4 Box (`multi_color_controller`, `box_stepper`, `box_rfid`) is not published. Its codes and channels are **unverified**; a Max 4 debug bundle with a forced load failure would settle it (#1067 is the related hardware issue).
- Plus 5 has no published source at all.
- Plus 4 published Klipper is 1.7.1 while shipping firmware is 2.1.1 ("new BOX firmware"); codes may have been added. Q1 Pro 6.x Box code is not published.
- QDE_004_008 and 004_025 call sites are unknown (compiled, Q2 only). QDE_004_012 and 024 were not found anywhere.
- Whether QDC_ codes ever reach a Moonraker websocket client, or only the `qidi-client` screen and logs, is unverified.
- The qidi-client (Q2/Max 4/Plus 5) and TJC panel (Plus 4/Q1 Pro) own QIDI's screen wording. The Plus 4 panel strings (`UI/xindi_800_480.tft`) include "Box self-test failed", "Extruder synchronization failure", "Detected tangled filament or blocking", "Automatic filament renewal failed", "feeding filament failed", "ejecting filament failed", "RFID read failed" (Portuguese copy seen), "BOX Offline". Which code maps to which panel string is not recoverable from strings alone.
- Pause coupling for QDE_004_013/018/022/023 depends on the run-out sensor macros and their order; **unverified** on hardware.
- HelixScreen has no QIDI hardware, so nothing here is confirmed against a live log. The quickest confirmation is any QIDI Box debug bundle grepped for `QDE_`.
