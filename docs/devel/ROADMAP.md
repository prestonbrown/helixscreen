# HelixScreen Development Roadmap

**Last Updated:** 2026-08-20 | **Status:** 1.0 (active)

---

## Project Status

| Area | Status |
|------|--------|
| **Production UI** | 30+ panels, 19+ overlays, 13+ modals, 340+ XML layouts |
| **First-Run Wizard** | 13-step guided setup (touch cal, WiFi, probe, input shaper, telemetry) |
| **Moonraker API** | 116 methods, abstraction boundary enforced |
| **Multi-Material (AMS)** | 7 backends, multi-unit, multi-backend, error visualization |
| **Tool Abstraction** | ToolState with tool-backend mapping, multi-extruder temps |
| **Spoolman** | 23 API methods, full CRUD, Spool Wizard, virtualized list with search |
| **Plugin System** | Core infrastructure complete |
| **Test Suite** | 1,050+ test files, 12,200+ Catch2 test cases, 8,500+ sections, 2,008 shell tests |
| **Platforms** | Pi 3/4/5, AD5M, AD5X, K1, K2, QIDI, Snapmaker U1, Centauri Carbon, macOS, Linux |
| **Printer Database** | 94 printer models with auto-detection |
| **Filament Database** | 66 materials with temp/drying/compatibility data |
| **Theme System** | Dynamic JSON themes with live preview |
| **Layout System** | Auto-detection for small (480x320) displays; ultrawide (1920x480) and portrait detection exists but the layouts themselves are alpha |
| **Sound System** | Multi-backend synthesizer (SDL, ALSA, PWM, M300), JSON themes |
| **Telemetry** | Opt-in crash reporting + session analytics + debug bundle upload |

*Volatile counts (XML files, test suite, databases) measured 2026-08-20.*

---

## Recently Completed

### XML `<repeat>` Looping ✅
**Completed:** 2026-07-16

Declarative repetition for the helix-xml engine: `<repeat count="N">…body…</repeat>` expands its body at load time with a zero-based `$i` iteration index, and `${name}` embedded composition self-wires repeated widgets to indexed subjects (`bind_text="demo_${i}_v"` → `demo_0_v`, `demo_1_v`, …). A subject-bound `count` (`count="row_count"`) reactively rebuilds the expansion when the subject changes, via async off-tree teardown (no synchronous deletion inside the count observer). Replaces C++ create-and-wire loops for fixed-N and subject-bound-count widget lists. Live demo in the test panel (`ui_xml/test_panel.xml`, "XML Repeat Demo" section).

**Docs:** `docs/devel/LVGL9_XML_GUIDE.md` § "Repeating fragments with `<repeat>`"

### XML Engine Extraction & LVGL 9.5 Upgrade ✅
**Completed:** 2026-02-18

Extracted the LVGL XML engine into standalone `lib/helix-xml/` library and upgraded LVGL from 9.4-pre to v9.5.0, gaining 274 commits of improvements (blur, drop shadow, flex rounding fixes, memory leak fixes, gesture threshold API, slot support). XML patches baked permanently into helix-xml; LVGL patches regenerated for v9.5. New umbrella headers (`helix_xml.h`), standalone globals, and forward declaration files decouple helix-xml from LVGL internals.

**Branch:** `feature/helix-xml`

### Power Panel Integration ✅
**Completed:** 2026-02-18

Home panel quick-toggle button for power devices (tap to toggle, long-press for full panel), Advanced panel entry with conditional visibility via `power_device_count` capability subject, device selection chips for choosing which devices the home button controls, and auto-discovery of Moonraker power devices on connect.

**Files:** `ui_panel_power.cpp`, `ui_panel_home.cpp`, `printer_capabilities_state.cpp`

### Multi-Unit AMS ✅
**Completed:** 2026-02-17

Multi-unit AMS overview panel, shared detail components (DRY refactor), per-slot error visualization with buffer health, AFC/Happy Hare device management overlays, mock backend deduplication via shared defaults modules. Visual verification complete.

