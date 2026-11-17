# Filament, Multi-Filament Systems and Sensors

Settings for loading and unloading filament, multi-filament systems (AFC, Happy Hare, ACE, tool changers and others), and filament sensors. Unless a row says otherwise these are **per printer** and live in your printer's entry, `printers.<id>` (see [The Settings File](settings-file.md#how-the-file-is-organised)). See [Settings: Printing](../guide/settings/printing.md) and [Filament](../guide/filament.md) for the walkthroughs.

[Back to the Configuration Reference](../CONFIGURATION.md)

---

## Loading, unloading and cooldown

These are in the `filament` section of the printer entry.

| Key | Default | Range | What it does |
|-----|---------|-------|--------------|
| `filament.auto_cooldown` | `true` | `true` or `false` | Whether HelixScreen turns the extruder heater off after a filament load or unload finishes. Turn it off if your filament system runs its own cooldown, as recent AFC versions do, so the two do not both drive the heater. **Settings > Printing > Cool nozzle after filament ops**. |
| `filament.cooldown_delay_seconds` | `120` | seconds | How long to wait after a load or unload before turning the heater off, so you can run several operations back to back without the nozzle cooling in between. `0` also turns auto-cooldown off, but prefer `auto_cooldown`, which is the one the switch writes. |
| `filament.extrude_speed` | `5` | `1` to `50` | The extrude and retract speed of the Filament panel, in mm/s. **Settings > Printing > Machine Limits > Extrude Speed**, or the speed dropdown on the Filament panel. This is a HelixScreen setting, not a Klipper limit. |
| `filament.extrude_length_0`, `_1`, `_2` | `5`, `10`, `25` | `1` to `500` | The lengths of the Filament panel's three length buttons, in mm. Press and hold a button to change it. |
| `filament.auto_open_editor` | `false` | `true` or `false` | Opens the slot editor when a filament is inserted. **Settings > Printing > Edit filament on insert**. |
| `filament.auto_color_map` | `false` | `true` or `false` | How a print's colours are matched to slots. Off maps by position; on matches by colour. See [Filament mapping](../guide/filament-mapping.md). |

**Allow cold load/unload** (**Settings > Printing**) is stored once for the whole screen as `safety.allow_cold_extrude`, default `false`. See [Safety and Alerts](safety-alerts.md).

### Favourite filaments

`filament.favorite_ids` is stored for the whole screen, not per printer. It lists the filament types you starred in a filament selector, in star order. It defaults to `[]`. Each entry is the filament's product id. Tapping a star writes it. Edit it only to reorder or clear it; an id that matches nothing is never shown.

### The external (bypass) spool

The external spool is not a setting. It is stored on the printer, in Moonraker's database, as a lane record named `bypass`, so every screen connected to the printer shows the same spool. An `external_spool` section left in `filament` by an earlier version is removed when the settings file is upgraded.

### Material temperatures

The temperatures behind each material preset are edited at **Settings > Printing > Material Temperatures** and saved with your filament overrides. The four preset buttons on the temperature panel are stored in `preset_materials` (whole screen). Edit them from the screen.

---

## Multi-filament systems

These are in the `ams` section of the printer entry, `printers.<id>.ams`. Each has a switch or control in **Settings > Printing > Multi-Filament System Management**, so edit them there rather than by hand.

```json
"ams": {
  "force_bypass_controls": false,
  "always_show_bypass_spool": false,
  "keep_spool_info_on_eject": true,
  "tag_links_spoolman": true,
  "unload_after_print": false
}
```

