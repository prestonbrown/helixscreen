# QIDI Plus 4 — Real-Device Analysis

**Date**: 2026-04-21
**Status**: Real device data analyzed (from user dump)
**Source**: Live Plus 4 running Kalico + Happy Hare + QIDI Box, Raspberry Pi 4 host (non-stock controller), user-provided Moonraker `info` / `objects/list` / `configfile` JSON, and `journalctl` HelixScreen log.

> **Caveat**: This user's Plus 4 is **heavily modded**. The stock Plus 4 ships with QIDI's closed-source controller board and TJC HMI display; this machine has a Pi 4 bolted on running Kalico (a Klipper fork) and a standard Moonraker stack. Details here describe what a modded/community-firmware Plus 4 exposes, not the stock SoC. Use with that caveat for preset heuristics that must also work on stock.

---

## 0. Stock Plus 4 display architecture (why HelixScreen on-device install is blocked)

The stock Plus 4 ships with an **MKS PI smart-panel** — the same architecture used across the 3-series (X-Max 3, X-Plus 3, X-Smart 3, Q1 Pro). It is a standalone microcontroller-driven HMI (TJC is the original Chinese OEM; Nextion is the same panel family licensed for global distribution) wired to the mainboard over serial UART, **not** a Linux framebuffer. The panel runs its own firmware and *is* the UI; the Klipper host pushes UI state and pre-rendered thumbnails into it via TJC commands.

### Panel hardware (confirmed)

From Sib6019's May 22, 2026 closeout report (`colpic_TJC8048X250_project_report.pdf`):

| Marking | Meaning |
|---------|---------|
| `TJC8048X250_011C_I_Z03` | Panel model + revision — 5" 800×480 capacitive HMI |
| `V1.65` (in QR `HXFJTQFKXSFK77FKV1.65+10806070`) | Panel firmware version |
| `CST3240` | Capacitive touch controller |
| `0500A013V1` | 5" LCD flex identifier |
| Header pinout `5V / A / B / Z / Y / GND`, `VCC = 5V` | RS-485-style serial header carrying TJC/Nextion UART |

### Host data path and UART map

```
slicer PNG
  └─> /home/mks/gene4.py        # PIL resize/pad → RGB565 (truncation)
       └─> /home/mks/libColPic.so   # ColPic_EncodeStr → palette+RLE+base-6 ASCII
            └─> *.tjc file
                 └─> xindi (/root/xindi/build/xindi, launched by start.sh)
                      └─> /dev/ttyS1 @ 115200 baud, 8N1 raw
                           └─> TJC8048X250 panel
```

Host is `mkspi` (ARM aarch64, Debian, Python 3.7.3). UART ownership on a live stock Plus 4:

| Device | Owner | Role |
|--------|-------|------|
| `/dev/ttyS0` | `klippy.py` | Klipper — printer MCU link |
| `/dev/ttyS1` | `xindi` | Display panel (TJC8048X250) @ 115200 |
| `/dev/ttyS2` | `klippy.py` | Klipper — secondary MCU link |
| `/dev/ttyACM0` | (USB-CDC) | Mainboard "QIDI BOX V1" (Klipper) |

`/etc/init.d/tuning` is **not** the display manager — it only sets CPU governors and thread affinity, and references `xindi` solely to pin its threads. The actual launcher is `/root/xindi/build/start.sh`.

### Thumbnail pipeline

Thumbnails are encoded via `/home/mks/libColPic.so` — a 12 KB closed-source ARM aarch64 binary exporting one symbol, `ColPic_EncodeStr`. The function packs an RGB565 pixel buffer into a custom RLE+palette format and base-6 ASCII-encodes the result into a `.tjc` blob the panel decodes. Seven stages, fully documented:

