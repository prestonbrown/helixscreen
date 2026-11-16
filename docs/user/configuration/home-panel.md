# Home Screen Layout

How HelixScreen stores your Home screen: its pages, which widgets are on them and where. The layout is **per printer** and is saved in `panel_widgets` in your printer's entry (see [The Settings File](settings-file.md#how-the-file-is-organised)). Arrange it on the screen instead of by hand: long-press the widget grid to enter Edit Mode. See [Home Screen Edit Mode](../guide/home-edit-mode.md) and the [widget catalog](../guide/home-widgets.md).

[Back to the Configuration Reference](../CONFIGURATION.md)

> Editing this by hand is rarely worth it. Edit Mode checks every value against your printer and your screen size, which a hand edit cannot do.

---

## Shape of the layout

```json
"panel_widgets": {
  "home": {
    "pages": [
      {
        "id": "main",
        "widgets": [
          { "id": "printer_image", "enabled": true, "col": 0, "row": 0, "colspan": 4, "rowspan": 4 },
          { "id": "print_status", "enabled": true, "col": 0, "row": 4, "colspan": 8, "rowspan": 4 }
        ]
      },
      {
        "id": "page_1",
        "widgets": [
          { "id": "temp_graph", "enabled": true, "col": 0, "row": 0, "colspan": 8, "rowspan": 6 }
        ]
      }
    ],
    "main_page_index": 0,
    "next_page_id": 2
  }
}
```

| Key | What it does |
|-----|--------------|
| `pages` | The list of pages. Each has an `id` (for example `"main"` or `"page_1"`) and a list of `widgets`. Up to 8 pages are supported. |
| `main_page_index` | Which page is the main page, shown on first connect and when you double-tap Home. `0` is the first page. |
| `next_page_id` | A counter HelixScreen uses to name new pages. Do not change it. |

Each widget entry has:

| Key | What it does |
|-----|--------------|
| `id` | Which widget it is (see the table below). |
| `enabled` | Whether the widget is shown. A widget with `enabled: false` waits in the Widget Catalog to be added back. |
| `col`, `row` | Where the widget's top-left corner sits, counted from 0 at the top left. `-1` for either means the widget has no position right now. |
| `colspan`, `rowspan` | The widget's width and height. |
| `config` | Optional settings for that widget, written by the gear button in Edit Mode. |

