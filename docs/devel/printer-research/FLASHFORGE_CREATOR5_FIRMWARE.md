# FlashForge Creator 5 Stock Firmware: Packages, 2.0.0, and the Stock UI

Research record, 2026-10-27. Stock firmware 1.9.9 (2026-09-07) and 2.0.0 (2026-10-14),
unpacked and compared. How HelixScreen supports the printer today lives in
[`../printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md`](../printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md).

## Getting a package

FlashForge's download page lags its releases. 2.0.0 was on the printer's update channel
and on FlashForge's storage for days while the page still listed 1.9.9 as newest.

Packages live in a public object store under a folder named "C5固件" ("C5 firmware"):

```
https://flashforge-resource.oss-us-east-1.aliyuncs.com/C5%E5%9B%BA%E4%BB%B6/Creator5-<software>-<control>-<YYYYMMDD>.tgz
```

The bucket refuses listing, but a missing object answers 404 rather than 403, so a HEAD
request per candidate name finds unlisted releases. The Creator 5 Pro has no separate file
there; ghzserg's mirror (`github.com/ghzserg/FF`, release `R`) carries both names with
identical sizes, and the Z-Mod wiki's native-firmware page lists every version.

| Software | Control (MCU) | Build date | Notes |
|----------|---------------|------------|-------|
| 1.7.3 | 1.1.5 | 2026-04-11 | First release |
| 1.7.8 | 1.1.7 | 2026-04-27 | |
| 1.9.4 | 1.2.6 | 2026-07-02 | Exclude objects, model isolation for print files |
| 1.9.5 | 1.2.6 | 2026-07-23 | Nozzle overheat protection, power-loss recovery |
| 1.9.6 | 1.2.9 | 2026-08-05 | VDS dryer, maintenance reminders, VFA compensation |
| 1.9.7 | 1.2.9 | 2026-08-10 | New magnetic sheet |
| 1.9.8 | 1.2.9 | 2026-08-19 | Upgrade-failure fix |
| 1.9.9 | 1.3.0 | 2026-09-07 | exFAT/NTFS USB, hidden SSID, 3MF nozzle info; cannot roll back to 1.9.8 or earlier |
| 2.0.0 | 1.3.1 | 2026-10-14 | Multi-plate, filament checks, rollback, calibration records |

## Unpacking

Unlike AD5X and AD5M images, which are plain tar, Creator 5 packages are encrypted with
OpenSSL's legacy DES3 and a key that is public in Klipper4FlashForge's
`config.env.example` (`FF_KEY`):

```bash
openssl des3 -d -k 'FFP0331&*%root' -salt -md md5 -in Creator5-2.0.0-1.3.1-20261014.tgz | tar -xf - -C outer/
tar -xf outer/software-2.0.0.tar.xz -C software/
```

`-md md5` is required on modern OpenSSL; the printer's own `unTar` uses OpenSSL 1.0.2,
whose default digest was MD5.

## Package layout: 1.9.x deltas, 2.0.0 full images

1.9.x packages are deltas. The outer tar carries separate `kernel-*`, `control-*` and
`library-*` archives, and `software-*` carries only the Klipper files that changed.

2.0.0 is a complete image. Everything moved inside `software-2.0.0.tar.xz`:

| Path in software/ | Contents |
|-------------------|----------|
| `kernel/2.0.8/` | OTA kernel for eMMC and SD-NAND units |
| `control/1.3.1/` | MCU images: `mainBoardGD.hex`, `eBoard.hex`, `heaterBoard.hex`, `levelBoard.hex`, `VDS_V1.0.2_0.hex` |
| `library/1.2.5/` | fonts, ffmpeg, the two sample `.gcode.3mf` models |
| `klipper/klippy.zip` | the whole klippy tree (133 extras) |
| `klipper/config/` | the eight `printer.*.cfg` files |
| `auth/` | `passwd`, `shadow` (at the top level in 1.9.x) |
| `firmwareExe.zip` | the stock screen app |

run.sh installs every component unconditionally, so any full image installs over any
version. That is the changelog's "firmware version rollback support": nothing on the
printer manages versions.

Tools that read stock paths need the new locations. Klipper4FlashForge's pack script
reads the unpacked `shadow` from the top of the software component to verify its baked
stock root hash, and silently skips the
check when the file is absent, which it is in 2.0.0. The hash itself is unchanged from
1.9.9, so the skip costs nothing today.

## The 2.0.0 changelog, mapped to code

All eight `printer.*.cfg` files are byte-identical between 1.9.9 and 2.0.0. Every
user-facing 2.0.0 feature lives in `firmwareExe` (MIPS32 LE, stripped C++, LVGL 8), which
HelixScreen replaces under Z-Mod. The behavior below was read from that binary's strings
and disassembly (capstone).

