# Printer Research

Reverse-engineering notes, hardware recon and firmware internals for specific printers and
devices. These are research records: each is dated and may describe a printer as it looked
before HelixScreen supported it. How HelixScreen supports a printer today (build target,
deploy, quirks, status) lives in [`../printers/`](../printers/README.md), and wins wherever the
two disagree.

## Where each printer stands

Status uses the user docs' labels ([`../../user/guide/supported-printers.md`](../../user/guide/supported-printers.md)):
**Tested** = verified by the team on real hardware, **Supported** = works with the noted
firmware, **Community** = confirmed by a user only, **Preliminary** = built from published
config, not hardware-verified. Build targets are `PLATFORM_TARGET` values in `mk/cross.mk`.

| Printer | SoC / arch | Panel | Firmware needed | Multi-material | Target | Status |
|---------|-----------|-------|-----------------|----------------|--------|--------|
| FlashForge Adventurer 5M / Pro | Allwinner, Cortex-A7 armv7 | 800x480 fbdev | Forge-X, Klipper Mod or ZMOD | none | `ad5m`, `ad5m-br` | Tested |
| FlashForge Adventurer 5X | Ingenic X2600, MIPS32 (nan2008) | 800x480 fbdev | ZMOD 1.7.0+ | IFS (4) | `mips` (alias `ad5x`) | Tested |
| FlashForge Creator 5 / 5 Pro | Ingenic X2000, MIPS32 | 800x480 (portrait fb, rotated) | Z-Mod or Reforge | 4-head toolchanger | `mips` (`make creator5`) | UI runs on device; toolchanger verified in mock only |
| Creality K1 / K1C / K1 Max / K1 SE | Ingenic X2000E, MIPS32r2 | 480x800 portrait fbdev | Guilouz, Simple AF or Guppy Mod (Moonraker) | optional CFS upgrade | `mips` (alias `k1`), `k1-dynamic` | Supported (K1C, K1 Max tested) |
| Creality K2 / K2 Plus / K2 Pro | Allwinner T113, Cortex-A7 armv7 | 480x800 portrait, rotated | stock | CFS (up to 16) | `k2` | Tested |
| Snapmaker U1 | Rockchip RK3562, aarch64 | 480x320 DRM | stock 1.2+ with Root access, or PAXX | SnapSwap 4-head | `snapmaker-u1` | Tested on PAXX; stock unverified |
| Elegoo Centauri Carbon 1 | Allwinner R528, Cortex-A7 armv7 | 480x272 fbdev | OpenCentauri COSMOS 26.07.0+ | none | `cc1`, `yocto` | Tested |
| QIDI Q2 / Max 4 | Cortex-A35, aarch64 | 480x272 / 800x480 fbdev | stock, FreeDi, FreeQIDI | QIDI Box | `pi` | Supported (Q2 community-validated) |
| QIDI Plus 4 and 3-series | RK3328, aarch64 | TJC serial HMI | stock | QIDI Box (Plus 4) | remote only | Supported as a remote target |
| Creality Sonic Pad | Allwinner R818, armv7 userspace | 1024x600 | SonicPad-Debian | n/a | `pi32` | Supported |
| Anycubic Kobra 2 Pro / 3 / S1, Sovol SV06 ACE | various | n/a | Rinkhals or community Klipper | ACE Pro | remote | Community |
| BigTreeTech K-Touch | ESP32-S3 | standalone panel | none (talks to any Moonraker) | n/a | `firmware/helixscreen-esp32` | Alpha, 1.1 betas |

The Kobra S1, Plus 4 and K2 rows rest on community captures as much as on our own hardware; the
documents below record which.

## Documents

