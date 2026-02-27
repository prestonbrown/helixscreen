# docs/devel/CLAUDE.md — Developer Documentation

All developer documentation lives here. When working on features, look up the relevant doc before guessing.

## Core Development

| Doc | When to read |
|-----|-------------|
| `DEVELOPMENT.md` | Build setup, dev environment, contributing |
| `ARCHITECTURE.md` | System design, component relationships, extended systems |
| `BUILD_SYSTEM.md` | Makefile internals, cross-compilation, patches |
| `TESTING.md` | Catch2 test infrastructure, test patterns |
| `LOGGING.md` | spdlog levels, when to use info vs debug vs trace |
| `COPYRIGHT_HEADERS.md` | SPDX license headers |
| `RELEASE_PROCESS.md` | Release workflow, versioning |
| `CI_CD_GUIDE.md` | CI pipeline, GitHub Actions |

## UI & XML

| Doc | When to read |
|-----|-------------|
| `UI_CONTRIBUTOR_GUIDE.md` | **Start here** for UI/layout work: breakpoints, tokens, colors, widgets, layout overrides |
| `LVGL9_XML_GUIDE.md` | XML syntax, all widgets (ui_card, ui_button, ui_markdown, etc.), bindings |
| `DEVELOPER_QUICK_REFERENCE.md` | Quick code patterns: modals, CSV parser, layout, migration |
| `MODAL_SYSTEM.md` | ui_dialog, modal_button_row, Modal subclass pattern |
| `THEME_SYSTEM.md` | Theme internals: style architecture, theme_core C API, adding themed widgets |
| `LAYOUT_SYSTEM.md` | Layout system internals: LayoutManager C++ API, auto-detection logic |
| `TRANSLATION_SYSTEM.md` | i18n: YAML strings -> code generation -> runtime lookups |
| `UI_TESTING.md` | Headless LVGL testing, UITest utilities |
| `GCODE_VIEWER_CONFIG.md` | GCode viewer configuration |
| `BED_MESH_RENDERING_INTERNALS.md` | Bed mesh 3D rendering internals |
| `PRE_RENDERED_IMAGES.md` | Pre-rendered image pipeline |

## Feature Systems

| Doc | When to read |
|-----|-------------|
| `FILAMENT_MANAGEMENT.md` | AMS, AFC (Box Turtle), Happy Hare, ValgACE, Tool Changer, multi-backend |
| `plans/2026-02-15-spool-wizard-status.md` | Spool creation wizard: 3-step flow, API methods, visual test plan |
| `MULTI_EXTRUDER_TEMPERATURE.md` | Multi-extruder temperature tracking, ExtruderInfo, dynamic subjects |
| `TOOL_ABSTRACTION.md` | ToolState singleton, ToolInfo, tool-to-backend mapping, DetectState |
| `INPUT_SHAPER.md` | Calibration panels, frequency response charts, CSV parser, PID |
| `PREPRINT_PREDICTION.md` | ETA prediction engine, phase timing, weighted history |
| `EXCLUDE_OBJECTS.md` | Object exclusion, per-object thumbnails, slicer setup |
| `PRINT_STATE_MACHINE.md` | Print lifecycle state machine: states, transitions, guards, resource lifecycle |
| `PRINT_START_PROFILES.md` | Print start phase detection, JSON profiles |
| `PRINT_START_INTEGRATION.md` | User-facing macro setup for print start tracking |
| `UPDATE_SYSTEM.md` | Update channels (stable/beta/dev), R2 CDN, Moonraker updater |
| `SOUND_SYSTEM.md` | Audio architecture, JSON themes, backends (SDL, PWM, M300). User guide: `../user/guide/settings.md#sound-settings` |
| `LED_CONTROL.md` | LED control system: 5 backends, auto-state lighting, control/settings overlays, home panel widget |
| `PRINTER_MANAGER.md` | Printer overlay, custom images, inline name editing |
| `TIMELAPSE.md` | Moonraker timelapse plugin integration |
| `CRASH_REPORTER.md` | Crash reporter: detection, delivery pipeline, CF Worker, modal UI |
| `CONFIG_MIGRATION.md` | Versioned config migration: adding new migrations, testing |
| `STANDARD_MACROS_SPEC.md` | Standard macro specifications |
| `POWER_BUTTON_HANDLING.md` | Power button behavior |

## Platform & Deployment

| Doc | When to read |
|-----|-------------|
| `INSTALLER.md` | Installation system, KIAUH extension, shell tests (bats) |
| `printers/QIDI_SUPPORT.md` | QIDI Q1 Pro/Plus platform |
| `printers/SNAPMAKER_U1_SUPPORT.md` | Snapmaker U1 toolchanger platform |
| `printers/CREALITY_K2_SUPPORT.md` | Creality K2 series platform |
| `printers/FLASHFORGE_AD5X_SUPPORT.md` | FlashForge Adventurer 5X (MIPS, ZMOD) |
| `ENVIRONMENT_VARIABLES.md` | All runtime and build env vars |

## Integration

| Doc | When to read |
|-----|-------------|
| `MOONRAKER_ARCHITECTURE.md` | Moonraker API abstraction, WebSocket integration |
| `PLUGIN_DEVELOPMENT.md` | Plugin API, lifecycle, UI injection, threading, examples |
| `TELEMETRY_ADMIN.md` | Telemetry pipeline, Analytics Engine, dashboard, scripts, secrets |

## Planning & Research

| Doc | When to read |
|-----|-------------|
| `ROADMAP.md` | Feature timeline, what's complete, what's next |
| `IDEAS.md` | Feature ideas and brainstorming |
| `plans/` | Active implementation plans |
| `plans/2026-02-23-xml-hot-reload.md` | XML hot reload: status, design decisions, Phase 3 stretch goal |
| `printer-research/` | Printer-specific research notes |
| `KLIPPERSCREEN_RESEARCH.md` | KlipperScreen competitive analysis |
| `MAINSAIL_RESEARCH.md` | Mainsail competitive analysis |

## Reference

| Doc | When to read |
|-----|-------------|
| `LVGL9_XML_ATTRIBUTES_REFERENCE.md` | Complete XML attribute reference |
| `LVGL9_XML_CHEATSHEET.html` | Quick XML cheatsheet (HTML) |
| `LVGL_XML_SITUATION.md` | LVGL XML licensing history and resolution (extracted to helix-xml) |
| `SLOT_COMPONENT_DESIGNS.md` | Slot component design patterns (ready to implement) |
| `plans/2026-02-18-helix-xml-plan.md` | Helix XML engine: extraction, upgrade & extension plan |
| `DOXYGEN_GUIDE.md` | Doxygen documentation setup |
| `FLAG_ICONS_SOURCE.md` | Flag icon asset sources |
| `480x320_UI_AUDIT.md` | Small display UI audit |
