# docs/devel/CLAUDE.md — Developer Documentation

All developer documentation lives here. When working on features, look up the relevant doc before guessing.

## Core Development

| Doc | When to read |
|-----|-------------|
| `DEVELOPMENT.md` | Build setup, dev environment, contributing |
| `HELIXCTL.md` | Driving the UI / screenshots via `helix-screen ctl`. **Read the socket-isolation box first** — a bare `ctl` drives whichever instance started first and still reports success |
| `ARCHITECTURE.md` | The 15-minute whole-app model (XML → Subjects → C++) + the routing table into the chapter series. Start here for "how does the app fit together" |
| `architecture/` | The 17-chapter architecture guide — one subsystem per chapter, ~1 hour each. `architecture/README.md` is the "I want to work on..." index |
| `THREADING.md` | **Single source of truth** for threading, async-callback, and object-lifetime rules. Read before any code that crosses a thread boundary, observes a subject, or destroys a widget |
| `BRANCHING.md` | The three long-lived branches, which way fixes flow between them, which one holds the beta channel, and the `RELEASE_CHANNEL` rule that keeps a trunk tag off the stable channel |
| `BUILD_SYSTEM.md` | Makefile internals, make target reference, cross-compilation, worktree workflow, ccache, patches |
| `SHARED_TREE.md` | Working beside other sessions: the `helix-claim` manual, what the git hooks do, committing in the shared tree, reading a tree someone else may be mid-merge in, colliding builds |
| `SCRIPTS_REFERENCE.md` | Full per-script notes behind the one-line `scripts/CLAUDE.md` index: flags, exit codes, opt-out markers, where each script's tests live |
| `REVIEW_RUBRIC.md` | The quality bar for reviews: crash families, silent-failure traps, what not to flag, what the gates already cover |
| `../../scripts/CLAUDE.md` | Index of `scripts/` — installer, release, asset regeneration, and the "Quality & Auditing" gate table covering every `check_*.py` lint and what it enforces |
| `TESTING.md` | Catch2 test infrastructure, test patterns |
| `MOCK_ENVIRONMENT_VARIABLES.md` | The `HELIX_MOCK_*` matrix, replay scripts, forced-modal and demo-injection knobs - every var that shapes a `--test` run. Runtime/display/logging vars stay in `ENVIRONMENT_VARIABLES.md` |
| `HIDDEN_TESTS_TRACKER.md` | Tests hidden from the default run (`[.]`), why each is hidden, and how to run them |
| `LOGGING.md` | spdlog levels, when to use info vs debug vs trace |
| `COPYRIGHT_HEADERS.md` | SPDX license headers |
| `RELEASE_PROCESS.md` | Release workflow, versioning |
| `CHANGELOG_STYLE.md` | How `CHANGELOG.md` entries are written: user-facing voice, hyphen separator, bare `(#N)` links, daily vs milestone shapes. Read before drafting a release's changelog section |
| `CHANGELOG_1_1_DRAFT.md` | Running 1.1 release notes for `main`; becomes the `## [1.1]` entry at release, then is deleted |
| `CI_CD_GUIDE.md` | CI pipeline, GitHub Actions |
| `ANDROID_PLAY_STORE.md` | Play Store publishing pipeline, one-time setup, promotion flow |
| `ANDROID_ASSETS.md` | How assets reach the APK and on-device storage. Read before touching `android/app/src/main/assets/` (a Gradle output, not source) |

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
| `COLOR_SWATCHES.md` | Any colour swatch or dot: the one painter (`ui_swatch.h`), grids, finishes, the hand-rolled-swatch gate |
| `THEME_SYSTEM.md` | Theme internals: ThemeManager style architecture, StyleRole roles, adding themed widgets, Glass appearance (families, `glass_mark`, the hardware gate) |
| `THEME_CONTRIBUTOR_GUIDE.md` | For people **creating themes** — JSON schema, palette design, testing. No C++ needed. |
| `LAYOUT_SYSTEM.md` | LayoutManager and the home widget grid: sizing, `default_layout.json` anchors, widget spans |
| `HOME_EDIT_MODE.md` | Home-grid edit mode gestures and paging. Read before touching `GridEditMode` or HomePanel's edit handlers |
| `PANEL_WIDGET_GUIDE.md` | Authoring a home widget: registry, class, XML, the size ladder and the checklist sized widgets pass; full-page widgets |
| `PLUGIN_DEVELOPMENT.md` | Writing a Lua plugin: manifest, XML rules, the helix.* API, permissions, limits, debugging |
| `PLUGIN_TUTORIAL.md` | Step-by-step build of the maintenance-meter widget plugin: annotated code, the why of each decision, the adaptive size ladder |
| `HELIXPRINT_PLUGIN.md` | The HelixPrint Moonraker plugin and the app's feature gating; read before adding or gating a plugin feature. User guide: `../user/guide/helixprint-plugin.md` |
| `PAGE_SCROLL_BUTTONS.md` | Chevron page-scroll gutter: where it auto-attaches and why it stops at a home widget tile. On by default on ESP32 only |
| `TRANSLATION_SYSTEM.md` | i18n: YAML strings -> code generation -> runtime lookups |
| `TRANSLATION_CONTRIBUTOR_GUIDE.md` | For **translators** — how to improve existing translations or add a new language. No code needed. |
| `UI_TESTING.md` | Headless LVGL testing, UITest utilities |
| `architecture/16-gcode-pipeline.md` | G-code from file to screen: parse/scan paths, footer fast path, tools-used cache, render modes, object picking. Start here for viewer work |
| `GCODE_VIEWER_CONFIG.md` | GCode viewer configuration |
| `BED_MESH_RENDERING_INTERNALS.md` | Bed mesh 3D rendering internals, the thread-free static mode with its sticky 2D fallback, and `BedMeshState` / `compute_mesh_stats` |
| `PERFORMANCE.md` | Measured CPU/RAM per board and what wakes the main loop. Read before claiming a CPU or RAM figure or touching timer periods |
| `GPU_ACCELERATION.md` | Measured GPU vs software rendering per board. Read before proposing rendering-path work |
| `FILAMENT_PATH_CANVAS.md` | Filament-path canvas: 3-layer model, pathgeo arc-fillet routing, shared tube stroker, RenderCtx phases, topology renderers (linear/hub/parallel/mixed) |
| `PRE_RENDERED_IMAGES.md` | Pre-rendered image pipeline |
| `GESTURE_RECOGNITION.md` | Pinch zoom and two-finger pan: LVGL recognizers, view_gestures, per-view mapping |
| `GALLERY.md` | Every app screenshot the docs show, grouped by area. Generated: `python3 scripts/gen_gallery.py` from `docs/images/doc-screenshots.yml` |