| Key | Default | What it does |
|-----|---------|--------------|
| `force_bypass_controls` | `false` | Shows the bypass controls and the external spool on the filament path even when the firmware reports no bypass position. It applies to Anycubic ACE Pro, Snapmaker U1, tool changers, QIDI Box, and Happy Hare setups where `[mmu_machine] has_bypass` is `0`. The row is hidden whenever the firmware does report a bypass, including on the Creality CFS, whose bypass always works. On Happy Hare, the bypass command works either way, so this makes bypass usable on `mmu_vendor: Other` setups and on uncalibrated type-A selectors. On the other systems there is no bypass command to send: the Bypass toggle reports that the operation is not supported, and the setting only controls whether the external spool is displayed and tracked. See [When bypass does not appear](../guide/filament.md#when-bypass-doesnt-appear). |
| `always_show_bypass_spool` | `false` | Keeps the external spool visible on the filament path while bypass is off. It applies to AFC systems (Box Turtle, OpenAMS) only. They publish a virtual bypass sensor whether or not one is wired, so without this the spool is drawn only while bypass is engaged. |
| `keep_spool_info_on_eject` | `true` | Keeps a lane's spool details after it empties, so reloading the same spool after maintenance needs no re-selection. Turn it off to start fresh whenever a lane empties. It applies only where HelixScreen's own lane record is the only spool information: tool changers (including MedusaHC and INDX), OpenAMS without its `[openams_spoolman]` component, and the ACE without the ACEPRO driver. Where the firmware holds the spool (AFC, Happy Hare, CFS, AD5X, QIDI Box, Snapmaker U1) the firmware decides and the setting is not shown. On AFC versions that report their own `remember_spool` setting, the **Keep Spool Info on Eject** switch in **Multi-Filament System Management** changes that setting on the printer for every lane instead. |
| `tag_links_spoolman` | `true` | When a spool whose tag names a Spoolman spool is put in a slot, links the slot to that spool, the same as picking it by hand. A spool you pick while the same tag stays seated is kept until a spool is put in again. The **Link Spools from Tags** switch is shown only on systems whose tags can carry a Spoolman spool number (Creality CFS) when Spoolman is set up. |
| `unload_after_print` | `false` | On AFC systems, whether your end-of-print macros unload the toolhead when a print finishes, so an empty toolhead at the next print start is expected rather than a warning. **Unload at end**, in the **Print Behavior** section of **Multi-Filament System Management**. Older configs stored it as `afc_unload_after_print`, which is renamed on upgrade. |

Other keys in the same section:

| Key | Default | What it does |
|-----|---------|--------------|
| `number_lanes_from_zero` | `false` | Numbers slots from 0 instead of 1. **Settings > Printing > Number slots from 0**. |
| `end_of_print_unload_tools` | `0` | A bit mask of the tools that unload when a print ends: bit N set means tool N unloads. Written by the standing print options. |
| `eject_distance` | `0` | How far, in mm (`0` to `2000`), a backend that ejects a lane pushes the filament out. `0` means the backend's own default. |
| `eject_velocity` | `0` | The eject speed in mm/s (`0` to `300`). `0` means the backend's own default. |
| `dryer_last_temp`, `dryer_last_duration` | `55`, `240` | The temperature (°C) and time (minutes) you last used to dry filament, offered again next time. |

The spool picture style, `ams.spool_style`, is stored for the whole screen. See [Appearance](appearance.md#filament-spool-picture).

---

## Filament sensors

Stored in `filament_sensors` in the printer entry. Sensors are found automatically from Moonraker. Assign roles at **Settings > Devices > Sensors**; see [Sensors](../guide/sensors.md).

```json
"filament_sensors": {
  "master_enabled": true,
  "sensors": [
    {
      "klipper_name": "filament_switch_sensor fsensor",
      "role": "runout",
      "enabled": true
    }
  ]
}
```

| Key | Default | What it does |
|-----|---------|--------------|
| `master_enabled` | `true` | Turns all filament sensor monitoring on or off. With `false`, sensor states are ignored and no runout detection happens. |
| `sensors` | `[]` | One entry per sensor. |

Each sensor entry has:

| Field | What it does |
|-------|--------------|
| `klipper_name` | The full Klipper object name, such as `"filament_switch_sensor fsensor"`. |
| `role` | `"none"`, `"runout"`, `"toolhead"` or `"entry"`. |
| `enabled` | Whether this one sensor is used. |
| `lane` | Optional. The filament slot or toolhead (counting from 0) the sensor watches, for printers with one sensor per head. Each head can then have its own `runout` sensor, and the check before a print reads the sensors of the heads the file uses. |
