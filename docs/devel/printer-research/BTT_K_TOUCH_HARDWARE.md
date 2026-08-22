# BTT K-Touch Hardware Recon

**Date:** 2026-07-13
**Context:** Phase 1c target hardware for the ESP32 display program (`docs/devel/plans/2026-06-10-esp32-display-device-design.md`). BigTreeTech's K-Touch is the concrete product behind the "BTT interest" noted in that spec. Findings below are from hands-on probing of a physical unit plus BTT's published firmware/repos.

## Confirmed by probing the physical unit (esptool over USB-C/CH340)

| Item | Value |
|------|-------|
| MCU | ESP32-S3 (QFN56) rev v0.2, dual LX7 @ 240MHz |
| PSRAM | 8MB embedded (ESP32-S3R8, octal, AP_3v3) |
| Flash | 16MB GigaDevice (mfr `c8`, device `4018`), quad (eFuse: 4 data lines, 3.3V) |
| USB | Type-C via **CH340 UART bridge** (`/dev/ttyUSB0`; NOT native USB-CDC/JTAG) |
| MAC | d8:3b:da:98:8c:fc (this unit) |
| Serial flashing | Works with stock esptool auto-reset. **921600 baud corrupts reads on CH340; 460800 is reliable.** |

No physical buttons on the unit — the wiki's "emergency stop / back / home buttons" are on-screen UI, not hardware. Physical controls: power switch (Battery / DC 5V / OFF, appears to be a hard power cut) and the magnetic charging dock connector.

## Stock firmware & partition layout (from bigtreetech/K-Touch release binaries)

Release folders ship 4 files: `bootloader.bin`, `firmware.bin`, `partition.bin`, `product.img`. **Binary-only — no source, no schematics.** Firmware v1.1.0 app image: 2.25MB, ESP-IDF, project name `K-Touch_V1_1_0_Beta1.bin`, flash mode DOUT @ 80MHz.

Partition table (decoded from `K-Touch_v1.1.0_partition.bin`):

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data/nvs | 0x009000 | 20KB |
| otadata | data/otadata | 0x00e000 | 8KB |
| app0 | app/ota_0 | 0x010000 | 4608KB (4.5MB) |
| app1 | app/ota_1 | 0x490000 | 4608KB (4.5MB) |
| spiffs | data/spiffs | 0x910000 | 7040KB (`product.img` = asset FS) |
| coredump | data | 0xff0000 | 64KB |

Calibration point for the Phase 0 size gate: BTT fits a full Klipper touch UI in 2.25MB with 4.5MB A/B slots. We control our own partition table, so larger app slots (~6-7MB A/B with a smaller asset FS) are available if needed.

Firmware strings confirm:
- Display driven via **`esp_lcd_new_rgb_panel`** (RGB parallel — the exact interface Phase 1c's board layer assumed)
- Backlight via **LEDC PWM**
- OTA via built-in web UI (`esp_http_server`), SoftAP fallback provisioning ("BTT K TOUCH SETTINGS MANAGER")
- **Shared codebase with the BTT Panda Touch** (Panda Touch strings all over the K-Touch binary) — same hardware family

## Pin map (from bigtreetech/PandaTouch_PlatformIO — **VERIFIED on K-Touch 2026-07-13**)

BTT publishes an official open bring-up project for the Panda Touch: 800×480 RGB LCD + GT911 + LVGL. K-Touch shares the board family, and `firmware/ktouch-probe/` confirmed every pin below on a real K-Touch: clean color bars + full white border (timings good), backlight on, GT911 product ID `'911'` read at 0x5D, touch coordinates sane across the full panel. Boot heap: 371KB internal + 8.0MB PSRAM free.

| Signal | GPIO |
|--------|------|
| LCD PCLK | 5 |
| LCD DE | 38 |
| LCD R3–R7 | 6, 7, 8, 9, 10 |
| LCD G2–G7 | 11, 12, 13, 14, 15, 16 |
| LCD B3–B7 | 17, 18, 48, 47, 39 |
| LCD reset | 46 |
| Backlight (LEDC PWM) | 21 |
| Touch: GT911 I²C SCL / SDA | 1 / 2 |
| Touch: GT911 IRQ / RST | 40 / 41 |

RGB565 wiring (5-6-5 bits). Build flags from that repo: `qio_opi` memory type, 80MHz flash.

LCD timings (from `pt_board.h`, verified working on K-Touch): PCLK 14.8MHz active-low, HSYNC pulse/back/front 4/16/16, VSYNC 4/32/32, **DE-only mode** (HSYNC/VSYNC not wired, pins -1). LCD reset dance: GPIO46 low 100ms → high 10ms. Backlight: LEDC 30kHz, 11-bit resolution. GT911 at 400kHz I²C, address 0x5D (IRQ held low through reset), stock uses rotation 1.

## K-Touch-specific unknowns

- Battery gauge / charge-state signal — **NOT on the touch I²C bus** (probe scan found only the GT911 at 0x5D). Either an ADC pin, a second I²C bus, or not readable at all (dumb charger IC).
- USB-drive "Expansion Interface" wiring (likely the S3's native USB-OTG peripheral on GPIO 19/20, since the Type-C data lines go to the CH340)
- Magnetic dock pinout (power only, or data?)

## Stock firmware backup

Full 16MB flash dump taken before any modification: `~/Code/Printing/ktouch-recon/ktouch_stock_full_16MB.bin` (outside repo).
- sha256 `1360f658103180fa28166808ec3eabf701733229bd55aec882888e21238375a4`, 16,777,216 bytes
- Verified: partition table at 0x8000 is byte-identical to the v1.1.0 release `partition.bin`
- Installed firmware: factory `K-Touch_B1_0_0_Beta` in app0 (built on **ESP-IDF v5.1.1**); otadata marks app1 active (an OTA update)

Restore with:

```bash
esptool --port /dev/ttyUSB0 --baud 460800 write-flash 0x0 ktouch_stock_full_16MB.bin
```

Recon workspace (release bins, strings dump, esptool venv): `~/Code/Printing/ktouch-recon/`.

## Implications for the ESP32 display program

1. **Hardware matches the plan's assumptions**: S3 + 8MB octal PSRAM + 16MB flash + 800×480 RGB panel + GT911-class touch. The Phase 1c board-config table gets a `btt_ktouch` entry; the Phase 0 audit's [HW] steps can run on this unit directly.
2. **CH340 instead of native USB** means flashing at 460800 baud and no USB-JTAG debugging; log console is the CH340 UART.
3. **Battery exists** (20-30 min runtime): terminal firmware wants backlight dimming/sleep policy and ideally a battery indicator — new requirements not in the 1c plan.
4. PandaTouch_PlatformIO is the reference for board bring-up code (esp_lcd RGB + GT911), subject to on-device verification.
