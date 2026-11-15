# Printer Hardware and Motion

Everything that describes one machine: its name, heaters, fans, sensors, enclosure, motion settings and macros. Every key on this page is **per printer**. It lives in your printer's entry, `printers.<id>`, where `<id>` is `default` unless you added more printers (see [The Settings File](settings-file.md#how-the-file-is-organised)). The setup wizard fills most of these in, and you can change most of them from the screen, so editing by hand is rarely needed.

[Back to the Configuration Reference](../CONFIGURATION.md)

---

## Identity

| Key | Default | What it does |
|-----|---------|--------------|
| `printer_name` | `""` | The name shown on the Home screen and in the Printer Manager. When it is empty, the printer type and then its address are shown instead. Change it in the Printer Manager, or tap the printer picture on the Home screen. |
| `type` | `""` | The printer model, for example `"Voron 2.4"`, `"AD5M"` or `"K1"`. It selects model-specific behaviour, so the wizard sets it from detection. |
| `hostname` | recorded by HelixScreen | The hostname Klipper reports. HelixScreen uses it to find the printer again if its address changes. |
| `wizard_completed` | `false` | Whether the setup wizard has run for this printer. Set it to `false` to run the wizard again at the next start. |

The Moonraker address and API key are in [Printer and Connection](network.md).

---

## Heaters, sensors and fans

Each value is the Klipper object name exactly as it appears in `printer.cfg`. The wizard picks them from what your printer reports, and the setup of each is in **Settings > Devices**.

| Key | Default | What it does |
|-----|---------|--------------|
| `heaters.hotend` | `"extruder"` | The heater used for the hotend. Extra extruders (`extruder1`, `extruder2`, and so on) and tool changers are found automatically. |
| `heaters.bed` | `"heater_bed"` | The heated bed heater. |
| `heaters.chamber` | `"auto"` | The heater that warms the enclosure. `"auto"` picks it by name, `"none"` removes chamber heater controls, or name the Klipper object, for example `"heater_generic chamber"`. Most printers work on `"auto"`. |
| `temp_sensors.hotend` | the wizard's pick | The sensor that reads the hotend temperature, when it is not the heater itself. |
| `temp_sensors.bed` | the wizard's pick | The sensor that reads the bed temperature, when it is not the heater itself. |
| `temp_sensors.chamber` | `"auto"` | The chamber temperature sensor. `"auto"` picks it by name, `"none"` hides chamber temperature, or name the Klipper object, for example `"temperature_sensor enclosure_bme"`. Set it when the chamber reads from the wrong sensor. |
| `fans.part` | `"fan"` | The part cooling fan. |
| `fans.hotend` | the wizard's pick | The hotend cooling fan. |
| `fans.chamber` | `""` | A chamber fan, for example `"fan_generic chamber_fan"`. Empty means none. |
| `fans.exhaust` | `""` | An exhaust fan. Empty means none. |
| `fans.aux` | `""` | An auxiliary cooling fan. |
| `fans.names.<fan object>` | none | A name you gave a fan in the Fans list. Rename fans there rather than by hand. |
| `extra_sensors` | `{}` | Extra temperature sensors to monitor beyond hotend and bed. Keys are display names, values are Klipper sensor names. |

How HelixScreen finds and maps chamber heaters and sensors, including the add-on heaters, is in the [Chamber Heater guide](../guide/chamber-heater.md).

### Hardware tracking

`hardware` holds what the wizard and Hardware Health remember: `optional` (optional hardware found), `expected` (hardware expected for this printer type) and `last_snapshot` (the last hardware state, used to notice changes). HelixScreen manages these. Do not edit them; use **Settings > Devices > Hardware Health**.

---

## Enclosure

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `enclosure_style` | `0` | `0` Auto, `1` Enclosed, `2` Open frame | Whether the printer counts as enclosed, which decides whether drying filament on the heated bed is offered. Auto treats it as enclosed when its model ships enclosed or a chamber heater is configured. **Settings > Printing > Enclosure**. |

---

## Motion

Set these from **Settings > Printing > Motion**, or the cog icon in the Motion screen's header. They back the jog pad and live in the `motion` section of the printer entry.

| Key | Default | Range | What it does |
|-----|---------|-------|--------------|
| `motion.jog_speed_xy` | `6000` | `60` to `60000` | Jog feedrate for X and Y moves, in mm/min. |
| `motion.jog_speed_z` | `600` | `60` to `60000` | Jog feedrate for Z moves, in mm/min. |
| `motion.fine_inner`, `fine_outer` | `0.1`, `1.0` | `0.01` to `200` | Millimetres moved per tap in Fine mode, for the inner and outer ring of the jog pad. |
| `motion.coarse_inner`, `coarse_outer` | `1.0`, `10.0` | `0.01` to `200` | The same for Coarse mode. |
| `motion.turbo_inner`, `turbo_outer` | `10.0`, `50.0` | `0.01` to `200` | The same for Turbo mode. |
| `motion.layout` | `0` | `0` jog pad, `1` Arrows, `2` Arrows + Home, `3` Axis Rows | How the Jog tab arranges its buttons. **Layout**. |
| `motion.show_actual_position` | `false` | `true` or `false` | Whether the coordinates on the Motion screen show the commanded position (`false`) or the measured one (`true`) when it opens. Tapping the Target / Actual chip beside the coordinates flips it. |
| `motion.bed_map_clearance` | `5` | `1` to `50` | Millimetres below which a move on the Bed tab first lifts Z before travelling. |

