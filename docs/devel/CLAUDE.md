# docs/devel/CLAUDE.md — Developer Documentation

All developer documentation lives here. When working on features, look up the relevant doc before guessing.

## Core Development

| Doc | When to read |
|-----|-------------|
| `DEVELOPMENT.md` | Build setup, dev environment, contributing |
| `HELIXCTL.md` | Driving the UI / screenshots via `helix-screen ctl` (replaces the old `-p`/`--panel` flags). **Read the socket-isolation box first** — a bare `ctl` drives whichever instance started first and still reports success |
| `ARCHITECTURE.md` | The 15-minute whole-app model (XML → Subjects → C++) + the routing table into the chapter series. Start here for "how does the app fit together" |
| `architecture/` | The 16-chapter architecture guide — one subsystem per chapter, ~1 hour each. `architecture/README.md` is the "I want to work on..." index |
| `THREADING.md` | **Single source of truth** for threading, async-callback, and object-lifetime rules. Read before any code that crosses a thread boundary, observes a subject, or destroys a widget |
| `BUILD_SYSTEM.md` | Makefile internals, make target reference, cross-compilation, worktree workflow, ccache, patches |
| `REVIEW_RUBRIC.md` | The quality bar for reviews: crash families, silent-failure traps, what not to flag, what the gates already cover |
| `../../scripts/CLAUDE.md` | Index of `scripts/` — installer, release, asset regeneration, and the "Quality & Auditing" gate table covering every `check_*.py` lint and what it enforces |
| `TESTING.md` | Catch2 test infrastructure, test patterns |
| `MOCK_ENVIRONMENT_VARIABLES.md` | The `HELIX_MOCK_*` matrix, replay scripts, forced-modal and demo-injection knobs - every var that shapes a `--test` run. Runtime/display/logging vars stay in `ENVIRONMENT_VARIABLES.md` |
| `HIDDEN_TESTS_TRACKER.md` | Tests hidden from the default run (`[.]`), why each is hidden, and how to run them |
| `LOGGING.md` | spdlog levels, when to use info vs debug vs trace |
| `COPYRIGHT_HEADERS.md` | SPDX license headers |
| `RELEASE_PROCESS.md` | Release workflow, versioning |
| `CHANGELOG_STYLE.md` | How `CHANGELOG.md` entries are written: user-facing voice, hyphen separator, bare `(#N)` links, daily vs milestone shapes. Read before drafting a release's changelog section |
| `RELEASE_1_0_CHECKLIST.md` | Everything blocking `v1.0.0` and the 1.1 devel track — the atomic `release/1.0` branch cut + `RELEASE_CHANNEL` flip, open milestone issues, what is and is not verified. Delete once 1.0 ships |
| `CHANGELOG_1_1_DRAFT.md` | Running release notes for everything on `devel/1.1` that is not on `main`. Kept out of `CHANGELOG.md` so the release tooling owns that file; becomes the `## [1.1]` entry at release, then delete |
| `CI_CD_GUIDE.md` | CI pipeline, GitHub Actions |
| `ANDROID_PLAY_STORE.md` | Play Store publishing pipeline, one-time setup, promotion flow |
| `ANDROID_ASSETS.md` | How `ui_xml/`/`assets/`/`config/` reach the APK. Read before touching anything under `android/app/src/main/assets/` — it is a Gradle build output, not source |

## UI & XML