1. Palette construction with hard cap (`ADList0`, default 1024 entries; early-exits silently once full)
2. Frequency sort, reverse insertion-order tie-break
3. Overflow handler (dead code in practice — gate never satisfied)
4. 32-byte header: version, width, height, magic `0x05DDC33C`, palette/data byte counts
5. Little-endian uint16 palette table
6. Pixel-data RLE with `(idx >> 5)` escape bytes for palette high-bits
7. Base-6 ASCII pack (3 → 4 bytes, `0x5C` substituted as `0x7E` to escape backslash)

A community pure-Python reimplementation exists (byte-for-byte verified, 30/30 cases against the original, May 22 2026 by `Sib6019`; reference report `colpic_TJC8048X250_project_report.pdf`). Files capture the full disassembly mapping and dead-code analysis. **Useful for FreeDi/community-firmware contributors** writing tooling that pushes to the stock panel from the host side. **Not useful for HelixScreen integration** — see below.

A practically-significant upstream-encoder bug surfaced during that review: `gene4.py`'s PNG→RGB565 conversion silently overflows the 1024-colour palette cap on photographic input — roughly 24% of pixels collapse to `palette[0]` (the dominant colour) through the encoder's linear-search miss path, because the dead overflow handler (stage 3) is never reached. A one-line PIL `.quantize(colors=256)` upstream of the `>>3/>>2/>>3` truncation drops MAE 15–25× and shrinks `.tjc` output ~17%. This is a fix for the MKS / FreeDi community, not us.

### Encoder binary fingerprint (for cross-checking community reimplementations)

| Field | Value |
|-------|-------|
| Path | `/home/mks/libColPic.so` |
| Size | 12,448 B ELF |
| Toolchain | GCC 8.3.0 |
| Build date | May 27, 2023 |
| Build ID | `70f8067e3777d486589c59ffcc0522039437436d` |
| Default invocation | `ColPic_EncodeStr(rgb16, w, h, out, w*h*10, 1024)` (size 200, palette cap 1024) |

### Why HelixScreen can't drive this panel

HelixScreen is an LVGL renderer that targets Linux framebuffer / DRM / SDL surfaces. There is no architecture in the codebase for pushing UI to a remote serial HMI, and adding one would not turn HelixScreen into a touchscreen UI replacement: the panel firmware is the UI; HelixScreen would be reduced to a thumbnail-encoder helper.

The on-device install path requires replacing the MKS panel with a Linux-driven HDMI/DSI touchscreen (the same hardware swap the 3-series needs). Remote-control mode — HelixScreen running on a separate Pi/tablet/etc. talking to Moonraker on port 7125 — works on stock Plus 4 firmware without any modification.

### Implications for future work

- **Do not propose** "TJC backend for HelixScreen" or "drive the stock panel via libColPic" — both are architecturally dead ends.
- If we ever want a printer-mounted display on a stock Plus 4, the only viable path is replacing the panel or augmenting the printer with a separate Pi-driven display.
- The colpic RE work belongs in the FreeDi / community-firmware ecosystem, not here.

### Operational gotcha (if anyone ever probes the stock panel)

Direct Nextion-style `connect` handshakes against `/dev/ttyS1` while `xindi` is running trigger Linux's interrupt-storm protection (`Disabling IRQ #58` in kernel log). The serial port goes dead until reboot. **Do not `pkill xindi`** to free the port either — QIDI's customised TJC protocol does not respond to stock Nextion identification queries, so seizing the port gains nothing. For any future sniffing, use a passive read or a hardware TX-line tap. The panel's identity is already established by convergent evidence (sticker + QR + UART map + verified gene4.py→libColPic.so pipeline); no further on-line probing is warranted.

---

## 1. Host & Firmware Fingerprint

