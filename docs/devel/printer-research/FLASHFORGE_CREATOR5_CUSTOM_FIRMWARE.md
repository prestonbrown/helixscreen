# FlashForge Creator 5 / 5 Pro: Custom Firmware Landscape

Research record, 2026-11-05. Surveys the replacement and add-on firmwares for the Creator 5
line and how each fits HelixScreen as of `origin/main` c12f253eab. Stock firmware itself is
covered in [`FLASHFORGE_CREATOR5_FIRMWARE.md`](FLASHFORGE_CREATOR5_FIRMWARE.md); what
HelixScreen supports today is in
[`../printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md`](../printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md).

Sources were read on 2026-11-05: project READMEs, docs trees and config files fetched from
GitHub, release lists, issue threads and the Z-Mod wiki. Nothing was run on a printer. The
printer this is for (a new Creator 5 on stock firmware) was not touched, so
its firmware version is unknown here.

## Summary

| | Stock FlashForge | Z-Mod | Reforge | OpenCreator CFW |
|---|---|---|---|---|
| Project | FlashForge | [ghzserg/zmod](https://github.com/ghzserg/zmod), C5 configs in [ghzserg/z_c5pro](https://github.com/ghzserg/z_c5pro) | [Klipper4FlashForge/firmware](https://github.com/Klipper4FlashForge/firmware) | [FlashForge-C5-Modding-Group/OpenCreator](https://github.com/FlashForge-C5-Modding-Group/OpenCreator) |
| Kind | Vendor | Add-on over stock software | Replaces the application layer, keeps stock MCU firmware | Full replacement, including MCU firmware |
| Host Klipper | FlashForge 0.12-era fork | Stock FlashForge klippy (no Klipper 13 on C5) | Klipper fork `creator5` branch at `b722a4c8` | Kalico fork |
| Moonraker / web | Stock app API on :8898 | Z-Mod Moonraker, Fluidd + Mainsail | Moonraker v0.11.0, Mainsail :80, Fluidd :81 | Latest Moonraker, Fluidd or Mainsail |
| Screen | `firmwareExe` (LVGL 8) | Stock UI by default; HelixScreen or GuppyScreen with `DISPLAY_OFF` | HelixScreen fork (`v1.2.0-beta.1-creator5`) | GrumpyScreen; HelixScreen start script added 2026-11-04 |
| Changer model | `firmwareExe` does pickups | `zmod_color` + `_T_IN`/`_T_OUT`, afclite AFC shim (1.7.4+) | `ff_toolchange` publishing klipper-toolchanger-shaped `toolchanger` + `tool T0..T3`, AFC standalone on top (master, 2026-11-04) | `creator5_toolchanger` + AFC standalone (`AFC_Toolchanger Tools`, `AFC_extruder extruder0..3`) |
| Per-head material/colour | Stock UI | `zmod_color.slots`, written with `CHANGE_ZCOLOR` | AFC lanes (`e0..e3`), Spoolman | AFC lanes, GrumpyScreen spool page |
| Stock FW accepted | n/a | 1.9.9, 2.0.0 | 1.9.6 and newer (floor only); 2.0.0 unconfirmed | 1.7.8 through 2.0.0 |
| Undo | n/a | `REMOVE_ZMOD` macro, or stock flash | Flash the stock package | Flash the stock package (restores stock MCU firmware) |
| Maturity | Shipping | C5 since 1.7.3 (2026-09-09, alpha); 1.7.5 on 2026-10-28; 476 stars, 625 commits by the lead | Last tagged release 2026-09-24; master far ahead; one main author (373 of 380 commits) | Public alpha, Alpha 6 on 2026-11-02; one main author |
| HelixScreen fit today | Cannot run on the panel | Best: implemented and field-verified | Likely works through the AFC backend; unverified on C5 | Likely works through the AFC backend; unverified on C5 |

**Recommendation: Z-Mod.** Reasons and risks are under "Recommendation" at the end.

## Stock FlashForge (baseline)

- Current release is 2.0.0 (control 1.3.1, 2026-10-14). 1.9.9 cannot be rolled back to
  1.9.8 or earlier; 2.0.0 is a full image whose changelog claims rollback support
  ([`FLASHFORGE_CREATOR5_FIRMWARE.md`](FLASHFORGE_CREATOR5_FIRMWARE.md), Klipper4FlashForge
  issue [#28](https://github.com/Klipper4FlashForge/firmware/issues/28)).
- Tool changes are not in Klipper: `virtual_sdcard` flags a `T<n>` and waits for
  `firmwareExe` to do the pickup (`FLASHFORGE_CREATOR5_FIRMWARE.md`, last paragraph). The
  stock UI also hosts the :8898 API and the cloud link, so it cannot be stopped without
  losing print orchestration (`FLASHFORGE_CREATOR5_PRO_SUPPORT.md`, "On-device bring-up"
  item 2).
- ssh: dropbear is already enabled on port 22 on a stock rootfs; there is just no published
  root password ([Reforge how-it-works](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/how-it-works.md)).
- Moonraker on stock: HelixScreen's support doc says it is present on 7125; Reforge's README
  says Moonraker is not in the stock package. Unverified which holds on a factory-fresh unit.
- HelixScreen fit: none on the panel (two processes would fight over `/dev/fb0`, and the UI
  owns tool pickup). A remote HelixScreen against stock Moonraker, if present, could watch
  but not drive tool changes.

## Z-Mod (ghzserg)

**Install.** Copy `Creator5-zmod-<ver>.tgz` (or `Creator5Pro-...`) to the root of a USB
stick unpacked; install can take up to 40 minutes. The Setup page tells you to restore
stock 1.9.9 first ([wiki Setup](https://wiki.zmod.link/Setup/)), while the README lists
Creator 5 compatibility as "only (1.9.9, 2.0.0)"
([zmod README](https://github.com/ghzserg/zmod#compatibility)). Which applies to a 2.0.0
printer is an open question. Releases come as standard (plugins downloaded) and full
(plugins bundled) packages ([1.7.5 release](https://github.com/ghzserg/zmod/releases/tag/1.7.5)).
A stock OTA can disable the mod; an `enable-zmod` package re-enables it
([wiki Native_FW](https://wiki.zmod.link/Native_FW/)).

**Undo and brick risk.** `REMOVE_ZMOD` removes it, `REMOVE_ZMOD FULL=1` fully uninstalls; a
USB `flashforge_init.sh` removal also exists ([wiki Setup](https://wiki.zmod.link/Setup/)).
Brick recovery is UART at 3.3 V; the wiki's full-firmware recovery images cover FF5M and
AD5X, not the Creator 5 (same page). An unbricking package exists for the C5 line
([wiki Native_FW](https://wiki.zmod.link/Native_FW/)). Z-Mod leaves MCU firmware and the
stock klippy in place (it runs "the native/original Klipper"; Klipper 13 is not supported on
C5: [wiki C5PRO](https://wiki.zmod.link/C5PRO/), [changelog 1.7.4](https://wiki.zmod.link/Changelog/)),
which is why its brick surface is the smallest of the three.

**Stack.** Stock FlashForge klippy, Z-Mod's Moonraker, Fluidd and Mainsail, root access
([zmod README](https://github.com/ghzserg/zmod)). Klipper 13 shapers are optional via
`KLIPPER_C5=1` ([wiki C5PRO](https://wiki.zmod.link/C5PRO/)).

**Screen.** Stock UI by default. `DISPLAY_OFF HELIX=1` runs HelixScreen instead
([changelog 1.7.0](https://wiki.zmod.link/Changelog/)); GuppyScreen is the other option
([wiki C5PRO](https://wiki.zmod.link/C5PRO/)). Custom colours/types and Orca filament sync
need the native screen off (same page). Z-Mod installs and updates HelixScreen itself
([HelixScreen INSTALL.md](../../user/INSTALL.md), Creator 5 row).

**Changer model.** `zmod_color` extra with `_T_IN T=<n>` / `_T_OUT`, `active_tool_id`,
`slots` and `palette` in its status (zmod 1.7.4+); 1.7.4 also publishes an AFC-shaped shim
(`zmod_afclite`, `AFC`, `AFC_lane E0..E3`, `AFC_unit C5`) for Fluidd/Mainsail
([`FLASHFORGE_CREATOR5_PRO_SUPPORT.md`](../printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md),
"Z-Mod tool changer support", field bundle `5WSPTZBL`). Per-head colour and type are set
with `COLOR` and stored in `mod_data/filament.json`
([wiki C5PRO](https://wiki.zmod.link/C5PRO/)). The `zmod_color` plugin source for C5 is not
published (support doc, "Load feeds through the afclite shim's `CHANGE_TOOL`").

**Maturity.** C5 alpha in 1.7.3 (2026-09-09), "everything that works on AD5X works here
too, except Klipper 13" in 1.7.4 (2026-09-24), 1.7.5 on 2026-10-28 with 3MF and up to 100
colours mapped to 4 heads ([changelog](https://wiki.zmod.link/Changelog/)). Commits land
daily on `z_c5pro` branch `1.7` (last 2026-11-05). The project is two years old across the
FF5M/AD5X/C5 line, 476 stars, ghzserg 625 commits, next contributor 81
(`gh api repos/ghzserg/zmod/contributors`). Issues are closed fast; C5 reports include a
c_helper mismatch on stock 1.9.7 ([#730](https://github.com/ghzserg/zmod/issues/730)) and
module-update breakage on AD5X ([#753](https://github.com/ghzserg/zmod/issues/753)). The
community is Russian-first with a Telegram bot ([wiki](https://wiki.zmod.link/C5PRO/)).

## Reforge (Klipper4FlashForge)

**Install.** One `Creator5-anvil-<date>.tgz` / `Creator5Pro-anvil-<date>.tgz` on a FAT32
stick, through the stock updater; the package refuses the other model and refuses stock
older than 1.9.6 ([installing.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/installing.md)).
First boot imports factory dock/nozzle calibration into Klipper and writes a random root
password to the stick (same page).

**Undo and brick risk.** Flashing the stock package is the uninstall; the mod ships no
FlashForge component and touches only `runFirmwareExe.sh` and the `firmwareExe` path, and
seeds its own config dir so stock `/usr/data/config` stays clean
([how-it-works.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/how-it-works.md),
[installing.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/installing.md)).
A printer that took an older release that replaced a start-up script needs a
[stock-recovery](https://github.com/Klipper4FlashForge/stock-recovery) package after the
stock flash ([support.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/support.md)).
MCU firmware is not reflashed. The project's own risk page shows a molten toolhead from a
failed grab ([is-this-for-you.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/is-this-for-you.md)).

**Stack.** Klipper fork `creator5` at `b722a4c8` with the `ff_*` extras, Moonraker v0.11.0,
Mainsail v2.18.2, Fluidd on :81, s6-rc supervision, root ssh
([versions.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/versions.md),
[features.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/features.md)).
Stock base pinned at 1.9.7 (versions.md). You lose FlashPrint/Orca FlashForge upload, the
app and cloud, power-loss recovery, timelapse, the VDS dryer, door pause
([is-this-for-you.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/is-this-for-you.md)).

**Screen.** Ships a HelixScreen fork, [Klipper4FlashForge/helixscreen](https://github.com/Klipper4FlashForge/helixscreen),
now `v1.2.0-beta.1-creator5` on master (versions.md, commit "Update HelixScreen to
v1.2.0-beta.1-creator5 (#44)", 2026-11-04). The last tagged release bundled 0.99.118 and
missed upstream C5 fixes ([#38](https://github.com/Klipper4FlashForge/firmware/issues/38)).
It drops a `printer_database.d/flashforge_creator5.json` override with `"ams_type": "afc"`
([file](https://github.com/Klipper4FlashForge/firmware/blob/master/pkgs/helixscreen/payload/helixscreen/config/printer_database.d/flashforge_creator5.json)).

**Changer model.** `ff_toolchange.py` (1694 lines, reverse-engineered from `firmwareExe`)
exposes `T0..T3`, `SELECT_TOOL`/`UNSELECT_TOOL`, `TOOLCHANGE_PARK`, and a `toolchanger`
object plus `tool T<n>` objects shaped like viesturz/klipper-toolchanger's
([ff_toolchange.py `get_status`](https://github.com/Klipper4FlashForge/firmware/blob/master/pkgs/klipper/payload/klipper/klippy/extras/ff_toolchange.py)).
Since the `afc-migration` merge on 2026-11-04, AFC sits on top: one `[AFC_extruder e<n>]`
standalone lane per head, owning `T0..T3`, `SET_MAP`, `SET_RUNOUT` and Spoolman
([toolchange.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/toolchange.md),
[filament-and-spools.md](https://github.com/Klipper4FlashForge/firmware/blob/master/docs/filament-and-spools.md)).
Mapping, infinite spool and Spoolman are marked "not yet on hardware" (features.md). Per-head
material: macros read a static `tool_material` table rather than HelixScreen's `lane_data`,
so PETG heads were loaded at PLA temperature ([#36](https://github.com/Klipper4FlashForge/firmware/issues/36));
the AFC migration may change this (unverified).

**Maturity.** Eight tagged releases 2026-08-25 to 2026-09-24, none since; master moved on
(`gh release list -R Klipper4FlashForge/firmware`). Monstrofil 373 of 380 commits. Seven
open issues, including a host `RESTART` hanging MCU bring-up
([#37](https://github.com/Klipper4FlashForge/firmware/issues/37)) and a "Timer too close"
shutdown during toolchange ([#34](https://github.com/Klipper4FlashForge/firmware/issues/34),
PR fix claimed). 1.9.9 support was fixed per the maintainer; a 2026-10-25 question about
2.0.0 is unanswered ([#28](https://github.com/Klipper4FlashForge/firmware/issues/28)). 2.0.0
compatibility is unverified.

## OpenCreator (FlashForge C5 Modding Group)

Two paths. **Legacy** keeps `firmwareExe` and adds root, a Loop Script and Mainsail
alongside it ([opencreator-legacy.md](https://github.com/FlashForge-C5-Modding-Group/OpenCreator/blob/main/docs/opencreator-legacy.md)).
**CFW** is the full replacement, below.

**Install.** Unzip the release to a FAT32 stick (4 GB minimum, 8 GB for the backup) and
reboot; a touchscreen installer
([touchscreen-installer](https://github.com/FlashForge-C5-Modding-Group/touchscreen-installer))
backs up, then installs Entware, LoopScript, Kalico, Moonraker, Fluidd/Mainsail,
GrumpyScreen and a root login, and flashes all four MCU boards; 25-30 minutes; needs a live
internet connection
([installation-and-safety.md](https://github.com/FlashForge-C5-Modding-Group/OpenCreator/blob/main/docs/installation-and-safety.md),
[installer.md](https://github.com/FlashForge-C5-Modding-Group/OpenCreator/blob/main/docs/installer.md)).
Entware's legacy-NaN support is switched on by writing one byte at a kernel-package-specific
address, known for kernels 2.0.1 to 2.0.8 (2.0.8 = FlashForge 2.0.0); any other kernel stops
the installer (installer.md, "Supported kernel packages"). Supports FlashForge 1.7.8 through
2.0.0 (installation-and-safety.md).

**Undo and brick risk.** Flash any stock package; it restores stock MCU firmware on every
board. Leftover dirs under `/usr/data`, the boot logo and an Nginx config remain
(installation-and-safety.md). Power loss during MCU flashing "can leave the printer needing
firmware recovery" (installer.md). Highest brick surface of the three: it is the only one
that replaces MCU firmware.

**Stack.** Kalico fork with C5 MCU ports for mainboard (GD32H737), eboard, heaterboard,
levelboard ([architecture.md](https://github.com/FlashForge-C5-Modding-Group/OpenCreator/blob/main/docs/architecture.md)).
Default ssh login `opencreator` / `FlashForge2026` (installer.md). Claims ACE Pro support,
power-loss recovery and 1.5-2x faster tool changes than stock 1.9.9
([status-and-roadmap.md](https://github.com/FlashForge-C5-Modding-Group/OpenCreator/blob/main/docs/status-and-roadmap.md),
[different.md](https://github.com/FlashForge-C5-Modding-Group/OpenCreator/blob/main/docs/different.md)).

**Screen.** GrumpyScreen (SimpleAF fork), which "can't extrude yet"
([is-cfw-for-you.md](https://github.com/FlashForge-C5-Modding-Group/OpenCreator/blob/main/docs/is-cfw-for-you.md)).
HelixScreen is not offered by the installer, but `opencreator-fs` gained
`startup/scripts/41-helixscreen.sh` on 2026-11-04, which starts
`/usr/data/helixscreen/bin/helix-launcher.sh` if present, rotation 90, Goodix touch
auto-pick ([file](https://github.com/FlashForge-C5-Modding-Group/opencreator-fs/blob/main/fs/usr/data/opencreator/startup/scripts/41-helixscreen.sh)).

**Changer model.** `creator5_toolchanger` extra plus AFC standalone:
`[AFC_Toolchanger Tools]`, `[AFC_extruder extruder0..3]` with `toolchanger_unit: Tools`,
`map: T<n>`, `custom_tool_swap: C5_TOOL_SELECT T=<n>`, `custom_unselect: C5_TOOL_DOCK`.
Carriage buttons are `gcode_button extruder_grab1..4` / `extruder_pos1..4`, chamber heater
in `printer.pro.cfg`, and `virtual_sdcard` sets `auto_creator5_start: True`
([printer.afc-standalone.cfg](https://github.com/FlashForge-C5-Modding-Group/opencreator-fs/blob/main/fs/usr/data/config/printer.afc-standalone.cfg),
[printer.base.cfg](https://github.com/FlashForge-C5-Modding-Group/opencreator-fs/blob/main/fs/usr/data/config/printer.base.cfg)).
Per-head material/colour lives in AFC lane state.

**Maturity.** Public alpha: ten releases from 2026-10-16 to Alpha 6 on 2026-11-02
(`gh release list -R FlashForge-C5-Modding-Group/OpenCreator`). "This is an alpha. Expect
bugs" (installation-and-safety.md). Cartrigger wrote all 113 docs commits and 176 of 190
`opencreator-fs` commits. GrumpyScreen macros and SBC mode not finished
(status-and-roadmap.md). Active Discord, link in the docs.

## Others

- **OpenCreator Legacy**: stock plus root and Mainsail (above). HelixScreen cannot own the
  panel here for the same reason as stock.
- [Monstrofil/creator5-toolchange](https://github.com/Monstrofil/creator5-toolchange): the
  toolchanger extras that became Reforge; last push 2026-08-22.
- [TigerTag FlashForge firmware](https://github.com/TigerTag-Project/FlashForge-TigerTag-Creator5-Firmware-Lan-and-Cloud):
  described as official FlashForge firmware with LAN and cloud at once and TigerTag RFID. A
  vendor variant, not a Klipper replacement. Not examined further.
- [d3dorsett/CleanFlashforgeCreator5Pro](https://github.com/d3dorsett/CleanFlashforgeCreator5Pro):
  a cleanup script, used by OpenCreator's installer.

No other replacement firmware for the C5 turned up (GitHub repo search "creator 5
flashforge", 2026-11-05).

## HelixScreen fit

What `origin/main` does on each, from the database entries
(`assets/config/printer_database.json`, `flashforge_creator_5` / `_pro`), discovery and the
backends.

### Z-Mod

- **Detection:** `gcode_button extruder_grab1` (95), four heads, `zmod_afclite`
  corroborator, chamber heater separating the Pro. Works, field-confirmed (`5WSPTZBL`).
- **Mount/park:** tool changer backend, Z-Mod row: `_T_IN T=<n>`, `_T_OUT`; Load is
  `CHANGE_TOOL LANE=E<n>` through the shim (contributor test,
  [#1779](https://github.com/prestonbrown/helixscreen/issues/1779)). Unload is manual by
  design. Works.
- **Per-head material/colour:** read from `zmod_color.slots`, written with
  `CHANGE_ZCOLOR`; field-confirmed on zmod 1.7.4.
- **Print start, calibration, chamber air:** `CALIBRATE_EXTRUDERS`, `CALIBRATE_VFA`,
  `AIR_CIRCULATION_*` wired.
- **Open:** swap step-bar markers on a live swap, stood-down runout sensors
  ([#1714](https://github.com/prestonbrown/helixscreen/issues/1714)). 1.7.5's 100-colour
  mapping and 3MF intake are new and untested against HelixScreen. Also stale:
  `docs/user/FAQ.md` still says per-head material waits on "Z-Mod's pending status update",
  which shipped in 1.7.4.
- **Gap size:** small. Hardware verification only.

### Reforge

- **Detection:** `ff_toolchange` (99) matches both entries; chamber heater separates
  them. Reforge also drops its own `printer_database.d` override under its own ids
  (`flashforge_creator5_pro`, `flashforge_creator5`). Our two entries list those ids in
  `id_aliases`, so the extension merge (`src/printer/printer_detector.cpp#merge_extension_file`)
  skips Reforge's copies and ours, with the preset and real image, is the only Creator 5
  entry of each name. Unverified on a real printer.
- **Mount/park and colour:** the AFC objects (`AFC_Toolchanger`, `AFC_extruder e0..e3`)
  make discovery pick the AFC backend, since an MMU wins over the tool changer
  (`FILAMENT_BACKEND_TOOLCHANGER.md`, Detection). The AFC backend already handles a
  `toolchanger_unit` head-per-lane setup: mount by `AFC_SELECT_TOOL`, Load by
  `CHANGE_TOOL LANE=`, exercised on the Snapmaker U1 PAXX AFC build
  ([#1824](https://github.com/prestonbrown/helixscreen/issues/1824),
  [#1850](https://github.com/prestonbrown/helixscreen/issues/1850)). Per-head material
  and colour come from AFC lanes. The `creator5_afc` mock persona publishes Reforge's
  object set and drives the production AFC backend; `creator5` keeps modelling the tagged
  releases without AFC. Untested on a C5.
- **Print start:** `[ff_print]` runs `FF_BEFORE_PRINT_START` before the first file line,
  the same shape as OpenCreator's `auto_creator5_start`, and `src/print/start_macro_injection.cpp`
  has a row for it: the `ff_print` section, unless the before-macro's `variable_prepare` is 0
  (the slicer profile then calls START_PRINT from the file).
- **AFC settings:** HelixScreen's AFC settings editor reads `AFC/AFC.cfg` and
  `AFC/AFC_Macro_Vars.cfg`; Reforge keeps its AFC config in `ff-afc.cfg` (overwritten by
  every update) plus `AFC/*.cfg` for the user's own. What the editor shows there is
  unverified.
- **Other:** Moonraker refuses `ff-*.cfg` reads ("reserved path"), so config-include
  parsing fails ([#38](https://github.com/Klipper4FlashForge/firmware/issues/38)). Reforge
  ships its own HelixScreen fork; running upstream instead is unverified.
- **Gap size:** small. Hardware time.

### OpenCreator CFW

- **Detection:** `gcode_button extruder_grab1` (95) plus four heads plus the chamber heater
  should resolve the right entry; no `ff_toolchange` or Z-Mod corroborator. Probably works;
  unverified.
- **Mount/park and colour:** AFC backend over `AFC_Toolchanger Tools` with lanes
  `extruder0..3`; tool swap through AFC to `C5_TOOL_SELECT` / `C5_TOOL_DOCK`. Same AFC path
  as Reforge. Likely works; untested.
- **Print start:** `auto_creator5_start` is already recognised
  (`src/print/start_macro_injection.cpp`).
- **Install:** HelixScreen goes in by hand to `/usr/data/helixscreen`; the 41-helixscreen.sh
  hook then starts it. GrumpyScreen would need disabling (how is unverified).
- **Gap size:** medium. No mock persona, no field bundle, an alpha whose config is moving
  daily.

## Recommendation

**Flash Z-Mod (1.7.5, standard package) and run HelixScreen with `DISPLAY_OFF HELIX=1`.**

1. It is the only firmware where HelixScreen's Creator 5 support is complete and
   field-verified (bundle `5WSPTZBL`, #1779), and the one most existing HelixScreen C5
   users run, so this printer becomes a test bed for the support that matters. The open
   #1714 items (live step bar, stood-down runout) can be closed on it.
2. Lowest brick surface: stock klippy and stock MCU firmware stay, the undo is a macro or a
   stock flash, and Z-Mod lists 2.0.0 as supported.
3. Most mature project: two years and three printer families, three C5 releases, daily
   commits, fast issue turnaround. Reforge has had no tagged release in six weeks and
   OpenCreator is a two-week-old alpha.
4. It keeps the stock UI one macro away, which helps when comparing behaviour to stock.

**Risks.** Z-Mod runs FlashForge's 0.12-era klippy with no Klipper 13 on C5, so it will
never be the "modern Klipper" option. Part of it is closed (`zmod_color`). The C5 install
docs conflict on whether a 2.0.0 printer must go to 1.9.9 first. The community is
Russian-first.

**Next.** Once the AFC migration is in a tagged Reforge release, Reforge is worth a second
pass on this printer: stock flash, then Reforge, then a debug bundle. It exercises the AFC
toolchanger path, the one Reforge and OpenCreator now share. OpenCreator is worth watching
but not flashing: it reflashes all four MCUs and its own docs call it alpha.

## Open questions

- Which stock version is the printer on (Settings > About)? It came out of the box, so 1.9.9
  or 2.0.0; FlashForge will offer the 2.0.0 OTA. Unverified.
- Does Z-Mod install directly on 2.0.0, or does the Setup page's "restore stock 1.9.9 first"
  still hold? Ask ghzserg or try the install on 2.0.0 and read the USB log.
- Does Reforge install and run on 2.0.0 (board firmware 1.3.1)? Open on
  [#28](https://github.com/Klipper4FlashForge/firmware/issues/28).
- Does upstream HelixScreen run on OpenCreator with GrumpyScreen off, and does the AFC
  backend handle `extruder0..3` lane names with `custom_tool_swap`?
- Is Moonraker present on a factory-fresh stock unit (our support doc and Reforge's README
  disagree)?
