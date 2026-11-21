# FlashForge error map (AD5M / AD5X / Creator 5)

Status: complete as of 2026-11-20 19:40 EST. Research only, nothing in the HelixScreen repo changed.
Anything marked **unverified** was inferred, not seen in source.

Paths below are relative to the clone root
`/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/71793b24-69aa-4392-a9a2-57deecc9232b/scratchpad/src-flashforge/`
unless they start with `src/`, `include/` or `docs/` (those are HelixScreen paths).

---

## 1. Firmware matrix and sources read

| Model | Firmware | Klipper host | Moonraker? | HelixScreen runs? | Source read (commit date) |
|---|---|---|---|---|---|
| AD5M / Pro | Stock | FF fork of Klipper v0.11 (2023), `/opt/klipper` | No | No | `AD5M_Series_Klipper` (2024-05-09): vanilla-looking klippy, no FF extras published (no load cell, no IFS) |
| AD5M / Pro | Forge-X 1.4.1 | stock klippy + Forge-X patches/plugins | Yes (0.9.3) | Yes | `ff5m` (2026-11-06): `.py/klipper/plugins/*.py`, `.py/klipper/patches/*.py`, `macros/*.cfg`, `config/*.cfg` |
| AD5M / Pro | Klipper Mod | mod's own Klipper | Yes | Yes | `kmod` = xblax/flashforge_ad5m_klipper_mod (2025-09-17): `printer_configs/*.cfg` (not in the brief; cloned for completeness) |
| AD5M / Pro | ZMOD 1.7.6 | stock klippy (v11, or opt-in Klipper 13) + zmod `.shell/*.py` | Yes | Yes (`DISPLAY_OFF HELIX=1`) | `z_ad5x` 1.7 branch (ships the `ff5m_*.cfg` too), `zmod` docs repo |
| AD5X | Stock | FF fork, Klipper v12 | No | No | none public; only zmod's docs about it |
| AD5X | ZMOD 1.7.6 (+ lessWaste / bambufy plugins) | stock klippy + `zmod_ifs.py`, `zmod_color.py`, `zmod_tenz.py` | Yes | Yes | `z_ad5x/.shell/*.py`, `z_ad5x/translate/en/{base,base_client,ad5x_*}.cfg`, `bambufy/bambufy.cfg` (2026-11-16) |
| AD5X | Forge-X port with `[ifs]` module | HelixScreen's 1-of-1 port | Yes | Yes | **no public source**; only `docs/devel/FILAMENT_BACKEND_AD5X_IFS.md` |
| Creator 5 / 5 Pro | Stock 2.0.0 | FF fork, `/usr/prog/klipper` | Yes (7125) | No (stock UI `firmwareExe`) | `k4ff` = Klipper4FlashForge/firmware (2026-11-04): Ghidra notes on `firmwareExe`, `docs/notes/70-error-codes.md`, `pkgs/klipper/.../ff_toolchange.py` |
| Creator 5 / 5 Pro | OpenCreator | Kalico fork | Yes | Yes | `opencreator-fs/fs/usr/data/config/*.cfg`, `oc-kalico/klippy/extras/{e_stop,creator5_toolchanger,c5_motion_guard,creator5_dryer}.py` (FlashForge-C5-Modding-Group/kalico, sparse), `OpenCreator/docs` |
| Creator 5 / 5 Pro | ZMOD z_c5pro 1.7.6 | **stock FF klippy** + zmod | Yes | Yes | `z_c5pro/.shell/*.py`, `z_c5pro/translate/en/c5pro_*.cfg` |
| Creator 5 / 5 Pro | Klipper4FlashForge ("anvil") | FF klippy + `ff_*` extras | Yes | Yes (ships a helixscreen package) | `k4ff` (not in the brief; it is where the E-code table came from) |

HelixScreen side read: `src/application/gcode_error_router.cpp`, `src/application/error_classify.cpp`,
`src/application/gcode_error_text.cpp`, `src/printer/klipper_error_table.cpp`,
`src/printer/firmware_error_envelope.cpp`, `include/firmware_error_envelope.h`,
`include/firmware_fault_codes.h`, `include/snapmaker_exceptions.h`,
`src/printer/ams_backend_ad5x_ifs.cpp` (fault paths), `docs/devel/FILAMENT_BACKEND_AD5X_IFS.md`,
`docs/devel/printer-research/FLASHFORGE_*`, `docs/devel/printers/FLASHFORGE_*`.

---

## 2. Numeric error-code scheme: the answer per firmware

**Yes, FlashForge stock firmware has an `E0NNN` scheme, and it lives in the stock UI
(`firmwareExe`), not in Klipper.** The app holds a language table of codes; it decides the code
from its own state machines and from Klipper state, and draws it on the stock screen. Klipper
never prints `E0NNN` itself on stock firmware. Separately, the FlashForge Klipper fork answers
the app over the API socket with JSON objects (`{"module": "...", "status": "fail", "coded": "MMMM-IIII-CCCC", "msg": "..."}`),
and some of those reach the gcode console as error text.

