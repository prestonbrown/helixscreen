# Custom Printer Definitions

This directory allows you to add custom printer definitions for auto-detection
without modifying the bundled `printer_database.json`.

## How It Works

HelixScreen loads printer definitions in two phases:
1. **Bundled database**: `config/printer_database.json`
2. **User extensions**: `config/printer_database.d/*.json` (this directory)

User definitions have higher priority and can:
- **Add new printers** - Create a file with a unique printer ID
- **Override bundled printers** - Use the same ID to replace a bundled definition
- **Disable bundled printers** - Set `"enabled": false` to hide from detection

## Adding a Custom Printer

Create a JSON file in this directory (e.g., `my-printer.json`):

```json
{
  "printers": [
    {
      "id": "my_custom_printer",
      "name": "My Custom Printer",
      "manufacturer": "Custom",
      "image": "generic-corexy.png",
      "heuristics": [
        {
          "type": "hostname_match",
          "field": "hostname",
          "pattern": "my-printer",
          "confidence": 90,
          "reason": "Matched hostname pattern"
        }
      ]
    }
  ]
}
```

## Optional Fields

| Field | Purpose |
|-------|---------|
| `enabled` | `false` to hide a bundled printer from detection/selection |
| `console_filters` | List of named filter-set names whose patterns suppress firmware noise in the G-code console. See **Console Filter Sets** below. |
| `console_filter_patterns` | Inline suppression patterns for noise specific to this one model, when a shared set would be overkill. Same pattern syntax as a set. |
| `screws_tilt_direction` | `"cw"` or `"ccw"` — override for bed-screw tightening direction. Set to `"ccw"` when the printer's Klipper `screw_thread` config disagrees with its physical screw geometry, causing `SCREWS_TILT_CALCULATE` to report inverted directions. HelixScreen flips CW↔CCW at display so following the UI actually levels the bed. Omit (or use `"cw"`) when Klipper's output matches reality. |

Example with a screws-tilt override:

```json
{
  "printers": [
    {
      "id": "my_custom_printer",
      "name": "My Custom Printer",
      "screws_tilt_direction": "ccw"
    }
  ]
}
```

## Console Filter Sets

Some firmware prints a great deal of internal chatter to the G-code console. A
filter set is a named list of patterns that suppress it, so several printers can
share one list instead of each carrying a copy.

A printer opts in by name:

```json
{
  "printers": [
    {
      "id": "my_custom_printer",
      "name": "My Custom Printer",
      "console_filters": ["creality_rs485"]
    }
  ]
}
```

You can also define your own sets, or replace a bundled one by using its name.
Sets merge before the printers that reference them, so a file may contain either
or both:

```json
{
  "console_filter_sets": {
    "my_noisy_board": {
      "description": "Chatter from my custom toolboard",
      "patterns": [
        "prefix:// tb_debug:",
        "substring:HEARTBEAT"
      ]
    }
  }
}
```

### Pattern syntax

| Form | Matches | Cost |
|------|---------|------|
| `prefix:TEXT` | Line starts with TEXT | cheapest |
| `substring:TEXT` | TEXT appears anywhere in the line | ~5x a prefix |
| `regex:PATTERN` | ECMAScript regex matches the line | ~200x a prefix |

Prefer `prefix:` and reach for `regex:` only when neither of the cheaper forms
can express the shape. Filtering runs on every console line on the UI thread, and
on a printer's own low-power board that difference is visible. Patterns are
evaluated cheapest-first regardless of the order you list them in.

Two filters in the console's settings (the gear icon) switch all of this off:
temperature reports and firmware noise are independently toggleable, and a user
pattern list in `settings.json` can add to or remove from whatever a printer
resolves to.

## Heuristic Types

| Type | Description |
|------|-------------|
| `hostname_match` | Match against Moonraker hostname |
| `sensor_match` | Match against temperature sensor names |
| `fan_match` | Match against fan object names |
| `fan_combo` | Multiple fan patterns must all match |
| `led_match` | Match against LED/neopixel names |
| `kinematics_match` | Match kinematics type (corexy, cartesian, delta) |
| `object_exists` | Check if a Klipper object exists |
| `stepper_count` | Count Z steppers (z_count_1, z_count_2, etc.) |
| `mcu_match` | Match MCU chip type |
| `build_volume_range` | Match build volume dimensions |
| `macro_match` | Match G-code macro names |

## Disabling a Bundled Printer

To hide a bundled printer from the selection roller:

```json
{
  "printers": [
    {
      "id": "flashforge_adventurer_5m",
      "enabled": false
    }
  ]
}
```

## Reload

After adding or modifying files, restart HelixScreen to reload the database.