## Feature Systems

| Doc | When to read |
|-----|-------------|
| `LABEL_PRINTER_SYSTEM.md` | Label printing: Brother QL, Phomemo, Niimbot, MakeID protocols; USB/TCP/Bluetooth transports |
| `FILAMENT_MANAGEMENT.md` | Filament system hub: multi-backend architecture, slot metadata, filament-op dispatch, endless spool, UI panels, dryer commands, device ops, mock mode, add-a-backend guide |
| `FILAMENT_ENVIRONMENT_ZONES.md` | Filament boxes and dryers: the `EnvironmentZone` model, discovery, what a backend publishes to get a box on screen |
| `FILAMENT_BACKEND_AFC.md`, `FILAMENT_BACKEND_OPENAMS.md`, `FILAMENT_BACKEND_HAPPY_HARE.md`, `FILAMENT_BACKEND_ACE.md`, `FILAMENT_BACKEND_TOOLCHANGER.md`, `FILAMENT_BACKEND_AD5X_IFS.md`, `FILAMENT_BACKEND_CFS.md`, `FILAMENT_BACKEND_QIDI_BOX.md`, `FILAMENT_BACKEND_SNAPMAKER_U1.md`, `FILAMENT_BACKEND_MULTIACE.md`, `FILAMENT_BACKEND_INDX.md`, `FILAMENT_BACKEND_LANE_STATUS.md` (any lane-status-spec RFC 0001 producer) | One leaf per filament backend: protocol, data sources, G-code commands, topology, capability table |
| `FILAMENT_BACKEND_MEDUSAHC.md` | MedusaHC hotend changer (not its own backend: `AmsBackendToolChanger` drives it). Read with `FILAMENT_BACKEND_TOOLCHANGER.md` |
| `printer-research/QIDI_BOX_HEATER.md` | QIDI Box PTC heater RE reference: Klipper objects, G-code commands, firmware variants, HelixScreen integration |
| `printer-research/CREALITY_CFS_K2_INTERNALS.md` | Creality K2 CFS module RE reference: box.cfg, `box` schema variants and field encodings, `BOX_*`/`M8200` commands, error codes, runout and auto-refill, the community Kalico port |
| `printer-research/CREALITY_CFS_K1_INTERNALS.md` | K1-family CFS `BOX_*` command reference and traps. Read before changing anything the CFS backend emits on K1 |
| `FILAMENT_SLOT_METADATA.md` | The lane record store (RFC 0001 shape, writer rule, editing gate). Pair with `../specs/filament_slots.md` for the wire format |
| `MULTI_EXTRUDER_TEMPERATURE.md` | Multi-extruder temperature tracking, ExtruderInfo, dynamic subjects |
| `TOOL_ABSTRACTION.md` | ToolState singleton, ToolInfo, tool-to-backend mapping, DetectState |
| `INPUT_SHAPER.md` | Calibration panels, frequency response charts, CSV parser, PID |
| `PRESSURE_ADVANCE_TUNING.md` | Pressure advance tuning by eye: tower, ramp, band picker, the Moonraker key slicers read |
| `BELT_TUNER.md` | Pluck-based belt tuner. **Read its Validation status first: green in CI, never measured a real belt, thresholds circular** |
| `BELT_TENSION.md` | Belt Tension: `TEST_RESONANCES` sweep comparison, the Start gate, the guided tuning loop, mock knobs |
| `PREPRINT_PREDICTION.md` | ETA prediction engine, phase timing, weighted history |
| `EXCLUDE_OBJECTS.md` | Object exclusion, per-object thumbnails, slicer setup, the read-only home tile and the map's Mini mode |
| `PRINT_STATE_MACHINE.md` | Print lifecycle state machine: states, transitions, guards, resource lifecycle |
| `PRINT_CONTROL_BUTTONS.md` | PrintControlButtons controller: owned subjects, pure view function, optimistic pending-action machine, 2x1 home widget, panel delegation |
| `PRINT_START_PROFILES.md` | Print-start phase detection: signal sources, JSON profile schema, authoring a profile for a new printer |
| `PRINT_START_INTEGRATION.md` | User-facing macro setup for print start tracking |
| `Z_OFFSET_PERSISTENCE.md` | Firmware that persists z-offset outside `gcode_move` (ZMOD AD5M/AD5X): why the idle reading lies, adding a firmware, Save Z-Offset |
| `POWER_LOSS_RECOVERY.md` | Power-loss resume backends; the Creality probe is **active and side-effectful** and must run before resume. Read before touching PLR |
| `UPDATE_SYSTEM.md` | Update channels (stable/beta/dev), R2 CDN, Moonraker updater |
| `SOUND_SYSTEM.md` | Audio architecture, JSON themes, backends (SDL, ALSA, PWM, M300). User guide: `../user/guide/settings/sound.md` |
| `SCREENSAVERS.md` | Screensavers: the registry, shared overlay/canvas/timer/pixel writer, the CPU gate and stored levels, color depth in builds, adding a saver |
| `LED_CONTROL.md` | LED control system: 5 backends, auto-state lighting, control/settings overlays, home panel widget |
| `SENSOR_MANAGEMENT.md` | Sensors: the hardware table, `SensorManager`, runout vs presence, adding a sensor |
| `COLD_PULL.md` | Guided cold pull: flow, heater runner, entry points, safety rules |
| `MAINTENANCE.md` | Maintenance reminders: the meters and their defaults, per-printer seeding and the database `maintenance` field, calendar limits, when notices fire, the four screens |
| `ACTION_PROMPTS.md` | Klipper `action:prompt_*` / `action:notify` support and adding a directive |
| `BLUETOOTH_SYSTEM.md` | Bluetooth plugin: dlopen ABI, BusThread threading, pairing and data paths, adding an ABI function |
| `USB_MANAGEMENT.md` | USB drives, the print-select USB source, label printers, HID scanners, the `--test` mock drive |
| `CHAMBER_HEATER.md` | Chamber heaters: backend registry (generic/dragonbreath/panda_breath), discovery, diagnostics subjects + card, ceiling rules, arbitration, verification logs, chamber air modes |
| `BED_DRYING.md` | Drying filament on the heated bed: the enclosure capability, the flow, the spools-on-the-bed latch and its five send-layer choke points, persistence across restarts and power loss |
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
| `printers/README.md` | Index of the per-printer support docs, and which shipped targets still lack one |
| `printers/FLASHFORGE_AD5M_SUPPORT.md` | FlashForge Adventurer 5M / Pro: Forge-X, Klipper Mod and ZMOD, the static `ad5m` and buildroot `ad5m-br` targets, the Allwinner backlight quirks |
| `printers/CREALITY_K1_SUPPORT.md` | Creality K1 series platform (K1, K1C, K1 Max) |
| `printers/QIDI_SUPPORT.md` | QIDI platform (Q2 + Max 4 on-device; Plus 4 + older 3-series TJC models are remote-only) |
| `printers/SNAPMAKER_U1_SUPPORT.md` | Snapmaker U1 toolchanger platform |
| `printers/CREALITY_K2_SUPPORT.md` | Creality K2 series platform |
| `printers/FLASHFORGE_AD5X_SUPPORT.md` | FlashForge Adventurer 5X (MIPS, ZMOD) |
| `printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md` | FlashForge Creator 5 line (MIPS): firmwares, chamber-heater model split, Z-Mod tool changer, mock persona |
| `printers/ELEGOO_CENTAURI_CARBON_YOCTO.md` | Elegoo Centauri Carbon: the `cc1` and `yocto` targets, and the local Yocto (OpenCentauri COSMOS) dev loop |
| `REMOTE_SCREEN.md` | The built-in screen server (#1825): frame capture, the HTTP routes and stream, remote touch through the synthetic pointer, pairing and the nginx security model, the Moonraker webcam entry |
| `LAN_CLIENT_AUTHORIZATION.md` | Firmware-brokered LAN pairing (printer screen approves a slicer/app, e.g. Snapmaker U1): protocol and traps |
| `architecture/17-esp32-firmware.md` | ESP32-S3 (K-Touch) firmware architecture. Read before touching `firmware/helixscreen-esp32/` |
| `ESP32_PORT.md` | Hands-on K-Touch firmware guide: what a shared `src/` change must respect, build and flash recipe, `sdkconfig.local` options, serial console, debugging table, gates |
| `ESP32_NATIVE_AUDIT.md` | ESP32-S3 native-port feasibility audit: the memory, flash and render measurements behind the port's feature gates |
| `ENVIRONMENT_VARIABLES.md` | All runtime and build env vars |

## Integration

| Doc | When to read |
|-----|-------------|
| `MOONRAKER_ARCHITECTURE.md` | Moonraker API abstraction, WebSocket integration |
| `RPC_ERROR_OWNERSHIP.md` | Who reports a failed JSON-RPC call: the caller's UI, the tracker's generic fallback, or `GcodeErrorRouter`'s `!!` broadcast. Read before adding an `on_error` to any gcode send |
| `TELEMETRY_ADMIN.md` | Telemetry pipeline, Analytics Engine, dashboard, scripts, secrets |

## Planning & Research

| Doc | When to read |
|-----|-------------|
| `plans/` | In-flight plans and specs, **point-in-time, not current truth**; deleted when the work ships (`../CLAUDE.md`). Verify every claim against the code before following one |
| `printer-research/README.md` | Printer research index, plus the overview matrix of every printer's SoC, panel, firmware, target and support status |
| `printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md` | AD5X IFS protocol reverse engineering |
| `printer-research/FLASHFORGE_CREATOR5_FIRMWARE.md` | Creator 5 stock firmware RE: decryption, image layout, stock UI features vs HelixScreen |
| `printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md` | Kobra S1 + ACE Pro real-log analysis: the ACEPRO driver on the `klipper-kobra-s1` firmware (`[ace_status]`), command surface, inventory model |

## Reference

| Doc | When to read |
|-----|-------------|
| `LVGL9_XML_ATTRIBUTES_REFERENCE.md` | Complete XML attribute reference |
| `LVGL9_XML_CHEATSHEET.html` | Quick XML cheatsheet (HTML) |
| `HELIX_XML_FORK.md` | **Read before touching `lib/helix-xml/`**: origin, licensing, the clean-room rule for anything LVGL Pro has |
| `SLOT_COMPONENT_DESIGNS.md` | Unbuilt XML-dedup proposals and the measured limits of the `lv_xml_expr.c` evaluator (integer-only). Nothing here has shipped |
| `FLAG_ICONS_SOURCE.md` | Flag icon asset sources |