| Firmware | E0NNN codes exist? | Visible to a Moonraker client? | Notes |
|---|---|---|---|
| AD5M stock | Yes (E0011, E0017, E0120 seen in zmod docs) | No. Stock has no Moonraker; codes only on the stock screen | E0011 = "Timer too close" / host-MCU timing (zmod `docs/en/Global.md` FIX_E0011). E0017 = fixed by `LOOKAHEAD_FLUSH_TIME 0.5 -> 0.150` (meaning **unverified**; probably a move-queue/lookahead stall). E0120 = "This is a Klipper error", the generic code; the real text is on the console (`docs/en/FAQ.md` "E0120") |
| AD5M ZMOD / Forge-X / Klipper Mod | Only on the stock screen while it is running | No. With HelixScreen running the stock UI is off; the console carries Klipper prose | ZMOD still names `FIX_E0011` / `FIX_E0017` as settings; the strings `E0011`/`E0017` appear only in settings prompts (`RESPOND PREFIX="//"`), never as a raised error |
| AD5X stock | Yes, same family (E0011, E0017 are cited for AD5X) | No Moonraker on stock | **unverified** that the AD5X table matches the C5 table; no AD5X firmwareExe decompile is public |
| AD5X ZMOD | Same as AD5M ZMOD | No | IFS errors are zmod prose, never codes. The IFS serial board has its own state byte (F13 `FFS_state`, 127 = driver error) and zmod return codes `RET_*` 0..6, both internal |
| C5 stock 2.0.0 | Yes, full table (~90 codes, `k4ff/docs/notes/70-error-codes.md`) | Partially. Moonraker is present, but HelixScreen does not run on stock. A Mainsail client sees Klipper prose plus FF JSON (`{"module":"estop",...,"coded":"0201-0000-0003",...}`); the E-code itself stays in firmwareExe | Codes are chosen in the app (`checkInstallExtruder`, `doPopupPause(2)` -> E0163, toolchange.c -> E0127+t etc., per the k4ff Ghidra notes) |
| C5 OpenCreator | firmwareExe removed. One E-code survives as text: `E0163: Paused. T%d clog detected: ...` (`oc-kalico/klippy/extras/creator5_toolchanger.py:1650`, raised through AFC_error) | Yes, as prose in the AFC error message | e_stop.py keeps the vendor JSON: `0201-0000-0001` "Probe triggered prior to movement" (line 280), `0201-0000-0002` "The estop is unqualified" (357, respond_info), `0201-0000-0003` "No trigger on probe after full movement" (327), `0201-0000-0005` wraps any other reason (325) |
| C5 ZMOD z_c5pro | Stock firmwareExe off when HelixScreen runs | Stock klippy JSON errors reach the console unchanged | ZMOD runs the stock klippy, so the e_stop JSON (2.0.0 replaced the probe-triggered one with plain text per `docs/devel/printer-research/FLASHFORGE_CREATOR5_FIRMWARE.md`) and any other FF JSON reach HelixScreen |
| C5 Klipper4FlashForge | Reproduces the codes as text: `"(firmware error E%04d)"` suffixes in `k4ff/pkgs/klipper/payload/klipper/klippy/extras/ff_toolchange.py:1033,1095,1144,1203,1220` | Yes, as prose suffixes | E0127+t, E0051+t, E0131+t, E0135+t, E0144 |

Two observations that matter for code reuse:

- The FF `"coded"` value is the same shape as the Snapmaker U1's **three-part basic form**
  (`id-index-code`, no level), which `include/snapmaker_exceptions.h` deliberately refuses to
  decode as a four-part code. HelixScreen's envelope decoder already has a `{"coded","msg"}` row
  (`src/printer/firmware_error_envelope.cpp` kShapes, Opaque scheme), so an FF JSON error already
  renders as its `msg` with the code kept for logs. Whether U1 and FF share an upstream vendor
  codebase is **unverified**.
- The E-code table is the best available spec of which hardware faults FlashForge distinguishes
  on the C5, and it gives us stable user-facing names to borrow (users search the web for
  "E0163"), but no firmware HelixScreen runs on emits most of them.

---

## 3. How errors reach a client

| Mechanism | What a Moonraker client sees | Used by | Print effect |
|---|---|---|---|
| Python `raise gcmd.error(msg)` / `self.gcode.error(msg)` / `printer.command_error(msg)` | `!! msg` in `notify_gcode_response`; the RPC that ran the command fails with the same text | zmod_ifs, zmod_color, zmod_tenz, zmod.py, Forge-X plugins, OC toolchanger | Aborts the command and the macro calling it. Inside a print file it **stops the print** (virtual_sdcard error) unless the macro catches it |
| Jinja `action_raise_error("...")` | `!! msg` | ZMOD base.cfg, Forge-X stock.cfg, OC printer.*.cfg | Same as above |
| `RESPOND TYPE=error MSG=...` | `!! msg` | ZMOD runout/jam/resume, Forge-X client.cfg, OC runout load | **Nothing by itself**; the macro around it decides (PAUSE, or nothing) |
| `RESPOND PREFIX="!!" MSG=...` and `respond_raw("!! ...")` | `!! msg` | Forge-X (bed pressure, probe mismatch, resurrection), ZMOD (`!! Nozzle hit bed...`, 3MF errors), OC motion guard | Informational unless paired with PAUSE/CANCEL |
| `respond_info` / `RESPOND PREFIX=info` / `action_respond_info` | `// msg` | IFS progress ("Insert filament 2 ..."), "IFS is not available", "IFS: sensor error: ...", mesh warnings | none |
| `RESPOND PREFIX=tgalarm_photo` / `_NOTIFY` | `tgalarm_photo msg` (Telegram bot hook) | every ZMOD runout/jam path, duplicated beside the `!!` line | none; a second copy of the same text |
| `action:prompt_*` dialogs | `// action:prompt_begin ...` lines | ZMOD "Resumption interrupted !!!", C5 "T{t} runout", bambufy `_INFO_DIALOG`, OC "Filament runout", "Not homed" | none; HelixScreen's action-prompt modal shows them |
| `invoke_async_shutdown(msg)` | Klippy shutdown: `webhooks.state_message` / `print_stats` error, plus `!! msg` and Klipper's restart footer | ZMOD nozzle-control ABORT (`zmod_tenz.py:193`, `spi_temperature.py:189`), webhooks JSON encoding error | **Kills Klipper**; print lost; needs FIRMWARE_RESTART |
| `PAUSE REASON=<word>` | no text; bambufy shows an `_INFO_DIALOG` prompt per reason | bambufy, lessWaste | Pauses |
| `M117 T<n> clog` | display_status message | Klipper4FlashForge clog port (`k4ff` notes 49) | Paused by the same macro |
| Status objects | `zmod_ifs` (`State` 127 = driver error, `Stall`, `Ports`), Forge-X `ifs.activity == "driver_error"` + `ifs.error`, OC `creator5_toolchanger.sensor_error` (string), OC `c5_motion_guard` status, afclite `error_state`+`message` | backends | none; standing state |
| FF JSON in error text | `!! {"module":"estop","status":"fail","coded":"0201-0000-0003","msg":"..."}` | C5 stock klippy (ZMOD z_c5pro), OpenCreator e_stop | as the raising command |

Language: **ZMOD text follows the ZMOD language setting.** Python modules branch on
`self.lang == 'ru'` (only Russian vs English), and the macros come from per-language
`translate/<lang>/` folders (13 languages: cs de en es fr it ja ko pt ru tr zh). An English
substring matcher therefore misses every ZMOD macro error on a non-English install, and the
Python ones on a Russian install. Forge-X, Klipper Mod and OpenCreator are English only.

