# Printer Presets

Platform-specific default configurations that skip the setup wizard.

## How Presets Work

For supported platforms (like AD5M), the preset is baked into the release package as `helixconfig.json`. This means:

- **Fresh installs**: Preset is used, wizard is skipped, touch works immediately
- **Upgrades**: Existing `helixconfig.json` is preserved (backup/restore in installer)

## Available Presets

| File | Platform | Notes |
|------|----------|-------|
| `ad5m.json` | Flashforge Adventurer 5M / 5M Pro | Touch calibration, hardware mappings, ForgeX macros. Printer type auto-detected at runtime (Pro vs non-Pro based on LED presence). |
| `voron-v2-afc.json` | Voron V2 with AFC | Reference config, not auto-baked |

## What's in a Preset

Presets contain the minimal config needed to skip the wizard:

- **Touch calibration** (`input.calibration`) - Hardware-specific, can't be auto-detected
- **Moonraker connection** (`printer.moonraker_host/port`) - localhost for on-device installs
- **Hardware mappings** (`printer.heaters`, `fans`, `leds`, `filament_sensors`) - Klipper object names
- **Expected hardware** (`printer.hardware.expected`) - For missing hardware warnings
- **Default macros** (`printer.default_macros`) - Platform-specific G-code macros
- **`wizard_completed: true`** - Skips the setup wizard

What's NOT in presets (auto-detected or user preference):
- `printer.type` - Auto-detected from Klipper hardware fingerprints
- `dark_mode`, `brightness`, `language` - User preferences, changeable in Settings

## Creating New Presets

1. Run through the setup wizard on the target hardware
2. Copy the generated `helixconfig.json`
3. Remove user preferences (`dark_mode`, `language`, etc.)
4. Remove `printer.type` if it should be auto-detected
5. Remove sensitive data (API keys)
6. Save as `config/presets/<platform>.json`

To bake a preset into a platform build, update `scripts/package.sh`.