| Doc | When to read |
|-----|-------------|
| `UI_CONTRIBUTOR_GUIDE.md` | **Start here** for UI/layout work: breakpoints, tokens, colors, widgets, layout overrides |
| `ONBOARDING.md` | Fresh checkout → first change: the new-contributor onramp |
| `YOUR_FIRST_CONTRIBUTION.md` | Annotated walkthrough of a real settings overlay + pattern tour of AMS for bigger features |
| `CONTRIBUTOR_GOTCHAS.md` | Symptom-indexed "if you see X, you forgot Y" — silent-failure traps in XML, translations, subjects |
| `LVGL9_XML_GUIDE.md` | XML syntax, all widgets (ui_card, ui_button, ui_markdown, etc.), bindings |
| `DEVELOPER_QUICK_REFERENCE.md` | Quick code patterns: modals, CSV parser, layout, migration |
| `MODAL_SYSTEM.md` | ui_dialog, modal_button_row, Modal subclass pattern |
| `THEME_SYSTEM.md` | Theme internals: ThemeManager style architecture, StyleRole roles, adding themed widgets |
| `THEME_CONTRIBUTOR_GUIDE.md` | For people **creating themes** — JSON schema, palette design, testing. No C++ needed. |
| `LAYOUT_SYSTEM.md` | Layout system internals: LayoutManager C++ API, auto-detection logic, and the home widget grid (`GridLayout` sizing, `assets/config/default_layout.json` anchors, widget span/`min_colspan` authoring) |
| `PAGE_SCROLL_BUTTONS.md` | Chevron page-scroll gutter: where it auto-attaches and why it stops at a home widget tile. On by default on ESP32 only |
| `TRANSLATION_SYSTEM.md` | i18n: YAML strings -> code generation -> runtime lookups |
| `TRANSLATION_CONTRIBUTOR_GUIDE.md` | For **translators** — how to improve existing translations or add a new language. No code needed. |
| `UI_TESTING.md` | Headless LVGL testing, UITest utilities |
| `architecture/16-gcode-pipeline.md` | G-code from file to screen: parse/scan paths, footer fast path, tools-used cache, render modes, object picking. Start here for viewer work |
| `GCODE_VIEWER_CONFIG.md` | GCode viewer configuration |
| `BED_MESH_RENDERING_INTERNALS.md` | Bed mesh 3D rendering internals |
| `FILAMENT_PATH_CANVAS.md` | Filament-path canvas: 3-layer model, pathgeo arc-fillet routing, shared tube stroker, RenderCtx phases, topology renderers (linear/hub/parallel/mixed) |
| `PRE_RENDERED_IMAGES.md` | Pre-rendered image pipeline |
| `GESTURE_RECOGNITION.md` | Research notes: pinch-to-zoom via LVGL 9.5 gesture recognition on evdev multi-touch |
| `GALLERY.md` | Screenshots of the main panels, regenerated by `scripts/screenshot.sh` |

## Feature Systems

