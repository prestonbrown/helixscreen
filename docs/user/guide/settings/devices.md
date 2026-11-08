# Settings: Devices

**Settings > Devices** is where you look after the hardware and services around your printer: its camera, fans, sensors, lights, power switches and Spoolman. Rows only appear for things HelixScreen finds in your Klipper and Moonraker setup, so your list may be shorter than the one pictured.

On the Settings screen, the **Devices** row shows a one-line health check: *All healthy*, *Needs attention* (Hardware Health lists something worth a look) or *Problem found* (hardware the printer needs is missing).

![Devices settings, top of the page](../../../images/user/settings-devices.png)

![Devices settings, scrolled to the bottom](../../../images/user/settings-devices-2.png)

---

## Hardware Health

Compares the hardware Klipper reports with what HelixScreen expects, and lists anything that doesn't match. The row itself shows the result: **No Hardware Issues**, or a count such as **3 Hardware Issues**. Its icon turns amber when something needs attention and red when something is critical.

Tap it to see the list:

![Hardware Health](../../../images/user/settings-hardware-health.png)

| Kind | Meaning |
|------|---------|
| **Critical** | Hardware the printer needs is missing, such as the nozzle heater |
| **Warning** | Hardware that used to be there is gone, such as a bed sensor |
| **Info** | New hardware HelixScreen hasn't seen before |
| **Session** | Hardware changed since HelixScreen last ran |

For each item you can:

- **Ignore**: mark it as optional. HelixScreen won't warn about it again, even if it goes missing. This works on critical items too, so a machine built without a nozzle heater can stop the warning about it.
- **Save** (new hardware only): add it to the expected list. HelixScreen will warn you if it disappears later.

A Klipper machine that isn't a printer, one whose `[printer]` section sets `kinematics: none`, is never warned about a missing nozzle heater.

Check this page after you add or remove hardware, so HelixScreen's list matches your printer.

---

## Camera

> Only shown when a webcam is set up in Moonraker. Not available on the ESP32 screen.

Opens your printer's camera full screen. See [Camera](../camera.md) for rotation, stream status and the home screen camera widget.

### Print photos

> Only shown when a webcam is set up in Moonraker.

When this switch is on, HelixScreen saves two pictures from your camera for every print: one once the second layer is done, so you can check the first layers went down well, and one when the print ends (finished, canceled or failed). They appear on the print's page in [Print History](../print-history.md#print-details--reprinting). It is on by default.

With the [HelixPrint plugin](../helixprint-plugin.md) installed, the printer takes the photos itself, for every print, whichever app started it, and whether or not a screen is connected. Without the plugin, the screen takes them while it is connected to the printer, so a print that runs while this screen is off or away gets no photos. On a screen that is not attached to the printer (a tablet, a desktop), the row says so and offers to install the plugin.

The photos are kept on the printer. They are removed when you delete the print from the history, and the oldest go once there are photos of 500 prints.

The K-Touch does not show the photos and cannot take them itself; with the plugin installed, the printer still takes them, and other screens show them.

---

## Fans

> Only shown when HelixScreen finds fans.

Lists every fan and its current speed. Tap a fan to rename it, for example from "fan_generic exhaust_fan" to "Exhaust". The new name is used everywhere fans appear. See [Fans](../fans.md).

---

## Sensors

> Only shown when HelixScreen finds sensors.

Lists your printer's sensors and lets you give each filament sensor a job:

| Role | What HelixScreen does with it |
|------|------------------------------|
| **None** | Nothing. The sensor is there but not watched |
| **Runout** | Pauses the print when the filament runs out |
| **Toolhead** | Watches for filament at the toolhead |
| **Entry** | Watches for filament where it enters the extruder path |

Door switches and other on/off inputs are listed under **Inputs**, where you mark each as **Door** or **Other** and turn the door-open warning on or off; see [Inputs (Doors and Buttons)](../sensors.md#inputs-doors-and-buttons).

HelixScreen tells switch sensors and motion sensors apart on its own; you don't choose. Other sensors (accelerometers, probes, humidity, filament width, color) are listed for information only.

The roles only decide what HelixScreen watches, with one exception. When you turn on bypass on a filament system, HelixScreen turns the toolhead runout sensor on at the printer if the filament system's software had left it off (common on Creality printers), and turns it off again when you leave bypass. That way a bypass print is still protected from running out. It happens automatically and doesn't change your settings here.

See [Sensors](../sensors.md) for the full guide.

---

## LED Settings

> Only shown when HelixScreen finds LEDs or lights.

Chooses which lights HelixScreen controls, what they do on their own during printing, and any macro-driven lights. See [LED Settings](led-settings.md) for the full guide.

---

## Power Devices

> Only shown when power devices are set up in Moonraker.

Turns individual relays and smart plugs on or off. It's the same screen as **Advanced > Power Devices**, or a long-press on the power button on the home screen. See [Power Device Control](../advanced.md#power-device-control).

---

## Spoolman

Connects HelixScreen to your [Spoolman](https://github.com/Donkie/Spoolman) server for spool tracking, weight updates, label printing and barcode scanning. For the bigger picture, see [Filament Tracking & Spoolman](../filament-tracking.md).

### Server Setup

If Spoolman isn't set up yet, this is what you see. Enter your Spoolman server's IP address (or host name) and port (7912 by default), then tap **Connect**. HelixScreen checks the connection and sets up Moonraker for you. You don't need to edit `moonraker.conf`.

### Server Status

Once connected, the page shows your Spoolman server and two buttons: **Change** to point at a different server, and **Remove** to disconnect it.

### Sync with Spoolman

Turn this on to keep spool weights up to date. HelixScreen asks Spoolman for new weights regularly and shows the filament left on the Home and Filament screens. Weights are also updated when a print starts, pauses or finishes.

### Refresh Interval

How often HelixScreen asks Spoolman for new weights: **30 seconds**, **1 minute**, **2 minutes** or **5 minutes**. Shorter is more up to date but uses more network traffic.

### Switch spools at each tool change

Spoolman takes filament off one spool at a time: whichever spool is active. On a printer with several tools or lanes, every tool change has to switch the active spool, or one spool gets charged for filament that came from another. Some printers do this themselves (AFC, Happy Hare with its Spoolman support on, a Snapmaker U1 running the extended firmware's SpoolLink, and printers whose tool-change macros set the spool); for those this row does nothing.

Turn it on to have the active spool follow the tool or lane that is printing, using the spools you linked to each tool or lane. With the [HelixPrint plugin](../helixprint-plugin.md#switch-spools-at-each-tool-change) installed, the plugin does it for every print, whichever app started it, and records which spools each print used; the print's details in [Print History](../print-history.md#print-details--reprinting) list them. Without the plugin, HelixScreen switches the spool itself, but only while it is connected and only for tools that print from their own extruder; on a remote screen, the row says so. Off by default. Not yet supported for Creality CFS and Anycubic ACE lane changes: there the active spool follows the loaded lane only while HelixScreen is connected, as before.

### Label Printer

Sets up a printer for spool labels with QR codes. See [Label Printing](../label-printing.md) for setup and supported printers. Not available on the ESP32 screen.

### Barcode Scanner

Chooses the USB or Bluetooth scanner used to scan the QR codes on your spool labels. HelixScreen finds scanners with "barcode" or "scanner" in their name on its own. If yours has a generic name (such as "TMS HIDKeyBoard"), pick it from the list here. Your choice is kept after a restart. See [Barcode Scanner](../barcode-scanner.md). Not available on the ESP32 screen.

---

[Back to Settings](../settings.md) | [Prev: Printing](printing.md) | [Next: Safety & Alerts](safety.md)
