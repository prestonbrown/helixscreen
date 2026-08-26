# FlashForge Adventurer 5X (AD5X) Research

**Date**: 2026-02-02
**Updated**: 2026-08-24 (ecosystem verification + first-party rig; see final section)
**Status**: Comprehensive research complete

## Executive Summary

The FlashForge Adventurer 5X (AD5X) is a multi-color 3D printer using the **IFS (Intelligent Filament Switching)** system for up to 4-color printing. **Critical finding**: The AD5X uses a **MIPS architecture** (unlike the AD5M's ARM Cortex-A7), requiring a completely different toolchain. **Forge-X does NOT support AD5X** - only **ZMOD** provides custom firmware support.

---

## 1. Hardware Specifications

### Processor & Memory

| Component | AD5X | AD5M/AD5M Pro |
|-----------|------|---------------|
| **CPU** | MIPS-based (documentation unclear) | Allwinner T113 (ARM Cortex-A7) |
| **RAM** | ~128MB | 128MB (~110MB usable) |
| **Storage** | 8GB | 8GB |
| **C Library** | glibc 2.25 (likely) | glibc 2.25 |

**Important**: The AD5X's documentation says "dual-core Cortex-A53 MCU" but developers note it requires "rebuilding BuildRoot for MIPS" - suggesting MIPS application processor with ARM MCU for motion.

### Display

| Spec | Value |
|------|-------|
| **Size** | 4.3 inches |
| **Resolution** | 720x480 (resistive touchscreen) |
| **Interface** | USB to mainboard |
| **Framebuffer** | `/dev/fb0` |
| **Touch Input** | `/dev/input/event0` (evdev) |

### Printing Specifications

| Spec | Value |
|------|-------|
| **Build Volume** | 220 x 220 x 220 mm |
| **Max Speed** | 600 mm/s |
| **Max Acceleration** | 20,000 mm/s² |
| **Nozzle Temp** | Up to 300°C |
| **Bed Temp** | Up to 110°C |
| **Kinematics** | CoreXY |

---

## 2. Stock Firmware

- **Klipper firmware** out of the box (like AD5M)
- **Klipper Version**: v11 (outdated, known bugs including E0011/E0017)
- Proprietary touchscreen UI (Qt-based)
- Integrated cloud connectivity

**Source Code**: [github.com/FlashForge/AD5M_Series_Klipper](https://github.com/FlashForge/AD5M_Series_Klipper) (GPL-3.0)

---

## 3. Multi-Color System (IFS)

The AD5X uses **IFS (Intelligent Filament Switching)** - NOT a toolchanger or traditional MMU.

### How IFS Works
1. **4 spool holders** with spring-loaded tension
2. **Single hotend** with direct-drive extruder
3. **Automatic filament cutting** - old filament cut and retracted
4. **Filament switching** - new filament fed from one of 4 spools
5. **Purge system** - old material purged before continuing

### IFS Hardware

| Component | Description |
|-----------|-------------|
| **IFS Board** | FFP0202_IFS_Con_Board (STM32-based) |
| **Serial Port** | `/dev/ttyS4` @ 115200 baud |
| **Sensors** | 4 filament presence, motion sensor, cutter sensor |

### IFS Klipper Objects
```ini
filamentValue - on eboard:PA3 (filament presence)
cutValue - on eboard:PA2 (cutter detection)
```

### Key IFS Macros (ZMOD)
```
IFS_F10 - Insert filament
IFS_F11 - Remove filament
IFS_F13 - Check IFS state
IFS_F15 - Reset driver
PURGE_PRUTOK_IFS - Purge filament
SET_CURRENT_PRUTOK - Set active filament
```

### Purge/Waste
- Default purge: **540mm³** (~0.67g per color swap)
- "Poop-free" mode available with ZMOD's `nopoop` plugin

---

## 4. Moonraker Availability

### Stock Firmware
**No Moonraker** - uses FlashForge's proprietary system

### With ZMOD
| Service | Port |
|---------|------|
| Moonraker | 7125 |
| Fluidd/Mainsail | 80 |
| Camera | 8080 |

---

## 5. Custom Firmware Options

### ZMOD (Recommended for AD5X)
**Repository**: [github.com/ghzserg/zmod](https://github.com/ghzserg/zmod)

| Feature | Status |
|---------|--------|
| AD5X Support | **Yes** |
| Klipper Version | **12 by default** (2026-08 correction — 13 is opt-in via `UPDATE_MCU FORCE=13` and is the community-identified Timer-too-close trigger on AD5X; see final section) |
| Moonraker | Yes |
| Fluidd/Mainsail | Yes |
| GuppyScreen | Yes |
| Stock UI | Preserved (dual-boot) |
| SSH Access | `root:root` on port 22 |

**Supported AD5X Firmware** (2026-08): discrete versions 1.1.7-1.2.3, 3.0.3, 3.0.9,
3.1.0. New units ship ABOVE the ceiling (ours: 3.1.4) — the install path is a
factory restore DOWN to a listed image.

**Installation Time**: Up to 40 minutes for AD5X

### Forge-X / KlipperMod
**Status**: NOT SUPPORTED for AD5X

> "AD5X is not supported by Klipper Mod. Sorry, none of the developers has that printer."

**Reason**: AD5X requires "rebuilding BuildRoot for MIPS and changing the paths."

---

## 6. Klipper Configuration Structure

### Objects Exposed (with ZMOD)

**Sensors**:
- `tvocValue` - TVOC air quality
- `weightValue` - Spool weight in grams
- `filamentValue` - Filament presence
- `cutValue` - Cutter sensor

**AD5X-specific Macros**:
```
FAST_CLOSE_DIALOGS (use instead of CLOSE_DIALOGS)
CAMERA_ON VIDEO=video3/video0/video99
DISPLAY_OFF
IFS_* family
START_PRINT / END_PRINT
```

**Note**: `NEW_SAVE_CONFIG` does NOT work on AD5X.

### Configuration Files
- `/root/printer_data/config/printer.cfg`
- `mod_data/user.cfg` - ZMOD user config
- `mod_data/filament.cfg` - Filament change parameters

---

## 7. Display Interface

### Hardware
- 4.3" resistive touchscreen
- 720x480 resolution
- Framebuffer at `/dev/fb0`
- Touch at `/dev/input/event0`

### Software Stack

| Component | Description |
|-----------|-------------|
| Stock UI | Qt 4.8.2 direct to framebuffer |
| GuppyScreen | LVGL-based standalone |
| KlipperScreen | GTK3-based, requires X11 (heavy) |

### Screen Control
```bash
# Disable screen
echo 4 > /sys/class/graphics/fb0/blank

# Backlight (Allwinner DISP2 ioctls on /dev/disp)
# SET_BRIGHTNESS: 0x102
# GET_BRIGHTNESS: 0x103
```

---

## 8. Root Access

### SSH Access (with ZMOD)
- **Credentials**: `root:root`
- **Port**: 22

---

## 9. HelixScreen Compatibility Assessment

### Major Challenges

| Challenge | Severity | Notes |
|-----------|----------|-------|
| **Architecture** | CRITICAL | MIPS, not ARM - full toolchain rebuild |
| **Toolchain** | CRITICAL | Cannot reuse AD5M Docker environment |
| **No Forge-X** | HIGH | Our AD5M support relies on Forge-X |
| **ZMOD Integration** | MEDIUM | Need to integrate with ZMOD instead |
| **IFS System** | MEDIUM | Multi-color workflow needs UI |

### What Would Be Needed

1. **New Toolchain**
   - Rebuild Buildroot for MIPS architecture
   - New Docker build environment
   - Test static linking

2. **ZMOD Integration**
   - Replace Forge-X-specific paths
   - Adapt installer for ZMOD structure
   - Test with `DISPLAY_OFF` mode

3. **Display Compatibility**
   - Same resolution concept as AD5M (720x480)
   - Same framebuffer interface

4. **IFS Support**
   - UI for multi-color workflow
   - Current filament/color status
   - Filament change prompts

### Estimated Effort
**HIGH** - This is a new platform port, not a minor variant.

---

## 10. Comparison: AD5X vs AD5M Pro

| Feature | AD5X | AD5M Pro |
|---------|------|----------|
| **Multi-Color** | Yes (IFS, 4 colors) | No |
| **Architecture** | MIPS | ARM (Allwinner T113) |
| **Enclosure** | Open frame | Fully enclosed |
| **Max Nozzle Temp** | 300°C | 280°C |
| **Build Height** | 220mm | 250mm |
| **Chamber LEDs** | No | Yes |
| **Air Filtration** | No | Yes |
| **Forge-X Support** | No | Yes |
| **ZMOD Support** | Yes | Yes |

### Key Differences for HelixScreen
1. **No Forge-X** - current installer won't work
2. **Different architecture** - different binary builds
3. **IFS system** - new UI components needed
4. **No chamber LEDs** - removes that control

---

## 11. Community Resources

### GitHub

| Repository | Purpose |
|------------|---------|
| [ghzserg/zmod](https://github.com/ghzserg/zmod) | PRIMARY for AD5X |
| [DrA1ex/ff5m](https://github.com/DrA1ex/ff5m) | Forge-X (AD5M only) |
| [ballaswag/guppyscreen](https://github.com/ballaswag/guppyscreen) | GuppyScreen |

### Communities
- **Discord**: FlashForge → mods-and-projects → Forge-X
- **Telegram**: @FF_5M_5M_Pro (Russian)
- **Reddit**: r/FlashForge

### Documentation
- [ZMOD Wiki](https://github.com/ghzserg/zmod/wiki)
- [ZMOD AD5X Page](https://github.com/ghzserg/zmod/wiki/AD5X_en)

---

## Recommendations

### Short Term
1. ~~**Do NOT prioritize AD5X support**~~ *[superseded 2026-08: support shipped — MIPS
   build, ZMOD integration, and the IFS backend are all in tree; see
   `FILAMENT_BACKEND_AD5X_IFS.md`]*
2. **Monitor ZMOD development** - if they produce ARM binaries, reassess
3. ~~**Verify architecture claims**~~ *[resolved: Ingenic XBurst II MIPS32r2/r5,
   per the April live capture in `FLASHFORGE_AD5X_PLATFORM_NOTES.md` — which also
   corrects this doc's early hardware table: 800x480 display, 485 MB RAM]~

### Medium Term (if demand justifies)
1. ~~Acquire AD5X unit for development~~ *[done 2026-08-23 — first-party rig,
   commissioned; see PLATFORM_NOTES "Helix Rig Observations"]~
2. ~~Create MIPS Buildroot environment~~ *[shipped - `make ad5x-docker`]~
3. ~~Develop ZMOD integration layer~~ *[shipped]~
4. ~~Design IFS multi-color UI~~ *[shipped - `ams_backend_ad5x_ifs`]~

---

## Conclusion

The AD5X is essentially a **new platform** requiring significant effort to support - different architecture than AD5M, no Forge-X support, and a unique multi-color system (IFS). Wait for clearer architecture info or community MIPS tooling before investing in support.

---

## 2026-08-24 Update: firmware ecosystem verification + first-party rig

Verified by web/API research and clone inspection on 2026-08-23/24; first-party rig
commissioned 2026-08-23 (ZMOD 1.7.2, stock base 1.1.7, stock Klipper 12 MCU).

### Ecosystem (one lineage, two live branches)

| Mod | Repo | AD5X? | Status 2026-08 |
|-----|------|-------|----------------|
| klipper-mod (xblax) | `xblax/flashforge_ad5m_klipper_mod` | No (refused, #296) | **dormant** since 2025-09-16 |
| Z-Mod (ghzserg) | `ghzserg/zmod` + `ghzserg/zmod_klipper` | **Yes — the only one** | active, expanding (1.7.2 2026-08-06; Creator 5 family next) |
| Forge-X (DrA1ex) | `DrA1ex/ff5m` | No ("pretty unlikely ever") | active, fastest-growing, AD5M/Pro specialist |

The three are mutually incompatible at macro/binary level (ZMOD's own README).
For HelixScreen this means: **AD5X support is structurally a ZMOD partnership** —
bus factor 1 (solo maintainer, donations), but currently shipping monthly with
AD5X assets out-downloading AD5M ~2:1, and carrying explicit HelixScreen
integration (`DISPLAY_OFF HELIX=1`, ZMOD 1.7.0+; the ZMOD FAQ names
GuppyScreen/HelixScreen as the alternative-screen mode).

### FlashForge factory trajectory

- Changelog transparency collapsed: the official update log froze at 2.7.2
  (Jul 2024), the wiki in mid-2025; 2026 releases (AD5M 5.x, AD5X 3.x — latest
  3.1.5-1.1.1-3.0.7, 2026-06-26) ship with **no public changelog**. A Russian
  community archive (flashforgelab.ru) is the de facto record; its dates match
  our fleet telemetry exactly.
- GPL source effectively abandoned: one AD5M klipper drop (early 2024), **nothing
  for AD5X ever**. AD5X modding is permanent reverse-engineering.
- OTA is effectively forced (AD5M 5.1.8 auto-installed and broke FlashPrint; USB
  rollback works). Taking stock OTA **disables ZMOD** (data preserved, re-enable
  via `AD5X-ENABLE-zmod.tgz`).
- The 2026 cloud/ads lockdown is the active driver pushing owners to mods;
  ZMOD ships `disable_chinese_clouds`. Stock closed the checkbox-feature gap
  (LAN mode, exclude objects, PID cal, PA + "vibration compensation", T0-T15
  IFS slicing) but not the open-ecosystem gap.

### Timer-too-close (the load-bearing reliability fact)

- ZMOD's tracker carries 7+ TTC issues Oct 2025-Aug 2026; #561 is verbatim the
  signature our crash telemetry saw ("AD5X repeatedly fails with MCU 'eboard'
  shutdown: Timer too close"); #698 open 2026-08-20.
- Community consensus: **Klipper 13 on AD5X is the trigger** ("stay on 12 for the
  AD5X"). ZMOD defaults the AD5X to Klipper 12; `UPDATE_MCU FORCE=13` opts in
  (host and MCU must switch together — a `v0.13.0-746-ZMOD` host string proves
  full 13 mode). Both of our incident rigs ran 13.
- **Control-arm data (our rig, Klipper 12, idle)**: eboard `bytes_retransmit` ≤ 9
  over a ~70-min session vs **50 with `retransmit_seq=542` within 581 s of
  cold-start idle** on the Klipper-13 incident rig. The noisy-eboard signature
  is K13-only. Details and method in `FLASHFORGE_AD5X_PLATFORM_NOTES.md`.
- Separate documented crash trigger: slicer "Exclude Models" output gcode
  (suspected in incident #3, whose shutdown followed an excluded-objects update
  by 20 s on a BBL-Slicer file, on an above-ceiling stock base).
- Forge-X's celebrated TTC fixes are all host-side (SCHED_RR klippy, timeout
  patches, ZRAM); their own FAQ concedes the MCU-processing-overload class is
  not host-fixable — the class our eboard incidents are in. Even their regression
  harness had to stop screenshotting during toolhead motion to avoid TTC.

### Where this leaves us

AD5X = ZMOD for the foreseeable future, with no convergence toward FlashForge as
an integration partner. Our exposure concentrates exactly where the vendor-module
design puts it (z-offset provider table now carries both ZMOD and Forge-X rows;
the IFS backend is the one heavy asset). The identified structural exit for the
IFS backend's 7,200-line protocol client is an upstream `get_status()` on
`zmod_ifs`/`zmod_color` — live-verified 2026-08-24 that both objects answer
`objects/query` with empty status (no `get_status`), which is the root cause of
all JSON/dialog-echo polling. Patch preparation is in flight.
