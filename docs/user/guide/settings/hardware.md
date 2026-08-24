# Settings: Hardware & Devices

The Hardware & Devices category contains settings for printers, peripherals, and external services connected to your printer. Items in this section appear or hide dynamically based on what HelixScreen detects from Klipper and Moonraker.

---

## Hardware Health

Tap to open the Hardware Health overlay, which lists detected hardware validation issues:

| Category | Meaning |
|----------|---------|
| **Critical** | Required hardware missing (e.g., no extruder heater) |
| **Warning** | Expected hardware not found (e.g., bed sensor disappeared) |
| **Info** | Newly discovered hardware that wasn't seen before |
| **Session** | Hardware changed since last session |

**Actions for non-critical issues:**

- **Ignore** — Mark as optional (won't warn again even if missing)
- **Save** — Add to expected list (will warn if it disappears later)

Use this when adding or removing hardware to keep HelixScreen's expectations accurate.

---

## Printers

> Only shown when [beta features](../beta-features.md) are enabled.

Manage all your configured printers. Tap to open the Printer Management overlay where you can:

- **Switch printers** — Tap any printer in the list to switch to it. HelixScreen disconnects from the current printer and connects to the new one.
- **Add a printer** — Tap "Add Printer" to launch the Setup Wizard for a new printer. You can cancel at any time to return to your current printer.
- **Delete a printer** — Tap the trash icon next to any non-active printer and confirm. You cannot delete the last remaining printer.

After switching, a toast notification confirms the new connection and you're taken to the Home panel.

---

## Camera

> Only shown when a webcam is detected (an enabled webcam is configured in Moonraker).

Tap to open a standalone fullscreen camera viewer showing the live feed from your printer's webcam.

---

## Multi-Filament System Management

> Only shown when a multi-filament system is detected.

Tap to open Device Operations for quick actions, calibration, and speed settings for multi-material systems. Supports AFC, Happy Hare, ACE, and other detected filament systems.

Most of what's inside varies by hardware, but four toggles appear here regardless of which system you have:

| Toggle | When it appears | What it does |
|--------|-----------------|--------------|
| **Unloads After Print** | AFC systems only | Retract filament back to its lane when a print finishes |
| **Keep Spool Info on Eject** | Systems whose firmware tracks spool ids per lane (AFC, Happy Hare) | Remember lane spool details across an eject, so reloading the same spool after maintenance needs no re-selection (on by default). Applies only to spools selected in HelixScreen; spools assigned elsewhere clear with the lane (for those, use the firmware's own retention, e.g. AFC's `remember_spool`). When that firmware retention covers every lane, it takes precedence and the toggle shows as disabled |
| **Always Show Bypass Spool** | AFC systems only | Keep the external spool visible on the filament path even while bypass is disengaged. AFC reports a bypass sensor whether or not one is wired, so it's hidden by default until bypass is actually engaged |
| **Enable Bypass Controls** | Only when your firmware reports **no** bypass | Show the bypass controls and the external spool anyway, for machines where you feed filament straight to the extruder. Applies to Anycubic ACE Pro, Snapmaker U1, tool changers, QIDI Box, and any Happy Hare config with `has_bypass: 0` |

What **Enable Bypass Controls** does depends on the system. On Happy Hare, `MMU_SELECT_BYPASS` works whether or not `[mmu_machine] has_bypass` is set, so the bypass becomes usable - relevant for `mmu_vendor: Other` setups such as a QIDI Box driven through Happy Hare, and for an uncalibrated type-A selector. On the Creality CFS the bypass works and is always shown, so this setting never appears there. On ACE, Snapmaker, tool changers, and QIDI Box there is no bypass command, so the Bypass toggle reports that the operation is not supported; there the setting only lets you record the material and color you loaded by hand, which keeps filament tracking and temperature presets correct.

See [Filament → When Bypass Doesn't Appear](../filament.md#when-bypass-doesnt-appear).

---

## Fans

> Only shown when fans are detected.

Tap to open the Fan Settings overlay. This shows all detected fans and their current speeds. You can rename any fan for easier identification — for example, rename "fan_generic exhaust_fan" to "Exhaust". Custom names appear everywhere fans are shown in the UI.

---

## Sensors

> Only shown when sensors are detected.

Tap to open the Sensor Settings overlay. Each detected filament sensor can be assigned a role:

| Role | Behavior |
|------|----------|
| **None** | Sensor present but not monitored |
| **Runout** | Pauses the print when filament runs out |
| **Toolhead** | Monitors filament at the toolhead |
| **Entry** | Monitors filament at the entry to the extruder path |

Whether a sensor is a switch or a motion sensor is detected automatically — it is not something you choose. Other detected sensors (accelerometers, probes, humidity, width, color) are listed as read-only information.

Role assignments control what HelixScreen *watches*. One thing switches at the printer itself: when you engage bypass on a filament system, HelixScreen turns the toolhead runout sensor on at the printer if the filament system's own software had left it off (common on Creality printers), and turns it back off when you disengage — so a bypass print is still protected against running out. This is automatic and doesn't change your settings here.

See [Sensors](../sensors.md) for the full guide.

---

## LED Settings

> Only shown when LED hardware is detected.

Tap to open the full LED configuration overlay. This is a large topic with its own page — see [LED Settings](led-settings.md).

---

## Power Devices

> Only shown when Moonraker power devices are configured.

Tap to open the Power Devices overlay, where you can toggle individual power relays and smart outlets on or off. This is the same panel accessible from **Advanced > Power Devices** or by long-pressing the home panel power button.

See [Power Device Control](../advanced.md#power-device-control) for full details on device selection and the home panel quick-toggle.

---

## Spoolman

> Only shown when Spoolman is configured in Moonraker.

Tap to open Spoolman integration settings. HelixScreen connects to your Spoolman server for spool tracking, weight sync, and barcode scanning.

> For the bigger picture — how filament tracking works with and without Spoolman, and how remaining weight is kept current — see [Filament Tracking & Spoolman](../filament-tracking.md).

### Server Setup

If Spoolman is not yet configured, you'll see a setup screen. Enter the IP address and port of your Spoolman server, then tap **Connect**. HelixScreen verifies the connection and configures Moonraker automatically — no manual editing of `moonraker.conf` needed.

### Server Status

When connected, the settings screen shows your Spoolman server URL along with options to **Change** the server address or **Remove** the configuration entirely.

### Sync with Spoolman

Toggle this on to enable automatic weight polling. When enabled, HelixScreen periodically queries Spoolman for spool weight updates and displays the remaining filament on the home panel and filament panel.

### Refresh Interval

Controls how often HelixScreen polls Spoolman for weight updates. Options: **30 seconds**, **1 minute**, **2 minutes**, or **5 minutes**. Shorter intervals give more up-to-date readings but generate more network traffic.

### Barcode Scanner

Configure which USB device to use as a barcode scanner for scanning Spoolman QR codes on spool labels. By default, HelixScreen auto-detects scanners by looking for devices with "barcode" or "scanner" in their name.

If your scanner uses a generic name (e.g., "TMS HIDKeyBoard"), tap this setting to manually select it from a list of connected USB HID devices. The selection is saved and persists across restarts.

### Label Printer

Opens label printer configuration for printing spool labels with QR codes. See [Label Printing](../label-printing.md) for full setup instructions and supported printers.

---

[Back to Settings](../settings.md) | [Prev: Printing](printing.md) | [Next: Safety & Notifications](safety.md)