### Multi-plate print files

preBuild.cpp (the pre-print screen) reads `Metadata/model_settings.config` from the 3MF:
`plater_id`, `gcode_file`, `thumbnail_file` per `<plate>`, adds previous/next plate
buttons, and starts the print with the chosen `platerId`. The file layout is the standard
Bambu/Orca one: `Metadata/plate_<N>.gcode`, `slice_info.config` (with per-plate
`filament_maps`, `<filament id type color used_g>` and `layer_filament_lists`), and
`model_settings.config` (`plater_name`, `filament_maps`, `filament_map_mode`).

### Filament consistency check

New for plain `.gcode` files: the `; filament_type =` header line is compared against the
per-extruder loaded types (`ex0_filament_type` .. `ex3_filament_type`). A mismatch logs
"gcode filament type does not match local extruder" and skips the temperature check. 3MF
files already went through a per-filament match (`checkFilamentIsSame`) in 1.9.9.

### Mixed high/low temperature warning

`PreBuild::getFilamentTemperatureGroup`:

1. Normalize the type: uppercase, drop spaces, `-` and `_`.
2. Any normalized type containing `TPU` is Low.
3. Otherwise match exactly against a 23-entry enum and map it through a jump table.

| Group | Materials |
|-------|-----------|
| Low | PLA, PETG, PLA-CF, PETG-CF, PVA, BVOH, TPU-90A, TPU-95A, TPU-64D, any TPU |
| Medium | HIPS |
| High | ABS, ASA, PET-CF, PA-CF, ASA-CF, ABS-GF, PPA-CF, PPS-CF |
| Unknown (never warns) | SILK, PAHT-CF, S-PAHT, S-Multi, ASA-GF, anything not listed |

`hasIncompatibleTemperatureFilament` warns when `hasLow && (hasHigh || hasMedium)`; High
with Medium alone passes. The dialog: "Printing high- and low-temperature materials
together may clog the nozzle or damage the printer. Continue printing?" The table has
inconsistencies (ABS-GF is High while ASA-GF is Unknown; PAHT-CF is Unknown), which is why
HelixScreen classifies from its filament database instead.

### Calibration records

`/usr/data/logs/calibrationRecords.log`, written by two functions that sit beside
`writeOffsetValueToFile` and `writeGrabValueToFile`: one writes `ST:` plus `T1:`..`T4:`
(per-tool nozzle offsets, by the neighboring name), the other `G-T1:`..`G-T4:` (tool
pickup positions). A "Calibration Data Record" screen reads it back with an Export button.
The field semantics come from function names, not decoded arithmetic.

### Klipper changes in 2.0.0

These reach Z-Mod users, because Z-Mod runs the stock klippy.

| File | Change |
|------|--------|
| toolhead.py | Short-move limits apply only when command density is high: a sliding window of the last 100 move times scales the limit between 80 and 200 moves/s (`dynamic_short_move_*` options). Static defaults loosened. |
| virtual_sdcard.py | Comment runs consumed in one batch; F-only `G0/G1` lines deferred to the next real move; no fixed 1 ms sleep per 5 ms batch; `EXCLUDE_OBJECT_END`/`START` wrapped around each tool change. |
| gcode_arcs.py, gcode_move.py | A Z-hop arc's capped feedrate no longer becomes the modal feedrate. |
| heaters.py | Setting `chamber_heater` above 25 °C runs `SET_IDLE_TIMEOUT TIMEOUT=3000`; reaching target runs `TIMEOUT=600`. Any user idle timeout is overwritten. |
| fan.py | `M106` forces `DC24V_CTL` on. |
| idle_timeout.py | Idle also turns off `chamber_heat_fan` and `chamber_loop_fan`. |
| e_stop.py | The probe-triggered-before-move error is a plain string instead of FlashForge's JSON error. |

The changelog's "extended print time" fix is most likely the toolhead and
`virtual_sdcard` throughput work.

### Automatic pressure advance

The stock UI runs the sweep, not Klipper. Recovered from `firmwareExe`
(`CommMgr::paTestMgr`) by the Klipper4FlashForge project
(github.com/Klipper4FlashForge/firmware, their note 51, "pa-calibration-recovered"),
and confirmed against the strings in our C5's 2.0.0 binary on 2026-11-09.

