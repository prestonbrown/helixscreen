# Klipper / Kalico user-facing error map

Sources read (shallow clones, parsed only, nothing executed):
- Klipper `c9559fa` (2026-11-20), Kalico `4b15247` (2026-11-18), Moonraker HEAD, under
  `/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/71793b24-69aa-4392-a9a2-57deecc9232b/scratchpad/src-klipper/`.
- Every raise site was extracted with an AST walk (`scratchpad/tools/extract.py`, output
  `scratchpad/tools/klipper.tsv` (569 rows) and `kalico.tsv` (741 rows); `kalico_only.msgs` lists the
  238 strings Kalico has that Klipper does not). Rows below are the user-facing subset.
- HelixScreen read-only at `feature/color-picker-v2` (`ecac859a72`).

Anything not seen in source is marked **unverified**. Frequency is my judgment (H/M/L).

---

## 1. How errors reach a client

### 1.1 The five channels

| # | Channel (as HelixScreen sees it) | Raised by | Shape | Example |
|---|---|---|---|---|
| A | `notify_gcode_response` line starting `!! ` | any `gcmd.error` / `printer.command_error` / `config.error` raised inside a gcode handler (`klippy/gcode.py` (`_respond_error`)) | **Only the first line** of the message goes out as `!! <line>`. If the message has more lines, the WHOLE message is sent first as `// line` info lines, then the `!!` line. | `// Extrude below minimum temp` / `// See the 'min_extrude_temp' config option for details` / `!! Extrude below minimum temp` |
| B | JSON-RPC error on `printer.gcode.script` (and other Klippy endpoints) | same exception, returned through webhooks `WebRequestError` | Moonraker `ServerError(msg, 400)`, JSON-RPC `{"code":400,"message":"<full multi-line text>"}`. Moonraker's own: 503 `Klippy Host not connected`, 503 `Klippy Disconnected`, 500 `Klippy request timed out` | `{"code":400,"message":"Must home axis first: 120.000 120.000 10.000 [0.000]"}` |
| A+B | Both at once | a script sent over RPC | The `!!` console copy AND the RPC error both arrive (gcode.py raises after `_respond_error` when `need_ack=False`). HelixScreen already dedups this pair (`rpc_error_correlation`, `raw_detail`). | |
| C | `webhooks.state` + `webhooks.state_message` (status subscription, `printer.info`), plus `notify_klippy_shutdown` / `notify_klippy_disconnected` / `notify_klippy_ready` | `printer.invoke_shutdown(msg)` (state `shutdown`), or a failed connect/config load (state `error`, or stuck `startup`) | `state` in `ready / startup / shutdown / error`. `state_message` is multi-line prose; for shutdown it is `<reason>` + optional detail + a canned trailer (1.2). | `MCU 'mcu' shutdown: Timer too close\nThis often indicates the host computer is overloaded...\nOnce the underlying issue is corrected, use the\n"FIRMWARE_RESTART" command ...\nPrinter is shutdown` |
| D | `print_stats.state == "error"` + `print_stats.message` | any gcode error while `virtual_sdcard` is printing a file (`extras/virtual_sdcard.py` (`work_handler`)) | `message` is `str(e)`: the full multi-line text, not just line 1. `on_error_gcode` then runs (default `TURN_OFF_HEATERS`, both forks). | `print_stats: {state:"error", message:"Move out of range: 251.000 100.000 0.300 [12.400]"}` |
| E | `// ` info lines | `respond_info` | Not errors, but several user-visible "errors" are only info. | `// Unknown command:"M600"`, `// Print is not paused, resume aborted`, `// BLTouch failed to verify sensor state; retrying.` |

### 1.2 Canned trailers appended to `state_message` (identical in Kalico)

- Host shutdown, non-MCU reason (`extras/error_mcu.py` (`_handle_analyze_shutdown`)): `<msg>` + `"\nOnce the underlying issue is corrected, use the\n\"FIRMWARE_RESTART\" command to reset the firmware, reload the\nconfig, and restart the host software.\nPrinter is shutdown\n"`.
- MCU shutdown (`_check_mcu_shutdown`): `MCU '<mcu>' shutdown: <reason>` (or `Previous MCU '<mcu>' shutdown: <reason>` when Klippy reconnects to an MCU that was already shut down), then `\n\n<clarify lines>\n` if a module registered one, then the generic hint from `Common_MCU_errors` (prefix match on reason), then the FIRMWARE_RESTART trailer.
- Config error at startup (`klippy.py` (`_connect`)): `<error>` + `"\nOnce the underlying issue is corrected, use the \"RESTART\"\ncommand to reload the config and restart the host software.\nPrinter is halted\n"`; `webhooks.state = "error"`.
- MCU connect error: `<error>` + FIRMWARE_RESTART trailer ending `Error configuring printer`.
- Protocol (firmware version) error: a fixed multi-line block starting `MCU Protocol error` (1.3 row MCU-14).
- Startup: `Printer is not ready\nThe klippy host software is attempting to connect.  Please\nretry in a few moments.`

### 1.3 Behaviors that matter for matching

1. **After a shutdown every command echoes the reason.** `gcode.py` (`cmd_default`) answers any command sent while not ready with `!! <first line of state_message>`, so the console fills with `!! MCU 'mcu' shutdown: ADC out of range` repeats (and `!! Printer is not ready` during startup). Key the shutdown off channel C, and treat a matching `!!` while `state != ready` as an echo.
2. **Mid-print command error = print aborted, not paused.** Channel D, heaters off. The only "pause" paths are `pause_on_runout`, an explicit `PAUSE`, or a macro that calls it.
3. **Shutdown mid-print:** `print_stats.py` has no shutdown handler (seen), so `print_stats.state` stays `printing` while `webhooks.state` goes `shutdown`. Moonraker history closes the job as `klippy_shutdown` (`history.py` (`_handle_shutdown`)).
4. **`M112` / Moonraker `printer.emergency_stop`** shut down with `Shutdown due to M112 command` / `Shutdown due to webhooks request`. HelixScreen's own e-stop produces the second. Other MCUs then shut down with `Command request`, but the host message is already set (first wins).
5. **Config errors never reach `!!`.** They only appear in `state_message` (state `error`), except `SAVE_CONFIG` and template-syntax errors raised at command time.
6. **Kalico** keeps every channel and trailer byte-identical. Its source is black-formatted, so wording comparisons were done on parsed strings, not grep.

---

## 2. Error tables by subsystem

Columns: **Ch** = channel (A command error, C-sd shutdown, C-cfg startup error, D also print abort, E info only).
**Effect** = what the printer does. **Freq** = H/M/L. **Helix** = what HelixScreen does today:
- `raw` : the generic path. `!!` goes through `error_classify::classify`; text is only `clean_error_text`'d (CRITICAL modal "Printer Error" if printing/paused, else a toast). No wording.
- `dlg` : shutdown/error `state_message` shown verbatim in the Klipper recovery dialog (`src/ui/ui_emergency_stop.cpp` (`update_recovery_dialog_content`)).
- `cfg` : `dlg` plus the "Klipper Config Error" title and a fix-the-file preface (`src/printer/printer_network_state.cpp` (`is_klippy_config_error`)).
- `rw` : rewritten text (`src/application/gcode_error_text.cpp`).
- `jr` : event-journal class only (`src/printer/printer_event_journal.cpp` `kClassMarkers`), no text.
- `K2` : worded only when a Creality K2 sends it as a `keyNNN` envelope (`src/printer/klipper_error_table.cpp`), not as prose.