| Key | Value |
|-----|-------|
| Moonraker `info.app` | **`Kalico`** (not `Klipper`) |
| Klipper version string | `v2026.02.00-6-g9500bd61-dirty` |
| Python | 3.11.2 |
| `cpu_desc` (Moonraker) | `"4 core ?"` |
| Distribution | Debian 12 (bookworm), kernel `6.12.75+rpt-rpi-v8` |
| `cpu_info.model` | `Raspberry Pi 4 Model B Rev 1.5` |
| `cpu_info.processor` | `aarch64`, 64-bit, 4 cores |
| `total_memory` | 3,830,704 kB (~3.8 GB) |
| Hostname | `qidiplus4` |
| Klipper path | `/home/pi/klipper`, user `pi` |
| Services active | `klipper`, `moonraker`, `KlipperScreen`, `mobileraker`, `octoapp`, `moonraker-obico`, `go2rtc`, `klipperfleet` |
| Canbus | `can0` gs_usb, 1 Mbit |

### Implication for detection

Code audit result: **there is no `webhooks.app` / `info.app` gate anywhere in `src/printer/`** that would reject Kalico (Danger-Klipper legacy name, now Kalico). Detection uses firmware-agnostic Moonraker object queries only, so Kalico should behave identically to stock Klipper. No action needed here — but if we ever add an app-name check in future, remember to include `Kalico`.

---

## 2. Physical Printer Config (from `configfile`)

### Kinematics & build volume

- `home_xy_position = "152.5, 152.5"` in the `beacon` section → **~305 × 305 mm bed center**
- TMC2240 on X and Y (high-torque steppers), TMC2209 on Z and Z1
- Z tilt + screws tilt adjust configured
- Probe: **Beacon** coil (serial-by-id `usb-Beacon_Beacon_RevH_...`), `y_offset=-31`, contact probing for homing (`home_method=contact`), fallback to `proximity` once homed; `home_y_before_x=true`; `home_z_hop=5 @ 30 mm/s`
- `home_xy_move_speed = 300 mm/s`

### Heaters & sensors

| Object | Role |
|--------|------|
| `heater_bed` | Bed (limits not shown in dump header) |
| `heater_generic chamber` + `temperature_combined chamber` | **Active chamber heater** (Plus 4 signature) |
| `heater_fan chamber_fan` | Chamber fan |
| `temperature_sensor chamber_probe` | Ambient chamber probe |
| `temperature_sensor chamber_thermal_protection_sensor` | Over-temp protection |
| `extruder` + `tmc2209 extruder` | Single hotend |
| `heater_fan hotend_fan` | Hotend fan (monitored by `HOTEND_FAN_CHECK` macro; 3000 RPM minimum) |
| `fan_generic cooling_fan` / `auxiliary_cooling_fan` / `exhaust_fan` | Part cooling, aux cooling, exhaust |
| `temperature_sensor mainboard_mcu` / `toolhead_mcu` | MCU internal temps |
| `temperature_host raspberry_pi` / `temperature_sensor raspberry_pi` / `temperature_fan pi_fan` | Pi SoC monitoring + active fan control |
| `controller_fan board_fan` | Main board fan |
| `hall_filament_width_sensor` | Filament diameter monitor |

### Other hardware

- `output_pin caselight` — case LED
- `output_pin buzzer` — status buzzer
- `heater_fan hotend_fan` + delayed-gcode `CHECK_ALL_FANS` (3 s cadence) → stall detection pattern
- MCUs: `mcu` (main), `mcu toolhead` (canbus `toquecan`), `mcu mmu` (see §3), `mcu beacon`, `mcu host`

### Firmware-retraction, KAMP, exclude_object, virtual_sdcard, pause_resume all configured

Standard Klipper-ish stack; nothing special for detection.

---

## 3. QIDI Box (multi-material unit) — via Happy Hare

The user runs the QIDI Box through **Happy Hare** rather than QIDI's stock Box protocol. Happy Hare abstracts the Box into its normal MMU pipeline and the printer exposes all the Happy Hare objects we already consume.

### `mmu_machine` — the canonical config

