# Lights (LEDs)

How HelixScreen remembers your light setup. Every key on this page is **per printer** and lives in the `leds` section of your printer's entry, `printers.<id>.leds`. Set up lights from **Settings > Devices > LED Settings**; see [Settings: LED Settings](../guide/settings/led-settings.md) for the walkthrough. Which light each Home screen Light button controls is set from that button's gear icon in Edit Mode (see [Home screen LED controls](../guide/home-panel.md#led-controls)).

[Back to the Configuration Reference](../CONFIGURATION.md)

---

## Startup and remembered colour

| Key | Default | Range | What it does |
|-----|---------|-------|--------------|
| `leds.led_on_at_start` | `false` | `true` or `false` | Turns your lights on when Klipper becomes ready: every light your Home screen Light buttons control, or the chamber light when you have not placed one. A WLED strip gets up to 5 seconds to answer before HelixScreen falls back to the chamber light. **LED on at Start**. |
| `leds.startup_brightness` | `80` | `0` to `100` | The brightness the lights come on at when `led_on_at_start` turns them on. It is independent of `last_brightness`, so lights can start at a fixed level wherever you left the slider. **Startup Brightness**, shown only while LED on at Start is on. |
| `leds.last_color` | `"#FFFFFF"` | `"#RRGGBB"` | The last colour you used, remembered between sessions. A plain whole number such as `16777215` is also accepted. |
| `leds.last_brightness` | `100` | `0` to `100` | The last brightness you used, remembered between sessions. |
| `leds.last_white` | `0.0` | `0.0` to `1.0` | The last white-channel level for strips that have one. |
| `leds.color_presets` | red, orange, green, cyan, blue, purple, pink | list of `"#RRGGBB"` | The preset colours in the colour picker. Whole numbers are also accepted. |

---

## Automatic LED Control

`leds.auto_state` changes your lights by themselves as the printer's state changes. Turn it on at **Settings > Devices > LED Settings > Automatic LED Control**.

```json
"auto_state": {
  "enabled": false,
  "strips": ["neopixel toolhead_leds"],
  "mappings": {
    "idle": { "action": "color", "color": "#FFFFFF", "brightness": 50 },
    "heating": { "action": "color", "color": "#FFD700", "brightness": 100 },
    "printing": { "action": "color", "color": "#FFFFFF", "brightness": 100 },
    "paused": { "action": "color", "color": "#FFD700", "brightness": 50 },
    "error": { "action": "color", "color": "#FF0000", "brightness": 100 },
    "complete": { "action": "color", "color": "#66BB6A", "brightness": 100 }
  }
}
```

| Key | What it does |
|-----|--------------|
| `enabled` | Whether automatic lighting is on. Default `false`. |
| `strips` | The lights Automatic LED Control acts on (the **Applies to** row). It is its own list, independent of what any Home screen Light button controls. |
| `mappings` | One entry for each printer state: `idle`, `heating`, `printing`, `paused`, `error` and `complete`. The values above are the defaults. |

Each mapping has an `action`: `"off"`, `"brightness"`, `"color"`, `"effect"`, `"wled_preset"` or `"macro"`. Which other fields apply depends on the action: `brightness` (0 to 100), `color` (`"#RRGGBB"`, or a whole number), `effect_name` (text), `wled_preset` (a whole number) and `macro_gcode` (the macro or G-code to run).

---

## Macro devices

`leds.macro_devices` lists lights you control with Klipper macros. Each becomes its own tab in the LEDs overlay. Set them up at **Settings > Devices > LED Settings > Macro Devices**.

```json
"macro_devices": [
  {
    "name": "Chamber Light",
    "type": "on_off",
    "on_macro": "LIGHTS_ON",
    "off_macro": "LIGHTS_OFF",
    "toggle_macro": "",
    "presets": []
  }
]
```

| Field | What it does |
|-------|--------------|
| `name` | The name shown on the device's card. |
| `type` | `"on_off"` (separate on and off macros), `"toggle"` (one toggle macro) or `"preset"` (several named presets). |
| `on_macro`, `off_macro` | The macros for an `on_off` device. |
| `toggle_macro` | The macro for a `toggle` device. |
| `presets` | For a `preset` device, a list of `{"name": "...", "macro": "..."}` entries. |

---

## Older and internal keys

The first three are kept for settings written by earlier versions, and editing them by hand does nothing useful. The last is a record HelixScreen keeps for itself.

| Key | What happens to it |
|-----|--------------------|
| `leds.strip` | A single strip name from a version that supported one strip. It is folded into the lights list on load and is otherwise unused. |
| `leds.selected_strips` | Once the list of lights every light button, Automatic LED Control and LED on at Start shared. It is read once, the first time HelixScreen finds your lights with no `auto_state.strips` saved, to give those now-separate settings a starting point. Automatic LED Control starts on the lights you had selected. Your Light buttons start on that light if you had selected one, on **All lights** if you had selected every light, and on the chamber light otherwise. |
| `leds.light_button_pending` | Written once by that migration, or by the setup wizard's LED step, and consumed the first time a Home screen Light button with no light of its own adopts it. It clears itself. |
| `leds.auto_paired_bases` | The paired on and off macros HelixScreen has already offered as a macro device. It is kept after you delete a device, so a macro you dismissed is not offered again. Do not edit it. |
