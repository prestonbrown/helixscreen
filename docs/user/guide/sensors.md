# Sensors

HelixScreen lists the sensors your printer reports to Klipper and groups them by kind. Filament switch and motion sensors, door and button inputs, and filament width sensors are things you configure: each gets a role, and filament and width sensors also get an **Enabled** switch. The other kinds are shown for information only.

Open the page from **Settings > Devices > Sensors**. A section appears only when the printer has at least one sensor of that kind, and the screen says **No sensors detected** when none are listed.

![Sensor Settings](../../images/user/settings-sensors.png)

Each section header shows how many sensors it lists. Changes to a role or a switch save immediately.

---

## Switch Sensors (Filament Runout & Motion)

**Switch Sensors** lists standalone filament sensors: switch-style runout sensors and motion (encoder) sensors. The row shows the sensor's kind (switch or motion), which HelixScreen detects itself. Sensors that belong to a multi-filament system (AMS, CFS, AFC and the like) are left out and managed by that system.

**Enable Monitoring** at the top of the section turns filament sensor monitoring on or off for all of these sensors at once.

### Roles

Each row has a **Role** dropdown:

| Role | What it is for |
|------|----------------|
| **None** | Not assigned, so not monitored |
| **Runout** | The sensor that watches for filament running out during a print |
| **Toolhead** | A sensor at the toolhead. On the Filament panel, Load warns if it already sees filament, and Unload warns if it sees none |
| **Entry** | A sensor where filament enters the printer. You can point the Home panel's Filament Sensor tile at it |

The **Enabled** switch on a row appears once the sensor has a role other than **None**. Only one sensor holds each role, so giving a role to a second sensor takes it from the first. The exception is **Runout**, which can be held once per lane or toolhead on printers where each has its own sensor, such as the Snapmaker U1.

A sensor whose name contains "runout" starts out with the **Runout** role if no other sensor has it. Everything else starts as **None**.

### What happens on a runout

The printer's firmware pauses the print when a sensor reports no filament. HelixScreen then shows a dialog that guides you through loading new filament and resuming. On a multi-filament system, the notice that a runout sensor has gone clear waits about 45 seconds, because a normal tool change also clears it for a while. On the Home panel, the **Filament Sensor** tile shows the sensor's state and opens the same load, unload and purge actions when tapped. Edit mode picks which sensor it follows. See [Home Panel](home-panel.md).

On a Creality K2 with no CFS attached, the toolhead filament sensor is listed here and can watch for runout like any other.

---

## Inputs (Doors and Buttons)

**Inputs** lists the on/off inputs your printer reports to Klipper as buttons: door and lid switches, front-panel buttons and, on tool-changing printers, the sensors that say whether each tool sits in its dock or on the carriage. The section appears only when the printer has at least one.

Each row shows the input's name and its live state. **--** means the printer has not reported it yet.

| Role | States shown |
|------|--------------|
| **Door** | **Opened** (in orange) or **Closed** |
| **Other** | **Pressed** or **Released** |

### Marking an input as a door

An input whose name contains "door", "lid" or "cover" starts as **Door**; everything else starts as **Other**. To change it:

1. Open **Settings > Devices > Sensors** and scroll to **Inputs**.
2. Tap the **Role** dropdown on the input's row.
3. Pick **Door** or **Other**.

The choice is remembered for this printer. A printer can have several doors, for example a front door and a top cover.

### If a closed door shows Opened

Door switches are wired either way round. If a door reads **Opened** while it is shut, turn on **Invert** on that row. Every input row has an **Invert** switch.

### Door-open warning

**Warn if a door is open**, at the top of the section, controls the door check. Under it the screen notes it applies "for materials that need a warm enclosure, like ABS or ASA".

- Before a print starts, HelixScreen warns you if any door reads open and the print uses a material that needs a warm enclosure. See [Door-open warning](printing.md#door-open-warning).
- When a door opens during a print of such a material, a short warning message appears. PLA and PETG prints, and prints whose materials HelixScreen cannot tell, never show it. If the printer's own configuration already reacts when that door opens (a door button that runs its own G-code), HelixScreen leaves the announcement to the printer and does not add a second one.

The check is on by default when the printer has at least one door input and off when it has none. Turn it off here if you print with the door open on purpose.

---

## Width Sensors

If your printer has a filament width sensor, it appears under **Width Sensors**. These sensors sit in the filament path and measure its diameter as it passes. HelixScreen recognizes two kinds, shown on the row:

| Kind | How it measures |
|------|-----------------|
| **TSL1401CL** | An optical line-scan sensor that reads the filament's shadow |
| **Hall** | A magnetic sensor that reads the filament's thickness |

Each row has a **Role** dropdown (**None** or **Flow Compensation**) and an **Enabled** switch. Only one sensor can hold **Flow Compensation**. The first width sensor found starts with that role.

The sensor with the **Flow Compensation** role, when enabled, supplies the live diameter (for example 1.75 mm) shown on the **Width Sensor** tile you can add to the Home panel. With no such sensor, the tile has nothing to show.

HelixScreen only displays the reading. Whether Klipper adjusts extrusion for the measured width is set in your printer's own configuration and G-code.

---

## Clog and Flow Detection

Printers with a Happy Hare or AFC system, or another MMU that reports flow, can show a live clog and flow meter. It lives on the Home panel's **Clog Detection** tile and in the filament system screen, not on this page. See [Clog Detection Widget](home-widgets-filament.md#clog-detection-widget) for what it reads and how to set its source, detection mode, detection length and danger zone. The meter only displays; the pause on a real clog comes from the printer's firmware.

---

## Read-Only Sensor Types

These sections list sensors for information. Probes, humidity sensors, accelerometers, load cells and temperature sensors have no setting you can change here, apart from the chamber pickers below.

| Section | What it covers | Kinds shown |
|---------|----------------|-------------|
| **Probe Sensors** | Z probes for bed leveling and meshes | Probe, BLTouch, Smart Effector, Eddy Current, Cartographer, Beacon, Voron Tap, Klicky, Creality ProTouch, Load Cell |
| **Humidity Sensors** | Chamber and dryer humidity | BME280, HTU21D, SHT3X, AHT10, AHT20, AHT20-F, AHT3x, temperature_oams |
| **Accelerometers** | Input shaper sensors | ADXL345, LIS2DW, LIS3DH, MPU9250, ICM20948 |
| **Load Cells** | Force and weight sensors | None |
| **Temperature Sensors** | MCU, host, driver and other temperature objects, including temperature fans and generic heaters such as a filament dryer | None |

A role that HelixScreen has already given a sensor (for example a probe's **Z Probe** role) shows beside **Role:** as plain text. A sensor the printer reports under a plain name such as "bltouch" is listed by its kind, with the Klipper name underneath.

---

## Chamber Assignment

Inside **Temperature Sensors**, two dropdowns let you override auto-detection when your chamber heater or sensor is not named "chamber":

- **Chamber Heater** lists the printer's generic heaters (the bed and extruders are left out).
- **Chamber Sensor** lists its temperature sensors.

Each starts on **Auto**, which shows what was detected in parentheses, or **(none detected)**. The last option, **None (disable)**, turns the role off. A saved choice the printer no longer reports stays in the list marked **not detected**, so you can clear it.

A chamber heater can supply the chamber temperature itself: when one is connected and no sensor is named, the heater's reading is used. A sensor you pick by hand wins. The sensor that serves as the chamber sensor is not repeated in the list below the dropdowns.

---

**Next:** [Security & Screen Lock](security.md) | **Prev:** [Fans](fans.md) | [Back to User Guide](../USER_GUIDE.md)