---

## 4. Error tables

Columns: **Msg** = exact text (`{}` = variable), **Where** = file:line, **Path** = how it reaches us,
**Effect** = print behavior, **HS** = does HelixScreen word it today
(Y = own wording, P = partial: generic modal, or our own detector fires on the same condition,
N = raw firmware text in the generic "Printer Error" modal/toast).

HelixScreen's generic handling for every N row: `!!` line -> `error_classify::classify` ->
GENERIC source, CRITICAL (sticky modal "Printer Error" with Resume/OK when paused) if printing or
paused, else WARNING toast, text cleaned by `clean_error_text` (which only rewrites "Must home"
and `spi_transfer_response`).

### 4a. AD5X IFS (ZMOD `zmod_ifs.py`, AD5X ZMOD only)

| # | Msg | Where | Path | Effect | Cause (plain) | Fix | HS |
|---|---|---|---|---|---|---|---|
| I1 | `Failed to extract filament from extruder` (ru: `Не удалось извлечь пруток из экструдера`) | `z_ad5x/.shell/zmod_ifs.py:1179` `cmd_IFS_REMOVE_PRUTOK` | `!!` + RPC error | Aborts unload; also runs `IFS_F112` + `IFS_F18` (stop, unlock all). During a color change in a print: print stops (**unverified**: whether `_A_CHANGE_FILAMENT` catches it) | After the cut and retract the toolhead sensor still sees filament: the cut failed, the tip is stuck in the gears, or the sensor is dirty | Heat the nozzle, pull the filament out by hand at the toolhead, check the cutter blade, then retry | P (op goes to ERROR "Filament System Error" with the raw text; `fail_op_after_macro`) |
| I2 | `No filament {n} in IFS` (ru `Нет прутка {n} в IFS`) | `zmod_ifs.py:820` `print_result(RET_SILK)` | `!!` when `info=False` (IFS_F10 CHECK=1), else `//` | Aborts that load, `IFS_F112` | The lane's presence sensor dropped while feeding: spool empty, filament broke, or never inserted past the sensor | Insert filament into lane n until the IFS grabs it, then load again | N |
| I3 | `Filament {n} stalled in IFS` | `zmod_ifs.py:822` `RET_STALL` | same as I2 | Aborts load | The lane motor turns but the motion sensor sees no movement: tangle on the spool, kinked PTFE, or slipping drive roller | Check the spool turns freely and the tube from lane n is not kinked; clear debris on the pinch roller | N |
| I4 | `Filament sensor triggered in extruder` | `zmod_ifs.py:818` `RET_EXTRUDER` | usually `//` (success path of a load) | none; normal end of a load | Filament reached the toolhead | none (not an error) | n/a |
| I5 | `Timeout exceeded` | `zmod_ifs.py:824` `RET_TIMEOUT` | `!!`/`//` | Aborts load | (unreachable in practice: `wait_for_state` raises I8 first) | as I8 | N |
| I6 | `IFS driver failure` | `zmod_ifs.py:828` `RET_RETRY` | `!!`/`//` | load retried `retry_count` times first | IFS board reported state 127 on every retry | Power-cycle the printer; reseat the IFS cable; run `IFS_F15` (driver reset) | P (Forge-X module path: `ifs.activity == "driver_error"` -> "IFS driver error"; ZMOD path N) |
| I7 | `{command}#{id} ret {reply} != {expected}` e.g. `F10#12 ret F10 err != ('F10 ok. FFS channel 2 feeding.',)` | `zmod_ifs.py:255` `send_command_and_wait` | `!!` | Aborts the IFS command; runs `_ENABLE_SENSOR` | The IFS board refused the command (wrong state, lane busy, unknown channel) | Wait for the IFS to finish, run `IFS_UNLOCK` / `IFS_F15`, retry | N |
| I8 | `Timeout waiting for response from command {command}#{id}` (ru `Таймаут ожидания ответа от команды ...`) | `zmod_ifs.py:262-267` | `!!` | Aborts; runs recovery `IFS_F112`+`IFS_F18` | No reply from the IFS board over RS-485 within 5 s: cable, board hang, or native screen fighting for the port | Check the IFS cable; make sure the native screen is off; power-cycle | N |
| I9 | `IFS: Timeout waiting for status {want}|5, received {state}` (ru `IFS: Вышло время для получения статуса ...`) | `zmod_ifs.py:331-335` `wait_for_state` | `!!` | Aborts load/unload; recovery F112+F18 | The IFS never reached the expected state within the step budget (10 s default, 120 s for loads). `{state}` decodes as: 11/22/33/44 still loading lane 1-4, 15/26/37/48 still unloading, 7/18/29/40 clamped, 3 polling | Filament is not moving: check for a jam in that lane's tube or at the toolhead entry; for loads, confirm the toolhead sensor works | N (backend has its own operation-timeout fault: `check_action_timeout`, 90 s) |
| I10 | `Speed cannot be = 0` | `zmod_ifs.py` `cmd_IFS_F10` / F11 | `!!` | refuses | A filament.json speed of 0 | Fix `filament_ifs_speed` in `mod_data/filament.json` | N |
| I11 | `IFS is not available` / `IFS доступен`-pair; `Empty response from device {cmd}` | `zmod_ifs.py:1260-1279` | `//` | none (state flips `available`) | RS-485 link lost / regained | Check the IFS cable and power | N (backend notes presence via status only) |
| I12 | `IFS: sensor error: Serial communication error: {e}` / `IFS: sensor error: Error data: {e}` | `zmod_ifs.py:1310,1313` | `//` | none | Serial port error / garbled frame | Reseat cable; power-cycle | N |
| I13 | `Error reading filament sensor: {e}` | `z_ad5x/.shell/zmod_ifs_switch_sensor.py:51,100` | `//` | none | IFS port sensor read failed | as I12 | N |
| I14 | `IFS Off` | `ad5x_display_off.cfg:246` `_IFS_OFF`, run by every IFS command when `self.ifs` is false | `//` | IFS commands silently do nothing | IFS not detected or disabled in ZMOD | Enable IFS / check cable; `SAVE_ZMOD_DATA` IFS setting | N (worth a "filament system offline" state) |
| I15 | `Filament jam detected (IFS).` | `z_ad5x/translate/en/ad5x_display_off.cfg:17` `_PRINT_IFS_MOTION PAUSE=1` | `!!` + `tgalarm_photo` | **PAUSE** | The IFS motion sensor saw no movement for `detection_length` mm while printing: tangle, kinked tube, wrong slot recorded as loaded, old filament still in the head, or a lane that would not unclamp | Check the active spool and tube; if the slot is wrong use `SET_EXTRUDER_SLOT`; if false alarms, raise `detection_length` in `[zmod_ifs_motion_sensor ifs_motion_sensor]` (zmod FAQ: 8 to 17); `filament_unload_into_tube` >= 70 | P (backend runout detector covers head-empty; a jam with the head still loaded is N) |
| I16 | `Filament runout detected (IFS).` + `Auto run _RESUME_MOTION_SENSOR` | `ad5x_display_off.cfg:23-29` (PAUSE=0 branch) | `//` | auto-resumes | Same sensor, configured not to pause | none | n/a |
| I17 | `Filament runout detected (Head)` | `ad5x_display_off.cfg:56` `_PRINT_HEAD INFO=0` via `ANALOG_PRUTOK` (`zmod_ifs.py:709`) | `!!` | **PAUSE** (`head_switch_sensor pause_on_runout: True`) | The toolhead switch went empty and no other lane holds the same type and color (or backup is off) | Load filament into the empty lane (or another), then Resume | Y (backend "Filament runout" fault with Load/Resume/Purge/Unlock actions) |
| I18 | `Filament runout detected (Head) Loading a similar rod for the tool T{n}` | `ad5x_display_off.cfg:52` `_PRINT_HEAD INFO=1` | `//` | auto-swaps to a same-type, same-color lane | Backup spool switchover | none | P (backend waits 180 s before raising its own runout when backup is on) |
| I19 | `Invalid SLOT. Valid: 1-4`, `Invalid T{i}: {t}. Valid: 1-4`, `Invalid material type: {}. Valid: {}`, `Specify HEX or TYPE`, `Invalid direction (0-1)`, `Invalid LEVELING`, `Missing FILENAME parameter` | `zmod_color.py:1010-2016` (`_t('error_*')`, 13 languages) | `!!` | refuses | Bad parameters, usually from a UI or plugin | (our own commands: should never fire; log) | N |
| I20 | `T4 or higher not supported on native screen, found T{n}` | `zmod_color.py:1383,1387,1684` | `!!` | print start refused | Sliced with more than 4 tools while the native screen is in use | Re-slice with at most 4 filaments, or turn the native screen off | N |
| I21 | `Spool auto-assignment failed, return code {n}` | `zmod_color.py:1567` | `!!` | print start refused | No loaded lane matches a tool's material/color (bitmask of material/color failures) | Load the right filaments or map tools by hand | N |
| I22 | `Error: CHANNEL {c} is out of range (max {m})` / `Error: CHANNEL parameter is required` / `Error: Display on` | `zmod_color.py:1012,1719,1732` | `!!` | refuses | Bad call; or a color command sent while the native screen owns the file | Turn the native screen off | N |

