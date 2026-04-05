# Printer Presets

Platform-specific default configurations for known-hardware builds.

## How Presets Work

For supported platforms (like AD5M), the preset is baked into the release package as `settings.json` by `scripts/package.sh` using convention-based lookup: if `config/presets/<PLATFORM_TARGET>.json` exists, it's used automatically.

- **Fresh installs**: Preset is used, abbreviated wizard runs (language + telemetry only)
- **Upgrades**: Existing `settings.json` is preserved (backup/restore in installer)

The preset sets `wizard_completed: false` so the abbreviated wizard runs on first boot. The `preset` field triggers **preset mode**, which skips all hardware configuration steps since the answers are already known.

## Available Presets

| File | Platform | Notes |
|------|----------|-------|
| `ad5m.json` | Flashforge Adventurer 5M / 5M Pro | Touch calibration, hardware mappings, ForgeX macros |
| `ad5x.json` | Flashforge Adventurer 5X | Same hardware as AD5M, different display settings |
| `cc1.json` | Centauri Carbon 1 | Minimal config, touch cal not yet calibrated |
| `voron-v2-afc.json` | Voron V2 with AFC | Reference config, not auto-baked |
| `artillerty-m1-pro.json` | Artillery M1 Pro | Touch calibration, hardware mappings, community macros |
## What's in a Preset

Presets contain only basic hardware configuration:

- **`preset`** - Platform identifier, triggers abbreviated wizard mode
- **`wizard_completed: false`** - Ensures abbreviated wizard runs on first boot
- **Touch calibration** (`input.calibration`) - Hardware-specific touch matrix
- **Display quirks** (`display.*`) - Platform-specific display driver settings
- **Moonraker connection** (`printer.moonraker_host/port`) - localhost for on-device installs
- **Hardware mappings** (`printer.heaters`, `fans`, `leds`, `filament_sensors`) - Klipper object names
- **Expected hardware** (`printer.hardware.expected`) - For missing hardware warnings
- **Default macros** (`printer.default_macros`) - Platform-specific G-code macros

What's NOT in presets:

- `printer.type` - Auto-detected from Klipper hardware fingerprints
- `hardware.last_snapshot` - Runtime data, populated on first connection
- `hardware.optional` - Runtime data
- `dark_mode`, `brightness`, `language` - User preferences, changeable in Settings

## Creating New Presets

1. Run through the setup wizard on the target hardware
2. Copy the generated `settings.json`
3. Add `"preset": "<platform>"` field
4. Set `"wizard_completed": false`
5. Remove: `printer.type`, `hardware.last_snapshot`, `hardware.optional`
6. Remove user preferences (`dark_mode`, `language`, etc.)
7. Remove sensitive data (API keys)
8. Save as `config/presets/<platform>.json`

The preset is automatically picked up by `scripts/package.sh` when the filename matches `PLATFORM_TARGET`.
