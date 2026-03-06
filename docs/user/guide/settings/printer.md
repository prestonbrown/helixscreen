# Settings: Printer

The Printer section contains settings for hardware connected to your printer. Items in this section appear or hide dynamically based on what HelixScreen detects from Klipper and Moonraker.

---

## Sensors

> Only shown when sensors are detected.

Tap to open the Sensor Settings overlay. Manage all detected filament sensors by choosing a role for each:

| Role | Behavior |
|------|----------|
| **Runout** | Pauses print when filament runs out |
| **Motion** | Detects filament movement (clog detection) |
| **Ignore** | Sensor present but not monitored |

Other detected sensors (accelerometers, probes, humidity, width, color) are listed as read-only information.

---

## AMS Management

> Only shown when an AMS (Automatic Material System) is detected.

Tap to open Device Operations for quick actions, calibration, and speed settings for multi-material systems. Supports AFC, Happy Hare, ValgACE, and other Klipper-based filament changers.

---

## Spoolman

> Only shown when Spoolman is configured in Moonraker.

Tap to open weight sync settings for Spoolman spool tracking integration.

---

## LED Settings

> Only shown when LED hardware is detected.

Tap to open the full LED configuration overlay. This is a large topic with its own page — see [LED Settings](led-settings.md).

---

## Retraction Settings

> Only shown when firmware retraction (`[firmware_retraction]`) is configured in Klipper.

Tap to open the retraction overlay. Configure G10/G11 firmware retraction parameters. **Changes apply immediately** and can be adjusted mid-print for tuning.

| Parameter | Range | Description |
|-----------|-------|-------------|
| **Enable Retraction** | On/Off | Master toggle for firmware retraction |
| **Retract Length** | 0–6 mm | Amount of filament to retract. 0.4–2 mm for direct drive, 4–6 mm for bowden. |
| **Retract Speed** | 10–80 mm/s | Speed of the retraction movement |
| **Unretract Extra** | 0–1 mm | Extra filament to prime after retraction to compensate for ooze |
| **Unretract Speed** | 10–60 mm/s | Speed of the prime (unretract) movement |

---

## Macro Buttons

Tap to open the Macro Buttons overlay. Configure quick-action buttons that appear on the Controls panel and standard macros:

- **Cooldown** — G-code command for the cooldown button
- **Load / Unload Filament** — Label and G-code for filament operations
- **Custom Macros** — Up to two custom macro buttons with configurable labels and G-code

---

[Back to Settings](../settings.md) | [Prev: Appearance](appearance.md) | [Next: Notifications](notifications.md)