bambufy / lessWaste on AD5X (macro plugins over ZMOD): no `!!` text. Each pause is a `PAUSE REASON=`
plus an `_INFO_DIALOG` prompt (`bambufy/bambufy.cfg:1610-1624`): `jam` "Motion jam detected on Port {p}",
`broken` "Broken filament near head, Port {p}", `runout` "Filament runout on Port {p}",
`nobackup` "Filament runout on Port {p}. No backup available", `empty` "IFS Port {p} empty. Insert filament",
`loading` "Jam detected while loading Port {p}", `backup` (no pause dialog; auto-swap). HS: P
(prompt shown by the action-prompt modal; the REASON word is not parsed, see
`docs/devel/FILAMENT_BACKEND_AD5X_IFS.md` "PAUSE-reason follow-up").

Forge-X AD5X `[ifs]` module: `ifs.error` free text + `activity == "driver_error"`. HS: Y for the
driver-error state ("IFS driver error" or the module's text). The module's message vocabulary
is **unverified** (no public source).

### 4b. ZMOD, all models (AD5M, AD5X, C5): `z_ad5x` / `z_c5pro` 1.7.6

| # | Msg | Where | Path | Effect | Model | Cause | Fix | HS |
|---|---|---|---|---|---|---|---|---|
| Z1 | `!! Nozzle hit bed or part detachment. Weight {w}>{max}. Z={z}. PAUSE. https://wiki.zmod.link/Global/#nozzle_control` | `z_ad5x/.shell/zmod_tenz.py:202`, `spi_temperature.py:169` | `respond_raw !!` | **PAUSE** | AD5M, AD5X (load cell) | The bed load cell read more than the NOZZLE_CONTROL weight limit: nozzle dragging on the part, a part knocked loose, or warping | Remove the loose part / clear the nozzle, then Resume; if false alarms raise `NOZZLE_CONTROL WEIGHT=` | N |
| Z2 | `Nozzle hit bed or part detachment. Weight {w}>{max}. Z={z}. FIRMWARE_RESTART. https://...` | `zmod_tenz.py:187-193`, `spi_temperature.py:189` | **Klipper shutdown** (`invoke_async_shutdown`) | Print lost | AD5M, AD5X | Same as Z1 below the ZCONTROL_Z height (ZCONTROL_AUTO) or with ZCONTROL_ABORT. Also fires when a home probes into a part on the bed (bundle XWPBR2DX) | Clear the bed, FIRMWARE_RESTART | N (shutdown text shown raw) |
| Z3 | `Cell Tare: Error. Weight: {a}->{b} https://wiki.zmod.link/FAQ/` | `zmod_tenz.py:148-152` | `!!` | Stops START_PRINT | AD5X, C5 **unverified** | Load cell would not zero: something resting on the bed, nozzle touching, or a load-cell fault | Clear the bed and nozzle, retry; see zmod FAQ "WeightValue" | N |
| Z4 | `Load cell reset error. Read FAQ: https://wiki.zmod.link/FAQ/` (+ `Weight at error: {w}`) | `translate/en/ff5m_klipper11.cfg:150` | `!!` | Stops START_PRINT | AD5M on stock Klipper 11 | Same as Z3 | Same; on native screen enable `SAVE_ZMOD_DATA PRINT_LEVELING=1` | N |
| Z5 | `Filament runout or jam detected` | `ff5m_motion_sensor.cfg:13` | `!!` | **PAUSE** (`pause_on_runout: True`) | AD5M with sensor as motion sensor | Spool empty or filament not moving | Load filament / clear jam, Resume; tune `detection_length` (8 to 17) | N |
| Z6 | `Filament runout detected` | `ff5m_switch_sensor_display_off.cfg:34` | `!!` | **PAUSE** after 30 s delay | AD5M switch sensor | Spool ran out | Load filament, Resume | N |
| Z7 | `Filament runout detected T{t}` | `z_c5pro/translate/en/c5pro_display_off.cfg:289` (also prompt "T{t} runout" at 250) | `!!` | **PAUSE** | C5 | Head t's presence switch went empty | Load filament into head t, Resume | P (toolchanger backend fault dialog **unverified** for this path) |
| Z8 | `Filament jam detected T{t}` | `c5pro_display_off.cfg:280` | `!!` | **PAUSE** (only if filament still present) | C5 | Head t's wheel (motion) sensor stopped while filament is present: a clog | Clear the clog (cold pull) on head t, Resume | N |
| Z9 | `Resumption interrupted !!! "{extruder}" is not hot enough (1)/(2), please heat it up and press RESUME again` + prompt | `base_client.cfg:181,188` | `!!` + action prompt | Resume refused, stays paused | all | The nozzle cooled during the pause (idle timeout) | Heat the nozzle to print temperature, Resume | N (HS Resume action already reheats first; the line still shows raw) |
| Z10 | `Resumption interrupted !!! "{sensor}" has detected that the filament has run out, please load it and press RESUME` + prompt | `base_client.cfg:215` | `!!` + prompt | Resume refused | all | Runout sensor still empty | Load filament, Resume | P (AD5X runout fault) |
| Z11 | `Error: File '{f}' was not verified in time. Print canceled.` / `Error: File '{f}' not specified. Print canceled.` / `Error: File {f} not found. Print canceled.` | `base.cfg:222,227,232` | `!!` | **CANCEL** | all | MD5 check could not finish / bad file name | Re-upload the file | N |
| Z12 | `File {f} has no MD5 checksum. Enable it in Orca. https://wiki.zmod.link/Recommendations/` | `base.cfg:237` | `!!` | print continues or cancels per FORCE_MD5 (**unverified** which) | all | Slicer did not add the MD5 line | Turn on MD5 output in Orca-FlashForge, or `SAVE_ZMOD_DATA FORCE_MD5=0` | N |
| Z13 | `File {f} MD5 mismatch. File deleted. Print canceled` / `... MD5 mismatch. Print canceled` / `MD5 check failed. Print canceled.` | `base.cfg:255,260,267` | `!!` | **CANCEL** (may delete the file) | all | Upload was corrupted (Wi-Fi/eMMC) | Re-upload the file | N |
| Z14 | `File {f} contains arc commands (G2, G3). Disable them. In Orca: ...` / `File {f} uses unsupported G17/G18/G19 commands. ...` | `base.cfg:241,247` | `!!` | **CANCEL** (**unverified**) | all | Arc fitting or spiral Z-hop in the slicer | Turn off arc fitting and spiral Z-hop, re-slice | N |
| Z15 | `WARNING: Last probe EXCEEDS saved value by {d} mm! Use SAVE_ZMOD_DATA MESH_TEST={n} ...` / `... BELOW mesh minimum by {d} mm! ...` / `... ABOVE mesh maximum by {d} mm! ...` | `base.cfg:1391,1498,1507` (`action_raise_error`) | `!!` | **Stops START_PRINT** | all | The pre-print probe disagrees with the saved mesh: wrong plate, different temperature, or debris | Clean the plate, re-mesh, or let ZMOD auto-run KAMP (`MESH_TEST`) | N |
| Z16 | `{MACRO} - this cleanup macro does NOT exist. LINE_PURGE is used. ...` | `base.cfg:1820` | `!!` | continues | all | Configured purge macro missing | Add it to user.cfg or disable priming | N |
| Z17 | `Wait for the music and power off the printer. Wait 10 seconds and power on again. MCU update will begin.` | `base.cfg:1868` | `!!` | n/a | all | UPDATE_MCU staged | Power-cycle (not reboot) | N |
| Z18 | `Parameter FORCE_MD5 moved to SAVE_ZMOD_DATA ...` (and DISABLE_PRIMING, DISABLE_SKEW_CORRECT, CLEAR) | `base_mod.cfg:98-124`, `base_display_off.cfg:124-150` | `!!` | refuses | all | Old START_PRINT parameters from an old slicer profile | Update the slicer start gcode | N |
| Z19 | 3MF: `!! 3MF Error: Plate N{p} not found in file`, `!! MD5 Error: Extracted G-code hash does not match the internal .md5 file`, `!! 3MF unpack error: {e}`, `!! 3MF: Moonraker metascan failed: {e}`, `!! 3MF: Printer is not ready`, `3MF: Moonraker metadata scan is already pending`, `PLATE must be >= 1`, `3MF: invalid path` | `z_ad5x/.shell/zmod.py:102-344` | `!!` / RPC | Print start refused | all, ZMOD >= 1.7.5 | Bad plate number or corrupt 3MF | Re-slice/re-upload; pick a plate that exists | N |
| Z20 | `!! ZEXCLUDE_MF: Update Orca Slicer >= 2.5.0 !!` | `z_c5pro/.shell/zmod.py:395` | `!!` | continues | all | Old slicer object labels | Update Orca | N |
| Z21 | `LOAD_ZOFFSET_NATIVE: File not found {a}, {b}` | `zmod.py:417` | `!!` | refuses | all | Native screen z-offset file missing | Set Z-offset once | N |
| Z22 | AFC shim: `AFC: {no_lane}`, `AFC: unknown lane {l}`, `AFC: {mat}? {types}`, `AFC: {resp}`, `AFC: bad MAP {t}` | `z_ad5x/.shell/zmod_afc.py:271-391` | `!!` | refuses | C5 (afclite shim) | Bad lane/material from a client | (ours: log) | N |
| Z23 | `!! Cannot update MCU 'eboard' config as it is shutdown` | stock Klipper prose; zmod FAQ "Cannot Update MCU" | Klipper shutdown | print lost | AD5M/AD5X | A soft reboot left the MCU running; or a shorted heater rebooted the printer mid-print | FIRMWARE_RESTART, or power off and on | covered by the Klipper effort; FF-specific fix text worth adding |
| Z24 | `MCU Protocol error`, `Unknown temperature sensor flashforge_loadcell`, `flashforge_loadcell: Required MCU command flashforge_loadcell_h1 is not available` | stock Klipper prose; zmod FAQ "MCU Protocol error" | Klipper config error / shutdown | Klipper not ready | AD5M/AD5X | Klipper version and MCU firmware do not match (Klipper 13 vs native 11/12) | Remove `klipper13 = 1` from `mod_data/variables.cfg` and power-cycle, or `UPDATE_MCU FORCE=13` | N |
| Z25 | `Timer too close` / `Communication timeout during homing` (stock screen E0011) | stock Klipper prose | shutdown / homing error | Print lost | AD5M/AD5X | Host or MCU missed its 25 ms deadline: CPU/eMMC overload, swap, exclude-object heavy files, fuzzy skin, Klipper 13 on AD5X | `SAVE_ZMOD_DATA FIX_E0011=1`; untick "Exclude models" in Orca; on AD5X `UPDATE_MCU FORCE=12` | covered by the Klipper effort; FF-specific fixes worth adding |

### 4c. Forge-X (AD5M / AD5M Pro)

| # | Msg | Where | Path | Effect | Cause | Fix | HS |
|---|---|---|---|---|---|---|---|
| F1 | `Bed pressure detected: {g} g.` | `ff5m/macros/base.cfg:53` (`weightValue` exceed_gcode) | `!!` | `ALARM`; **M112 emergency stop** if printing and over `weight_check_max` | Load cell saw weight on the bed: nozzle crash, part knocked off | Clear the nozzle/part, FIRMWARE_RESTART | N |
| F2 | `!! Detected bed pressure. Please ensure the bed is clean!` | `.py/klipper/plugins/load_cell_tare.py:103` | `!!` | lowers the bed, continues tare | Something on the bed before tare | Clean the bed | N |
| F3 | `Tare conformation did not reset.` / `Load cell tare failed: weight {w} > threshold {t}` / `Load cell tared, but no confirmation from level_pin; configured to skip.` / `Load cell tare failed. No tare confirmation received` | `load_cell_tare.py:56,79,82,85` (`_raise_error`) | `!!` | **CANCEL_PRINT REASON="Cell tare failed!"** when printing | Load cell would not zero or did not confirm | Clear the bed; power-cycle; check the load-cell cable | N |
| F4 | `!! ERROR: MD5 checksum mismatch: {a} != {b}` / `!! ERROR: File doesn't exists: {f}` / `MD5 check failed. Print cancelled!` / `WARNING: No MD5 checksum found in G-code.` | `.py/klipper/plugins/md5_check.py:39-79` | `!!` | **CANCEL_PRINT** (may delete file) | Corrupted upload | Re-upload | N |
| F5 | `Probe result doesn't match. Expected difference within {t} mm, but got {d} mm.` | `macros/base.cfg:339` | `!!` | `_CHECK_BED_MESH_HANDLE_FAIL` re-meshes or fails (**unverified** which) | Saved mesh no longer matches the plate | Re-run bed mesh | N |
| F6 | `Unable to start print. Filament runout detected!` | `macros/base.cfg:1118` | `!!` | **CANCEL_PRINT REASON="FILAMENT RUNOUT"** | Runout sensor empty at start | Load filament | N |
| F7 | `Filament runout detected!` | `macros/headless.cfg:39` | `!!` | PAUSE (**unverified**) | Spool ran out | Load, Resume | N |
| F8 | `Resume aborted !!! "{ex}" not hot enough, please heat up again and press RESUME` / `... detects no filament, please load filament and press RESUME` | `macros/client.cfg:148,155,166` (Klipper Mod identical: `kmod/printer_configs/client.cfg:140-158`) | `!!` + prompt | stays paused | as Z9/Z10 | as Z9/Z10 | N |
| F9 | `Missing 'EXTRUDER_TEMP' or 'BED_TEMP' parameter!` + 2 hint lines | `config/stock.cfg:72-74`, `macros/headless.cfg:142-144` | `!!` | START_PRINT fails | Slicer start gcode wrong or wrapped | `START_PRINT EXTRUDER_TEMP=... BED_TEMP=...` on one line | N |
| F10 | Resurrection: `!! The resurrection file missing!`, `!! Failed to resurrect. ...` (invalid file, missing field, file missing, size mismatch, bed mesh missing), `!! The printer isn't in a resurrection state!` | `.py/klipper/plugins/resurrection.py:179-385` | `!!` | recovery refused | Power-loss recovery data unusable | Start the print over | N |
| F11 | `Shutdown triggered by button` | `macros/headless.cfg:54` (Klipper Mod `macros.cfg:431`) | `!!` | shutdown | Power button | none | N |
| F12 | `NEW_RESTART is deprecated ...`, `NEW_SAVE_CONFIG is deprecated ...`, `!! Parameter {k} is deprecated. Use {n} instead!`, `Unknown parameter: "{k}"`, `Updating readonly parameter ...` | `stock.cfg:183`, `base.cfg:1440`, `plugins/mod_params.py:249-286` | `!!` | refuses | Old command names | Use the new name | N |

### 4d. Creator 5: OpenCreator (Kalico) and Klipper4FlashForge

| # | Msg | Where | Path | Effect | Cause | Fix | HS |
|---|---|---|---|---|---|---|---|
| C1 | `E0163: Paused. T{t} clog detected: filament is present but the wheel stopped moving. Clear the filament path before resuming.` | `oc-kalico/klippy/extras/creator5_toolchanger.py:1650` (AFC_error) | `!!` via AFC | **PAUSE** | Head t's filament wheel stopped while filament is present | Cold pull / clear the head t path, Resume | N |
| C2 | `T{t} wheel failure: mount sensors invalid: {e}` / `T{t} wheel failure: active tool state is inconsistent; print paused` | `creator5_toolchanger.py:1626,1636,1642` | `!!` via AFC | **PAUSE** | Clog check could not trust tool sensors | See C4 | N |
| C3 | `Conflicting dock/grab sensors; motion blocked` / `Multiple heads appear attached; motion blocked` / `Missing dock confirmation for T{a},T{b}; motion blocked` | `creator5_toolchanger.py:1006,1010,1024`; also standing as `creator5_toolchanger.sensor_error` | `!!` + status field | Tool motion refused; mesh and loads refuse with `Tool sensors invalid, not meshing: {e}` / `Tool mount sensors invalid: {e}` (`printer.mesh.cfg:23`, `printer.filament.cfg:105,155`) | A dock or grab switch reads wrong: a head not seated in its dock, a dirty/failed switch, or the carriage left holding a head | Check every head is seated in its dock; check the dock/grab switches in Settings > Sensors | N (standing `sensor_error` is a natural status-driven fault) |
| C4 | `T{t} stayed in its dock after {n} grabs; heaters disabled` / `Tool sensor verification failed; heaters disabled` / `T{t} was not confirmed in its dock` / `T{t} grab sensor did not engage at mount` | `creator5_toolchanger.py:1247-1253,1446` | `!!` | Toolchange fails, heaters off; print stops (**unverified** whether paused or cancelled) | Pickup failed (stock E0051+t / E0139) | Re-home, check the head moves freely in the dock, retry | N |
| C5 | `T{t} is not seated in its dock (dock sensor {s}, mount sensor {m} at X=.. Y=.. after {n} approaches); not releasing it (heaters off). XY may have lost steps: re-home before docking` | `creator5_toolchanger.py:1352` | `!!` | Toolchange fails, heaters off | Release failed (stock E0135+t / E0142) | Re-home XY, then dock | N |
| C6 | `Home {axes} before Creator 5 motion` / `Creator 5 toolchanger is already moving` / `Confirm all heads are docked before print homing` / `Attached head remains after recovery; Z homing blocked` | `creator5_toolchanger.py:1075,1082,1536,1538` | `!!` | refuses | Precondition | Home / dock all heads | P ("Must home" rewrite does not match "Home X before") |
| C7 | `!! Motion guard: {axis} skipped steps: {n} electrical cycles ({4n} full steps) so far` / `{axis} stalled ({n} so far)` / `, pausing` / `pause failed: {e}` | `oc-kalico/klippy/extras/c5_motion_guard.py:166-184` | `!!` | **PAUSE** when `action=pause` | X/Y motor lost steps or stalled: crash into a part, belt slip | Remove the obstruction, re-home before resuming | N |
| C8 | `{"module": "estop", "status": "fail", "coded": "0201-0000-0003", "msg": "No trigger on probe after full movement"}` (and `-0001` "Probe triggered prior to movement", `-0005` wrapped reason; `-0002` "The estop is unqualified" as info) | `oc-kalico/klippy/extras/e_stop.py:280-357`; stock 2.0.0 klippy has the same family | `!!` JSON | Calibration step fails | Nozzle-offset/levelboard probe never touched (-0003, stock E0149) or was already pressed (-0001, stock E0151) | Clean the nozzle and the levelboard pin; check the build plate is removed for offset calibration | P (envelope decoder strips the JSON, shows msg; no cause/fix) |
| C9 | `No filament detected in T{t}` / `T{t} has no print temperature to load at` | `opencreator-fs/.../printer.filament.cfg:347,349` | `!!` | stays in runout prompt | Runout Load tapped with an empty sensor / no target temp | Insert filament until the sensor sees it / set a temperature | N |
| C10 | `Pause the print before loading filament` / `... feeding filament` / `... extruding`, `Heat the selected tool before feeding filament`, `Loading requires a physically attached Creator 5 tool`, `Active hotend does not match the attached tool`, `T{m} is mounted but not requested; cool and dock it before loading other tools` | `printer.filament.cfg:61-194,373-384` | `!!` | refuses | Precondition | as stated | N |
| C11 | `Remove the build plate before offset calibration. Once removed, rerun C5_CALIBRATE_OFFSETS with BUILDPLATE_REMOVED=1` / `Build plate may still be installed; ...` | `printer.creator5.cfg:644`, `creator5_toolchanger.py:1899` | `!!` | refuses | (stock E0147) | Remove the plate | N |
| C12 | `Print cancelled during print start` | `printer.creator5.cfg:169` | `!!` | cancel | user cancelled | none | N |
| C13 | `Dryer not connected on {port}` | `creator5_dryer.py:219` | `!!` | refuses | Dryer cable | Check cable | N |
| C14 | Klipper4FlashForge: `T{t} ... (firmware error E0127+t)`, `... (firmware error E0051+t)`, `... (E0131+t)`, `T{t} release failed: {d} (firmware error E0135+t)`, `... (E0144)` | `k4ff/pkgs/klipper/payload/klipper/klippy/extras/ff_toolchange.py:1033-1222` | `!!` | Toolchange fails | as C4/C5 | as C4/C5 | N |
| C15 | Klipper4FlashForge clog: `PAUSE` + `M117 T{n} clog` | `k4ff/docs/notes/49-runout-recovered.md:97` | display message | PAUSE | as C1 | as C1 | N |

### 4e. Stock C5 E-code table (spec reference; reaches no HelixScreen install as a code)

From `k4ff/docs/notes/70-error-codes.md` (Ghidra of firmwareExe 2.0.x). Useful as the vocabulary
for wording C1-C11 and the Klipper-prose cases, and as the code to show beside our text so users
can search for it:

- Toolchanger: E0127-E0130 T1-T4 not in dock (cannot pick up); E0051-E0054 pickup failed; E0131-E0134 already in dock (cannot release); E0135-E0138 release failed; E0139 mount sensor not triggered; E0140 dock sensor not triggered; E0141 mount sensor still triggered; E0142 dock sensor still triggered; E0143/E0144 state error after pickup/release; E0145 lock motor current abnormal (raiser disputed); E0146 unlock sensor not triggered; E0160 cannot release, homing failed; E0165 extruder not installed.
- Leveling/eddy: E0041 leveling sensor cannot reset; E0147 build plate not removed; E0148 leveling extremes out of range; E0149 sensor not triggered at max travel; E0150 homing timeout; E0151 eddy probe triggered before move; E0152/E0153 repeat measurements out of range; E0161 bed eddy position abnormal; E0167/E0168 platform not installed.
- Heating: E0063-E0066 extruder 1-4 heating error; E0123 bed; E0077 chamber; E0154 drive motor temperature (PCB fan).
- Axes: E0012/13/14 X/Y/Z signal not triggered; E0155/56/57 limit still triggered after retract; E0158 sensor error; E0159 homing error.
- Comms: E0002 MCU lost; E0124 eboard; E0125 heaterboard; E0126 levelboard; E0170-E0173 lost communication with main/extruder/heater/level MCU; E0005 printer not ready (wiring).
- Print: E0162 runout; E0163 clog; E0164 spaghetti (dead string); E0166 print file error; E0169 model mismatch, re-slice; E0175 internal error on command; E0176 move out of range; E0021 camera; E0174 copy log; E0999 unknown.

---

## 5. Recommended rows to word first

Ranked by (how often it happens) x (how bad the raw text reads) x (whether it stops a print):

1. **I15 `Filament jam detected (IFS).`** (pauses; most common AD5X stop per the zmod FAQ; the fix list is concrete).
2. **I1 `Failed to extract filament from extruder`** (the one real error on the unload/color-change chain; dangerous if the user just retries cold).
3. **I9 `IFS: Timeout waiting for status X|5, received Y`** and **I8** (unreadable numbers; decode `received` into "lane N still loading/unloading").
4. **Z1/Z2 nozzle-control hit** and **F1 bed pressure** (Z2 and F1 kill the print or Klipper; users do not know what NOZZLE_CONTROL is).
5. **Z15 mesh-check WARNINGs** and **Z11-Z14 MD5/file checks** (stop or cancel a print before it starts; the fixes are slicer settings).
6. **Z9/Z10 and F8 "Resumption interrupted"** (very common after a long pause; we can show it as a normal "heat up first" note, since our Resume reheats).
7. **C1 E0163 clog**, **C7 motion guard**, **C3/C4/C5 toolchanger sensor and pickup faults** (C5 is newer, but these are its print-stopping faults).
8. **Z5/Z6/Z7/Z8 runout/jam on AD5M and C5** (pause; a plain "load filament, then Resume" plus the detection_length hint).
9. **Z24/Z25 FF-specific fixes** for Klipper prose (Klipper 13 mismatch, FIX_E0011): add FF hints to the rows the Klipper effort writes, keyed on the printer family.
10. **C8 FF JSON coded errors**: add cause/fix for the three e_stop codes; everything else in that envelope already decodes to its msg.

---

## 6. Matching strategy

- **Follow `include/snapmaker_exceptions.h` where a stable identity exists:** the FF `coded` values (`0201-0000-000N`, three-part) and the OpenCreator/K4FF `E0NNN` strings. Key a small table on those (summary + cause + fix), exactly as the U1 table does, and leave the msg as the fallback. The envelope decoder already extracts `code` (Opaque); a `flashforge` row keyed on the three-part code is the cheapest win. Do not feed three-part FF codes into `decode_exception_code` (it would misread them; it already refuses three-part forms).
- **Everything else is prose; anchor it.** Match `!!` lines by a prefix-anchored pattern per row (`^Filament jam detected \(IFS\)`, `^IFS: Timeout waiting for status (\d+)\|5, received (\d+)`, `^Timeout waiting for response from command (F\d+)#\d+`, `^No filament (\d+) in IFS`, `^Filament (\d+) stalled in IFS`, `Nozzle hit bed or part detachment\. Weight (\d+)>(\d+)`), capture the lane/tool/state numbers and put them into our text. Use `helix::Regex`, never `std::regex`.
- **Gate by firmware.** Only try the ZMOD/IFS rows when discovery says ZMOD (`zmod_ifs` / `zmod_color` objects), Forge-X rows on Forge-X, OC rows when `creator5_toolchanger` exists. Same "provider table" shape as `firmware_fault_codes.h`, so the router stays firmware-blind.
- **Language.** ZMOD can run in 13 languages. Two options: (a) build the matcher per language from `translate/<lang>/*.cfg` and the `_t()` tables in `zmod_color.py` (all strings are in the source), or (b) prefer state over text: the IFS `State` field in `zmod_ifs` status, `print_stats.state == paused`, the runout sensors, `creator5_toolchanger.sensor_error`, `c5_motion_guard` status. For the Python IFS errors only English and Russian exist; adding the Russian forms covers them fully. Recommend (b) where a status field exists, (a) for the rest, English + Russian first.
- **De-duplicate.** ZMOD raises most runout/jam lines twice (`!!` plus `tgalarm_photo`) and often also opens an action prompt with the same meaning. Treat `tgalarm_photo` lines as non-errors (they are not `!!`, so this already holds) and suppress our own modal when the firmware's prompt is up, or vice versa.
- **Pair with the existing AD5X backend.** I1, I6, I9 should become the `operation_detail` of the IFS backend's fault (it already moves to ERROR and shows the raw `!!`), so the "Filament System Error" dialog gets our cause and fix plus the right recovery actions (Unlock, Reset driver, Cold pull).

---

## 7. Gaps and open questions

- **No stock AD5M/AD5X firmwareExe decompile is public.** The E0011/E0017/E0120 meanings come from zmod docs only; E0017's actual condition is **unverified**. Whether the AD5M/AD5X table equals the C5 table is **unverified**.
- **Stock C5 2.0.0 klippy source is not public** (only OpenCreator's Kalico, which keeps some vendor code). The full list of FF JSON `coded` errors on stock klippy is unknown beyond the e_stop family; HelixScreen's own C5 doc says 2.0.0 turned the probe-triggered one into plain text. A strings pass over a stock `/usr/prog/klipper/klippy` would settle it.
- **Mapping between FF `coded` values and stock E-codes** (`0201-0000-0003` likely E0149, `-0001` likely E0151) is **unverified**.
- **Forge-X AD5X `[ifs]` module** has no public source; its `ifs.error` vocabulary is **unverified**.
- **Print effect of IFS errors mid-print:** whether `_A_CHANGE_FILAMENT` / `T<n>` macros catch `gcode.error` from `zmod_ifs` (pause) or let it stop the print was not traced; **unverified**.
- **lessWaste** plugin source was not cloned (bambufy only).
- **Klipper Mod** was skimmed only for raised errors; it has no load-cell or IFS layer, so it adds nothing FF-specific beyond F8/F11.
- **Language coverage:** only `translate/en` and the Russian branches were read; the other 11 languages exist and would need extraction for option (a).
- Severity rows marked "(**unverified**)" for Z12, Z14, F5, F7, C4 need a run on a device or a deeper macro trace.