**Branch:** `feature/multi-unit-ams` (76 commits, 146 files, +34k/-14k lines)

### Debug Bundle Upload ✅
**Completed:** 2026-02-16

Support diagnostics system for remote troubleshooting:
- **Debug bundle collector** gathers logs, crash data, config, and system info
- **Upload to Cloudflare Worker** endpoint for support team access
- **Modal UI** with progress feedback in Settings panel
- 193 unit tests

**Files:** `debug_bundle_collector.cpp`, `ui_debug_bundle_modal.cpp`

### Probe Management Overlay ✅
**Completed:** 2026-02-15

Probe configuration and management overlay in Advanced panel:
- **Auto-detection** of probe types: Cartographer, Beacon, Tap, Klicky, BLTouch, eddy current
- **BLTouch panel** with live Z-offset adjustment
- **First-run wizard step** for probe sensor selection
- Hidden when no probe detected

**Files:** `ui_probe_overlay.cpp`, `ui_xml/probe_overlay.xml`, `ui_xml/probe_bltouch_panel.xml`

### Active Extruder Temperature Tracking ✅
**Completed:** 2026-02-16

Unified temperature tracking for multi-extruder/multi-tool printers:
- Dynamic nozzle label showing active tool number
- `PrinterTemperatureState` tracks active extruder across tool changes
- Integrated with LED auto-state and telemetry

### Z-Axis Direction Flip Toggle ✅
**Completed:** 2026-02-15

Settings option to invert Z movement buttons for printers where the auto-detection heuristic gets it wrong.

### Hardware Discovery Improvements ✅
**Completed:** 2026-02-16

Skip expected hardware in new discovery check — reduces false-positive "new hardware detected" prompts after initial wizard setup.

---

## Current Priorities

### 1. Plugin Ecosystem

**Status:** Core infrastructure complete, expanding ecosystem

The plugin system launched with version checking, UI injection points, and async execution.

**Next steps:**
- [ ] LED Effects plugin → production quality
- [ ] Additional plugin examples for community
- [ ] Plugin documentation refinement

**Files:** `src/plugin/plugin_manager.cpp`, `docs/devel/PLUGIN_DEVELOPMENT.md`

### 2. Production Hardening

**1.0 hardening audit completed 2026-03-28.** 22 items fixed across crash safety, network resilience, config robustness, SHA256 update verification, comprehensive i18n (218 strings wrapped in lv_tr()), UI consistency, and test quality. See git history for details.

Remaining items for production readiness:
- [x] Structured logging with log rotation
- [x] Streaming file operations on AD5M — accepted for 1.0 on field evidence (no crashes reported in this area at the large-file print sizes users are running; an explicit 50MB+ on-device spot-check is deferred post-1.0)

---

## What's Complete

### Core Architecture
- LVGL 9.5 with declarative XML layouts via `lib/helix-xml/` (346 XML files)
- Reactive Subject-Observer data binding
- Design token system (no hardcoded colors/spacing)
- RAII lifecycle management (PanelBase, ObserverGuard, SubscriptionGuard)
- **Dynamic theme system** with JSON themes, live preview, and theme editor
- **Layout system** with auto-detection for ultrawide, portrait, and small displays (the ultrawide and portrait *layouts* are alpha — detection and grid sizing only, no panel overrides)
- Seven responsive breakpoint tiers (micro through xxlarge)
- Observer factory pattern (`observe_int_sync`, `observe_string_async`, etc.)
- **Versioned config migration** for seamless upgrades between releases
- **Moonraker API abstraction boundary** — 116 methods, UI decoupled from WebSocket layer
- **Modal system** standardized via `ui_dialog` + `modal_button_row` components
- **God class decomposition** — PrinterState into 13 domain classes, SettingsPanel into 5 overlays, PrintStatusPanel into 8 components

