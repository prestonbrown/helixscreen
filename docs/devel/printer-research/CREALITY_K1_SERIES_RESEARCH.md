# Creality K1C and K1 Max Research

**Date**: 2026-02-02
**Status**: Pre-support research. The K1 series is supported; the platform (build, deploy,
firmware prerequisites, measured hardware) is documented in
[`../printers/CREALITY_K1_SUPPORT.md`](../printers/CREALITY_K1_SUPPORT.md), which supersedes this page where they
disagree. The CFS upgrade's firmware internals are in [`CREALITY_CFS_K1_INTERNALS.md`](CREALITY_CFS_K1_INTERNALS.md).

## Executive Summary

The Creality K1C and K1 Max are high-speed CoreXY printers running Klipper on an **Ingenic X2000E MIPS processor**. They have official root access via the touchscreen settings. The display runs on a framebuffer (`/dev/fb0`) using a proprietary `display-server` (believed to be LVGL-based). **GuppyScreen** has proven that custom LVGL-based UIs work on this hardware.

---

## 1. Hardware Specifications

### Processor & Memory

| Component | Specification |
|-----------|---------------|
| **CPU** | Ingenic X2000E, MIPS32r2 dual-core @ 1.2 GHz |
| **RAM** | 256 MB LPDDR2 (SIP - System-in-Package) |
| **Storage** | 8 GB eMMC |
| **Additional Core** | XBurst 0 @ 240 MHz (security/real-time control) |
| **ISA Features** | MIPS32 R5 + MIPS SIMD (128-bit MSA) |

### Display

| Attribute | Value |
|-----------|-------|
| **Size** | 4.3 inches |
| **Resolution** | 480 x 400 pixels |
| **Type** | Full-color capacitive touchscreen |
| **Interface** | Framebuffer at `/dev/fb0` |
| **Touch Input** | evdev-based |

### K1C vs K1 Max Differences

| Feature | K1C | K1 Max |
|---------|-----|--------|
| **Build Volume** | 220 x 220 x 250 mm | 300 x 300 x 300 mm |
| **Rated Power** | 350W | 1000W |
| **AI Camera** | Standard | Included |
| **AI LiDAR** | Not included | Included |
| **Ethernet Port** | No | Yes |
| **Nozzle Type** | Tri-metal "Unicorn" (hardened) | Hardened steel |

---

## 2. Stock Firmware

### Operating System
- **Creality OS** - custom Linux built on Buildroot 2020.02.1
- **Python**: 3.8.2
- **glibc**: 2.26
- **Klipper**: Creality fork (dated 2022, missing upstream features)

### Partition Layout

| Partition | Size | Purpose |
|-----------|------|---------|
| uboot | 1 MB | Bootloader |
| kernel | 8 MB | Linux kernel |
| rootfs | 300 MB | Root filesystem |
| userdata | ~6.7 GB | User data/prints |

### Key Services
- **Klipper** - Motion control
- **display-server** - LVGL-based proprietary UI
- **Monitor** - Watchdog/system monitor
- **mjpeg-streamer** - Camera streaming

---

## 3. Moonraker Availability

### Stock Firmware
**No Moonraker** - Creality uses proprietary communication.

### After Rooting
Moonraker installable via:
1. **Guilouz Helper Script** - Adds Moonraker, Fluidd/Mainsail
2. **Guppy Mod (ballaswag)** - Includes mainline Klipper + Moonraker

### Ports (when installed)
| Port | Service |
|------|---------|
| 7125 | Moonraker API |
| 4000 | Mainsail |
| 4001 | Fluidd |
| 8080-8083 | Camera streams |

### Memory Warning
Creality warns: "Enabling services like Moonraker may occasionally lead to excessive memory usage and system crashes" due to limited 256 MB RAM.

---

## 4. Custom Firmware Options