```text
num_gates                 = 4
mmu_vendor                = Other
mmu_version               = 1.0
selector_type             = VirtualSelector
variable_bowden_lengths   = 0
variable_rotation_distances = 0
require_bowden_move       = 1
filament_always_gripped   = 1
has_bypass                = 0
environment_sensor        = temperature_sensor box1_env
filament_heater           = heater_generic box1_heater
max_concurrent_heaters    = 1
homing_extruder           = 1
```

**Two important Box-specific fields** are wired through `mmu_machine` into Happy Hare:

- `environment_sensor = temperature_sensor box1_env` → Box enclosure temp+humidity (AHT2X, I²C)
- `filament_heater = heater_generic box1_heater` → Box dryer heater

If HelixScreen consumes these fields from `mmu_machine` at detection time it gets "this Happy Hare instance is driving a dryer box" for free — no vendor-specific code needed for the dryer UI.

### Box hardware exposed

| Object | Purpose |
|--------|---------|
| `mcu mmu` | Dedicated **STM32F401CC** over USB (`usb-Klipper_stm32f401xc_...`), marked `is_non_critical=True` |
| `heater_generic box1_heater` | Dryer heater, PID (`kp=63.418, ki=1.342, kd=749.125`), min/max -100/100 °C, control `pid` |
| `temperature_sensor box1_env` | **AHT2X** (temp + humidity), I²C bus `i2c3` on `mmu` MCU, addr `0x38` (56 dec), report 5 s |
| `temperature_sensor box1_heater_temp_a` / `_b` | NTC 100K (`MGB18-104F39050L32`) front/back heater bed sensors |
| `temperature_combined box1_heater` | Mean of the two bed sensors for control |
| `verify_heater box1_heater` | `max_error=400`, `check_gain_time=600` (dryers heat slowly) |
| `heater_fan box1_heater_fan_a_box1` / `_b` | Dual heater-assist fans |
| `controller_fan box1_board_fan` | Board cooling |
| `temperature_sensor Box1_STM32` | MMU MCU die temp |

### Sensors (Happy Hare `mmu_sensors`)

