# AnyCubic CoreXY Printers with Moonraker Support

**Date**: 2026-02-02
**Status**: Comprehensive research complete

## Executive Summary

The most relevant AnyCubic CoreXY printers are the **Kobra S1 Combo** and **Kobra 3 Combo** series. They run **KobraOS** (Klipper rewritten in Golang) and can be extended with **Rinkhals** custom firmware for standard Moonraker. **HelixScreen already has ACE backend support** for the ACE Pro multi-material system.

> ### Correction (2026-06-13)
>
> This document's original title lumped the entire Kobra family under "CoreXY". That is **wrong** and was the likely origin of a kinematics bug fixed on 2026-06-13.
>
> - **CoreXY (enclosed):** Kobra S1, Kobra S1 Max — *only these two*.
> - **Cartesian bedslingers:** Kobra 2 Pro, Kobra 3, Kobra 3 V2, Kobra 3 Max.
>
> The printer database previously mislabeled **Kobra 3 as corexy**; it is a cartesian bedslinger (verified against the official Anycubic Klipper-go `printer_*.cfg` files). This is now fixed in the DB. See the verified fingerprint table appended at the end of this document.

---

## 1. Hardware Specifications

### Kobra 3 / Kobra 3 Combo (Most Popular)

| Component | Specification |
|-----------|---------------|
| **Processor** | Rockchip RV1106G3 (ARM Cortex-A7 @ 1.2GHz) |
| **RAM** | 256MB DDR3L (integrated) |
| **Storage** | 8GB eMMC |
| **Mainboard** | Trigorilla Spe B v1.1.x |
| **MCU** | Huada HC32F460 (stepper control) |
| **Display** | 4.3" Capacitive Touchscreen (tilting) |
| **Connectivity** | WiFi (2.4GHz), USB-A (3 ports) |
| **Build Volume** | 250 x 250 x 260mm |
| **Max Speed** | 600mm/s |
| **Hotend** | 300°C max |

### Kobra S1 / Kobra S1 Combo (CoreXY, Enclosed)

| Component | Specification |
|-----------|---------------|
| **Motion** | CoreXY (enclosed) |
| **Build Volume** | 250 x 250 x 250mm |
| **Max Speed** | 600mm/s (300mm/s recommended) |
| **Acceleration** | 20,000 mm/s² |
| **Hotend** | 320°C max |
| **Multi-Color** | Up to 8 colors (2x ACE Pro) |
| **Price** | $549-$749 |

### Kobra S1 Max Combo (Large CoreXY)

| Component | Specification |
|-----------|---------------|
| **Build Volume** | 350 x 350 x 350mm |
| **Heated Chamber** | 65°C active heating |
| **Hotend** | 350°C max |
| **Multi-Color** | Up to 16 colors (ACE 2 Pro) |
| **Price** | $849-$1,099 |

### Rockchip RV1106G3 Details

| Feature | Specification |
|---------|---------------|
| **Architecture** | ARM Cortex-A7 (32-bit) |
| **Clock** | 1.2 GHz |
| **Memory** | Integrated 256MB DDR3L |
| **NPU** | 1.0 TOPS INT8 (AI features) |

---

## 2. Stock Firmware: KobraOS

### What KobraOS Is
- **Based on Klipper** but rewritten in **Golang** ("klipper-go") — this is the
  **"K3" software base**. The newer Kobra X moved to a **"K4" base that is a C++
  Klipper port** with mandatory RSA-signed firmware (Rinkhals-blocked). The
  unpublished community "Kobrian" system has been seen replacing that OS with
  Debian and vanilla Klipper on the Kobra X's own board, with HelixScreen as
  the screen; KobraNest is a published Debian system for the Kobra S1. See
  [KLIPPER_FORKS_AND_PORTS.md](KLIPPER_FORKS_AND_PORTS.md) for the full Go→C++
  story, the Kobrian and KobraNest details and the GPL/clean-room analysis.
- **Closed source** with locked configs
- **LVGL-based UI** for touchscreen
- Configs in `/userdata/app/gk/config/`