### Panels & Features
- **30 Production Panels:** Home, Controls, Motion, Print Status, Print Select, Settings, Advanced, Macros, Console, Power, Print History, Spoolman, AMS, AMS Overview, Bed Mesh, PID Calibration, Z-Offset, Screws Tilt, Input Shaper, Extrusion, Filament, Temperature, and more
- **19 Overlays:** WiFi, Timelapse Settings, Firmware Retraction, Machine Limits, Fan Control, Exclude Object, Print Tune, Theme Editor, AMS Device Operations, AMS Section Detail, AMS Spoolman, Network Settings, Touch Calibration, Printer Manager, Printer Image, LED Control, Probe Management, and more
- **13 Modals:** AMS Edit, AMS Loading Error, Change Host, Crash Report, Debug Bundle, Exclude Object, Plugin Install, Print Cancel, Runout Guidance, Save Z-Offset, Spoolman Edit, Action Prompt
- **First-Run Wizard:** Touch Cal → Language → WiFi → Moonraker → Printer ID → Heaters → Fans → AMS → LEDs → Filament Sensors → Probe Sensors → Input Shaper → Summary (13 steps, conditional skipping)
- **Calibration Workflows:** PID tuning (live graph, fan control, material presets), Z-offset with live adjust, Screws Tilt, Input Shaper (frequency response charts, CSV parser, per-axis results)
- **Bed Mesh:** 3D visualization with touch rotation, profile switching, 38 FPS optimized rendering
- **Sound system:** Multi-backend audio (SDL, ALSA, PWM, M300) with JSON themes and volume control
- **Timelapse:** Plugin detection, install wizard, settings UI, real-time event handling, render progress, video management
- **Filament tracking:** Live consumption during printing, slicer estimate on completion
- **Display rotation:** Support for 0/90/180/270 across all binaries
- **Camera/Webcam:** Live MJPEG streaming — home/status-panel camera tiles (`CameraWidget`) plus a standalone fullscreen viewer from Settings → Hardware & Devices (shown only when `printer_has_webcam`), config modal for stream URLs (`src/system/camera_stream.cpp`, `src/ui/panel_widgets/camera_widget.cpp`, `src/ui/modals/camera_config_modal.cpp`)
- **Telemetry:** Opt-in crash reporting, session analytics, and debug bundle upload via Cloudflare Worker backend
- **Pre-print ETA prediction** using weighted-average historical timing data
- **Exclude objects** with object list overlay, thumbnails, and confirmation flow
- **Markdown viewer** (`ui_markdown`) with theme-aware rendering and subject binding
- **Custom printer images** with inline name editing in printer manager overlay
- **Probe management** with auto-detection for Cartographer, Beacon, Tap, Klicky, BLTouch, eddy current
- **Active extruder tracking** with dynamic nozzle labels for multi-tool printers
- **Z-axis direction flip** toggle for inverted motion printers

### Multi-Material (AMS) & Tool Abstraction

**7 backend implementations** supporting diverse hardware:

| Backend | Hardware | Topology | Slots | Key Capabilities |
|---------|----------|----------|-------|-----------------|
| **Happy Hare** | ERCF, Tradrack, 3MS, Night Owl | Linear | 1-16 | Tool mapping, bypass, runtime-editable endless spool (single-unit), lane eject, `MMU_HEATER` dryer |
| **AFC** | Box Turtle, ViViD (AFC-Klipper-Add-On) | Hub | 1-16 | Editable endless spool, auto-heat on load, buffer health, 12+ device actions |
| **Tool Changer** | viesturz/klipper-toolchanger | Parallel | 1-16 | Mounted/detect state, per-tool filament systems |
| **ACE** | Anycubic ACE Pro (ValgACE/BunnyACE/DuckACE) | Hub | 4 | Integrated dryer control (temp/duration/fan), REST polling |
| **AD5X IFS** | FlashForge Adventurer 5X | Hub | 4 | Intelligent Filament Switching, ZMOD firmware |
| **CFS** | Creality K2 series (RS-485) | Hub | 4-20 | Creality Filament System, multi-unit |
| **Snapmaker U1** | Snapmaker U1 SnapSwap toolchanger | Parallel | 4 | RFID spool recognition, four-head tool switching |
| **Mock** | Development simulation | All | Config | Simulates all backend types with realistic multi-phase operations |