### Guilouz Creality Helper Script
**URL**: [github.com/Guilouz/Creality-K1-and-K1-Max](https://github.com/Guilouz/Creality-K1-and-K1-Max)

- Moonraker installation
- Fluidd/Mainsail web interfaces
- GuppyScreen (optional)
- Custom macros and improved configurations
- Input shaper calibration tools

### Guppy Mod (ballaswag)
**URL**: [github.com/ballaswag/creality_k1_klipper_mod](https://github.com/ballaswag/creality_k1_klipper_mod)

| Feature | Stock | Guppy Mod |
|---------|-------|-----------|
| Klipper | Creality fork (2022) | Mainline |
| Buildroot | 2020.02.1 | 2024.02.2 |
| Python | 3.8.2 | 3.11.8 |
| Memory Usage | ~46% on boot | ~36% on boot |
| Display UI | display-server | GuppyScreen |

**Note**: Guppy Mod breaks PRTouch - requires alternative probe.

---

## 5. Klipper Configuration Structure

### MCU Architecture (Multi-MCU)

| MCU | Chip | Serial Port | Purpose |
|-----|------|-------------|---------|
| **mcu** | GD32F303RET6 | /dev/ttyS7 | Main motion control |
| **nozzle_mcu** | GD32F303CBT6 | /dev/ttyS1 | Hotend, LED, accelerometer |
| **leveling_mcu** | GD32E230F8P6 | /dev/ttyS9 | Load cell probing |

### Motion System
```ini
[printer]
kinematics: corexy
max_velocity: 1000
max_accel: 20000
```

### Exposed Klipper Objects

```ini
# Motion
[stepper_x], [stepper_y], [stepper_z]
[tmc2209 stepper_x], [tmc2209 stepper_y], [tmc2209 stepper_z]

# Heating
[extruder]
[heater_bed]
[heater_generic chamber_heater] # K1 Max only

# Temperature Sensors
[temperature_sensor mcu_temp]
[temperature_sensor chamber_temp]

# Fans
[fan]
[fan_generic fan0], [fan_generic fan1], [fan_generic fan2]
[heater_fan hotend_fan]

# Probing
[prtouch_v2] # Creality's pressure-based leveling
[bed_mesh]
[adxl345]

# Input Shaper
[input_shaper]
```

### No Multi-Extruder/Toolchanger
K1 series are **single extruder** printers. Third-party solutions like **Co Print KCM** can add multi-material.

---

## 6. Display Interface

### Stock Display System
- **Framebuffer**: `/dev/fb0`
- **Framework**: LVGL (suspected based on reverse engineering)
- **Resolution**: 480 x 400

Display can be accessed directly:
```bash
# Kill display-server and Monitor
# Write directly to framebuffer
cat random_data > /dev/fb0
```

### GuppyScreen (Alternative UI)
**URL**: [github.com/ballaswag/guppyscreen](https://github.com/ballaswag/guppyscreen)

| Feature | GuppyScreen |
|---------|-------------|
| **Framework** | LVGL 8.3.x |
| **Display** | Direct framebuffer (no X11/Wayland) |
| **Touch** | evdev |
| **Communication** | Moonraker WebSocket API |
| **Architectures** | MIPS, ARM64, x86_64 |

GuppyScreen proves custom LVGL UIs work on K1 hardware.

---

## 7. Root Access

### Official Root Access (Post firmware ~1.3.x)
Available via touchscreen:
1. Settings → Root Account Information
2. Accept disclaimer
3. Wait 30 seconds, confirm

### SSH Credentials

| Firmware | Password |
|----------|----------|
| Stock (rooted) | creality_2023 |
| Guppy Mod | guppy |

```bash
ssh root@<printer_ip>
```

---

## 8. Community Resources

### GitHub Repositories

| Repository | Purpose |
|------------|---------|
| [CrealityOfficial/K1_Series_Klipper](https://github.com/CrealityOfficial/K1_Series_Klipper) | Official Klipper source |
| [Guilouz/Creality-K1-and-K1-Max](https://github.com/Guilouz/Creality-K1-and-K1-Max) | Helper Script & Wiki |
| [ballaswag/creality_k1_klipper_mod](https://github.com/ballaswag/creality_k1_klipper_mod) | Guppy Mod |
| [ballaswag/guppyscreen](https://github.com/ballaswag/guppyscreen) | LVGL Touch UI |
| [ballaswag/k1-discovery](https://github.com/ballaswag/k1-discovery) | Hardware RE docs |

### Communities
- **Reddit**: r/crealityk1
- **Creality Forum**: forum.creality.com
- **Guilouz Wiki**: guilouz.github.io/Creality-Helper-Script-Wiki/