The ring labels on the jog pad show whatever distances you set.

---

## Z-offset

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `z_offset.step_index` | `2` | `0` 0.05 mm, `1` 0.025 mm, `2` 0.01 mm, `3` 0.005 mm | The step size of the Z-offset nudge buttons on the Print Tune overlay, remembered between prints. Choosing a size on the overlay writes it. |
| `z_offset.save_mode` | `0` | `0` Auto, `1` Show, `2` Hide | Whether HelixScreen offers to save Z-offset changes to the printer config. Hide removes every Save Z-Offset button (header, Controls, Home widget) and the save step of Z-offset calibration. Baby-stepping still works for the session. Auto hides them on printers where each tool keeps its own Z offset (tool changers) and shows them elsewhere. **Settings > Printing > Motion > Save Z-Offset Buttons**. |

---

## Capability overrides

HelixScreen detects features such as bed mesh and nozzle cleaning from what Moonraker reports. A capability override forces a feature on or off when detection is wrong. They live in `capability_overrides` in the printer entry.

```json
"capability_overrides": {
  "bed_mesh": "auto",
  "qgl": "auto",
  "z_tilt": "auto",
  "nozzle_clean": "auto",
  "heat_soak": "auto",
  "chamber": "auto",
  "speaker": "auto",
  "led": "auto"
}
```

Each takes `"auto"` (use detection), `"enable"` (force on) or `"disable"` (force off). Typical uses:

- Enable `heat_soak` when you have a chamber but no chamber heater, since the soak macro works without one.
- Disable `qgl` on a printer where it is defined but not used.
- Enable `bed_mesh` if detection missed it.
- Disable `speaker` on a board whose buzzer is detected but not wired, so HelixScreen stops sending it M300 beeps.
- Set `led` to `"disable"` to hide every light control. Lights are also covered by **Settings > Devices > Chamber light fitted**.

A feature you forced on shows a **set manually** chip in the Printer Manager.

---

## Macros

### Quick buttons and standard actions

The `standard_macros` section is **per screen**. It chooses what the four quick-action buttons on the Controls panel do and which of your Klipper macros HelixScreen runs for each standard action. Set it at **Settings > Printing > Macro Buttons**, which lists the macros your printer defines.

```json
"standard_macros": {
  "quick_button_1": "clean_nozzle",
  "quick_button_2": "bed_level",
  "quick_button_3": "",
  "quick_button_4": "",
  "unload_filament": "MY_UNLOAD_ROUTINE"
}
```

| Key | Default | What it does |
|-----|---------|--------------|
| `quick_button_1` | `"clean_nozzle"` | What button 1 does. |
| `quick_button_2` | `"bed_level"` | What button 2 does. |
| `quick_button_3`, `quick_button_4` | `""` | Buttons 3 and 4, empty by default. |
| `load_filament`, `unload_filament`, `purge`, `pause`, `resume`, `cancel`, `bed_mesh`, `bed_level`, `screws_tilt`, `clean_nozzle`, `heat_soak`, `park_toolhead` | `""` | The macro run for each standard action. Empty means detect it from your Klipper config. A macro name pins that action to your macro. |

A quick button takes any of the standard action names above, `"builtin:light"` (turns the button into a light switch, hidden while no light can be controlled), or `""` (hides the button). A key that is **absent** is different from `""`: while a light is controllable and no button is set to `"builtin:light"`, the first button whose key is absent and that would otherwise be empty becomes the light switch. A button set to `""` is never filled that way.

On a printer with a filament system, `load_filament` and `unload_filament` also decide who performs the operation. Left empty, the filament system does it. Set to a macro, your macro does it and the filament system's own handling is skipped for that operation. See [Customizing which macro runs](../guide/filament.md#customizing-which-macro-runs).

### Cooldown

`default_macros` (per printer) holds G-code HelixScreen runs for built-in buttons. Only `cooldown` is read: it is what HelixScreen sends when you tap a **Cool Down** button, for example on the Preheat widget or the Filament panel. The other entries (`load_filament`, `unload_filament`, `macro_1`, `macro_2`) are written with defaults and ignored. Each entry can be a plain G-code string or an object with `label` and `gcode`.

The default turns off the extruder and the bed:

```json
"default_macros": {
  "cooldown": "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\nSET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0"
}
```

While `cooldown` is still the default, HelixScreen also turns off every additional extruder and the chamber heater it knows about. Once you write your own text, HelixScreen sends exactly that and adds nothing. Separate lines with `\n`, or name a Klipper macro instead, for example `"cooldown": "MY_COOLDOWN_MACRO"`:

```json
"cooldown": "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\nSET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0\nSET_FAN_SPEED FAN=bed_fan SPEED=0"
```

### Hidden macros and parameter defaults

`macros.hidden` and `macros.param_defaults` hold the macros you hid from the Macros panel and the parameter values you last used. They are written by the Macros panel.

---

## Power devices

`power.selected_devices` lists the Moonraker power devices shown on the Power panel. Choose them at **Settings > Devices > Power Devices**.