| Document | Covers |
|----------|--------|
| [FLASHFORGE_AD5M_TOOLCHAIN_NOTES.md](FLASHFORGE_AD5M_TOOLCHAIN_NOTES.md) | Why `ad5m` links fully static: glibc 2.25 on the printer, the toolchains ruled out, link fixes |
| [FLASHFORGE_AD5X_RESEARCH.md](FLASHFORGE_AD5X_RESEARCH.md) | AD5X hardware, stock firmware, ZMOD, and the 2026-08 firmware-ecosystem survey |
| [FLASHFORGE_AD5X_PLATFORM_NOTES.md](FLASHFORGE_AD5X_PLATFORM_NOTES.md) | The runtime environment HelixScreen deploys into on a ZMOD AD5X: chroot layout, logs, ZMOD display lifecycle, rig observations |
| [FLASHFORGE_AD5X_IFS_ANALYSIS.md](FLASHFORGE_AD5X_IFS_ANALYSIS.md) | IFS serial protocol, Klipper objects, the ZMOD IFS Python module, bambufy vs lessWaste, unload semantics verified against ZMOD source |
| [CREALITY_K1_SERIES_RESEARCH.md](CREALITY_K1_SERIES_RESEARCH.md) | K1C / K1 Max hardware, stock firmware, rooting, community firmware |
| [CREALITY_CFS_K1_INTERNALS.md](CREALITY_CFS_K1_INTERNALS.md) | K1-family CFS box wrapper (`box_wrapper.cpython-38-mipsel`): `BOX_*` semantics, tn_data.json, deferred-failure and resume traps. Read before changing anything the CFS backend emits on K1 |
| [CREALITY_K2_PLUS_RESEARCH.md](CREALITY_K2_PLUS_RESEARCH.md) | K2 Plus hardware and Creality OS |
| [CREALITY_CFS_K2_INTERNALS.md](CREALITY_CFS_K2_INTERNALS.md) | K2 CFS module (`box_wrapper.cpython-39.so`): box.cfg, `box` schema variants and encodings, commands, error codes, runout, the community Kalico port |
| [CREALITY_K1_VS_K2_COMMUNITY.md](CREALITY_K1_VS_K2_COMMUNITY.md) | K1 vs K2 aftermarket and community ecosystem |
| [CREALITY_NEBULA_PAD_RESEARCH.md](CREALITY_NEBULA_PAD_RESEARCH.md) | Nebula Pad add-on touchscreen |
| [SNAPMAKER_U1_RESEARCH.md](SNAPMAKER_U1_RESEARCH.md) | U1 toolhead layout, published firmware sources, on-device layout, PAXX extended firmware |
| [QIDI_PLUS_4_RESEARCH.md](QIDI_PLUS_4_RESEARCH.md) | A modded Plus 4 (Pi 4 + Kalico + Happy Hare + QIDI Box) from a user dump; TJC panel and `libColPic` encoder |
| [QIDI_BOX_HEATER.md](QIDI_BOX_HEATER.md) | QIDI Box PTC heater: Klipper objects, command surfaces, firmware variants, HelixScreen integration |
| [ANYCUBIC_KOBRA_COREXY_RESEARCH.md](ANYCUBIC_KOBRA_COREXY_RESEARCH.md) | Kobra S1 / 3 series, ACE Pro, Rinkhals, native `filament_hub` interface |
| [ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md](ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md) | ACE Pro on the mainline-Python Kobra S1 fork, from real logs; raw samples in [`kobra-s1-ace-1098/`](kobra-s1-ace-1098/README.md) |
| [BTT_K_TOUCH_HARDWARE.md](BTT_K_TOUCH_HARDWARE.md) | K-Touch (ESP32-S3) hardware recon and stock firmware layout |
| [KLIPPER_FORKS_AND_PORTS.md](KLIPPER_FORKS_AND_PORTS.md) | Vendor Klipper reimplementations, how vendors skip Moonraker, MQTT-vs-Moonraker transport notes |

## Add-on touchscreens

| Device | Display | CPU | OS |
|--------|---------|-----|-----|
| Creality Nebula Pad | 4.3" 480x272 resistive | Allwinner T113 | Creality OS (closed) |
| Creality Sonic Pad | 7" 1024x600 capacitive | Allwinner R818 | Klipper-based; HelixScreen runs on it under SonicPad-Debian |
| BTT Pad 7 | 7" 1024x600 capacitive | Allwinner H616 | Klipper + KlipperScreen |

## Community firmware projects

| Project | Printers | Notes |
|---------|----------|-------|
| [Forge-X](https://github.com/DrA1ex/ff5m) | AD5M / Pro | Moonraker, GuppyScreen; HelixScreen ready-made image at [prestonbrown/ff5m](https://github.com/prestonbrown/ff5m) |
| [ZMOD](https://github.com/ghzserg/zmod) | AD5M / Pro, AD5X | Moonraker; installs and updates HelixScreen on the AD5X |
| [Klipper Mod](https://github.com/xblax/flashforge_ad5m_klipper_mod) | AD5M / Pro | Buildroot; can package HelixScreen via `ad5m-br` |
| [Guilouz Helper Script](https://github.com/Guilouz/Creality-K1-and-K1-Max) | K1 series | Moonraker, Fluidd |
| [Guppy Mod](https://github.com/ballaswag/creality_k1_klipper_mod) | K1 series | Mainline Klipper |
| [OpenCentauri COSMOS](https://github.com/OpenCentauri/cosmos) | Centauri Carbon | Klipper + Moonraker, Yocto-based |
| [PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) | Snapmaker U1 | SSH, remote screen, extras |
| [Rinkhals](https://github.com/jbatonnet/Rinkhals) | Anycubic Kobra | Moonraker overlay |
| [ValgACE](https://github.com/agrloki/ValgACE) | any Klipper | ACE Pro driver |