| Doc | When to read |
|-----|-------------|
| `LABEL_PRINTER_SYSTEM.md` | Label printing: Brother QL, Phomemo, Niimbot, MakeID protocols; USB/TCP/Bluetooth transports |
| `FILAMENT_MANAGEMENT.md` | Filament system hub: multi-backend architecture, slot metadata, filament-op dispatch, endless spool, UI panels, dryer, device ops, mock mode, add-a-backend guide |
| `FILAMENT_BACKEND_AFC.md`, `FILAMENT_BACKEND_HAPPY_HARE.md`, `FILAMENT_BACKEND_ACE.md`, `FILAMENT_BACKEND_TOOLCHANGER.md`, `FILAMENT_BACKEND_AD5X_IFS.md`, `FILAMENT_BACKEND_CFS.md`, `FILAMENT_BACKEND_QIDI_BOX.md`, `FILAMENT_BACKEND_SNAPMAKER_U1.md` | One leaf per filament backend: protocol, data sources, G-code commands, topology, capability table |
| `FILAMENT_BACKEND_MEDUSAHC.md` | MedusaHC hotend changer. NOT its own backend: it is a klipper-toolchanger printer plus two add-ons (dock sensors that outrank `toolchanger.tool_number`, and a servo feeder). Read with `FILAMENT_BACKEND_TOOLCHANGER.md` |
| `QIDI_BOX_HEATER.md` | QIDI Box PTC heater RE reference: Klipper objects, G-code commands, firmware variants, HelixScreen integration |
| `CREALITY_CFS_INTERNALS.md` | Creality K1-family CFS box-wrapper RE reference: `BOX_*` command semantics, <tn_data.json>, deferred-failure and resume traps, staged loading, serial timeouts. Read before changing anything the CFS backend emits on K1 |
| `FILAMENT_SLOT_METADATA.md` | Internal notes on `FilamentSlotOverrideStore`: per-backend integration, hardware-event clearing, lifetime discipline, local cache, legacy migration. Pair with `../specs/filament_slots.md` for the public wire format. |
| `MULTI_EXTRUDER_TEMPERATURE.md` | Multi-extruder temperature tracking, ExtruderInfo, dynamic subjects |
| `TOOL_ABSTRACTION.md` | ToolState singleton, ToolInfo, tool-to-backend mapping, DetectState |
| `INPUT_SHAPER.md` | Calibration panels, frequency response charts, CSV parser, PID |
| `BELT_TUNER.md` | Pluck-based belt tension tuner: Klipper UDS accel stream, pluck detection, harmonic pitch estimation. **Read its Validation status section first - the feature is green in CI and has never measured a real belt, and its thresholds are circular** |
| `PREPRINT_PREDICTION.md` | ETA prediction engine, phase timing, weighted history |
| `EXCLUDE_OBJECTS.md` | Object exclusion, per-object thumbnails, slicer setup |
| `PRINT_STATE_MACHINE.md` | Print lifecycle state machine: states, transitions, guards, resource lifecycle |
| `PRINT_CONTROL_BUTTONS.md` | PrintControlButtons controller: owned subjects, pure view function, optimistic pending-action machine, 2x1 home widget, panel delegation |
| `PRINT_START_PROFILES.md` | Print start phase detection, JSON profiles |
| `PRINT_START_OBSERVERS.md` | The whole pre-print observer system: arming, the five signal sources (console, probe lines, bed-mesh flap, toolhead position, fallbacks), threading/lifetime rules, and which tests pin what |
| `PRINT_START_INTEGRATION.md` | User-facing macro setup for print start tracking |
| `Z_OFFSET_PERSISTENCE.md` | Firmware that stores the z-offset outside `gcode_move` and zeroes the live one between prints (ZMOD on AD5M/AD5X): why the idle reading lies, the `persisted_z_offset` subjects, the relative-vs-absolute `SET_GCODE_OFFSET` rule, and the one-row recipe for adding a firmware |
| `POWER_LOSS_RECOVERY.md` | Resume-after-power-loss: the passive Snapmaker backend vs the **active, side-effectful** Creality probe, capability detection via `print_stats.power_loss` presence, and the mandatory probe-before-resume safety invariant |
| `UPDATE_SYSTEM.md` | Update channels (stable/beta/dev), R2 CDN, Moonraker updater |
| `SOUND_SYSTEM.md` | Audio architecture, JSON themes, backends (SDL, ALSA, PWM, M300). User guide: `../user/guide/settings/display-sound.md#sound` |
| `LED_CONTROL.md` | LED control system: 5 backends, auto-state lighting, control/settings overlays, home panel widget |
| `CHAMBER_HEATER.md` | Chamber heaters: backend registry (generic/dragonbreath/panda_breath), discovery, diagnostics subjects + card, ceiling rules, arbitration, verification logs |
| `PRINTER_MANAGER.md` | Printer overlay, custom images, inline name editing |
| `MULTI_PRINTER.md` | Multi-printer management: config v4, soft restart, printer switching |
| `TIMELAPSE.md` | Moonraker timelapse plugin integration |
| `CRASH_REPORTER.md` | Crash reporter: detection, delivery pipeline, CF Worker, modal UI |
| `CONFIG_MIGRATION.md` | Versioned config migration: adding new migrations, testing |
| `STANDARD_MACROS_SPEC.md` | Standard macro specifications |
| `MACROS_PANEL.md` | Macros panel architecture, parameter handling, home panel widgets |

## Platform & Deployment