**Multi-backend architecture:**
- Multiple concurrent filament systems per printer (e.g., tool changer + AFC)
- Per-backend event routing and subject storage
- Tool-to-backend mapping via `ToolInfo.backend_index`

**Multi-unit AMS:**
- Overview panel with grid of unit cards (mini slot bars, hub sensor dots, error badges)
- Inline detail view with zoom transition
- Per-slot error indicators with severity (info/warning/error)
- Buffer health monitoring with fault proximity visualization (AFC)

**Tool abstraction layer (ToolState):**
- `ToolInfo` struct: offsets, extruder/heater/fan names, backend mapping, detect state
- Active tool tracking with dynamic nozzle labels for multi-tool printers
- Multi-extruder temperature with per-extruder subjects and selector UI

**UI components (16 XML files):**
- **AMS Panel** — Slot grid, path canvas (spool → hub → toolhead → nozzle), backend selector
- **AMS Overview Panel** — Multi-unit grid with inline detail
- **Device Operations Overlay** — Dynamic backend-specific sections (AFC: 7 sections, 6 when tip forming is not the active tip method; Happy Hare: 5 sections)
- **Context Menu** — Per-slot actions: load, unload, edit, Spoolman assign, tool mapping, endless spool backup
- **Spool Wizard** — 3-step creation: Vendor → Filament → Spool Details with modal forms for new vendors/filaments
- **Spoolman Panel** — Browse, search, edit, delete with virtualized list and context menus
- **AMS Mini Status** — Home panel widget, click to open
- Modals: AMS Edit (color/material/brand), Loading Error, Dryer (temp/duration presets)

**Spoolman integration:**
- 23 API methods: full CRUD for spools, filaments, vendors + external catalog
- Auto-active spool sync on slot load
- Weight polling with reference counting
- Material compatibility validation with toast warnings

**Also:**
- Spool visualization with 3D-style gradients and animations
- Print color requirements display from G-code metadata
- External spool bypass support
- 15 dedicated test files covering all backends, multi-unit, tool mapping, endless spool, device actions

### Filament Database
- **66 materials** with temperature ranges, drying parameters, density, compatibility groups
- 12 compatibility groups (PLA, PETG, ABS_ASA, PA, TPU, PC, HIGH_TEMP, PP, PE, CoPE, EVA, SBS)
- Material alias resolution ("Nylon" → "PA", "Polycarbonate" → "PC")
- Dryer presets dropdown populated from database
- Endless spool material validation

### Plugin System
- Dynamic plugin loading with version compatibility checking
- UI injection points for extensibility
- Thread-safe async plugin execution
- Settings UI for plugin discovery and management
- LED Effects proof-of-concept plugin

### Moonraker Integration
- WebSocket client with auto-reconnection
- JSON-RPC protocol with timeout management
- 116 API methods: print control, motion, heaters, fans, LEDs, power devices, print history, timelapse, screws tilt, firmware retraction, machine limits, Spoolman (full CRUD), input shaper, probe, database, and more
- Full mock backend for development without real printer

### Installer & Deployment
- **KIAUH extension** for one-click install
- **Bundled uninstaller** (`install.sh --uninstall`) with previous UI restoration
- **2,008 shell tests** across 116 BATS test files
- **Installer pre-flight checks** for Klipper/Moonraker on AD5M and K1
- **QIDI & Snapmaker U1** platform support

### Build System
- Parallel builds (`make -j`)
- Docker cross-compilation for Pi (aarch64) and AD5M (armv7-a)
- Pre-commit hooks (clang-format, quality checks)
- CI/CD with GitHub Actions
- Icon font generation with validation
- Incremental compile_commands.json generation for LSP

---

## Backlog (Lower Priority)