- `pre_gate_switch_pin_0..3` (4 pre-gate filament detect switches) — these define **num_gates=4**
- `gate_switch_pin` — single shared gate sensor
- `extruder_switch_pin` + `extruder_switch_pin2` with `extruder_analog_range = "13000, 20000"` — hall dual-channel sensor at the extruder
- `mmu_encoder` with `encoder_resolution=3.0` (mm/pulse, Happy Hare's standard)

### LEDs — QIDI-specific **red/green-only**

```text
led qidi_slot_0  red_pin=mmu:PA2   green_pin=mmu:PA1
led qidi_slot_1  red_pin=mmu:PB5   green_pin=mmu:PB4
led qidi_slot_2  red_pin=mmu:PA15  green_pin=mmu:PA14
led qidi_slot_3  red_pin=mmu:PC5   green_pin=mmu:PC4

mmu_leds unit0:
  exit_leds = led:qidi_slot_0..3
  black_light = (1, 0, 0)   # "black" lamp is RED here
  white_light = (0, 1, 0)   # "white" lamp is GREEN here
```

**Caveat**: the QIDI Box has **no blue channel** on slot LEDs. Any HelixScreen UI preview of slot LEDs on a QIDI Box must fall back to a red/green mapping. The `black_light = (1,0,0)` / `white_light = (0,1,0)` convention above is the same one Happy Hare's shipped Qidi LED preset uses.

Effect presets: Happy Hare `mmu_led_effect` entries `bb_mmu_*` (`black`, `red_breathing`, `white_breathing`, `red_fast`, `white_fast`, `red_blink`, `white_blink`, `red_static`, `white_static`, `white_dim`, `pink_static`) — all respecting the 2-channel constraint.

### Gear steppers (4 drive motors)

```text
stepper_mmu_gear_0..3
  rotation_distance   = 13.6
  step_pulse_duration = 100 ns
  microsteps          = 16
```

All four share the same mechanical characteristics (single-pancake spool gear drive on each slot) — confirms VirtualSelector architecture (each slot has own gear motor, no physical selector).

### Box macros exposed to gcode

| Macro | Role |
|-------|------|
| `BOX_HOME_XY` | Home X/Y only (Z untouched) |
| `BOX_PRINT_START` | Tool-aware print start (takes `EXTRUDER`, `HOTENDTEMP` params) |
| `BOX_CUSTOM_PARK` | Park at XY, accounts for y>305 trash zone |
| `BOX_CLEAR_FLUSH` | Chute-flush routine |
| `BOX_CLEAR_OOZE` | Chute-ooze clean |
| `BOX_CLEAN_NOZZLE` | Flush + ooze combined |
| `BOX_MOVE_TO_CUTTER` | Travel to cutter X=304, Y=20 |
| `BOX_DO_CUT` | Actuate cutter (Y=4.5 bump) |
| `BOX_END_PRINT` | Box end-of-print raise |
| `DISABLE_BOX_HEATER` | `MMU_END` + clear dryer temp |
| `CUT_FILAMENT`, `EXTRUSION_AND_FLUSH`, `TRY_RESUME_PRINT`, `TRY_SELFRECOVERY` | Supporting recovery/flow |
| `TOOL_CHANGE_START`, `TOOL_CHANGE_END` | QIDI-native tool-change wrapper |
| `E_LOAD`, `E_UNLOAD`, `E_BOX` | Extruder-stage load/unload + `MMU_EJECT GATE={slot}` |
| `UNLOAD_T0..T3`, `T0..T3` | Tool macros forwarding to Happy Hare |
| `WIPIN`, `POWER_OFF_VENT`, `POWER_ON_VENT`, `MOVE_TO_TRASH` | Chamber vent + purge |

**Detection opportunity**: the `BOX_*` macro family is a strong "QIDI Box present" signal independent of any vendor field (`mmu_vendor="Other"` on this unit). A heuristic match on `BOX_PRINT_START` + `BOX_DO_CUT` + `UNLOAD_T0..T3` would give us a reliable Box detection even when users set Happy Hare vendor to `Other`.

### Working area coordinates learned from the macros

- **Cutter**: X=304, Y=4.5 (active) / Y=20 (safe) — back-right corner
- **Purge chute**: Y=305–324, X=55–95 — behind bed rear edge (y=305 = bed edge)
- **Trash zone**: Y > 305 (anywhere behind bed)
- **Bed extent**: 305 × 305 (matches `home_xy_position` center)

Useful for a future Plus 4 print-start visualization or park-pos preview.

---

## 4. HelixScreen Behavior Observed (from log)

Log source: `message.txt`, systemd `journalctl` capture around Apr 16 21:14 local / Apr 16 19:14 UTC.

### ✅ Working

- Happy Hare detected as MMU backend
- Box temperature/humidity via `temperature_sensor box1_env` expected to surface in AMS environment UI (log shows `[AMS Environment] Created`, `[AMS Environment] Callbacks registered`)
- German locale fully loaded (`Koppeln`, `Abbrechen`, `Kopplung...`)
- Bluetooth scanner pairs successfully (58:C5:C7:8C:1F:87)
- Lazy registration of AMS widgets (SpoolCanvas, AmsSlot, FilamentPath, EndlessSpoolArrows) succeeds
- Panel switch completes in 16.2 ms

### 🐞 Bugs surfaced

1. **Slot count off-by-two (6 vs 4)**
   Repeatedly in log:
   ```
   [AMS State] Synced from backend - type=Happy Hare, slots=6, action=Idle, segment=Spool
   [AmsDetail] Path canvas configured: slots=6, unit=-1, hub_only=false
   [AmsPanel] Slot count changed to 6
   [AMS Panel] Created 6 slot widgets via shared helpers
   ```
   But `mmu_machine.num_gates = 4`. HelixScreen is defaulting or reading the wrong field. Needs investigation — suspect either a hard-coded `6` fallback in the Happy Hare backend or we're reading a stale `mmu.num_gates` path that's not populated (MoonrakerObjects vs ConfigFile). Most likely the Box doesn't publish `mmu.num_gates` live before a `MMU_STATUS` macro call, and our code defaults to `6`.

2. **`PrinterDetector` cannot load database on device** — *likely a stale-install artifact from the config/assets split*
   ```
   [INTERNAL] [PrinterDetector] Failed to open config/printer_database.json
   [PrinterDetector] Cannot lookup capabilities without database
   [PrinterDetector] Cannot lookup z_offset_calibration_strategy without database
   [Prerendered] Using printer image: assets/images/printers/prerendered/voron-24r2-300.bin
   ```
   `helix::find_readable("printer_database.json")` (src/application/data_root_resolver.cpp:87) checks the writable user config dir first, then falls back to `<data_dir>/assets/config/printer_database.json`. An install upgraded from before the config/assets split can end up with neither path populated (seed never re-extracted, user config cleaned). The error text reports the user-path it would *prefer* — not that the seed is missing.
   **Note**: the `qidi_plus_4` preset **does exist** (`assets/config/printer_database.json:2227-2320`) and specifies `"image": "qidi-plus-4.png"`, which also exists at `assets/images/printers/qidi-plus-4.png`. Fix the DB-not-loading issue and detection + image should just work — no new assets needed for this printer.

3. **Translation gaps (German)**
   ```
   lv_translation_get: `Pair Bluetooth Scanner` tag is not found
   lv_translation_get: `?` tag is not found
   ```
   `Pair Bluetooth Scanner` needs a translation key + German string. The `?` tag looks like a placeholder leaking from the Spoolman/filament side — worth tracking separately.

4. **InputScanner: paired but no evdev**
   ```
   [InputScanner] No BT HID evdev found for mac=58:C5:C7:8C:1F:87
   [BarcodeScannerSettings] Post-pair HID probe: hid_ok=false
   ```
   Matches the known USB-scanner project in memory (`project_usb_scanner_progress`) — this scanner pairs over Bluez but does not expose a `/dev/input/event*` node; needs the raw HID fallback path.

5. **LVGL XML `error` constant missing**
   ```
   lv_xml_get_const_internal: No constant was found with name "error"
   ```
   In EndlessSpoolArrows component. Minor polish bug.

6. **ALSA buffer underruns**
   ```
   [ALSASound] Buffer underrun, recovering
   ```
   Expected on Pi 4 under load; not actionable unless it's frequent.

---

## 5. What a Plus 4 preset needs

Given the data above, a `qidi-plus4` entry in `printer_database.json` should cover:

```jsonc
{
  "qidi-plus4": {
    "manufacturer": "QIDI",
    "model": "Plus 4",
    "build_volume": { "x": 305, "y": 305, "z": 280 },   // Z from spec (280 mm)
    "bed_type": "textured_pei",                          // stock
    "hotend_max_temp": 370,
    "bed_max_temp": 120,
    "chamber": {
      "present": true,
      "heated": true,
      "max_temp": 65
    },
    "probe_type": "beacon_contact",                      // matches dump
    "z_offset_calibration_strategy": "beacon_autocal",
    "kinematics": "corexy",
    "capabilities": {
      "ams_type": "qidi_box",                            // already planned per QIDI_SUPPORT.md
      "ams_slots_default": 4,
      "filament_runout_sensor": true,
      "filament_width_sensor": true,
      "filament_motion_sensor": true,
      "case_light": true,
      "buzzer": true,
      "chamber_fan": true,
      "aux_cooling_fan": true,
      "exhaust_fan": true
    },
    "image": "qidi-plus4-305.bin",                       // need asset
    "detection": {
      "hostnames": ["qidiplus4", "qidi-plus4", "qidi_plus_4"],
      "required_objects": ["heater_generic chamber"],
      "qidi_macros": ["CLEAR_NOZZLE", "M141", "M191"]
    }
  }
}
```

**Open assets needed** before shipping the preset:
- Prerendered 3D image at the standard sizes (mirror the AD5M/Voron recipe in `assets/images/printers/prerendered/`)
- A source PNG in `assets/images/printers/` for the runtime fallback path

---

## 6. Action items

### Short-term (ship independently of hardware access)

1. **Treat Kalico as Klipper** in app-name checks — verify whether this is already done; if not, extend the accepted list to include `Kalico`.
2. **Investigate "slots=6" mystery.** Code inspection (`src/printer/ams_backend_happy_hare.cpp:465-475`) shows slots are **not** defaulted to 6 — they're initialized from `gate_status.size()`. And `num_gates` parsing at lines 393-442 correctly reads `printer.configfile.settings.mmu.num_gates` in all forms (int/string/array), including string `"4"` → single 4-slot unit at lines 969-976. So the 6 has to come from:
   - the `gate_status` array actually being length 6 (possible if Happy Hare exposes a longer array than `num_gates` declares — check `printer.mmu.gate_status` in a fresh Moonraker dump from this user), **or**
   - a transient pre-status default before `gate_status` arrives (needs runtime trace).
   Next step: ask the user for `curl http://qidiplus4/printer/objects/query?mmu` to see the live `gate_status` / `num_gates` payload.
3. **Fill out existing `qidi_plus_4` preset** — it only has `image`, `print_start_profile`, `ams_type`, `notes`, and heuristics. Add explicit specs: `build_volume`, `hotend_max_temp`, `bed_max_temp`, `chamber` (present, heated, max_temp), `kinematics: "corexy"`, `probe_type: "beacon_contact"`, `z_offset_calibration_strategy`. The missing-specs pattern is shared across all QIDI entries (`qidi_x_plus_3` has the same gaps) — fix the schema once, apply everywhere.
4. **Prerendered image pipeline**: PNG `qidi-plus-4.png` exists; `assets/images/printers/prerendered/` is empty (pipeline generates `.bin` on-device at first use via `PrinterCache`). No action needed here unless we decide to ship prerendered `.bin` files for cold-start performance.
5. **Add `Pair Bluetooth Scanner` translation key** and a German string (`Bluetooth-Scanner koppeln`).
6. **Happy Hare → QIDI Box heuristic**: when `mmu_machine.filament_heater` is set AND the gcode namespace contains `BOX_PRINT_START`, label the UI as **"QIDI Box (via Happy Hare)"**. This distinguishes from stock Happy Hare on a Voron/Blackbox 8-track install.  *No existing code does this — macro scan lives in `printer_detector.cpp:577-579` for preset matching only; AMS backend choice is static from `printer_database.json` (`src/printer/ams_backend_qidi.cpp:22-58`).*

### Blocked on test hardware (for later)

7. Native `AmsBackendQidi` write-path validation on real hardware — implemented (`FILAMENT_BACKEND_QIDI_BOX.md`), field-verified through logs only (#1030).
8. Red/green LED color previews in the slot UI when backend is QIDI Box — current preview assumes RGB, and blue will read as "off" on real hardware.
9. RFID MIFARE Classic readout path when using stock QIDI firmware (not Happy Hare).

---

## 7. References

- Upstream Happy Hare QIDI MMU config: [moggieuk/Happy-Hare](https://github.com/moggieuk/Happy-Hare) (`config/mmu_hardware.cfg` QIDI vendor sections)
- QIDI Plus 4 product page: [qidi3d.com](https://qidi3d.com/)
- Kalico Klipper fork: [kalico.gg](https://kalico.gg/)
- [`FILAMENT_BACKEND_QIDI_BOX.md` § QIDI Box](../FILAMENT_BACKEND_QIDI_BOX.md#qidi-box-qidi-plus4--q2--max4)
- [`QIDI_SUPPORT.md`](../printers/QIDI_SUPPORT.md) for platform/installation side
