# Fans

HelixScreen automatically discovers every fan your printer reports to Klipper and groups them by whether you can control them. You can adjust controllable fan speeds, watch automatic fans, and rename any fan so it is easier to recognize across the screens.

---

## Where to Find Fans

| Screen | Where | What it does |
|--------|-------|--------------|
| **Cooling** card | **Controls** panel | A **Part Fan** slider for quick changes, plus a short list of your other fans. Tap the card to open **All Fans**. |
| **All Fans** | Tap the **Cooling** card on Controls, or the fan widget on the Home screen, or the fans on the print status screen | Live speed control with a dial for each controllable fan. |
| **Fans** | **Settings > Devices > Fans** | A list of every fan, mainly for renaming. On some printers it also holds chamber air settings. |

---

## Fan Types

HelixScreen identifies each fan by its Klipper configuration and shows a short type label beside it in **Settings > Devices > Fans**:

| Type | Klipper config | Controllable? |
|------|----------------|---------------|
| **Part** | Part cooling fan (`[fan]`) | Yes |
| **Generic** | `[fan_generic]` | Yes |
| **Output Pin** | Creality-style `[output_pin]` fan | Yes, except one named as a hotend, extruder or heatbreak fan (the K2's hotend fan), which runs with the heater and is shown as automatic |
| **HE** | `[heater_fan]` (hotend cooling) | No, automatic |
| **Controller** | `[controller_fan]` (electronics cooling) | No, automatic |
| **Temp** | `[temperature_fan]` (thermostatic) | No, automatic |

**HE** (hotend) is the same short name the hotend fan's chip uses on the print screen.

**Controllable fans** are ones you can set a speed on directly. **Automatic fans** are managed by Klipper based on temperature or printer activity, so HelixScreen shows their current speed but does not let you change it.

---

## The All Fans Screen

![All Fans screen](../../images/user/controls-fan.png)

Open it by tapping the **Cooling** card on the **Controls** panel. Controllable fans come first, then automatic ones.

- **Controllable fans** appear as circular dials. Drag the dial, or tap along its ring, to set the speed from 0 to 100%. A switch under the dial turns the fan on or off and reads **On** or **Off**. The fan icon spins faster as the speed rises, and the new speed takes effect immediately.
- **Automatic fans** appear as read-only cards marked with a circled **A** and the word **Auto**. They show the current speed as a percentage, and the RPM as well when the fan reports a tachometer reading. You cannot adjust them, because Klipper sets their speed.

Speed changes you make show up everywhere else in HelixScreen immediately, even before the printer confirms the command.

### Chamber Air

On printers whose firmware groups the chamber fans into air modes, a **Chamber air** row appears at the top of the All Fans screen with up to three buttons:

| Button | What the printer does | Typical use |
|--------|-----------------------|-------------|
| **Recirculate** | Keeps chamber air inside the enclosure to hold heat | ABS, ASA and other warping-prone materials |
| **Exhaust** | Vents chamber air out of the printer to keep it cool | PLA, TPU |
| **Off** | Turns the chamber circulation fans off | |

Tap a button to switch modes. The printer sets the chamber fans for that mode, and you can still adjust each fan's dial afterwards. A button appears only when the printer offers that mode. On other printers the row is hidden.

Two printers have it today:

- **FlashForge Creator 5 Pro running Z-Mod.** The buttons run the printer's `AIR_CIRCULATION_INTERNAL`, `AIR_CIRCULATION_EXTERNAL` and `AIR_CIRCULATION_STOP` macros. The buttons do not stay highlighted, because the printer does not report which mode is active once its fans have been changed.
- **Snapmaker U1 with the Top Cover fitted.** The printer reports its active mode, so the matching button stays highlighted. The exhaust and recirculation fans appear as dials on the same screen, and HelixScreen keeps the fan that the active mode drives at 10% or above and refuses to turn on the fan that the mode keeps off. With the cover off, the row is hidden and nothing is offered, because the firmware ignores the commands.

#### Top Cover Notices and Targets (Snapmaker U1)

The first time HelixScreen sees the Top Cover fitted, it tells you "Top Cover detected" and offers **Open Fans**. Afterwards it asks "Recalibrate input shaping?", recommending that you recalibrate after connecting the cover, with **Calibrate now** or **Later**. A detection that happens during a print waits until the print ends. If the cover is taken off later, a warning says it was disconnected and asks you to reconnect it before continuing, to protect print quality. A cover that is already off when HelixScreen starts gives no warning.

**Settings > Devices > Fans** has a **CHAMBER AIR** group at the top on this printer. It sets what the **Exhaust** and **Recirculate** buttons send:

| Setting | Range |
|---------|-------|
| **Exhaust: keep chamber at or below** | 20 to 80°C |
| **Exhaust fan speed** | 10 to 100% |
| **Recirculation fan speed** | 10 to 100% |

---

## The Fans Settings Screen

![Fan Settings](../../images/user/settings-fans.png)

**Settings > Devices > Fans** lists every fan HelixScreen found, in two groups, **CONTROLLABLE FANS** and **AUTO FANS**, each with a count. Each row shows the fan's name, its Klipper name, its type label and its current speed. If your printer has no fans, the screen says "No fans detected".

### Renaming a Fan

Klipper fan names like `fan_generic exhaust_fan` are not always friendly. You can give any fan a custom display name.

**From the Fans settings screen:**

1. Tap a fan row in the list.
2. Type a new name in the **Rename Fan** window.
3. Tap **Rename**.

**From the All Fans screen:** long-press a fan dial or status card to open the same window. Dragging a dial never counts as a long press.

The custom name is saved and appears everywhere that fan is shown in HelixScreen.

---

**Prev:** [Settings](settings.md) | **Next:** [Sensors](sensors.md) | [Back to User Guide](../USER_GUIDE.md)