| Feature | Effort | Notes |
|---------|--------|-------|
| **Belt tension visualization** | Future | Accelerometer-based CoreXY belt comparison; reuses frequency chart |
| **OTA updates** | Future | UpdateChecker downloads + installs; needs auto-apply without user interaction |
| ~~Update hash verification~~ | ~~Low~~ | ✅ Done — SHA256 verified from R2 manifest before install, graceful skip on GitHub fallback |
| **Pre-migration config backup** | Low | Snapshot config *before* running versioned migrations, cleanup on success. Not yet built — `helix::config_backup::write_rolling_backup()` (`src/system/config_backup.cpp`) provides rolling primary+fallback backups on every save, but no migration-scoped snapshot exists. |
| **Printer DB schema validation** | Low | Validate required fields in printer_database.json entries, detect duplicate IDs |
| **Text overflow audit** | Medium | 254/346 XML files carry no `long_mode` truncation handling (2026-08-20); long translated strings can overflow |
| ~~Breakpoint coverage~~ | ~~Medium~~ | ✅ Done — 87 XML files now reference breakpoints/`min_width` (2026-08-20; the old "~10" count predated the tier rollout) |
| **UI test coverage** | High | Add panel interaction tests — the old 13% ratio is stale, `src/` has since grown to ~700 .cpp files |
| **480x320 follow-through** | Medium | Surviving worklist from the 480x320 audit: print-select list rows hardcode an ~880px minimum width (`ui_xml/print_file_list_row.xml`), sensors-overlay dropdowns pin `min_width="250"`, and ten panels carry no tiny-tier refs at all |
| **Power consumption display** | Medium | Tasmota/Mainsail-style energy monitoring: voltage, current, power, energy usage for power devices |
| **Plugin system tests** | Medium | Only mock tests exist; add real plugin load/unload/injection tests |
| **Error-path integration tests** | High | Disconnect mid-print, settings corruption recovery, AMS hardware desync |
| **Remove libinput dependency** | Medium | Refactor to use direct evdev for all input; eliminates libinput build/link complexity across cross-compile targets |
| **Missing docs** | Medium | SENSOR_MANAGEMENT, GCODE_RENDERING_ARCHITECTURE, ACTION_PROMPTS, BLUETOOTH_SYSTEM, USB_MANAGEMENT |
| **2D renderer crease/edge shading** | Future | Per-segment surface-normal estimate from inter-layer contours; darken where normals break sharply (creases, ridges) for a 3D look without the 3D renderer. Needs a post-parse pass building per-layer contour maps |
| **Safe mode on crash loop** | Medium | Crash-loop detection (3+ in 120s, `Application::run()`) only resets the counter today — act on it: skip custom panel widget config, skip plugins, or boot a minimal safe-mode UI with a banner |
| **Declarative `<subjects>` in XML** | Medium | Engine capability (helix-xml): purely presentational state (accordion expand, tab index, toggle visibility) declared in XML without a backing C++ class; C++ only for real printer/system data |
| **libhv logging via spdlog** | Low | Route libhv's own log output into the spdlog pipeline |

### Hardware verification owed

Shipped against mock/static analysis only — each needs a pass on real hardware before it can be called verified:

- **QIDI Box stock-firmware path** — destubbed load/unload sequence (#1041) ships blind; Camden to verify the full automated sequence on Q2 + Box stock firmware
- **CB1 DRM-DPMS idle power-off** (#1049) — in-process connector DPMS for no-backlight devices shipped, never verified on the CB1 Allwinner-HDMI rig it targets
- **AMS backend error-recovery (Happy Hare slice) + status-error-center bridge** — shipped blind (no HH/QIDI/AD5X hardware); only the U1 slice was verified live
- **AD5X IFS seated-chan Fix 4** — never shipped (single "Unload current" affordance for the unknown-seated state); the seated-chan fixes that did ship have no AD5X rig to verify against

---

## helix-xml Engine

The XML engine lives in its own repository and keeps its roadmap there, on GitHub:

**https://github.com/prestonbrown/helix-xml/issues**

`lib/helix-xml/` in this tree is a submodule pointing at that repo — edit it in place, push from
inside the submodule, then commit the bumped pointer here. It is MIT — a permanent
fork of the engine LVGL removed from core in v9.5 — and it has no upstream. Anything LVGL Pro also
has must be built clean-room from published docs; see `HELIX_XML_FORK.md`.

Currently open:

| Item | Why |
|------|-----|
| [#1 Real `<slot>` declarations](https://github.com/prestonbrown/helix-xml/issues/1) | The current slot support is a name-lookup lookalike whose failure mode reports a misleading "STALE BINARY" error. Blocks `SLOT_COMPONENT_DESIGNS.md` |
| [#2 Multi-argument props (`<param>`)](https://github.com/prestonbrown/helix-xml/issues/2) | Already faked once as the hardcoded `bind_text-fmt`. Gives custom widgets real signatures |
| [#3 `<enumdef>`](https://github.com/prestonbrown/helix-xml/issues/3) | Validates enum attributes on the 37 C++-registered widgets, and gives `tools/xml-linter` something to check |
| Phase-1 resolver refactor (`lv_xml_resolve` purity, arena ownership, dropped bit) | Independent groundwork the since-deleted codegen design assumed; worth doing on its own — the only piece of that design that outlived the doc |

---

## Known Technical Debt

**Resolved (2026-01):**
- ~~PrinterState god class~~ → Decomposed into 13 domain classes
- ~~PrintStatusPanel~~ → Extracted 8 focused components
- ~~SettingsPanel~~ → Extracted 5 overlay components

**Remaining:**
- **Application class** (1249 lines) → Extract bootstrapper and runtime
- **Singleton cascade pattern** → UIPanelContext value object
- **Code duplication** → PanelBase/OverlayBase with RAII subjects (complete)
- **NavigationManager intimacy** → Extract INavigable interface

**Deferred past 1.0 (known, accepted):**
- **God classes** (Application 3.3K LOC, AmsState 3.5K LOC, ThemeManager 3.6K LOC) — real debt, but refactoring pre-1.0 is high risk for regressions. They work.
- **Recursive mutex in AmsState** — ugly pattern but functional with current usage.
- **Shutdown flag races** — theoretical window, mitigated by alive guards in practice.
- **UpdateQueue frozen TOCTOU** — the frozen flag is a safety net that works "well enough"; fixing it is fiddly atomic work for marginal gain.
- **Static subject lifecycle** — documented, understood, tested.

**Planned post-1.0 refactoring (active intent, not yet scheduled):**
- **Dynamic overlay allocation** — migrate existing `ui_overlay_*` and `ui_settings_*` files from the `get_global_*()` + `init_global_*()` singleton pattern to on-demand construction with destroy-on-pop ownership held by NavigationManager. Saves memory on small devices (AD5M 14–20MB budget) and unblocks multi-instantiation. Mechanism unresolved: destroy-on-pop vs memory-pressure eviction, NavigationManager API changes, per-instance event callback adapter pattern. New overlays should not use the singleton pattern; see `YOUR_FIRST_CONTRIBUTION.md` § "Going forward: dynamic overlays".
- **Per-gate / per-slot / per-lane drying control** (#1026) — today the dryer is a single shared control surface with per-unit environment *readout* (temp/humidity). Independently drying specific gates (Happy Hare `MMU_HEATER … GATES=`, per-gate `drying_state` array, per-gate countdowns) needs a per-gate dryer model + UI. Deferred: no per-gate/EMU hardware to validate against. See `FILAMENT_MANAGEMENT.md` § "Happy Hare Specifics".

---

## Docs debt

Consolidated 2026-08-20 from the docs-cleanup pass; sizes re-measured then.

| Item | Effort | Notes |
|------|--------|-------|
| **Contributor entry path** | Low | Four overlapping on-ramp docs (DEVELOPMENT / DEVELOPER_QUICK_REFERENCE / ONBOARDING / YOUR_FIRST_CONTRIBUTION) — converge on one marked path |
| **ENVIRONMENT_VARIABLES.md size** | Medium | 2,512 lines — split by runtime / build / mock |
| **FILAMENT_MANAGEMENT.md size** | Medium | 4,082 lines — split per backend |
| **XML fork doc name** | Low | Name promises an XML-status survey; the content is fork-origin and licensing history |
| **user/guide routing** | Low | camera, fans, print-history, security, and sensors guides unrouted in `docs/user/CLAUDE.md` |
| **TESTING vs UI_TESTING** | Low | Overlapping scopes — merge, or split cleanly by layer |
| **helixctl golden variants** | Medium | Screenshot/verification harness has no size or theme golden variants |
| **CHANGELOG prose template** | Low | Wanted for release-notes consistency |
| **G-code renderer design homeless** | Medium | The complete renderer design was deleted with the old G-code visualization doc, absorbed nowhere — `GCODE_VIEWER_CONFIG.md` covers settings/shading only, no architecture chapter owns the pipeline |
| **plans/ gate exemption** | Low | Design specs moved into `plans/` are name-exempt in `check_doc_refs.py`; carve out `DEVEL_EXEMPT_SUBDIRS` if their refs should resolve again |
| **Stale superpowers refs** | Low | Tracked plan files still cross-link via `docs/superpowers/` paths (dead on a fresh clone); four refs in src/tests/plans point at files that no longer exist anywhere |
| **User-docs pass** | Medium | Follow-up: accuracy and coverage pass over `docs/user/` |
| **Flat devel-docs pass** | Medium | Follow-up: next tranche of flat `docs/devel/*.md` into organized homes |
| **GcodeToolRemapper rationale** | Low | The remapper stays generic with ACE as consumer; the rationale was lost when the tool-remapper design was absorbed (2026-06-16). Capture it in `FILAMENT_MANAGEMENT.md` when next touched |
| **Stale exact counts** | Low | 116 methods (x2), 23 Spoolman API methods, 13-step wizard predate the docs-cleanup re-measurement and were never re-verified |

---

## Design Philosophy

HelixScreen is a **local touchscreen** UI - users are physically present at the printer. This fundamentally differs from web UIs (Mainsail/Fluidd) designed for remote monitoring.

**We prioritize:**
- Tactile controls optimized for touch
- At-a-glance information for the user standing at the machine
- Calibration workflows (PID, Z-offset, screws tilt, input shaper)
- Real-time tuning (speed, flow, firmware retraction)

**Lower priority for this form factor:**
- Job queue (requires manual print removal between jobs)
- System stats (CPU/memory) — not diagnosing remote issues
- Remote access/monitoring features

Don't copy features from web UIs just because "competitors have it" — evaluate whether it makes sense for a local touchscreen.

---

## Target Platforms

| Platform | Architecture | Status |
|----------|--------------|--------|
| **Raspberry Pi 4/5 (64-bit)** | aarch64 | Docker cross-compile |
| **Raspberry Pi (32-bit)** | armv7-a (armhf) | Docker cross-compile |
| **BTT Pad** | aarch64 | Same as Pi |
| **Adventurer 5M** | armv7-a | Static linking (glibc 2.25) |
| **Creality K1** | MIPS32 | Static linking |
| **QIDI** | aarch64 | Detection heuristics + print profile |
| **Snapmaker U1** | armv7-a | 480x320 display support |
| **Creality K2** | ARM | Static musl (tested on K2 Plus) |
| **macOS** | x86_64/ARM64 | SDL2 development |
| **Linux** | x86_64 | SDL2, CI/CD tested |

---

## Contributing

See `docs/devel/DEVELOPMENT.md#contributing` for code standards and git workflow.

**Key references:**
- `CLAUDE.md` - Project patterns and critical rules
- `docs/devel/ARCHITECTURE.md` - System design and principles
- `docs/devel/LVGL9_XML_GUIDE.md` - XML layout reference
- `docs/devel/DEVELOPMENT.md` - Build and workflow guide