Placeholders: `{heater}` config section name (`extruder`, `heater_bed`, `heater_generic chamber`), `{mcu}` mcu name (`mcu`, `EBBCan`, `rpi`), `{stepper}` stepper section (`stepper_x`, `extruder`), `{axis}` short endstop name (`x`, `z1`, `probe`), `{pos}` = `%.3f %.3f %.3f [%.3f]` (X Y Z [E]).

### 2.1 Heaters and thermal

| ID | Message pattern | Source | Ch | Effect | Freq | Plain cause | Fix at the printer | Helix |
|---|---|---|---|---|---|---|---|---|
| HT-1 | `Heater {heater} not heating at expected rate` + `\nSee the 'verify_heater' section in docs/Config_Reference.md\nfor the parameters that control this check.` | `extras/verify_heater.py` (`heater_fault`) | C-sd | Shutdown, heaters off, print lost | H | ONE message for two faults: the heater did not rise `heating_gain` (2 C) within `check_gain_time` (20 s hotend, 60 s bed), OR the temperature sat below target long enough to exceed `max_error` (120 degree-seconds), e.g. a fan blast or a fallen-out thermistor mid-print. | Check the heater cartridge and its wiring and the thermistor is seated in the block. A part-cooling fan blowing on the block, a missing silicone sock, or a new high-flow hotend can trip it; re-run PID tuning. Only then relax `[verify_heater {heater}]`. FIRMWARE_RESTART. | dlg, jr |
| HT-2 | `MCU '{mcu}' shutdown: ADC out of range` + `\n\nSensor '{heater}' temperature {t} not in range {min}:{max}\n` (clarify, only when the last reading is out of range) + `This generally occurs when a heater temperature exceeds\nits configured min_temp or max_temp.` | `src/adccmds.c` (MCU), `extras/adc_temperature.py` (`_clarify_adc_range`), `extras/error_mcu.py` | C-sd | Shutdown | H | The MCU itself saw a thermistor/ADC reading past `min_temp`/`max_temp`. Very low = thermistor unplugged or broken wire (open circuit reads as very cold); very high = shorted thermistor or a genuine overheat. A cold room below `min_temp` (often 0 C) also trips it. **unverified** (electrical reading, not source): open reads cold, short reads hot. | Read the temperature named in the second line. If it is far below room temperature: reseat or replace the thermistor and its connector. If very high: let it cool, look for a short or pinched wire. FIRMWARE_RESTART. | dlg, jr |
| HT-3 | `MCU '{mcu}' shutdown: Thermocouple reader fault` (Kalico adds clarify detail and the ADC hint) | `src/thermocouple.c` (MCU), Kalico `extras/spi_temperature.py` (`_clarify_fault`) | C-sd | Shutdown | M | MAX31855/31856/31865/6675 amplifier reported a fault or out-of-range reading. Klipper only logs which fault bit (`Max31865 RTD input is disconnected`, `MAX31855 : Open Circuit`, ...) to klippy.log; Kalico adds it to the message. | Check the thermocouple/PT100 leads at the amplifier board and the SPI wiring. FIRMWARE_RESTART. | dlg, jr |
| HT-4 | `Requested temperature ({t}) out of range ({min}:{max})` | `extras/heaters.py` (`set_temp`), `extras/temperature_fan.py` | A, D | Command rejected; mid-print aborts | H | A target above `max_temp` (or below `min_temp`) was requested, usually by slicer start gcode for a hotter filament. | Lower the temperature in the slicer/filament profile, or raise `max_temp` in `[extruder]` only if the hotend is rated for it. | raw |
| HT-5 | `{SENSOR} temperature {t} outside range of {min}:{max}` for `LM75`, `BME280`, `BME680`, `BMP180`, `SHT3X`, `HTU21D`, `AHT10`; `HOST temperature {t} below minimum temperature of {min}.` / `above maximum ...`; `COMBINED SENSOR ...` (3 forms) | `extras/lm75.py`, `bme280.py`, `sht3x.py`, `htu21d.py`, `aht10.py`, `temperature_host.py`, `temperature_combined.py` | C-sd | Shutdown | L | An auxiliary sensor (chamber, host CPU) left its configured min/max. Kalico words HOST as `HOST temperature {t} outside range of {min}:{max}`. | Check the sensor and its range in the config; for HOST, the Pi/host is overheating (add cooling). | dlg |
| HT-6 | `Error on 'TEMPERATURE_WAIT': missing MINIMUM or MAXIMUM.` | `extras/heaters.py` | A | Rejected | L | Macro bug. | Fix the macro. | raw |
| HT-7 | `pid_calibrate interrupted` | `extras/pid_calibrate.py` | A | PID run stops | L | Cancelled or interrupted. | Re-run. | raw |
| HT-K1 | Kalico: `Cannot activate '{name}' as MPC control is not fully configured. Run 'MPC_CALIBRATE' or ensure ...`; `Heater '{h}' is being calibrated`; `Ambient MPC {s} Temperature {t} outside range of ...` (shutdown); `MPC Heater Block {s} Temperature ...` (shutdown); `pid_profile: Unknown profile [{p}] for heater [{h}].` | Kalico `extras/control_mpc.py`, `heaters.py`, `mpc_*_temperature.py` | A / C-sd | | L | MPC heater control not calibrated, or a profile name typo. | Run `MPC_CALIBRATE HEATER={h}`; check `pid_profile` names. | raw / dlg |

### 2.2 MCU and communications

All MCU shutdowns arrive as `MCU '{mcu}' shutdown: <reason>` (or `Previous MCU '{mcu}' shutdown:`) plus hint plus trailer. FIRMWARE_RESTART clears them.

