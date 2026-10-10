# Snapmaker U1 Research

Background on the U1's hardware, firmware sources and on-device layout. How HelixScreen builds
for, deploys to and drives the U1 is in
[`../printers/SNAPMAKER_U1_SUPPORT.md`](../printers/SNAPMAKER_U1_SUPPORT.md); the filament
backend and the `print_task_config` command reference are in
[`../FILAMENT_BACKEND_SNAPMAKER_U1.md`](../FILAMENT_BACKEND_SNAPMAKER_U1.md).

## Hardware

The SoC, display, touch controller, storage and recovery are tabulated in the support doc. What
this page adds is the toolhead layout from the community config
([JNP-1/Snapmaker-U1-Config](https://github.com/JNP-1/Snapmaker-U1-Config)):

- Four independent heads ("SnapSwap"), each with its own stepper, heater and thermistor, nozzle
  fan (`[heater_fan eX_nozzle_fan]`), filament motion sensor (`[filament_motion_sensor eX_filament]`),
  analog park detector and inductance coil for nozzle-height calibration.
- Park positions along Y=332.2 at X = 35.0, 102.7, 170.2, 237.7 (about 67.5 mm apart).
- `switch_accel: 25000` for the tool-change moves. Swaps take about 5 s with no purge.
- Rated 1,000,000 swaps for the system and 250,000 per head (pogo pins).

## Firmware sources

Snapmaker published its GPL code on 2026-03-30:

| Repository | Changes from upstream |
|------------|-----------------------|
| [Snapmaker/u1-klipper](https://github.com/Snapmaker/u1-klipper) | Multi-toolhead, eddy-current probing, RFID, power-loss recovery. U1-specific config and macros live under `lava/` |
| [Snapmaker/u1-moonraker](https://github.com/Snapmaker/u1-moonraker) | Snapmaker Cloud integration, 3MF support |
| [Snapmaker/u1-fluidd](https://github.com/Snapmaker/u1-fluidd) | Essentially Fluidd v1.36.2 |

The touchscreen UI (`/usr/bin/gui`) and the camera/MQTT daemon (`/usr/bin/unisrv`) are not
published.

### Toolchanging without klipper-toolchanger

The U1 does not use [viesturz/klipper-toolchanger](https://github.com/viesturz/klipper-toolchanger):
there is no `[toolchanger]` object and no `[tool T*]` sections. It is native multi-extruder
(`extruder` through `extruder3`) with custom state fields (`park_pin`, `active_pin`,
`activating_move`, `state`), and `T0`-`T3` macros that park and fetch heads and call
`ACTIVATE_EXTRUDER`. Generic toolchanger detection therefore never fires on a U1, which is why
it has a dedicated backend (`AmsBackendSnapmaker`) detected from the U1's own objects.

## On-device layout

| Path | Contents |
|------|----------|
| `/` | Read-only SquashFS root, with an OverlayFS upper on `/oem` (kept across boots only while `/oem/.debug` exists) |
| `/home/lava/printer_data/config/` | Klipper configuration |
| `/userdata/` | Persistent ext4 data partition |

Init scripts of interest: `S50dropbear` (SSH, gated on debug mode on stock), `S60klipper`,
`S61moonraker`, `S90lmd` (camera supervisor `/usr/bin/lmd`). Which `S99*` launcher starts the
stock UI depends on the firmware; the support doc's display-takeover section has the table.

The U1 uses Rockchip A/B slots: `updateEngine --misc=other --reboot` switches slot, MaskRom
recovers a bricked unit, and flashing stock firmware from USB reverts every modification.

## Extended firmware (PAXX)

[paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware)
is a Docker-built overlay over the stock SquashFS image. It enables SSH (`root`/`lava`, password
`snapmaker`), v4l2-mpp camera support, the `fb-http` remote screen, Fluidd/Mainsail, AFC-Lite
(a status-only shim, not a real AFC), OpenRFID and Tailscale. It installs and uninstalls through
the touchscreen's own firmware-update menu. Unpack/repack tooling:
[paxx12/u1-firmware-tools](https://github.com/paxx12/u1-firmware-tools).

## Community

- Forum: [Snapmaker U1 category](https://forum.snapmaker.com/c/snapmaker-products/87)
- Discord: Snapmaker server, `#u1-printer`