- **Klipper side**: `[pa_adjust]` (`printer.base.cfg`) registers `PA_ACTION ACTION=<a> PC=<pc>`
  and `PA_GET`, both forwarded to `mcu eboard`. `ACTION=11` arms the eBoard's capture of a
  16-bit sample stream on its USART3; any other value stops it and the eBoard scores the
  trace. `PA_GET` replies `Result is value=<n>`, and only 9 passes. Neither command has
  `desc=`, so neither is in `/printer/gcode/help`, and `pa_adjust` has no status, so it is not
  in `objects/list`: the config section is the only evidence it exists.
- **The latch**: `SET_PIN PIN=enable_pin_tmc_x VALUE=1.00` (and `_y`) **disables** the X/Y
  drivers through a second latch on PH2/PH3 that Klipper knows nothing about. The sweep's
  `G1` moves are stepped against dead drivers, so the head stays parked over the purge chute
  (X275 Y254, per-tool offset dx {0, -2, -2, -2}, dy {0, 0, 1.5, -8}) and about 1 g of
  filament drops into it. Both pins read 0 at rest.
- **One line per candidate**, in the order 0.0100 0.0200 0.0150 0.0350 0.0250 0.0300 0.0400,
  Y = 50 + 5·i: `PA_ACTION ACTION=11 PC=666`, `SET_PRESSURE_ADVANCE ADVANCE=<k>`,
  `G1 X40 Y<y> F30000`, six segments alternating `F1080` / `F10980` at E 0.0568 mm per mm,
  `PA_ACTION ACTION=0 PC=666` with **no M400 before it**, then `M400`, `PA_GET`.
- **Selection**: at most 5 sweeps of 7 lines; a sweep's winner is its smallest passing K; the
  result is the mean of the first 3 winners. Fewer than 3 is a failure.
- **Restore** (`resetTmcPa`): both pins to 0, then `SET_KINEMATIC_POSITION` to the park.
- **Apply**: per tool for one print with the stock `SET_PA_ADVANCE T0=.. ENABLE=1`
  (in FlashForge's virtual_sdcard module), cleared at print end.

HelixScreen runs this sweep itself on stock and Z-Mod firmware; see
[`FLASHFORGE_CREATOR5_PRO_SUPPORT.md`](../printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md#automatic-pressure-advance).

## Stock UI features (1.9.9 and 2.0.0) against HelixScreen

From the English string table in `firmwareExe`, checked against HelixScreen and Z-Mod's
Creator 5 macros (`ghzserg/z_c5pro`, `translate/en/c5pro_generic.cfg`).

| Stock feature | Printer-side hook | HelixScreen |
|---------------|-------------------|-------------|
| Extruder offset calibration | Z-Mod `CALIBRATE_EXTRUDERS` | Supported: Tool Offsets sends it and Z-Mod's own prompt runs it |
| VFA vertical stripe compensation | Z-Mod `CALIBRATE_VFA T=<n>`, or stock `[stepper_resonance_tester]` | Supported: Vertical Artifact Compensation (Advanced > Calibration) |
| Chamber air circulation | Z-Mod `AIR_CIRCULATION_INTERNAL` / `EXTERNAL` / `STOP` | Supported: Chamber air row in the fan overlay |
| Door open warning, "Door Switch" toggle | `gcode_button topDoor`, `frontDoor` | Supported: Settings > Sensors > Inputs; warns for warm-enclosure materials only |
| Device status page (dock, pickup, door, limits, eddy) | `gcode_button extruder_pos1-4`, `extruder_grab1-4`, endstops | Inputs shown live in Settings > Sensors; endstops not yet |
| Mixed-temperature warning | none (UI logic) | Supported, classified from the filament database |
| Multi-plate 3MF | none (file format) | Supported, with slicer plate names from `model_settings.config` |
| Static IP | OS network config | prestonbrown/helixscreen#1900 |
| Seven maintenance reminders | none | prestonbrown/helixscreen#1901 (HelixScreen tracks three) |
| Speed modes (50 / 100 / 140 %), mute mode | `M220`, `MUTE_MODE_ENABLE` | Speed slider only |
| Extruder position (dock) calibration | stock app drives `MOTOR_GRAB*` directly | No Z-Mod macro exists |
| VDS dryer box | stock app over its own link | Not reachable from Klipper |
| Cloud binding, cloud slicing queue, LAN-only mode, AI detection | FlashForge cloud | Out of scope |

Already covered by HelixScreen: exclude objects, reprint, timelapse, PID, bed mesh, input
shaper, per-tool load/unload and runout through Z-Mod, flow calibration as a pre-print
option, nozzle-size and material mismatch checks, hidden SSID, factory reset, log export.

Tool changes themselves are not in Klipper on stock firmware: `virtual_sdcard` sets a flag
on a `T<n>` line and spins until `firmwareExe` finishes the pickup. Under Z-Mod the
changer macros replace that loop.
