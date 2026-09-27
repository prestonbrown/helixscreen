# Settings: System

The System category covers security, network configuration, privacy settings, and maintenance actions.

---

## Security

Set up a screen lock with a PIN code to prevent unauthorized access to your printer controls. Tap to open the Security overlay.

**When no PIN is set:**

- **Set PIN** — Create a 4–6 digit numeric PIN. You'll be asked to enter it twice to confirm.

**When a PIN is set:**

- **Change PIN** — Update your PIN. You must enter the current PIN first, then enter and confirm the new one.
- **Remove PIN** — Disable the PIN entirely. Requires entering the current PIN for confirmation.
- **Auto-lock** — Toggle automatic screen locking. When enabled, the screen locks after the display sleep timeout. You'll need to enter your PIN to unlock.

When the screen is locked, a full-screen lock overlay appears with a numeric keypad. Enter your PIN and tap the checkmark to unlock. If you enter the wrong PIN, an error message appears briefly. An **Emergency Stop** button remains accessible in the top-right corner of the lock screen while a print is running, so you can always halt the printer in an emergency without unlocking.

The PIN is stored securely as a one-way hash in your settings — the actual digits are never saved in plain text. A factory reset clears all security settings.

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

## Host

Shows the current Moonraker host address (e.g., `localhost:7125`). Tap to open the **Change Host** dialog where you can enter a new IP address and port to connect to a different printer.

After changing the host, HelixScreen disconnects from the current printer and reconnects to the new one.

---

## Touch & Input

Opens a sub-page that groups all touch-related settings: calibration, debug visualization, jitter filtering, and scroll feel. See [Touch & Input](touch-input.md) for the full reference.

---

## Plugins

> Only shown when beta features are enabled (tap Current Version 7 times in About).

View installed plugins and their status. Plugins extend HelixScreen with additional capabilities like custom LED effects, overlays, and integrations.

---

## Performance

> Only shown when performance data is available.

The row description shows a live summary of host load. Tap to open the Performance overlay, which shows real-time host CPU and memory usage along with per-MCU load for each connected controller board. Useful for spotting an overloaded host or a struggling MCU when prints stutter or the UI feels sluggish.

---

## Share Usage Data (Telemetry)

Toggle anonymous usage telemetry that helps improve HelixScreen. Data collection is completely anonymous — no personal information, printer names, or file names are ever sent.

When enabled, a **View Telemetry Data** row appears below the toggle. Tap it to see exactly what data will be sent. See the [Telemetry & Privacy](../../TELEMETRY.md) documentation for full details on what is and isn't collected.

---

## Log Level

Control how much detail HelixScreen writes to its logs. This is useful when troubleshooting issues or gathering diagnostic information for a bug report.

| Level | What it captures |
|-------|-----------------|
| **Warn** | Errors and warnings only (quiet) |
| **Info** | Connection events, panel changes, milestones (default) |
| **Debug** | State changes, API calls, component init (use this for bug reports) |
| **Trace** | Everything including LVGL internals (very verbose, rarely needed) |

Changes take effect immediately, with no restart required. Set to **Debug** before reproducing a problem, then set back to **Info** when done.

> **Tip:** Debug and Trace levels increase CPU usage and log volume. Don't leave them enabled long-term.

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

[Back to Settings](../settings.md) | [Prev: Safety & Notifications](safety.md) | [Next: Help & About](help-about.md)
