# Settings: System

The System section covers network configuration, hardware management, privacy settings, and maintenance actions.

---

## Network Settings

> Hidden on Android (the OS manages networking).

Tap to open the Network Settings overlay with a two-column layout:

**Left column — Status:**
- **WiFi** — Toggle on/off, view connection status (SSID, IP address, MAC address, signal strength). Shows a 2.4GHz indicator if your hardware only supports that band.
- **Ethernet** — View connection status (IP address, MAC address) — read-only, no toggle.
- **Test Network** — Verify internet connectivity. Disabled when no network is connected.

**Right column — Available Networks:**
- Scans and lists available WiFi networks with signal strength indicators
- Tap a network to connect (enter password if needed)
- **Add Hidden Network** — Connect to a network that doesn't broadcast its SSID
- **Refresh** button to re-scan (shows a spinner while scanning)

---

## Power Devices

> Only shown when Moonraker power devices are configured.

Tap to open the Power Devices overlay, where you can toggle individual power relays and smart outlets on or off. This is the same panel accessible from **Advanced > Power Devices** or by long-pressing the home panel power button.

See [Power Device Control](../advanced.md#power-device-control) for full details on device selection and the home panel quick-toggle.

---

## Host

Shows the current Moonraker host address (e.g., `localhost:7125`). Tap to open the **Change Host** dialog where you can enter a new IP address and port to connect to a different printer.

After changing the host, HelixScreen disconnects from the current printer and reconnects to the new one.

---

## Printers

> Only shown when [beta features](../beta-features.md) are enabled.

Manage all your configured printers. Tap to open the Printer Management overlay where you can:

- **Switch printers** — Tap any printer in the list to switch to it. HelixScreen disconnects from the current printer and connects to the new one.
- **Add a printer** — Tap "Add Printer" to launch the Setup Wizard for a new printer. You can cancel at any time to return to your current printer.
- **Delete a printer** — Tap the trash icon next to any non-active printer and confirm. You cannot delete the last remaining printer.

After switching, a toast notification confirms the new connection and you're taken to the Home panel.

---

## Touch Calibration

> Only shown on touchscreen displays that need calibration.

Recalibrate if taps register in the wrong location:

1. Tap **Touch Calibration** in Settings
2. Tap each crosshair target as it appears on screen (3 points)
3. Test that taps land correctly in the verify area
4. Calibration saves automatically

The row description shows "Calibrated" or "Not calibrated" status.

If this option doesn't appear in your Settings, your screen type doesn't normally need calibration. If you still need to force it, see the [Touch Calibration Guide](../touch-calibration.md) for alternative methods including CLI flags and config file options.

---

## Hardware Health

> Only shown when hardware issues are detected.

Tap to open the Hardware Health overlay, which lists detected issues:

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

## Plugins

> Only shown when beta features are enabled (tap Current Version 7 times in About).

View installed plugins and their status. Plugins extend HelixScreen with additional capabilities like custom LED effects, overlays, and integrations.

---

## Share Usage Data (Telemetry)

Toggle anonymous usage telemetry that helps improve HelixScreen. Data collection is completely anonymous — no personal information, printer names, or file names are ever sent.

When enabled, a **View Telemetry Data** row appears below the toggle. Tap it to see exactly what data will be sent. See the [Telemetry & Privacy](../../TELEMETRY.md) documentation for full details on what is and isn't collected.

---

## Restart HelixScreen

Restart the display application. Useful after changing settings that require a restart (like theme changes) or if the UI becomes unresponsive. Shows a brief "Restarting..." toast before the app restarts.

---

## Factory Reset

Clears **all** HelixScreen settings and restarts the Setup Wizard. This resets:

- All appearance, display, and sound settings
- LED configuration
- Printer connection details
- Sensor roles and hardware expectations
- All other preferences

**Does not affect** your Klipper configuration, Moonraker, or any files on the printer itself.

---

## Install Update

> Only shown when an update is available.

Download and install the available HelixScreen update. Shows a confirmation dialog with the version number, then displays download progress with a cancel option. After download completes, prompts to restart.

---

[Back to Settings](../settings.md) | [Prev: Motion](motion.md) | [Next: Help & About](help-about.md)