| Doc | When to read |
|-----|-------------|
| `INSTALLER.md` | Installation system, KIAUH extension, shell tests (bats) |
| `printers/CREALITY_K1_SUPPORT.md` | Creality K1 series platform (K1, K1C, K1 Max) |
| `printers/QIDI_SUPPORT.md` | QIDI platform (Q2 + Max 4 on-device; Plus 4 + older 3-series TJC models are remote-only) |
| `printers/SNAPMAKER_U1_SUPPORT.md` | Snapmaker U1 toolchanger platform |
| `printers/CREALITY_K2_SUPPORT.md` | Creality K2 series platform |
| `printers/FLASHFORGE_AD5X_SUPPORT.md` | FlashForge Adventurer 5X (MIPS, ZMOD) |
| `YOCTO_BUILD.md` | Building HelixScreen as a Yocto recipe |
| `LAN_CLIENT_AUTHORIZATION.md` | Firmware-brokered LAN pairing: firmwares that ask the printer's own screen to approve a slicer or phone app (Snapmaker Orca / Snapmaker App on a U1). Protocol, the no-capability-gate design, and the traps |
| `AD5M_KMOD_VARIANT.md` | Building HelixScreen as a native variant inside the AD5M Klipper Mod firmware |
| `plans/ESP32_NATIVE_AUDIT.md` | ESP32-S3 (BTT K-Touch) native-port feasibility audit — memory/flash/render budgets behind the `firmware/` port |
| `ENVIRONMENT_VARIABLES.md` | All runtime and build env vars |

## Integration

| Doc | When to read |
|-----|-------------|
| `MOONRAKER_ARCHITECTURE.md` | Moonraker API abstraction, WebSocket integration |
| `RPC_ERROR_OWNERSHIP.md` | Who reports a failed JSON-RPC call: the caller's UI, the tracker's generic fallback, or `GcodeErrorRouter`'s `!!` broadcast. Read before adding an `on_error` to any gcode send |
| `PLUGIN_DEVELOPMENT.md` | Plugin API, lifecycle, UI injection, threading, examples |
| `TELEMETRY_ADMIN.md` | Telemetry pipeline, Analytics Engine, dashboard, scripts, secrets |

## Planning & Research

| Doc | When to read |
|-----|-------------|
| `plans/` | The single tracked home for in-flight plans and specs — **point-in-time, not current truth.** Scaffolding, deleted in the same change that ships the work (lifecycle convention: `../CLAUDE.md`). A plan records what was intended when it was written; several prescribe approaches the shipped code has since diverged from, and they read as instructions. Verify every predicate against the code before following one. Live example: `plans/2026-06-25-ad5x-ifs-seated-chan-robustness.md:118-120` tells you to gate on `head_filament_`, which `include/ams_backend_ad5x_ifs.h:80-83` now documents as untrustworthy on its own — the shipped gate is `head_switch_seen_ && !head_switch_present_`. |
| `printer-research/` | Printer-specific research notes |
| `printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md` | AD5X IFS protocol reverse engineering |
| `printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md` | Kobra S1 + ACE Pro real-log analysis: mainline-Python Klipper fork path (`[ace_status]`), command surface, inventory model |

## Reference

| Doc | When to read |
|-----|-------------|
| `LVGL9_XML_ATTRIBUTES_REFERENCE.md` | Complete XML attribute reference |
| `LVGL9_XML_CHEATSHEET.html` | Quick XML cheatsheet (HTML) |
| `HELIX_XML_FORK.md` | **Read before touching `lib/helix-xml/`** — fork origin (`a15dcbeb5`), MIT licensing position, why there is no upstream, the clean-room rule for anything LVGL Pro also has, and the upstream feature gap analysis |
| `SLOT_COMPONENT_DESIGNS.md` | Two unbuilt XML-deduplication proposals (network state icons, capability-gated setting rows), what the other two designs turned into, and the **measured** limits of the `lv_xml_expr.c` evaluator: integer-only, so string formatting cannot move to XML formulas. Nothing here has shipped |
| `FLAG_ICONS_SOURCE.md` | Flag icon asset sources |
