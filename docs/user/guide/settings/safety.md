# Settings: Safety & Notifications

The Safety & Notifications category covers emergency controls and print alert preferences.

---

## E-Stop Confirmation

| State | Behavior |
|-------|----------|
| **Off** (default) | Tapping E-Stop fires immediately — fastest emergency response |
| **On** | Shows a confirmation dialog requiring a tap-and-hold before triggering |

Enable this if you find yourself accidentally hitting the E-Stop button. Disable it if you need the fastest possible emergency response.

---

## Cancel Escalation

When a print cancel is sent, some printers take a long time to finish their cancel routine (parking tools, cooling down, running CANCEL_PRINT macros). Cancel Escalation adds a safety net: if the cancel doesn't complete within a timeout, HelixScreen automatically escalates to an emergency stop (M112).

| Setting | Options |
|---------|---------|
| **Cancel Escalation** | On/Off toggle. **Off by default.** |
| **Escalation Timeout** | 15, 30, 60, or 120 seconds. Only shown when escalation is enabled. Default: 30 seconds. |

**When to leave this off:**
- Toolchangers that need to park tools during cancel
- Printers with long CANCEL_PRINT macros
- Any printer where the cancel routine is expected to take more than a few seconds

**When to turn this on:**
- Simple printers where cancel should complete quickly
- If you've experienced "stuck" cancels where the printer never returns to idle

---

## Confirm before running macros

| State | Behavior |
|-------|----------|
| **Off** (default) | Tapping a macro button runs it immediately |
| **On** | Shows a confirmation dialog before running any macro |

Enable this if you have macros that move the toolhead, heat the printer, or perform other actions you don't want triggered by an accidental tap.

---

## Allow cold load/unload

| State | Behavior |
|-------|----------|
| **Off** (default) | Filament load/unload is blocked when the nozzle is below the minimum extrude temperature |
| **On** | Allows load/unload to run even when the nozzle is cold |

By default, HelixScreen won't run a filament load or unload while the nozzle is too cold to extrude, matching Klipper's cold-extrude safety check. Turn this on if your load/unload macros heat the nozzle themselves before extruding, so the operation isn't blocked before your macro gets a chance to warm up.

---

## Cool nozzle after filament ops

| State | Behavior |
|-------|----------|
| **On** (default) | The extruder heater is turned off a couple of minutes after a load or unload finishes |
| **Off** | The nozzle stays at whatever temperature the operation left it |

A filament change heats the nozzle to material temperature. Left alone, it would sit there indefinitely — burning power and slowly cooking the filament in the melt zone. So HelixScreen turns the heater off once you're done. The delay (two minutes by default) is there so you can run several loads and unloads back to back without the nozzle cooling between them; each new operation restarts the clock. Nothing happens while a print is running — an active job manages its own heat.

**Turn this off if your filament system already does it.** [AFC](../filament.md) has its own post-operation cooldown, and other multi-material firmware is adding the same. Two independent timers driving one heater is confusing at best. Leave whichever one you prefer in charge, and switch the other off.

The setting is per printer, so an AFC machine can opt out while your other printers keep the built-in behavior. To change the two-minute delay, see [`cooldown_delay_seconds`](../../CONFIGURATION.md#cooldown_delay_seconds).

---

## Print Completion Alert

Controls how HelixScreen notifies you when a print finishes, is cancelled, or fails — when you're not already on the print status screen.

| Mode | Behavior |
|------|----------|
| **Off** | No visual notification (sound still plays if enabled) |
| **Notification** | Brief toast message at the top of the screen |
| **Alert** (default) | Full-screen modal showing print stats — duration, layers, filament used — with confetti for successful prints |

To change: **Settings > Safety & Notifications > Print Completion Alert** dropdown.

> **Note:** Print errors always show the full alert modal regardless of this setting, since errors need immediate visibility. If you're already on the print status screen when a print ends, no notification is shown (the panel itself shows the result).

Sound always plays for terminal print states (complete, cancelled, error) regardless of alert mode, as long as the master Sounds toggle is on.

---

## On-screen Alerts

Toasts are the brief banners that slide in at the top of the screen — "Filament loaded", "Saved", "Update available". If the informational ones feel chatty, **Settings > Safety & Notifications > On-screen Alerts** sets the lowest level that is allowed to interrupt you:

| Level | What still toasts |
|-------|-------------------|
| **All** (default) | Everything — info, success, warnings and errors |
| **Warnings & errors** | Info and success messages are held back |
| **Errors only** | Only error toasts appear |

Held-back notifications are not lost — they still land in the notification history (open it from the Notifications widget on the Home panel). The filter never silences a full-screen error dialog, and it never silences the [Print Completion Alert](#print-completion-alert) above; both bypass it on purpose.

---

[Back to Settings](../settings.md) | [Prev: Hardware & Devices](hardware.md) | [Next: System](system.md)