> **Positions and sizes are counted in half cells, not whole cells.** The grid is laid out in half-cell steps so a few widgets can sit on half-cell boundaries. A one-cell widget is `"colspan": 2, "rowspan": 2`, and the two-by-two Printer Image above is `4` by `4`. Multiply by two when you turn a size from the widget catalog or the [Home Screen guide](../guide/home-panel.md#available-widgets), which are written in whole cells.

> **What `col: -1` and `row: -1` mean.** The widget is switched on but has nowhere to sit right now, usually because the grid was full the last time HelixScreen laid the page out. It is not disabled. As soon as a cell frees up, because you removed another widget, unplugged hardware another widget needed, or opened the same layout on a screen with a bigger grid, it places itself again. You do not need to add it again from the catalog.

Each screen size keeps its own arrangement, because a different size fits a different number of widgets. A layout that you have not edited follows HelixScreen's defaults for your screen and printer, and updates when those defaults improve. Once you move or add a widget, your arrangement is kept as it is.

---

## Widget settings

A widget's `config` holds the choices you make with its gear button. Which keys apply depends on the widget:

| Key | Widgets | Values |
|-----|---------|--------|
| `display_mode` | Temperatures, Fan Speeds, Temperature Sensors | `"stack"` (default) or `"carousel"` |
| `layout_style` | Print Status | `"detailed"` for the expanded card, otherwise the compact one |
| `fan` | Fan | The name of the fan to show |
| `sensor`, `sensors` | Temperature Sensors | One sensor name, or a list of names |
| `sensors` | Temperature Graph | A list of the series to plot, each with its `name`, whether it is `enabled` and its `color` |
| `follow_overlay` | Temperature Graph | `true` to follow the temperature overlay |
| `macro`, `color`, `require_confirmation` | Macro Button | The macro to run, the icon tint, and whether a tap asks first |
| `device` | Power | The Moonraker power device to bind |
| `icon` | Macro Button, Power, Temperatures, Fan Speeds, Temperature Sensors | An icon name |
| `rotation`, `flip_h`, `flip_v` | Camera | `0`, `90`, `180` or `270`, and two booleans |
| `source` | Camera | The webcam to show, exactly as Moonraker lists it. Empty or absent picks one automatically |
| `source`, `danger_threshold` | Clog Detection | The detection source, and the danger-zone percentage |
| `source` | Filament Sensor | Which sensor role the tile follows: `"auto"` (default), `"runout"`, `"toolhead"` or `"entry"` |
| `material_index` | Preheat | Which material profile the buttons preheat to |
| `led` | LED Light | Which light this button controls: a light id such as `"neopixel chamber_light"`, `"all"` for every light, or omitted for the chamber light |

`require_confirmation` works like this. Omitted, a tap on the button asks first: with a parameter form when the macro takes parameters, otherwise with the **Settings > Safety & Alerts > Confirm before running macros** dialog. `false` runs the macro on a single tap with no parameters and no dialog. Dangerous macros ask whatever you set. Set it from the widget's **Options** tab. See [Macro Button confirmation](../guide/home-panel.md#macro-button-confirmation).

---

## Widget ids

For what each widget does and how large it can get, see the [Home Screen guide](../guide/home-panel.md#available-widgets). This table gives the id to use in the file, the widget's name on screen, whether it is on in a fresh layout, and what it needs.

| Id | Widget | On by default | Needs |
|----|--------|---------------|-------|
| `printer_image` | Printer Image | Yes | |
| `print_status` | Print Status | Yes | |
| `control_buttons` | Print Controls | No | |
| `print_stats` | Print Stats | No | |
| `display_message` | Printer Message | No | |
| `speed_flow` | Speed / Flow | No | |
| `z_offset` | Z-Offset | No | |
| `exclude_map` | Exclude Objects | No | Klipper `exclude_object` |
| `job_queue` | Job Queue | No | The job queue enabled in Moonraker |
| `camera` | Camera | No | |
| `temperature` | Nozzle Temperature | Yes | |
| `nozzle_temps` | Nozzle Temperatures | No | |
| `bed_temperature` | Bed Temperature | Yes | |
| `chamber_temperature` | Chamber Temperature | No | A chamber temperature sensor |
| `temp_stack` | Temperatures | No | |
| `thermistor` | Temperature Sensors | No | Extra temperature sensors |
| `temp_graph` | Temperature Graph | No | |
| `preheat` | Preheat | No | |
| `fan_stack` | Fan Speeds | Yes | |
| `filament_ops` | Filament Operations | No | |
| `fan` | Fan | No | |
| `ams` | Multi-Filament System Status | No | A multi-filament system or MMU |
| `bypass` | Bypass | No | A filament system with bypass |
| `active_spool` | Active Spool | No | |
| `filament` | Filament Sensor | Yes | A filament sensor |
| `filament_buffer` | Filament Buffer | No | A filament pressure sensor or sync feedback |
| `humidity` | Humidity | No | A humidity sensor |
| `dryer` | Filament Dryer | No | A filament dryer |
| `width_sensor` | Width Sensor | No | A width sensor |
| `clog_detection` | Clog Detection | No | A clog detector or filament motion sensor |
| `favorite_macro` | Macro Button | No | |
| `macros` | Macros | No | |
| `gcode_console` | GCode Console | No | |
| `motion` | Motion | No | |
| `toolhead` | Toolhead | No | |
| `bed_mesh_tile` | Bed Mesh | No | Bed mesh support |
| `tool_switcher` | Tool Switcher | No | |
| `power_device` | Power | No | A Moonraker power device |
| `led` | LED Light | Yes | An LED or light |
| `led_controls` | LED Controls | No | An LED or light |
| `network` | Network | No | |
| `host_health` | Host Health | No | Moonraker performance data |
| `notifications` | Notifications | Yes | |
| `clock` | Digital Clock | No | |
| `maintenance` | Maintenance | No | |
| `tips` | Tips | Yes | |
| `shutdown` | Shutdown/Reboot | No | A host that can shut down (not Android) |
| `firmware_restart` | Firmware Restart | No | |
| `lock` | Lock Screen | No | |

A widget whose hardware is missing stays hidden even when it is enabled. `power_device`, `fan`, `fan_stack`, `thermistor`, `favorite_macro`, `temp_graph` and `led` can appear more than once, and an extra copy gets an id such as `favorite_macro:2`. A new widget in a later release is added to your layout with its default state. An id HelixScreen does not recognise is ignored.