### Official Klipper-go Release
[github.com/ANYCUBIC-3D/klipper-go](https://github.com/ANYCUBIC-3D/klipper-go)

Supports: Kobra 3, Kobra 3 V2, Kobra 3 Max, Kobra S1

**Limitations:**
- "Certain proprietary features not open source"
- Not compatible with standard Python Klipper ecosystem
- No standard Moonraker WebSocket subscriptions

---

## 3. Multi-Material System: ACE Pro

> **See also:** [`ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md`](ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md)
> — real-log teardown of a Kobra S1 running the **mainline-Python Klipper fork**
> (`klipper-kobra-s1`) with a custom `[ace_status]` Moonraker component. Refines several
> figures below (baud, USB selection, `GET_INFO` schema) with observed data.

### Specifications

| Feature | Specification |
|---------|---------------|
| **Slots** | 4 per unit |
| **Max Capacity** | 8 colors (2 units), 16 (S1 Max) |
| **Dryer** | Dual 200W PTC, 55°C max |
| **RFID** | Auto filament detection |
| **Connection** | USB (CDC-ACM serial) |
| **Baud Rate** | 115200 |

### USB Identification
```
/dev/serial/by-id/usb-ANYCUBIC_ACE_0-if00
/dev/ttyACM0
```

### Communication Protocol
JSON-based serial protocol with framed messages:
```
Header: \377\252
Length: 2 bytes
Payload: JSON ({"id":2948,"method":"get_status"})
Checksum: 2 bytes
```

**Commands:**
- `get_status` - Query device state
- `drying` - Start drying
- `drying_stop` - Stop drying

---

## 4. Moonraker Availability

### Stock
**No standard Moonraker** - KobraOS uses proprietary communication.

### With Rinkhals
Full Moonraker support:

| Service | Port |
|---------|------|
| Mainsail | 80, 4409 |
| Fluidd | 4408 |
| SSH | 22 (root:rockchip) |
| Camera | mjpg-streamer |

---

## 5. Custom Firmware Options

### Rinkhals (Recommended)
**Repository**: [github.com/jbatonnet/Rinkhals](https://github.com/jbatonnet/Rinkhals)

| Feature | Status |
|---------|--------|
| Moonraker | Yes |
| Mainsail/Fluidd | Yes |
| SSH | root:rockchip |
| Stock UI | Preserved |
| Install | USB `.swu` file |

**Supported Printers:**
| Printer | Supported Firmware |
|---------|-------------------|
| Kobra 3 | 2.4.4.7, 2.4.5 |
| Kobra 2 Pro | 3.1.2.3, 3.1.4 |
| Kobra S1 | 2.5.8.8, 2.5.9.9, 2.6.0.0 |
| Kobra 3 Max | 2.5.1.3, 2.5.1.7 |
| Kobra 3 V2 | 1.1.0.1, 1.1.0.4 |
| Kobra S1 Max | 2.1.6 |

### DuckPro-Kobra3
**Repository**: [github.com/utkabobr/DuckPro-Kobra3](https://github.com/utkabobr/DuckPro-Kobra3)

- Modified Moonraker (emulates missing features)
- Python 3.11, Nginx, Fluidd/Mainsail

---

## 6. Klipper Configuration

### With Rinkhals
Standard printer.cfg access via Moonraker:
- `printer_mutable.cfg` - User-modifiable (editing voids support)

### Filament Hub Object
```ini
[filament_hub]
serial: /dev/serial/by-id/usb-ANYCUBIC_AMS-CDC_ACM_...
baud: 115200
max_volumes: 16
enable_rfid: 1
```

---

## 7. Display Interface

### Hardware
- **Size**: 4.3" capacitive touchscreen
- **Resolution**: ~480x320 or 480x272 (typical for 4.3")
- **Framework**: **LVGL** (confirmed by Rinkhals LVGL injection)
- **Framebuffer**: `/dev/fb0`

### For HelixScreen
- Framebuffer access available
- Touch via Linux input subsystem
- Must coexist with or replace KobraOS UI
- LVGL already in use - compatible base

---

## 8. Root Access

### With Rinkhals
- **SSH**: `root@<printer-ip>:22`
- **Password**: `rockchip`

### System Details
- **OS**: Linux 5.10.160 (ARMv7)
- **Device**: "Rockchip RV1106G IPC38"

---

## 9. HelixScreen Compatibility

### Existing ACE Support

HelixScreen already has a complete ACE backend:
- `include/ams_backend_ace.h`
- `src/printer/ams_backend_ace.cpp`

**Features:**
- WebSocket subscription to the native `filament_hub` object (Rinkhals/GoKlipper) or the `ace` / `ace_instance_N` objects (community drivers)
- REST polling at ~500ms on the community fallback path only
- Load/unload filament (`ACE_CHANGE_TOOL`)
- Dryer control (35-55°C)
- 4-slot hub topology

**REST Endpoints (community fallback only, see [`FILAMENT_BACKEND_ACE.md`](../FILAMENT_BACKEND_ACE.md)):**
- `GET /server/ace/info` - System info (optional)
- `GET /server/ace/status` - Current state
- `GET /server/ace/slots` - Slot details

### What Would Be Needed

1. **Rinkhals Installation** - Provides Moonraker
2. **klipper-go Differences** - none for the ACE: GoKlipper's `filament_hub` is subscribed over WebSocket like any Klipper object
3. **Display Options**:
   - **External**: HelixScreen on Pi connected to Moonraker
   - **On-printer**: Replace stock UI (challenging - weak CPU/RAM)

### Resource Warning
> "Those printers are quite weak in terms of CPU and Memory. Every additional app/feature and client will make the experience slower and might crash."

---

## 10. ACE Klipper Drivers

### ValgACE (Primary — includes Moonraker component)
[github.com/agrloki/ValgACE](https://github.com/agrloki/ValgACE) - ACE Pro driver for any Klipper printer. Provides the `ace_status.py` Moonraker component with REST endpoints required by HelixScreen.

### Installation (ValgACE)
```bash
git clone https://github.com/agrloki/ValgACE.git
cd ValgACE && ./install.sh
```

Add to printer.cfg:
```ini
[include ace.cfg]
```

### G-code Commands

| Command | Description |
|---------|-------------|
| `ACE_CHANGE_TOOL TOOL=n` | Load slot n (-1 to unload) |
| `ACE_FEED LENGTH=x SPEED=y` | Feed filament |
| `ACE_RETRACT` | Retract filament |
| `ACE_START_DRYING TEMP=t DURATION=m` | Start drying |
| `ACE_STOP_DRYING` | Stop drying |
| `ACE_STATUS` | Query status |

### REST API Endpoints
- `GET /server/ace/info`
- `GET /server/ace/status`
- `GET /server/ace/slots`

### Alternative Drivers

| Project | Target |
|---------|--------|
| [ACEPROK1Max](https://github.com/swilsonnc/ACEPROK1Max) | Creality K1 Max |
| [ACEPROSV08](https://github.com/szkrisz/ACEPROSV08) | Sovol SV08 |

---

## 11. Community Resources

### Discord
- **Rinkhals**: [discord.gg/3mrANjpNJC](https://discord.gg/3mrANjpNJC)

### GitHub

| Repository | Purpose |
|------------|---------|
| [jbatonnet/Rinkhals](https://github.com/jbatonnet/Rinkhals) | Custom firmware |
| [utkabobr/DuckPro-Kobra3](https://github.com/utkabobr/DuckPro-Kobra3) | Alternative CFW |
| [ANYCUBIC-3D/klipper-go](https://github.com/ANYCUBIC-3D/klipper-go) | Official Golang Klipper |
| [agrloki/ValgACE](https://github.com/agrloki/ValgACE) | ACE Pro driver |
| [printers-for-people/ACEResearch](https://github.com/printers-for-people/ACEResearch) | ACE reverse engineering |

### Documentation
- **Rinkhals Docs**: [jbatonnet.github.io/Rinkhals](https://jbatonnet.github.io/Rinkhals/)

---

## Recommendations

### Target Configuration
- **AnyCubic Kobra S1 Combo** (CoreXY, enclosed)
- **Rinkhals firmware** for Moonraker
- **ACE Pro** supported natively through the `filament_hub` object; Rinkhals needs no extra component

### Integration Path
1. **External HelixScreen** device (Pi + touchscreen)
2. Connect to Rinkhals Moonraker
3. ACE backend handles ACE Pro

### Challenges
- Limited onboard resources for direct printer install

---

## Conclusion

AnyCubic Kobra S1 Combo is a good HelixScreen target:
- ARM architecture (Cortex-A7)
- Rinkhals provides Moonraker
- ACE Pro **already supported** via ACE backend
- Best approach: external HelixScreen device connecting to printer's Moonraker

---

## Verified Detection Fingerprints & ACE Interface (2026-06-13)

The facts below were verified from the official Anycubic Klipper-go `printer_*.cfg` files, GoKlipper `extras_ace.go`, and Rinkhals `mmu_ace.py`, and are the basis for the printer-database entries landed 2026-06-13. They supersede any kinematics or ACE-object claims earlier in this document.

### Per-model kinematics, build volume, and distinguishing Klipper objects

| Model | Kinematics | Build volume (`pos_max`, mm) | Distinguishing Klipper object(s) |
|-------|-----------|------------------------------|----------------------------------|
| Kobra 2 Pro | cartesian (bedslinger) | — | — |
| Kobra 3 | cartesian (bedslinger) | 278.5 × 260 × 262 | `cs1237` (load cell) |
| Kobra 3 V2 | cartesian (bedslinger) | 278.5 × 260 × 262 | `cs1237` (load cell) |
| Kobra 3 Max | cartesian (bedslinger) | 478 × 440 × 502 | `stepper_y1` (dual-Y), `filament_tracker` (gpio) |
| Kobra S1 | corexy (enclosed) | 265 × 277 × 253 | `filament_tracker` (adc) |
| Kobra S1 Max | corexy (enclosed) | ~350 (unpublished) | — |

`filament_hub` (the ACE hub object — see below) is present on any Combo-equipped model regardless of kinematics, so it distinguishes "has ACE" but not the printer model.

### Native ACE = `filament_hub` (not `ace`)

The native Anycubic GoKlipper ACE registers the Klipper object **`filament_hub`** (config section `[ace]`), confirmed in `extras_ace.go` and Rinkhals `mmu_ace.py`. The earlier assumption that real Anycubic ACE registers an object named `ace` was wrong — only the **community** ValgACE/BunnyACE/DuckACE drivers use `ace`, and **ValgACE is niche** (it targets ACE Pro hardware on a *non-Anycubic* DIY printer; DuckACE is abandoned). Community drivers integrate via Moonraker macros/REST endpoints rather than a native Klipper object.

**Native `filament_hub.get_status()` schema** (flat, single hub, 4 slots):

- `status`
- `dryer{status, target_temp, duration, remain_time}`
- `temp`
- `slots[]{index, status(empty/ready/preload/running/runout), sku, type, color[r,g,b]}`
- `current_filament` = `"<unitId>-<localIndex>"` (e.g. `"0-2"`); empty/absent = nothing loaded

Multi-unit "Combo" (8 slots) is a Rinkhals-layer abstraction stacked above the single-hub GoKlipper object.

**Native G-code verbs** (real, from `extras_ace.go`):

`ACE_CHANGE_TOOL TOOL={n|-1}`, `ACE_FEED INDEX= LENGTH= SPEED=`, `ACE_RETRACT INDEX= LENGTH= SPEED=`, `ACE_ENABLE_FEED_ASSIST INDEX=`, `ACE_DISABLE_FEED_ASSIST INDEX=`, `ACE_START_DRYING TEMP= DURATION=`, `ACE_STOP_DRYING`. (`ACE_RECOVER` / `ACE_RESET` are **not** native.)

> For the full backend detail — detection order, REST community fallback, threading model, and capability/dryer tables — see **`docs/devel/FILAMENT_MANAGEMENT.md` § "ACE (Anycubic ACE Pro)"**.