| ID | Reason text | Source | Ch | Effect | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|---|
| MCU-1 | `Timer too close` + hint `This often indicates the host computer is overloaded. Check for other processes consuming excessive CPU time, high swap usage, disk errors, overheating, unstable voltage, or similar system problems on the host computer.` | `src/sched.c`, hint `extras/error_mcu.py` | C-sd | Shutdown | H | The host (Pi/SBC) fell behind sending moves: CPU or swap pressure, a slow SD card, an undervolted Pi, webcam streaming, a heavy UI. Also very high step rates. | Reduce host load (stop webcam/timelapse/extra services), check the Pi power supply and SD card, then FIRMWARE_RESTART. If it repeats at one spot, lower microsteps or speed. | dlg, jr |
| MCU-2 | `Missed scheduling of next digital out event` / `... hard pwm event` / `... pca9685 event` + hint `This is generally indicative of an intermittent communication failure between micro-controller and host.` | `src/gpiocmds.c`, `src/pwmcmds.c` | C-sd | Shutdown | M | A heater/fan pin's `max_duration` safety window expired because the host's next update did not arrive in time (USB/CAN dropout, host stall). | Check the USB/CAN cable and termination, keep the cable away from stepper wires, check host load. FIRMWARE_RESTART. | dlg, jr |
| MCU-3 | `Rescheduled timer in the past` / `Stepper too far in past` + hint `This generally occurs when the micro-controller has been requested to step at a rate higher than it is capable of obtaining.` | `src/sched.c`, `src/stepper.c` | C-sd | Shutdown | M | Step rate too high for that MCU (high microsteps x high speed), or a host stall. | Lower `microsteps` or max speed/velocity; use interpolation. FIRMWARE_RESTART. | dlg, jr |
| MCU-4 | `Lost communication with MCU '{mcu}'` (host message, no `MCU shutdown:` prefix) | `klippy/mcu.py` (`check_timeout`) | C-sd | Shutdown | H | The host heard nothing from that MCU for several seconds: loose/failed USB or CAN cable, toolhead board brown-out, CAN bus not terminated, EMI. | Check the cable and connectors to the named board (often the toolhead CAN board), its power, CAN termination. FIRMWARE_RESTART. | dlg (`fault_modal_registry.h` notes the text) |
| MCU-5 | `MCU '{mcu}' spontaneous restart` | `klippy/mcu.py` (`_handle_starting`) | C-sd | Shutdown | M | That board rebooted on its own: power dip, brown-out, ESD, a short on a fan/heater. | Check the board's power supply and wiring for shorts. FIRMWARE_RESTART. | dlg |
| MCU-6 | `Watchdog timer!` | `src/*/watchdog.c` | C-sd | Shutdown | L | MCU firmware hung and the watchdog reset it. | Power-cycle; reflash firmware if it repeats. | dlg |
| MCU-7 | `Move queue overflow` | `src/basecmd.c` | C-sd | Shutdown | L | Too many queued steps for the MCU's memory. | Lower step rate; report as a bug if persistent. | dlg |
| MCU-8 | `Command request` + hint `This generally occurs in response to an M112 G-Code command or in response to an internal error in the host software.` | `src/basecmd.c` | C-sd | Shutdown | M | The host told the MCU to stop (M112, e-stop, or a host-side fault). Usually a secondary message; the host's own reason is the real one. | Find the earlier error in the console; FIRMWARE_RESTART. | dlg |
| MCU-9 | `MCU '{mcu}' I2C request to addr {n} reports error {NACK|START_NACK|START_READ_NACK|TIMEOUT}`; MCU reasons `I2C timeout occured`, `i2c timeout`, `I2C NACK error encountered!`, `Failed to send i2c start` | `extras/bus.py` (`i2c_write/read`), `src/*/i2c.c` | C-sd | Shutdown | L | An I2C device (sensor, display, expander) stopped answering. | Check the I2C device wiring/address. FIRMWARE_RESTART. | dlg |
| MCU-10 | `Shutdown due to M112 command` | `klippy/gcode.py` (`cmd_M112`) | C-sd | Shutdown | M | Emergency stop from a console/macro/slicer. | FIRMWARE_RESTART when safe. | dlg |
| MCU-11 | `Shutdown due to webhooks request` | `klippy/webhooks.py` | C-sd | Shutdown | M | E-stop pressed in a UI (Moonraker `printer.emergency_stop`, HelixScreen's own e-stop). | FIRMWARE_RESTART when safe. | dlg (Helix knows it caused it) |
| MCU-12 | `Shutdown due to {msg}` (default `action_emergency_stop`) | `extras/gcode_macro.py` (`_action_emergency_stop`) | C-sd | Shutdown | L | A macro deliberately stopped the printer; text after "due to" is the macro's own. | Read the macro's reason. | dlg |
| MCU-13 | `mcu '{mcu}': Unable to connect` (+ connect trailer `Error configuring printer`); also `mcu '{mcu}': No communication with MCU`, `Invalid CAN uuid` | `klippy/serialhdl.py` | C-cfg | Klipper never ready | H | The serial path / CAN uuid in `[mcu]` is wrong, the board is unpowered or unflashed, or the cable is unplugged. | Check `serial:` / `canbus_uuid:` in printer.cfg (`ls /dev/serial/by-id/`), power and cable, then FIRMWARE_RESTART. | `is_klippy_mcu_connect_error` matches `mcu '` + `Unable to connect`; it also matches `Timeout on connect`, which Klipper only logs (`serialhdl.py:100`) and never puts in `state_message` |
| MCU-14 | `MCU Protocol error` + `This is frequently caused by running an older version of the firmware on the MCU(s). Fix by recompiling and flashing the firmware.` + `Your Klipper version is: {v}` + `MCU(s) which should be updated:` + list | `extras/error_mcu.py` (`_check_protocol_error`) | C-cfg | Never ready | H (after every Klipper update) | Host Klipper was updated; board firmware is older. | Rebuild and flash firmware for each listed MCU (the message names them), then FIRMWARE_RESTART. | dlg |
| MCU-15 | `MCU '{mcu}' CRC does not match config`, `Failed automated reset of MCU '{mcu}'`, `Unable to configure MCU '{mcu}'`, `MCU '{mcu}' error during config: {reason}`, `Can not update MCU '{mcu}' config as it is shutdown`, `Too few moves available on MCU '{mcu}'` | `klippy/mcu.py` | C-cfg | Never ready | M | Board kept an old config, refused the new one, or is too small for the config. | FIRMWARE_RESTART (power-cycle the printer if it repeats). | dlg |
| MCU-16 | `Internal error in stepcompress` | `extras/motion_queuing.py` | C-sd | Shutdown | L | Host step generation failed (bad kinematics math, extreme move). | FIRMWARE_RESTART; report with klippy.log. | dlg |
| MCU-17 | `Internal error on command:"{cmd}"` / `Internal error during ready callback: {e}` / `Internal error during connect: {e}` / `Unhandled exception during run` / `Exception in flush_handler` | `gcode.py`, `klippy.py`, `motion_queuing.py` | C-sd / C-cfg | Shutdown (a Python exception in a handler, often a third-party plugin) | M | A Klipper module or plugin crashed. | Note the command; check klippy.log; update or remove the plugin; FIRMWARE_RESTART. | dlg |
| MCU-K1 | Kalico: `mcu '{mcu}': non-critical MCU is disconnected`; `Cannot use endstop on disconnected MCU '{mcu}'`; `Cannot set pin on disconnected MCU '{mcu}'`; `ADXL: {name} could not connect because mcu: {mcu} is non_critical_disconnected!` (and MPU/LIS2DW/ICM) | Kalico `serialhdl.py`, `mcu.py`, accel modules | A | Command rejected, no shutdown | L | Kalico's `is_non_critical` MCU (e.g. an accelerometer board) is unplugged; Kalico keeps running without it. | Plug the board in; Kalico reconnects it. | raw |

### 2.3 Motion and homing

| ID | Message pattern | Source | Ch | Effect | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|---|
| MV-1 | `Must home axis first: {pos}` (cartesian, corexy, corexz, hybrid, polar, generic_cartesian, deltesian); `Must home first: {pos}` (delta, rotary_delta) | `kinematics/*.py` (`check_move`) via `toolhead.py` (`move_error`) | A, D | Rejected / print aborted | H | A move was sent before homing (motors were disabled by idle timeout, or a macro moves before G28). | Home all axes (G28), then retry. | rw: `clean_error_text` turns any text containing `Must home axis` into "Must home axes first" (position dropped). `Must home first` (delta) is NOT matched: case-sensitive `must home` misses it. |
| MV-2 | `Move out of range: {pos}` | `toolhead.py` (`move_error`) via kinematics | A, D | Rejected / aborted | H | The target is past `position_min`/`position_max`: model too big or off-center, wrong bed size in slicer, a macro park position outside the bed, Z offset sending Z negative. | Check the model fits the bed and the slicer's bed size matches the printer; check macros that park/purge. The four numbers are the target X Y Z [E]. | K2 key585 only; prose path raw (`toolhead_move_sender.cpp` passes it through so the user sees limits) |
| MV-3 | `No trigger on {axis} after full movement` (`probe` for a probe, `x`, `y`, `z`, `z1`, ...) | `extras/homing.py` (`homing_move`) | A | Homing/probing fails | H | The endstop/probe never triggered within the full travel: unplugged or broken switch, wrong pin or `!` inversion, sensorless homing threshold too low, probe not deploying, or the bed too far for the probe. | Check the switch with `QUERY_ENDSTOPS` (or `QUERY_PROBE`); check its wiring and pin. Sensorless: tune `driver_SGTHRS`. Probe: make sure Z can travel far enough (lower `position_min`). | jr |
| MV-4 | `Endstop {axis} still triggered after retract` | `extras/homing.py` (`home_rails`) | A | Homing fails | M | After backing off, the switch still reads triggered: switch stuck, inverted logic (`!` missing), or too small `homing_retract_dist`. | Run `QUERY_ENDSTOPS` with the axis away from the switch; if it reads TRIGGERED, invert the pin or fix the switch. | jr |
| MV-5 | `Communication timeout during homing` | `klippy/mcu.py` (`home_wait`) | A | Homing fails | M | Multi-MCU homing (e.g. toolhead CAN board endstop + mainboard steppers) lost sync over USB/CAN. | Check CAN wiring/bitrate and host load. Kalico tunables: `[danger_options] multi_mcu_trsync_timeout`. | raw |
| MV-6 | `Error during homing move: {e}` / `Error during homing {axis}: {e}` | `extras/homing.py` | A | Homing fails | L | Wrapper; the cause is `{e}`. | Match the inner text. | raw |
| MV-7 | `Homing failed due to printer shutdown` / `Probing failed due to printer shutdown` | `extras/homing.py` | A | Secondary to a shutdown | M | Shutdown happened during homing. | Look at the shutdown reason. | jr |
| MV-8 | `Must home X and Y axes first` | `extras/safe_z_home.py` | A | Rejected | M | `G28 Z` with safe_z_home before X/Y are homed. | Home X and Y first (or plain G28). | raw (no `Must home axis` substring) |
| MV-9 | `Cannot determine position of toolhead on axis {a} after homing` | `extras/homing.py` | A | Homing fails | L | Kinematics could not compute position (dual carriage, odd config). | Check kinematics config. | raw |
| MV-10 | `Can only home Z with probe:z_virtual_endstop` / `Can't home to mix of probe:z_virtual_endstop rails` | `extras/homing.py` | A | Rejected | L | Config: probe virtual endstop used on a non-Z rail. | Fix config. | raw |
| MV-11 | `Machine does not support G20 (inches) command`; `Unable to parse move '{line}'`; `Invalid speed in '{line}'` | `extras/gcode_move.py` | A, D | Rejected / aborted | L | Corrupt or non-Klipper gcode file. | Re-slice for Klipper. | raw |
| MV-K1 | Kalico: `Early homing trigger on second home!` | Kalico `extras/homing.py` | A | Homing fails | M (Kalico sensorless) | Sensorless second-home triggered before `min_home_dist`. | Tune StallGuard threshold or homing speed. | raw |

### 2.4 Extrusion

| ID | Message pattern | Source | Ch | Effect | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|---|
| EX-1 | `Extrude below minimum temp` + line 2 `See the 'min_extrude_temp' config option for details` | `kinematics/extruder.py` (`check_move`) | A, D | Rejected / aborted | H | Extruding while the hotend is below `min_extrude_temp` (default 170 C): load/unload pressed cold, or the start gcode extrudes before the heat-wait. | Heat the nozzle to the filament's printing temperature and wait, then retry. | K2 key111 only; AMS backends (QIDI/CFS) word it in their own flows; prose path raw; jr |
| EX-2 | `Extrude only move too long ({req}mm vs {max}mm)` + `See the 'max_extrude_only_distance' config option for details` | `kinematics/extruder.py` | A, D | Rejected / aborted | M | One E-only move longer than `max_extrude_only_distance` (default 50 mm): load/unload macros, Bowden purges, or relative/absolute E confusion (M82/M83) in start gcode. | Load in shorter steps, or raise `max_extrude_only_distance` in `[extruder]` (Bowden printers often need 100+). Check the slicer uses relative/absolute E consistently. | jr |
| EX-3 | `Move exceeds maximum extrusion ({a}mm^2 vs {b}mm^2)` + `See the 'max_extrude_cross_section' config option for details` | `kinematics/extruder.py` | A, D | Rejected / aborted | M | A print move extrudes far more than its length allows: E-mode mismatch (absolute vs relative), a firmware-retraction mix-up, or a very wide first-layer line / purge line. | Make the slicer's E mode match the start gcode (M83 relative is usual); raise `max_extrude_cross_section` only for deliberate fat purge lines. | jr |
| EX-4 | `Extrude when no extruder present` / `Extruder not configured` / `'{name}' is not a valid extruder.` / `Active extruder does not have a stepper` | `kinematics/extruder.py` | A | Rejected | L | Gcode targets a tool that does not exist (T1 on a single-extruder printer). | Re-slice for this printer; fix tool macros. | raw |

### 2.5 Probes and leveling

| ID | Message pattern | Source | Ch | Effect | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|---|
| PR-1 | `Probe triggered prior to movement` | `extras/homing.py` (`probing_move`) | A | Probe/home fails | H | Probe already reads triggered before descending: probe not retracted (BLTouch stuck), nozzle on the bed, inverted pin, Klicky not docked/attached correctly. | Run `QUERY_PROBE` with the probe clear of the bed; if TRIGGERED, fix the pin inversion or the probe; raise Z first. | jr |
| PR-2 | `Probe samples exceed samples_tolerance` (each retry first shows `// Probe samples exceed tolerance. Retrying...`, `extras/probe.py:395`) | `extras/probe.py` | A | Probe/mesh/z_tilt fails | H | Repeated samples at one point differ more than `samples_tolerance`: dirty nozzle (ooze), loose probe mount, wobbly bed, fast probing speed. | Clean the nozzle, tighten the probe mount, slow `speed`, raise `samples_tolerance_retries`. | raw |
| PR-3 | `Must home before probe` | `extras/probe.py` | A | Rejected | M | PROBE without Z homed. | G28 first. | raw (`Must home` but not `Must home axis`: not rewritten) |
| PR-4 | `BLTouch failed to verify sensor state` (retries as `// ...; retrying.`) / `BLTouch failed to raise probe` / Kalico also `BLTouch failed to deploy` | `extras/bltouch.py` | A | Probe/mesh fails | M | BLTouch pin did not report as expected: bent pin, blinking red (self-test fault), wiring, wrong `sensor_pin`/`control_pin`, clone needing `pin_up_touch_mode_reports_triggered: False`. | `BLTOUCH_DEBUG COMMAND=reset`, check the pin moves freely, check wiring; for clones set the touch-mode options. | raw |
| PR-5 | `Toolhead moved during probe activate_gcode script` / `... deactivate_gcode script` | `extras/probe.py` | A | Probe fails | L | Dock/attach macro leaves the toolhead elsewhere. | Fix the probe macros. | raw |
| PR-6 | `horizontal_move_z can't be less than probe's z_offset` | `extras/probe.py` | A | Rejected | L | Config/macro value. | Raise `horizontal_move_z`. | raw |
| PR-7 | Eddy (Klipper and Kalico `probe_eddy_current`): `Must calibrate probe_eddy_current first`; `probe_eddy_current sensor outage`; `Failed calibration - incomplete sensor data` / `- No usable data`; `Unable to detect tap: {e}`; `Tap not configured`; `Invalid ldc1612 id (got {a},{b} vs {c},{d}). This is generally indicative of connection problems (e.g. faulty wiring) or a faulty ldc1612 chip.`; Kalico also `Sensor readout error`, `Timeout waiting for eddy current probe sample collection`, `Failed calibration - frequency not increasing each step` | `extras/probe_eddy_current.py`, `extras/ldc1612.py` | A | Probe fails | M | Eddy probe uncalibrated, its coil too far from the bed, or bad wiring. | Run `PROBE_EDDY_CURRENT_CALIBRATE CHIP={name}`; check the coil height (about 2-3 mm) and cable. | raw |
| PR-8 | Beacon, Cartographer, BTT Eddy (vendor fork), Scanner | not in either repo | | | H (popular) | Third-party plugins; wording lives in their repos. | | **gap** |
| PR-9 | Load cell (Klipper new): `Load Cell not calibrated`; `Load cell sensor reported errors while probing: {n} errors, {m} overflows`; `ERROR: Sensor is saturated with too much load! ...`; `ERROR: Tare and Calibration readings are the same! Check wiring ...`; Kalico `Load Cell Probe Error: current absolute force of {f}g exceeds force_safety_limit (+/-{l}g) before probing!` | `extras/load_cell*.py` | A | Probe fails | L (rising: Prusa Core One, Snapmaker U1) | Load cell uncalibrated, overloaded, or miswired. | `LOAD_CELL_CALIBRATE`, check wiring. | raw |
| PR-10 | `Aborting quad_gantry_level required adjustment {a} is greater than max_adjust {b}` | `extras/quad_gantry_level.py` | A | QGL fails | M | Gantry far out of level (after a crash, power-off sag), or wrong probe points / Z motor order. | Manually level the gantry roughly (power off, push it down to the frame stops), check `[quad_gantry_level]` points and Z motor order; or raise `max_adjust`. | raw |
| PR-11 | `Retries aborting: Probed points range is increasing. {extra}` (QGL extra: `Possibly Z motor numbering is wrong`); `Too many retries` | `extras/z_tilt.py` (`RetryHelper`) | A | Z_TILT/QGL fails | M | Corrections make it worse: Z motors mapped to the wrong corners, or the probe is inconsistent. | Check that `stepper_z`..`z3` match the corners in config; check probe repeatability (`PROBE_ACCURACY`). | raw |
| PR-12 | `bed level exceeds configured limits ({x}mm)! Adjust screws and restart print.` | `extras/screws_tilt_adjust.py` | A | Rejected | L | `SCREWS_TILT_CALCULATE MAX_DEVIATION=` exceeded. | Turn the screws as reported. | raw |

### 2.6 Bed mesh

| ID | Message pattern | Source | Ch | Effect | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|---|
| BM-1 | `bed_mesh: Unknown profile [{name}]` | `extras/bed_mesh.py` | A, D | Rejected / aborted (common in start gcode) | M | `BED_MESH_PROFILE LOAD=` names a profile that was never saved. | Run BED_MESH_CALIBRATE and SAVE_CONFIG, or fix the name in start gcode. | raw |
| BM-2 | `bed_mesh: Mesh extends outside of the fade range, please see the fade_start and fade_end options inexample-extras.cfg. fade distance: {d} mesh min: {a}mesh max: {b}` (Klipper's own missing spaces); `bed_mesh: ERROR, fade_target lies outside of mesh z range ...` | `extras/bed_mesh.py` | A | Mesh rejected | L | Mesh spread larger than the fade distance. | Level the bed mechanically or raise `fade_end`. | raw |
| BM-3 | `bed_mesh: invalid min/max points`, `bed_mesh: probe_count must be odd for round beds`, `bed_mesh: cannot exceed a probe_count of 6 when using lagrange interpolation...`, `bed_mesh: invalid probe_count option when using bicubic interpolation...`, faulty region overlaps | `extras/bed_mesh.py` | C-cfg (or A with params) | | L | Config. | Fix `[bed_mesh]`. | cfg only if `in section '` present; most do not contain it |
| BM-4 | Probing failures during a mesh are the PR-* rows. | | | | | | | |
| BM-K1 | Kalico: `No mesh has been loaded`; `bed_mesh: Cannot probe zero reference position at ({x}, {y}) as it is located within a faulty region...`; `bed_mesh: min/max points too close together` | Kalico `extras/bed_mesh.py` | A | | L | | | raw |

### 2.7 TMC drivers

| ID | Message pattern | Source | Ch | Effect | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|---|
| TMC-1 | `TMC '{stepper}' reports error: {REG}: {hex8}{ field=val...}` where REG is `DRV_STATUS` (or `READRSP@RDSEL2` on 2660) or `GSTAT`, followed by every nonzero field. Error flags: `ot=1(OvertempError!)`, `s2ga=1(ShortToGND_A!)`, `s2gb=1(ShortToGND_B!)`, `s2vsa=1(ShortToSupply_A!)`, `s2vsb=1(ShortToSupply_B!)` (5160/2240), `cs_actual=0(Reset?)` (driver reset), GSTAT `reset=1(Reset)`, `drv_err=1(ErrorShutdown!)`, `uv_cp=1(Undervoltage!)`. Warn-only flags that appear in the line but do not trip it alone: `otpw=1(OvertempWarning!)`, `ola`/`olb` (OpenLoad), `t120`/`t143`/`t150`/`t157`. | `extras/tmc.py` (`_query_register`), formatters `extras/tmc2130.py`, `tmc5160.py`, `tmc2209.py` | C-sd (periodic check, `invoke_shutdown`), A when raised during an enable or `INIT_TMC` | Shutdown | H | Decode by flag: `ot` driver overheated (current too high, no cooling); `s2g*`/`s2vs*` motor wire short or damaged motor/cable/driver; `uv_cp` or `reset`/`cs_actual=0` motor power (12-24 V) dropped or was off while the board ran (common: PSU off, e-stop relay, loose VIN); `drv_err` driver shut itself down after one of these. | Per flag: overtemp: lower `run_current`, add driver cooling. Short: power off, inspect the motor cable and connector of the named stepper, swap motor to test. Undervoltage/reset: check that motor power is on and its connector tight. FIRMWARE_RESTART. | dlg |
| TMC-2 | `Unable to read tmc uart '{stepper}' register {REG}` (retried 3 times first) / `Unable to write tmc uart '{stepper}' register {REG}` / `Unable to write tmc spi '{stepper}' register {REG}` | `extras/tmc_uart.py`, `tmc2130.py` | C-sd (or A) | Shutdown | H | Klipper cannot talk to the driver: motor power off (TMC2209 UART needs VMOT), driver not seated, wrong `uart_pin`/`cs_pin`, missing jumpers. At boot this is only logged (`TMC {name} failed to init`) and surfaces when the motor is first enabled. | Make sure motor power is on, reseat the driver, check the pin in `[tmc2209 {stepper}]`. FIRMWARE_RESTART. | dlg |
| TMC-3 | `TMC5262 {s} PLL fault after initialization: {e}` / `... PLL commit timed out` | `extras/tmc5262.py` | A | | L | New driver init failure. | Check driver clock config. | raw |
| TMC-4 | Config/command: `Unknown field name '{f}'`, `Unknown register name '{r}'`, `Could not find config section '[{s}]' required by tmc driver`, `TMC SPI chain must have same length` | `extras/tmc.py`, `tmc2130.py` | A / C-cfg | | L | Typo. | Fix. | raw / cfg |

### 2.8 Input shaper and accelerometers

| ID | Message pattern | Source | Ch | Effect | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|---|
| IS-1 | `Invalid adxl345 id (got {a} vs {b}). This is generally indicative of connection problems (e.g. faulty wiring) or a faulty adxl345 chip.`; same for `lis2dw`, `lis3dh`, `mpu` (`Invalid mpu id (got {a}). ...`) | `extras/adxl345.py`, `lis2dw.py`, `mpu9250.py` | A | Test fails | H (for anyone running resonance tests) | Accelerometer not answering: unplugged, wrong SPI pins, wrong `cs_pin`, damaged cable. `got 0` usually means no connection. | Check the accelerometer cable and `[adxl345] cs_pin`; run `ACCELEROMETER_QUERY`. | raw |
| IS-2 | `Failed to set ADXL345 register [0x{r}] to 0x{v}: got 0x{g}. ...` (and LIS2DW) | same | A | Test fails | M | Intermittent SPI link. | Shorten/shield the cable, reseat. | raw |
| IS-3 | `accelerometer '{chip}' measured no data`; `{axis}-axis accelerometer measured no data`; `No accelerometer measurements found` | `extras/resonance_tester.py`, `adxl345.py` | A | Test fails | M | Chip present but no samples (SPI timing, wrong MCU). | Check wiring; `ACCELEROMETER_QUERY`. | raw |
| IS-4 | `Failed to import \`numpy\` module, make sure it was installed via \`~/klippy-env/bin/pip install\` (refer to docs/Measuring_Resonances.md for more details).` | `extras/shaper_calibrate.py` | A | Calibration fails | M | numpy not installed on the host. | Install numpy into the klippy venv (KIAUH or `~/klippy-env/bin/pip install numpy`). | raw |
| IS-5 | `Timeout on wait for 'spi_transfer_response' response` **unverified** that this is the exact text users see (built from `mcu.py:71` `Timeout on wait for '%s' response`) | `klippy/mcu.py` | A | Test fails | M | Accelerometer SPI stalled. | Reseat; retry. | rw: any text containing `spi_transfer_response` becomes "Accelerometer communication failed. Try again." |
| IS-6 | `No accelerometers specified that can measure resonances over axis '{a}'`; Kalico `No accelerometer chips configured. At least one of accel_chips, accel_chip, or accel_chip_x/accel_chip_y must be specified.` | `extras/resonance_tester.py` | A | | L | `[resonance_tester]` missing `accel_chip`. | Add it. | raw |

### 2.9 Configuration (startup)

All appear as `state_message` with the RESTART trailer; `webhooks.state == "error"`. Fix the file, then RESTART (FIRMWARE_RESTART not needed).

| ID | Message pattern | Source | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|
| CF-1 | `Option '{opt}' in section '{sec}' must be specified` | `configfile.py` | H | A required setting is missing. | Add `{opt}` under `[{sec}]`. | cfg |
| CF-2 | `Option '{opt}' is not valid in section '{sec}'` | `configfile.py` (`check_unused_options`) | H | Typo, an option moved/renamed in a Klipper update, or a Kalico-only option on Klipper. Kalico raises it too unless `[danger_options] error_on_unused_config_options: False`. | Fix the spelling or remove the line; check the Klipper changelog for renamed options. | cfg |
| CF-3 | `Section '{sec}' is not a valid config section` | same | H | Unknown section (typo, plugin not installed). | Install the plugin or remove/rename the section. | cfg |
| CF-4 | `Unable to parse option '{opt}' in section '{sec}'`; `Option ... must have minimum of / maximum of / must be above / must be below {n}`; `Option ... must have {n} elements`; `Choice '{v}' for option '{opt}' in section '{sec}' is not a valid choice` | `configfile.py` | M | Value has the wrong type or range. | Correct the value. | cfg |
| CF-5 | `Pin '{pin}' is not a valid pin name on mcu '{mcu}'` | `mcu.py` | M | Wrong pin name for that board. | Copy the pin from the board's sample config. | **not matched** by `is_klippy_config_error` |
| CF-6 | `pin {pin} used multiple times in config`; `Pin {pin} reserved for {a} - can't reserve for {b}`; `pin {p} is reserved for {r}`; `Shared pin {p} must have same polarity`; `Invalid pin description '{d}' ...`; `Unknown pin chip name '{c}'` | `pins.py` | M | Two features on one pin, or bad pin syntax. | Give each feature its own pin. | partial: `used multiple times` and `Unknown pin chip name` matched; the rest not |
| CF-7 | `Include file '{glob}' does not exist`; `Recursive include of config file '{f}'`; `Unable to open config file {f}` | `configfile.py` | M | A `[include]` points at a missing file (often a removed plugin's cfg). | Fix or remove the include. | partial: only `Unable to open config file` matched |
| CF-8 | `Unknown config object '{name}'`; `Unable to load module '{section}'`; `Printer object '{name}' already created` | `klippy.py` | M | Missing plugin / duplicate section. | Install plugin / remove duplicate. | **not matched** |
| CF-9 | `Error loading template '{name}'\nline {n}: {src} # {msg}` (syntax, raised as gcode error at load) / `Error loading template '{name}': {exc}` (config error) | `extras/gcode_macro.py` | M | Jinja syntax error in a macro. | Fix the macro on the named line. | cfg |
| CF-10 | `gcode command {cmd} already registered`; `Heater {h} already registered` | `gcode.py`, `heaters.py` | L | Duplicate macro name / duplicate heater. | Rename. | not matched |
| CF-K1 | Kalico: `Kalico requires Python 3`; `Module '{name}' found in both extras and plugins!`; `Primary MCU cannot be marked as non-critical!`; `min_power and off_below are both set. Remove one!` | Kalico | L | | | not matched |

### 2.10 G-code parsing and macros

| ID | Message pattern | Source | Ch | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|
| GC-1 | `Unknown command:"{CMD}"` | `gcode.py` (`cmd_default`) | **E only** (`// `), never `!!` | H | Command or macro does not exist (M600 without a macro, slicer-specific codes, a macro name typo). Printing continues. | Define the macro or remove it from the slicer gcode. | jr marker `unknown command` (but the line is `//`, not `!!`; whether the journal sees `//` lines is **unverified**) |
| GC-2 | `Error on '{line}': missing {PARAM}` / `unable to parse {P}` / `{P} must have minimum of {n}` (also maximum / above / below) | `gcode.py` (`GCodeCommand.get`) | A, D | M | Bad parameter. | Fix the command or macro. | raw |
| GC-3 | `Malformed command '{line}'` | `gcode.py` | A, D | L | Garbage gcode line. | Re-slice. | jr |
| GC-4 | `The value '{v}' is not valid for {PARAM}{...}` | `gcode.py` (`_cmd_mux`) | A | M | Wrong `HEATER=`/`STEPPER=`/`SENSOR=` name. | Use a configured name. | raw |
| GC-5 | `Error evaluating '{macro}:gcode': {ExceptionType}: {msg}` (e.g. `gcode_macro PRINT_START:gcode': UndefinedError: 'dict object' has no attribute 'BED_TEMP'`) | `extras/gcode_macro.py` (`render`) | A, D | H | A macro referenced a missing parameter or object (slicer did not pass BED_TEMP, object not configured). | Pass the parameter from the slicer start gcode, or give it a `default()`. | jr marker `error evaluating` |
| GC-6 | `{action_raise_error text}` (arbitrary) | `extras/gcode_macro.py` (`_action_raise_error`) | A, D | M | A macro deliberately aborted with its own words (Happy Hare, AFC, KAMP, PRINT_START guards). | Read the text; it is the macro author's. | raw (correct: author wording) |

### 2.11 Filament sensors and runout

| ID | Message / signal | Source | Ch | Effect | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|---|
| FS-1 | No error text. Runout is logged to klippy.log only (`Filament Sensor {name}: runout event detected, Time {t}`). Client sees `filament_switch_sensor {name}.filament_detected=false`, `pause_resume.is_paused=true`, `print_stats.state=paused`, then `runout_gcode` output. | `extras/filament_switch_sensor.py`, `runout_helper` | state change | Pause (`pause_on_runout: True` default) | H | Filament ran out or broke, or the sensor misread. | Load filament, then RESUME (heat first if idle_timeout dropped the hotend). | journal "Filament runout"; HelixScreen's own runout UI |
| FS-2 | `// Print already paused`, `// Print is not paused, resume aborted` | `extras/pause_resume.py` | E | none | M | Double pause/resume. | | not shown as error (correct) |
| FS-3 | hall_filament_width_sensor config errors | `extras/hall_filament_width_sensor.py` | C-cfg | | L | | | cfg (contains no `in section '`: **not matched**) |

### 2.12 Exclude object, SAVE_CONFIG, virtual SD / print files

| ID | Message pattern | Source | Ch | Freq | Plain cause | Fix | Helix |
|---|---|---|---|---|---|---|---|
| EO-1 | `There is no current object to cancel` | `extras/exclude_object.py` | A | L | `EXCLUDE_OBJECT CURRENT=1` outside any object. | None needed. | raw |
| EO-2 | `// EXCLUDE_OBJECT_END called, but no object is currently active`, `// EXCLUDE_OBJECT_END NAME={n} does not match the current object NAME={m}`; `// Excluding object {NAME}` | same | E | L | Slicer labeling quirk. | None. | n/a |
| SC-1 | `SAVE_CONFIG section '{sec}' option '{opt}' conflicts with included value` | `configfile.py` (`_disallow_include_conflicts`) | A | M | The value being saved (e.g. `z_offset`, PID) is also set in an included file, so SAVE_CONFIG refuses. | Remove/comment that option in the included file (the message names section and option), then SAVE_CONFIG again. | raw |
| SC-2 | `Existing config autosave is corrupted. Can't complete SAVE_CONFIG`; `Unable to read existing config on SAVE_CONFIG`; `Unable to parse existing config on SAVE_CONFIG`; `Unable to write config file during SAVE_CONFIG` | `configfile.py` | A | L | Hand edits broke the `#*#` block, or a read-only/full filesystem. | Fix the autosave block at the end of printer.cfg; check disk space. | raw |
| SD-1 | `SD busy` | `extras/virtual_sdcard.py` | A | M | Start/select a file while a print is running or paused. | Cancel or finish the current print first. | raw |
| SD-2 | `Unable to open file` | same | A | M | File missing/renamed/unreadable on the host. | Re-upload the file. | raw (Moonraker-side "File not found" already worded in `moonraker_error.h`) |
| SD-3 | `SDCARD_RESET_FILE cannot be run from the sdcard`; `Unable to get file list`; `SD write not supported` | same | A | L | | | raw |
| SD-4 | Printer not ready on any command: `Printer is not ready` (first line of `message_startup`), or the first line of the current shutdown reason | `gcode.py` (`cmd_default`) | A | H (side effect) | See 1.3 item 1. | Wait for startup / FIRMWARE_RESTART. | `moonraker_error.h` words the RPC 503 side; console echo raw |

---

## 3. Recommended top 30 to word first

Ranked by frequency x confusion (how lost a user is from the raw text). Each is one wording row in the style of `snapmaker_exceptions.h`: summary, cause, fix, plus captured placeholders.

| Rank | ID | Key | Summary to show | Captures |
|---|---|---|---|---|
| 1 | MCU-1 | `Timer too close` | Printer's computer fell behind | mcu |
| 2 | HT-2 | `ADC out of range` | Temperature sensor reading is impossible | mcu, sensor, temp, min, max |
| 3 | HT-1 | `not heating at expected rate` | Heater is not heating as expected | heater |
| 4 | MCU-4 | `Lost communication with MCU` | Lost connection to a control board | mcu |
| 5 | TMC-1 | `TMC '...' reports error` | Stepper driver fault (per flag: overheat / short / power loss) | stepper, register, flags |
| 6 | TMC-2 | `Unable to read tmc uart` / `write tmc` | Can't talk to a stepper driver | stepper, register |
| 7 | MCU-14 | `MCU Protocol error` | Board firmware needs updating | MCU list |
| 8 | MCU-13 | `': Unable to connect` | Control board not found | mcu |
| 9 | EX-1 | `Extrude below minimum temp` | Nozzle too cold to extrude | (current temp from state) |
| 10 | MV-2 | `Move out of range:` | Move would go past the printer's limits | x, y, z, e |
| 11 | MV-1 | `Must home axis first` / `Must home first` | Printer needs homing first | |
| 12 | MV-3 | `No trigger on ... after full movement` | Endstop or probe never triggered | axis |
| 13 | PR-1 | `Probe triggered prior to movement` | Probe already triggered | |
| 14 | PR-2 | `Probe samples exceed samples_tolerance` | Probe readings inconsistent | |
| 15 | CF-2 | `is not valid in section` | Unknown setting in config | option, section |
| 16 | CF-1 | `must be specified` | Required setting missing | option, section |
| 17 | CF-3 | `is not a valid config section` | Unknown config section | section |
| 18 | GC-5 | `Error evaluating '` | Macro error | macro, exception text |
| 19 | EX-2 | `Extrude only move too long` | Filament move too long | requested, max |
| 20 | EX-3 | `Move exceeds maximum extrusion` | Too much filament for this move | a, b |
| 21 | HT-4 | `Requested temperature (` | Temperature above heater's limit | requested, min, max |
| 22 | MCU-2 | `Missed scheduling of next` | Board missed a timed update | mcu, event kind |
| 23 | MCU-3 | `Rescheduled timer in the past` / `Stepper too far in past` | Motors asked to step too fast | mcu |
| 24 | MCU-5 | `spontaneous restart` | A control board rebooted | mcu |
| 25 | PR-4 | `BLTouch failed to` | BLTouch didn't respond | which |
| 26 | IS-1 | `Invalid adxl345 id` (and siblings) | Accelerometer not connected | chip, got |
| 27 | PR-10/11 | `quad_gantry_level required adjustment` / `Retries aborting:` | Gantry leveling failed | adjust, max |
| 28 | SC-1 | `SAVE_CONFIG section` ... `conflicts with included value` | Can't save: value set in another file | section, option |
| 29 | MV-4 | `still triggered after retract` | Endstop stuck triggered | axis |
| 30 | BM-1 | `bed_mesh: Unknown profile [` | Bed mesh profile not found | name |

Also worth a cheap row because Helix causes or knows them: MCU-11 `Shutdown due to webhooks request` ("Emergency stop pressed") and MCU-10 `Shutdown due to M112 command`.

---

## 4. Matching strategy

1. **Normalize first.** Strip `!! `; for `state_message` strip the `MCU '{mcu}' shutdown: ` / `Previous MCU '{mcu}' shutdown: ` prefix (capture `mcu`) and the trailers in 1.2 (cut at `\nOnce the underlying issue is corrected` and at the hint text). What remains is the reason line plus optional clarify lines.
2. **Key on a stable prefix of the reason, anchored, case-sensitive.** These literals are unchanged between Klipper and Kalico at the read commits, and the MCU ones are compiled into firmware (`shutdown("...")` in `src/`), so they change only when the MCU protocol does. Klipper's own `error_mcu.py` (`error_hint`) uses exactly this (prefix match on the reason), a good sign the strings are meant to be stable. Suggested keys:
   - Prefix: `Timer too close`, `Missed scheduling of next `, `ADC out of range`, `Rescheduled timer in the past`, `Stepper too far in past`, `Thermocouple reader fault`, `Command request`, `Watchdog timer!`, `Move queue overflow`, `Lost communication with MCU '`, `Heater `+`not heating at expected rate`, `TMC '`+`' reports error: `, `Unable to read tmc uart '`, `Unable to write tmc uart '`, `Unable to write tmc spi '`, `Must home axis first`, `Must home first`, `Move out of range`, `Extrude below minimum temp`, `Extrude only move too long`, `Move exceeds maximum extrusion`, `No trigger on `, `Endstop `+`still triggered after retract`, `Probe triggered prior to movement`, `Probe samples exceed`, `BLTouch failed to `, `Requested temperature (`, `Error evaluating '`, `Error loading template '`, `MCU Protocol error`, `Shutdown due to `, `SAVE_CONFIG section '`, `bed_mesh: Unknown profile [`, `Invalid adxl345 id`, `Invalid lis2dw id`, `Invalid lis3dh id`, `Invalid mpu id`, `Invalid ldc1612 id`.
   - Startup/config: `Option '` + (`' must be specified` | `' is not valid in section '`), `Section '` + `' is not a valid config section`, `Pin '` + `' is not a valid pin name on mcu '`, and the `pins.py` set.
3. **Capture with small regexes, not sentence matches** (use `helix::Regex`, never `std::regex`):
   - `^Heater (.+) not heating at expected rate` -> heater
   - `^Sensor '([^']+)' temperature ([-\d.?]+) not in range ([-\d.]+):([-\d.]+)` (clarify line of ADC) -> sensor, temp, min, max (`?` when the callback failed)
   - `^Lost communication with MCU '([^']+)'`, `^mcu '([^']+)': Unable to connect`, `^MCU '([^']+)' spontaneous restart`
   - `^(Must home axis first|Move out of range): ([-\d.]+) ([-\d.]+) ([-\d.]+) \[([-\d.]+)\]`
   - `^No trigger on (\S+) after full movement`, `^Endstop (\S+) still triggered after retract`
   - `^TMC '([^']+)' reports error: (\S+):\s+([0-9a-f]{8})(.*)$`, then scan the tail for `(\w+)=1\(([A-Za-z_]+)!?\)` and for `cs_actual=0(Reset?)`; pick the wording by the worst error flag (`s2*` > `ot` > `uv_cp`/`reset`/`cs_actual=0` > `drv_err`).
   - `^Unable to (read|write) tmc (uart|spi) '([^']+)' register (\w+)`
   - `^Requested temperature \(([-\d.]+)\) out of range \(([-\d.]+):([-\d.]+)\)`
   - `^Extrude only move too long \(([\d.]+)mm vs ([\d.]+)mm\)`
   - `^Option '([^']+)' (?:in section '([^']+)' must be specified|is not valid in section '([^']+)')`
   - `^Error evaluating '([^']+)': (\w+): (.*)`
4. **Prefer the structured signal when one exists**: runout from `filament_switch_sensor` state, shutdown from `webhooks.state`, print abort from `print_stats.state=="error"` with `print_stats.message` as the text to classify (it is the full message, better than the `!!` first line).
5. **One table, two presenters.** Return a classification (id, captures, severity, recovery: FIRMWARE_RESTART / RESTART-after-fix / retry / home) and let the toast, modal, recovery dialog and journal each render it. This would replace the three hand-written matchers that already exist (`gcode_error_text.cpp` rewrites, `printer_event_journal.cpp` `kClassMarkers`, `printer_network_state.cpp` `is_klippy_config_error` / `is_klippy_mcu_connect_error`), which today disagree (MV-1 case sensitivity, CF-5/CF-8 misses, the `Timeout on connect` key that never appears).
6. **Unknown keeps the firmware text**, as `exception_message()` does for the U1: an unmatched reason shows Klipper's words unchanged.
7. **Translation**: wording strings via `TR_NOOP`; captures are inserted with `fmt::runtime` like the K2 table formatters.

---

## 5. Gaps and open questions

1. **Not covered by source here:** Beacon, Cartographer, BTT Eddy (vendor fork), Happy Hare, AFC, Klipper-TMC autotune, KAMP and other plugins. Their wording lives in their own repos; Happy Hare/AFC already have HelixScreen backends. Creality, Snapmaker, Qidi, Elegoo and FlashForge vendor forks change some of these strings (K2's `key585` is MV-2 and `key111` is EX-1 as coded envelopes).
2. **`Timeout during endstop homing`:** `extras/probe.py` checks for this text to append `HINT_TIMEOUT`, but nothing in Klipper at `c9559fa` raises it (grep of `klippy/` and `src/`). It looks unreachable now; the probe-out-of-travel case arrives as `No trigger on probe after full movement` without the hint. **unverified** whether an older Klipper still on users' printers raises it.
3. **`is_klippy_mcu_connect_error` matches `Timeout on connect`**, which Klipper only writes to klippy.log, never to `state_message`. Harmless but dead.
4. **`clean_error_text` MV-1 rewrite** is case-sensitive and matches `Must home axis` or lowercase `must home`: it misses delta `Must home first`, `Must home before probe` and `Must home X and Y axes first`, and drops the position numbers from `Must home axis first: ...`.
5. **`is_klippy_config_error` misses** CF-5 (`is not a valid pin name on mcu`), CF-7 includes, CF-8 (`Unknown config object`, `Unable to load module`), most `pins.py` errors and Kalico-only config errors. Those currently get the generic shutdown/error dialog without the "fix the file then RESTART" framing.
6. **Journal `kClassMarkers`** classify `filament`/`runout` by substring, which also catches unrelated AFC/Happy Hare errors that mention filament; `unknown command` is matched although that line is a `//` info line, not `!!`. **unverified** whether `//` lines reach the journal classifier at all.
7. **Kalico `!!` from non-errors:** Kalico's `pid_calibrate.py` writes `respond_raw("!! Target calibration temperature of {t}°C is <= 15°C below ...")` as a warning. The generic classifier will present it as an error (CRITICAL if printing).
8. **HT-2 temperature direction**: open-circuit reads cold / short reads hot is electrical knowledge with the usual pull-up wiring, not something in source. **unverified** for every board/sensor type (PT1000 on some boards differs).
9. (resolved) PR-2 retry text confirmed in source.
10. **SD-2 / Moonraker start-print errors**: Moonraker's `printer.print.start` file checks were not read; **unverified** which of Moonraker's or Klipper's text a missing file produces first.
11. **Kalico `[danger_options] temp_ignore_limits`** (and deprecated `adc_ignore_limits`) disables the min/max checks behind HT-2/HT-3/HT-5. A Kalico user with it set never sees those errors; worth knowing when answering "why didn't it stop".
12. **Open question for the wording pass:** should a shutdown's recovery dialog show our wording INSTEAD of `state_message`, or above it (keeping Klipper's text for support threads)? The config-error path already does "preface + raw", which seems the right model to extend.
