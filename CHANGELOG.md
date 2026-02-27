# Changelog

All notable changes to HelixScreen will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.13.7] - 2026-02-27

### Added
- Dropdown options now support translation via `options_tag` attribute
- Broad internationalization pass across C++ UI code with `lv_tr()` calls
- Touch calibration can be forced on startup with `HELIX_TOUCH_CALIBRATE` environment variable
- Thumbnail Only option for G-code render mode in display settings

### Fixed
- AMS tray height reduced for better proportions in slot grid
- G-code metadata parser rejects percentage values in extrusion width fields
- Cancel button now appears immediately when starting a print
- AFC multi-unit bugs: nozzle navigation, lane sorting, and current tool derivation

## [0.13.6] - 2026-02-26

### Added
- Fan speed overlay opens directly when tapping the fan icon in carousel mode
- Dual-output Pi builds: single compilation produces both DRM and fbdev binaries

### Fixed
- LED light state now syncs correctly from hardware on status updates
- Empty AMS slots are clickable with placeholder circle and context menu
- LED widget initial state and reactive bindings fixed
- Print details delete button is now icon-only, giving more room for the print button
- 2D fallback disabled on print details panel; thumbnails used instead
- AMS slot positioning fixes for hidden spool containers and LINEAR selector box sizing
- Keyboard overlay crash when cleanup nulls alternatives mid-use (#207)
- Use-after-free in LED and temperature widget button user data
- Tool badge now shown on empty unassigned AMS gates
- Printer database JSON parsing hardened against type mismatches
- ELF architecture validation uses platform key instead of uname
- Updated Ender 5 Max printer image
- Installer preserves user files in `printer_database.d` during upgrades
- AMS flow animation no longer runs infinitely, fixing 50%+ CPU usage on AMS panel

### Changed
- XML event callbacks registered at startup instead of widget attach time
- Panel switching and widget creation optimized for ARM
- Spoolman vendor list fetched from dedicated endpoint instead of downloading all spools
- AMS gate observer rebuilds coalesced to reduce startup churn

## [0.13.5] - 2026-02-26

### Added
- Touch jitter filter for noisy controllers like Goodix GT9xx with automatic breakout detection
- Auto-detect swapped touch axes during calibration, especially for Creality SonicPad and similar devices with misreported axis orientation
- Power Devices entry in System settings
- AMS filament system header bar now shows system logo and name with declarative bindings
- AMS LINEAR output path with animated slide beneath active slot
- Update telemetry tracking: success/failure recording across update lifecycle with analysis tools
- Seven new telemetry event types with thread-safe recording
- `--debug-touches` flag for touch input diagnostics

### Fixed
- Header back button touch target expanded to full title width for easier navigation
- AMS bypass spool centered on filament line with label moved beneath
- AMS spools centered inside tray with support for variable AFC lane count
- Happy Hare AMS now uses LINEAR topology with SELECTOR butted against prep sensors
- Float-to-int conversion guards against NaN/Inf values (#206)
- AD5X platform now correctly maps to K1 MIPS binary (#203)
- Async callback use-after-free in Spoolman spool selection
- Null guards for keyboard events and NEON blend path (#207, #208)
- Installer no longer shows misleading 'corrupt download' message on slow CDN connections
- Jitter filter correctly disabled after breakout for smooth scrolling

### Changed
- HomePanel refactored into self-contained widgets
- CI build split into separate compile and test jobs

## [0.13.4] - 2026-02-25

### Added
- BMP and GIF format support for custom printer images
- Invalid custom printer images shown as disabled with lazy import and instant gallery refresh
- In-process fbdev display fallback when DRM initialization fails (no restart needed)
- Top-level exception handler prevents unhandled crashes from silently terminating the application

### Fixed
- SonicPad Goodix (gt9xxnew_ts) touchscreen now triggers calibration wizard when kernel reports zero ABS ranges
- DRM backend no longer falls back to `/dev/dri/card0` when no suitable DRM device exists
- Fan carousel arc thumb disabled on auto-controlled fans that don't accept manual speed changes
- Observer cleanup ordering hardened to prevent cascading use-after-free during shutdown
- Thread safety and crash telemetry improvements across observer guards and lifecycle management

### Changed
- Macro search resolves only active include-chain config files, improving performance on large configurations
- Touch calibration and wizard skip decisions promoted to info-level logging for easier diagnostics

## [0.13.3] - 2026-02-25

### Added
- AFC tool change progress display on print status panel with current/total tool change counter
- AFC mock tool change progress in test mode for development

### Fixed
- MT-only touchscreens (e.g., Goodix gt9xxnew_ts on Nebula Pad) now detected correctly — previously invisible due to missing ABS_MT_POSITION_X/Y bit checks
- ABS range queries fall back to multitouch axes when legacy ABS_X/ABS_Y are absent, enabling rotation mismatch detection on MT-only devices
- Installer now preserves user data directories (custom_images, printer_database.d) during upgrades
- Watchdog no longer launches external splash screen in DRM mode, preventing a crash loop
- ObserverGuard cleanup lambda prevents use-after-free when releasing observers
- Print status filament row decluttered by removing redundant "Filament" and "Active" labels
- AFC mock toolchange progress default now set in constructor for consistent test behavior

### Changed
- README LVGL badge updated to 9.5, added helixscreen.org link

## [0.13.2] - 2026-02-25

### Added
- FlashForge AD5X platform support (#203)
- Touch calibration CLI flag (`--calibrate-touch`) and `input.force_calibration` config option
- Touch calibration standalone user guide
- Improved touch calibration UX with tap-to-begin, progress counting, and flash feedback

### Fixed
- Touch input on fbdev devices no longer applies a redundant rotation transform (#186)
- Shutdown sequence hardened against use-after-free crashes
- DRM device configuration now validates before use and falls back to auto-detection on failure
- Diagnostic logging added for DRM initialization failures and startup platform info
- Alive guards and lifecycle safety added to 5 crash-prone components
- WebSocket disconnected before clearing app globals to prevent spurious shutdown errors
- TemperatureSensorManager shutdown crash prevented with alive guard
- Happy Hare MMU slot data now received via mmu object subscription (#214)
- Splash screen skipped on DRM-only systems to prevent master contention
- Telemetry queue file writes now atomic to prevent empty file on interrupted save
- DRM rotation patch includes header declaration (fixes Pi cross-compilation)
- Launcher e2e tests mock system commands to avoid hitting dev machine

## [0.13.1] - 2026-02-25

### Added
- Robust touch calibration with multi-sample input filtering, ADC saturation rejection, and post-compute validation
- Smart calibration auto-revert with 10-second timeout and broken-matrix detection
- DRM plane rotation support for `rotate` config on Raspberry Pi

### Fixed
- Use-after-free crashes in AMS modal destructor and sidebar (#199, #201)
- Static-linked OpenSSL for Pi fbdev variants (fixes missing libssl.so.1.1 on some systems)
- Creality SonicPad/Nebula display backlight no longer killed by display-sleep service
- OTA update downloads no longer fail for releases larger than 50 MB (limit raised to 150 MB)
- Touch calibration verify handler simplified with dead wizard callbacks removed

## [0.13.0] - 2026-02-24

The rendering engine gets a major upgrade — the 3D G-code viewer is ported from TinyGL to OpenGL ES 2.0 with per-pixel Phong shading, and Pi builds gain GPU-accelerated DRM+EGL rendering with automatic framebuffer fallback. A first-boot rotation probe auto-detects display orientation, and the UI gains carousel modes for temperature and fan widgets, frosted-glass modal backdrops, and a new shared progress bar component with gradient indicators.

### Added
- GPU-accelerated DRM+EGL rendering for Raspberry Pi with automatic fbdev fallback
- 3D G-code renderer ported from TinyGL to OpenGL ES 2.0 with per-pixel Phong shading and camera-following light
- First-boot display rotation probe with auto-rotating touch coordinates
- Automatic DRM/fbdev binary selection and dual-binary Pi releases
- Carousel display mode for temperature and fan stack widgets with long-press toggle
- Shared progress bar component with dynamic gradient indicator
- Frosted-glass backdrop blur effect on modals
- G-code render mode setting visible when 3D rendering is available
- Chamber temperature overlay on controls panel
- Print lifecycle state extraction for cleaner print status management
- 3D G-code viewer in print file detail panel with async loading and AMS color support
- Pinch-to-zoom gesture support for 3D G-code viewer
- Icons on Delete and Print buttons in print details card
- SSL enabled for native desktop builds
- Translation updates with 7 obsolete keys removed
- External spool widget for printers without AMS
- Loading spinner overlay and 3D rotate hint icon on print file detail panel
- AFC tool changer support with proper SELECT_TOOL/UNSELECT_TOOL commands and extruder dropdown

### Fixed
- Use-after-free crashes in G-code viewer, power panel, mDNS callbacks, thumbnail loading, and AMS widget cleanup (#182, #192, #193)
- Scroll jitter in virtual list views caused by layout-invalidating calls
- Safe name-based widget lookup prevents miscast crashes in event handlers (#194, #195)
- GLES 3D build correctly disabled on macOS (no EGL headers)
- Stale thumbnail and progress data no longer persists when a new print starts
- AMS spools no longer show as full when Spoolman initial_weight is null
- AMS loaded filament card swatch color now updates reactively
- Overlays now close when clicking the navbar button for the active panel
- Progress bar draw no longer triggers lv_inv_area assertion
- Overflow row click passthrough on controls panel
- Temperature chart validates widget before updating series data
- Renamed Voron Micron to PFA Micron in printer database
- AMS edit modal remaining weight defaults and display
- Header bar back button click area expanded for better touch targeting
- 3D viewer camera framing and gesture responsiveness
- Pinch-to-zoom rendering during gesture
- DRM flush timeout from glReadPixels on cached frames
- Print buttons stay at bottom when preprint options are hidden
- Empty preprint options card hidden when no options available
- Z-adjust button icons corrected for bed-moving printers
- Z-adjust button order fixed so down arrow is on bottom
- 3D viewer crash when loading UI triggers on hidden widgets
- Timelapse callback registration

### Changed
- Tertiary theme color changed from orange to blue-violet
- Inline progress bars replaced with shared progress_bar component
- AMS panels refined with more compact loaded filament card and tighter spacing
- Edit icons changed from secondary to tertiary color
- Deprecated AMS mock environment variables removed

## [0.12.1] - 2026-02-23

### Added
- Daily active devices and cumulative growth charts in analytics dashboard
- Zstd compression for debug symbol uploads (~60% size reduction)

### Fixed
- XML `inner_align="contain"` and `inner_align="cover"` now work correctly — images were rendering at native size instead of scaling to fit their containers
- Telemetry device counting uses unique devices instead of sessions for cumulative growth
- Discord invite links updated across all documentation
- Worktree setup script resolves main tree path correctly when run from inside a worktree
- Android CMake build includes helix-xml library
- GitHub release titles no longer show duplicate version numbers

## [0.12.0] - 2026-02-23

A major infrastructure release — LVGL is upgraded to v9.5.0, and the XML layout engine has been extracted into its own library (`helix-xml`) for independent development. The Android port lands with initial build system and CI pipeline support. Developer experience improves with XML hot reload for live UI editing.

### Added
- Android port: build system, APK packaging, asset extraction, CI builds, and release pipeline
- XML hot reload for live UI editing during development (`HELIX_HOT_RELOAD=1`)
- Klipper ERROR state recovery dialog with firmware restart option
- AMS unit names are now pretty-printed for readability
- Widget-safe async callback utilities for thread-safe UI updates

### Fixed
- Use-after-free in deferred observer callbacks (#174)
- Modal use-after-free crash during navigation
- USB drive callback crash from background thread (now marshaled to main thread)
- AMS crash on quit from unjoinable scenario/dryer threads
- Spoolman weight polling skipped when filament backend already tracks weight
- Spoolman active spool management for Happy Hare and AFC backends
- AFC Unload button re-enables after lane scan resets filament state
- AFC mixed topology: correct tool count, hub sensor detection, and status display
- AMS current slot label accuracy for multi-unit and tool changer setups
- AMS overview right column capped at 200px to prevent layout overflow
- Keyboard restores screen position when dismissed via backdrop click
- Firmware restart widget shown for all non-READY Klippy states
- Responsive tokens applied to picker layouts, fan name widths, and panel widget spacing
- Font clipping and padding on panel widgets
- Gcode viewer segfault
- Update check errors now visible in the UI
- Installer uses printf for ANSI escape compatibility

### Changed
- LVGL upgraded from 9.4-pre to v9.5.0
- XML engine extracted to `lib/helix-xml/`, decoupled from LVGL internals
- MoonrakerAPI decomposed into domain-specific modules (Rest, Job, Motion, File, FileTransfer, Timelapse, Advanced)
- Z-offset utilities extracted into shared module
- Wi-Fi status polling refactored to async with responsive connect/disconnect updates

## [0.11.1] - 2026-02-22

### Added
- Panel widgets dim when Moonraker is disconnected or Klippy is not ready
- AMS slot bars resize responsively based on home panel row density
- Bypass spool widget and filament path topology support for AMS systems
- Happy Hare v4 parsing with full v3 backwards compatibility

### Fixed
- AMS "Currently Loaded" display now shows the correct filament in multi-backend setups (e.g., AMS_2 load no longer snaps to AMS_1 state)
- Use-after-free crash on AMS overview back-navigation
- Use-after-free crash when temperature graph chart widget is destroyed
- Bypass path and toggle hidden for tool changers (not applicable)
- Watchdog uses fork-based reboot fallback for crash dialog reliability
- Auto-restart after update install instead of showing unnecessary restart dialog

### Changed
- Filament page: purge button separated from extrude, operations layout reorganized

## [0.11.0] - 2026-02-22

A feature-rich release — fan speeds are now a first-class widget with spinning animations and density-aware labels, chamber temperature gets its own full control panel, and the Printer Manager overlay is available to everyone (no longer gated behind beta). Under the hood, dynamic observer lifetime safety prevents use-after-free crashes, and the MoonrakerClient has been decomposed for maintainability.

### Added
- Chamber temperature panel with graph, presets, and sensor-only monitoring mode — tap the chamber row in the Temperatures widget to open it
- Fan stack widget enabled by default with density-aware compact labels (P/H/C when space is tight)
- Spinning fan icon animations on dials, status cards, and home panel widgets
- Crash history tracking in debug bundles — past crash submissions with GitHub issue references
- SubjectLifetime tokens for safe observation of dynamic per-fan, per-sensor, and per-extruder subjects
- Micro layout support (480x272) with compact controls, theme editor, and display overlays
- Ender-3 V3 KE printer image and hostname detection
- MoonrakerClient decomposition: RequestTracker and DiscoverySequence extracted as independent modules
- Translations synced and fan stack labels localized across all languages

### Fixed
- Printer Manager overlay no longer gated behind beta features — accessible to all users
- Setup wizard auto-fills default port 7125 when the port field is left empty on Test Connection
- Printer detection: hostname_exclude heuristic prevents Ender-3 V3/V3 KE model ambiguity
- Heap corruption during change-host reconnection (double-free in observer cleanup)
- Stack overflow on Pi from `lv_obj_is_valid()` in hot paths (HeatingIconAnimator crash)
- HeatingIconAnimator theme observer uses ObserverGuard to prevent heap corruption
- Config preservation during in-app upgrades (three-layer merge prevents config loss)
- Factory reset now restarts the app automatically
- Installer download progress bar and error reporting
- Release download logic with timeout and speed limits
- Thermistor picker position and temp stack click targets
- Printer image snapshot alignment when clearing
- Home panel widget spacing, fan labels, and click targets
- Print start toast and phase tracking properly gated behind beta features
- Various compiler warnings and dead code removed

### Changed
- MoonrakerClient internals decomposed: discovery callbacks stored as members, stale connection guard added
- Navigation bar widened at medium and large breakpoints
- Printer image snapshotted to eliminate per-frame scaling (performance improvement)
- Install Update row moved under Check for Updates in About section
- Orphan AboutOverlay removed (was unreachable dead code)

## [0.10.14] - 2026-02-22

### Fixed
- AFC unload button and context menu now work on AFC firmware versions that don't expose a top-level `filament_loaded` field (e.g., Box Turtle with `lane_data_enabled=false`)
- AFC `current_load` field parsed as fallback when `current_lane` is absent, fixing loaded lane detection on newer AFC versions
- Crash-hardened 15 vectors found during 48-hour audit

## [0.10.13] - 2026-02-22

Crash hardening and new features — favorite macro widgets let you pin and run macros from the home panel, filament controls get dedicated Extrude/Retract buttons, and Wi-Fi status updates are now async and responsive. Under the hood, the MoonrakerAPI monolith has been split into domain-specific modules.

### Added
- Favorite macro panel widgets with macro picker and automatic parameter detection
- Extrude and Retract buttons replace Purge on filament page, with configurable speed
- Wi-Fi status updates respond immediately to connect/disconnect events

### Fixed
- Multiple crash fixes: glyph null guard, use-after-free in async AMS/telemetry callbacks, SIGSEGV in widget cleanup and render thread race
- AFC Unload button now re-enables after lane scan detects filament state change
- Observer generation counters prevent stale callbacks after controls repopulate
- Unknown CLI arguments warn instead of crash-looping
- Z-offset tune overlay save bug
- Macro parameter modal no longer stomps across widget slots
- Widget picker and overlay cleanup uses safe deletion
- USB drive callbacks marshaled to main thread to prevent crashes
- Panel widget padding and font clipping on small screens

### Changed
- MoonrakerAPI split into 8 domain-specific modules (Job, Motion, File, FileTransfer, Advanced, Rest, Timelapse, History)
- Wi-Fi backend uses async status polling instead of blocking queries
- Z-offset utilities extracted into shared module

## [0.10.12] - 2026-02-21

A stability and polish release focused on crash fixes, responsive UI improvements, and internal refactoring. The home panel widget system is now fully decoupled from HomePanel, keyboard input is more reliable, and several threading bugs have been resolved.

### Added
- Carousel widget component with wrap-around, auto-advance timer, indicator dots, and scroll detection
- Thermistor widget for monitoring custom temperature sensors on the home panel
- Responsive breakpoint subject (`ui_breakpoint`) for reactive visibility changes across screen sizes
- `HELIX_LOG_LEVEL` env var and `--log-level` CLI flag for fine-grained log control
- `HELIX_DPI` env var for overriding display DPI
- `HELIX_SKIP_SPLASH` env var to bypass splash screen
- Long-press auto-insert for alternate keyboard characters
- DWARF debug info pipeline for better crash backtrace resolution

### Fixed
- AFC mutex deadlock when error messages were emitted during state parsing
- Startup deadlock and shutdown race condition in mock mode
- Keyboard backspace and character insertion broken when cursor is mid-string
- Keyboard crash when textarea widget is deleted while keyboard is open
- Framebuffer stomping between splash screen and main process on Pi
- Kernel console text bleeding through LVGL UI on framebuffer devices
- Carousel wrap setting ignored in scroll end callback
- Resistive touchscreen detection for NS2009/NS2016 controllers (#135)
- Fan status hidden on tiny screens
- Slider row padding increased to prevent handle clipping
- Dropdown rows now wrap text responsively in settings
- Network item click handling uses correct event target
- Soft keyboard registered for hidden network modal inputs
- Self-update handles NoNewPrivileges; stale `.old` files cleaned on startup
- Installer polkit rules no longer contain untemplated placeholders
- `enP*` interface naming cleaned up (#145)

### Changed
- Home panel widgets fully decoupled from HomePanel — PanelWidget system with self-registration, per-panel config, and independent lifecycle
- AMS backends share extracted `AmsSubscriptionBackend` base class
- MoonrakerAPI split: `MoonrakerHistoryAPI` and `MoonrakerSpoolmanAPI` extracted
- Settings consumers migrated to domain-specific managers (Display, System, Input, Audio, Safety)
- Panels and overlays migrated to batch callback registration
- Temperature formatting consolidated into `ui_temperature_utils`
- Observer factory adopted across all remaining legacy observers
- Shell test suite optimized from ~4 min to ~90s

## [0.10.11] - 2026-02-20

### Added
- Customizable home panel widgets with drag-to-reorder — choose which widgets appear and arrange them to your preference
- External spool support — set filament type and color for the bypass/direct-drive spool, visible in system path canvas and detail views with Spoolman quick-assign
- SlotRegistry unified slot state management across all AMS backends (AFC, Happy Hare, ValgACE, mock)
- 3D tube rendering for filament paths with curved routing, glow effects, and flow particle animations
- Pipe-routed tube drawing with cable-harness nesting for multi-tool overview
- Pulse animation on target slot during filament swap
- Infimech TX printer support (#139)
- Sovol SV06 printer image

### Fixed
- Graceful shutdown on SIGINT/SIGTERM — no more crash on quit
- Filament subjects now update on first Moonraker status even when state matches defaults
- AMS endcap seam eliminated at straight-to-curve tube junctions
- AMS per-unit topology for filament segment in mixed mode
- AMS layout matching works regardless of initialization order
- AMS context menu positioning and badge sizing for 2-digit slots and tools
- AMS global slot numbering and detail view swatch color
- AFC virtual bypass sensor toggle uses correct sensor name
- Stealthburner polygon centering offset corrected
- Wi-Fi SSID retrieved from wifi list instead of invalid device field
- ABS mismatch detection re-enabled with generic HID range exclusion (#135, #137)
- Default print completion alert changed from notification to alert
- Installer drops unnecessary sudo from systemctl checks and adds polkit error handling
- Home panel falls back to generic printer image when file missing
- Home panel uses filament sensor count for hardware-dependent widget gating

### Changed
- AMS overview uses Stealthburner toolhead for Voron printers
- AMS detail view uses badge-style tool labels
- Sensor rows use status_pill component for type badges
- AMS sidebar extracted as shared component across panel types

## [0.10.10] - 2026-02-19

### Added
- Output pin LED backend for brightness-only chamber lights and enclosure LEDs — auto-detects `[output_pin]` devices with PWM slider or on/off toggle
- Individual X and Y homing buttons in controls quick actions
- Clear Spool context menu action for assigned-but-empty AMS slots
- AFC version warning when firmware is below v1.0.35
- Configurable Allwinner backlight ENABLE/DISABLE ioctls for broader SBC compatibility
- Udev and polkit rules for non-root backlight and Wi-Fi access on Pi

### Fixed
- Self-update under systemd NoNewPrivileges — installer now correctly skips privileged operations during in-place updates
- Installer preserves settings.json, helixscreen.env, and config across updates
- Render thread crash from NULL draw buffer race condition
- AFC unit topology now uses name-based matching instead of fragile index ordering
- Toolchanger uses SELECT_TOOL instead of ASSIGN_TOOL to avoid remapping
- Thumbnail paths resolved correctly for files in subdirectories
- File browser poll timer resumes after returning to print selection panel
- Timeouts added to long-running G-code calls to prevent UI hangs
- Systemd service dependency cycle from multi-user.target removed
- Self-restart uses `_exit(0)` instead of `exit(0)` to avoid background thread races
- Mock sensor dots restored for AMS prep sensors

### Changed
- Update check cooldown reduced from 60 minutes to 10 minutes
- SDL display hints cleaned up for better cross-platform performance

## [0.10.9] - 2026-02-19

### Added
- AMS sensor error states with visual indicators for hub, extruder, and lane sensor faults
- Happy Hare pre-gate sensor support for filament detection at each gate
- External tool change step progress detection — swaps and loads initiated from gcode or other UIs now show correct progress steps

### Fixed
- AFC gcode commands corrected to match actual AFC-Klipper-Add-On API: `CHANGE_TOOL`, `TOOL_UNLOAD`, `SET_MAP`, `RESET_FAILURE`, and `SET_BOWDEN_LENGTH` now use correct command names and parameters
- AFC per-lane commands (`SET_LONG_MOVE_SPEED`, `AFC_RESET_MOTOR_TIME`) now apply to all lanes instead of only the first
- AFC bowden length per-extruder now maps through unit membership to find the correct hub
- AMS mini status widget fills available height in multi-unit stacked layouts
- AMS tool badge labels use pre-formatted buffers with auto-sized badge width
- AMS backend skipped for tool changes when backend doesn't manage the tool
- AMS nozzle count corrected for mixed topology with unique per-lane tool mappings
- Happy Hare tip method detection reads from configfile on startup
- Mock mixed topology corrected to match real hardware (Box Turtle=HUB, AMS_2=PARALLEL)
- Worktree setup script auto-detects worktree when run without arguments

## [0.10.8] - 2026-02-19

### Added
- Debug bundle now collects Moonraker state, config, and Klipper/Moonraker logs via REST API
- PII sanitization in debug bundles for emails, API tokens, webhook URLs, and MAC addresses

### Fixed
- Crash from running animations when navigating away from a panel (#128)
- Crash from NULL font pointer during AMS bar layout rebuild
- Crash from stale async callbacks in gcode viewer
- Systemd update watcher stuck in infinite restart loop due to PathExists check
- Debug bundle log fetching handles HTTP 416 Range Not Satisfiable responses

## [0.10.7] - 2026-02-18

### Fixed
- AMS context menu UX: hidden tool dropdown, auto-close on backup select, conflict toast, and infinity icon for unlimited backup
- AMS Load/Unload/Eject buttons now work correctly from context menu
- AMS filament sensor toasts suppressed during active load/unload operations
- AFC `SET_RUNOUT` parameter corrected from `RUNOUT_LANE` to `RUNOUT`
- AFC 'Loaded' hub status correctly mapped to available instead of loaded
- AFC tip method detection from config with inline comment stripping
- Spoolman polling log noise suppressed unless spool weights actually changed
- Touch calibration wizard disabled ABS mismatch override for HDMI devices
- Power device probe no longer shows error toast on printers without power component
- Moonraker update manager switched from `type: zip` to `type: web` with systemd restart watcher

### Changed
- Removed dead AmsSlotEditPopup code replaced by context menu

## [0.10.6] - 2026-02-18

### Fixed
- Infinite CPU loop when saving Spoolman spool assignments on AFC and Happy Hare systems — Spoolman weight polling now updates slot state without sending G-code back to firmware, breaking a feedback cycle that saturated the CPU

## [0.10.5] - 2026-02-18

### Added
- **Android port**: Initial Android build system with CMake/Gradle, APK asset extraction, SDL fullscreen, and CI release pipeline
- **Power panel**: Moonraker power device control with home panel toggle and advanced menu integration
- Widget-safe async callback utilities for LVGL event handling

### Fixed
- AMS crash on quit from unjoinable scenario/dryer threads
- AMS right column capped at 200px max width for proper flex layout
- AMS tool count, hub sensor, and status corrected for mixed-topology AFC
- AMS current slot label improved for multi-unit and tool changer displays
- AMS 'Tooled' status handled correctly with production data regression tests
- Gcode viewer SEGV from unsafe async callback
- History totals computed from job list instead of hardcoded mock values
- Update check errors now visible in settings UI
- Installer uses printf for ANSI escapes instead of echo for POSIX compliance

### Changed
- Test output cleaned up: ~637 spurious warning/error lines silenced

## [0.10.4] - 2026-02-18

Slicer-preferred progress, Klipper M117 display messages, interactive AMS toolheads, and a batch of AMS rendering and stability fixes.

### Added
- Slicer-preferred progress via `display_status` — uses slicer-reported percentage over file position when available (#122)
- Klipper M117 display message shown on home panel print card (#124)
- Clickable AMS toolheads with docked dimming for parallel topology
- Per-lane eject and reset actions in AMS context menu
- Opacity and dim support for nozzle renderers
- Animated icons on controls panel

### Fixed
- Stale "Print" button text when print state changes (#125)
- Touch calibration wizard now shown for capacitive screens with ABS range mismatch (#123)
- AMS backend priority: MMU preferred over toolchanger when both are present
- AMS filament path lanes aligned with spool visual centers at all breakpoints
- AMS nozzle unloaded color unified and tool changer filament segments corrected
- AMS nozzle tip color changed to charcoal with idle path line fix
- AMS mini status bar sizing no longer applies 2/3 height scaling
- AMS toolchangers use `T{n}` gcode with click lockout during operations
- AFC slots only marked LOADED when `tool_loaded` is true
- Crash from NULL font pointer in AMS panel backend selector (#110)
- Touch input auto-detection scoring improved for multi-input systems (#117)
- `touch_device` config setting now read from the correct location
- Stale thumbnail no longer persists when a new print is started externally
- Self-update survives systemd cgroup kill (#118)
- UI switch size preset initialized before optional parse
- Filament panel left column layout flattened for proper flex_grow on temperature graph
- Noisy `assign_spool` warning downgraded to trace for virtual tool mappings
- Moonraker update manager release name uses tag-only format for compatibility
- `systemctl restart` uses `--no-block` to eliminate race window during updates
- Docker build uses GitHub mirror for zlib download

### Changed
- Crash reporting worker converted from JavaScript to TypeScript with GitHub App integration for dedup issue creation

## [0.10.3] - 2026-02-17

Big AMS release — unified slot editor with Spoolman integration, mixed-topology AFC support, error state visualization, and a major DRY refactor of shared drawing utilities across all AMS panels. Also adds 34 new translations and fixes several installer issues.

### Added
- **Unified Slot Editor**: New AMS slot edit modal with inline Spoolman picker, side-by-side vendor/material dropdowns, cancel/save flow, and in-use spool disabling
- **Spoolman Slot Saver**: Change detection and automatic save flow for slot-to-spool assignments with filament persistence
- **Mixed AFC Topology**: Box Turtle PARALLEL + OpenAMS HUB coexisting in a single AFC system
- **AMS Error Visualization**: Slot error dots, hub tinting, pulsing animations, severity colors, and aligned error detection across mini status and slot views
- **Shared Drawing Utilities**: Consolidated color, contrast, severity, fill, bar-width, display-name, logo fallback, pulse, error badge, slot bar, and container helpers
- Canonical `SlotInfo::is_present()` presence check for consistent slot detection
- Picker sub-view XML and header declarations for the unified edit modal
- 34 new translations across all 8 target languages with missing `translation_tag` attributes added
- Debug bundle fetch/display helper script
- ARM unwind tables and /proc/self/maps in crash reports

### Fixed
- Non-translatable strings (product names, URLs, OK) incorrectly wrapped in lv_tr()
- AMS edit modal Spoolman callbacks not marshalled to main thread
- AMS bypass detection, Happy Hare speed params, and other deferred TODOs resolved
- AMS bypass, dryer, reset, and settings hidden for tool changers
- Brand/spoolman_id missing from AFC and multi-AMS mock slots
- Load button enabled when slot already loaded
- Change Spool button label not updating correctly (ui_button_set_text)
- Static instance pointer for edit modal callbacks
- Updater diagnostic logs too noisy (downgraded to debug), added 2-min install timeout
- zlib updated from 1.3.1 to 1.3.2; Ubuntu CI build timeout bumped to 45min
- Installer stale .old directory blocking repeated updates (PR #102, thanks @bassco)
- Installer false-fail when cleanup_old_install hits root-owned hooks.sh

### Changed
- AMS panels refactored to use shared drawing utilities (DRY across 5 UI files: slot, mini status, overview, spool canvas, panel)
- Assign Spool removed from AMS context menu, replaced by unified slot editor
- Deprecated C-style wrapper APIs and legacy compatibility code removed
- Bundled installer regenerated with latest module changes

## [0.10.2] - 2026-02-17

This release significantly improves multi-tool printer support with per-tool spool persistence and an extruder selector for filament management, decouples Spoolman from AMS backends for cleaner architecture, and fixes several crash bugs and installer issues.

### Added
- Extruder selector dropdown for multi-tool printers in filament management
- Per-tool spool persistence decoupled from AMS backends
- Crash analytics dashboard with crash list view
- Load base and platform metadata in crash telemetry events
- Filament type tracking in print outcome telemetry events
- Discord notifications on successful releases

### Fixed
- Use-after-free during toast notification replacement (fixes #98)
- Dangling pointer after external modal deletion in AMS dryer dialog (fixes #97)
- Crash from null font pointer in AMS mini status overflow label (fixes #90, #91)
- Unsafe move operators corrupting lv_subject_t linked lists in setup wizard
- OTA updater "Installer not found" regression from systemd PATH resolution
- ELF architecture validation for K1/MIPS platform
- Static-linked OpenSSL for pi32 with post-install ldd verification
- AMS context menu positioning and ghost button borders
- Hidden tray and redundant tool badges for tool changers
- AMS bypass, dryer, reset, and settings visibility for tool changers
- Click-through on nozzle icon component
- Navigation bar buttons not filling available width, with lingering focus rings

### Changed
- Spoolman integration decoupled from AMS backends into standalone architecture
- Nozzle icon extracted into reusable component with consolidated tool badge logic
- Codebase migrated to `helix::` namespace with modernized enum classes
- Telemetry worker updated to support schema v2 nested fields

## [0.10.1] - 2026-02-16

### Added
- Debug bundle upload for streamlined support diagnostics
- Unified active extruder temperature tracking across multi-tool setups
- Dynamic nozzle label showing tool number for multi-tool printers
- Configurable size property for filament sensor indicator
- PrusaWire added to printer database
- Email notifications for debug bundle uploads (crash worker)
- 69 new translations across 8 languages

### Fixed
- Use-after-free in deferred observer callbacks (fixes #83)
- Crash from error callbacks firing during MoonrakerClient destruction
- Crash from SubscriptionGuard accessing destroyed MoonrakerClient on shutdown
- Crash from theme token mismatch in AMS backend selector
- Observer crash on quit from NavigationManager init ordering
- Spdlog call in ObserverGuard static destructor causing shutdown hang
- Tool badge showing unnecessarily with single-tool printers
- Hardware discovery falsely flagging expected devices as new
- Setup wizard not clearing hardware config on re-run
- Wizard port input accepting non-numeric characters
- Splash screen suppressing rendering without an external splash process
- Noisy WLED and REST 404 logs downgraded from warn to debug
- AMS slot info updates logged on every poll instead of only on change
- Installer using bare sudo instead of file_sudo for release swap/restore
- AMS edit modal Spoolman callbacks not marshalled to main thread

### Changed
- Spoolman vendor/filament creation moved to modal dialogs
- Spool wizard graduated from beta
- Lifetime checks added to SubscriptionGuard and ObserverGuard
- Shutdown cleanup self-registered in all init_subjects() methods

## [0.10.0] - 2026-02-15

Major feature release bringing full Spoolman spool management, a guided spool creation wizard, multi-unit AMS support for AFC and Happy Hare, probe management, and a Klipper config editor. Also adds Elegoo Centauri Carbon 1 support and fixes several crash bugs.

### Added
- **Spoolman Management**: Browse, search, edit, and delete spools with virtualized list, context menu, and inline edit modal
- **New Spool Wizard** (beta): 3-step guided creation (Vendor → Filament → Spool Details) with dual-source data from Spoolman server and SpoolmanDB catalog, atomic creation with rollback
- **Multi-unit AMS**: Support for multiple AMS/AFC/Happy Hare units with per-unit overview panel, shared spool grid components, and error/buffer health visualization
- **Probe Management** (beta): BLTouch panel with deploy/retract/self-test, probe type detection for Cartographer, Beacon, Tap, and Klicky
- **Klipper Config Editor**: Structure parser with include resolution, targeted edits, and post-edit health check with automatic backup restore
- **Elegoo Centauri Carbon 1**: Platform support with dedicated build toolchain and presets
- AFC error notifications with deduplication and action prompt suppression
- Android-style clear button for all search inputs
- Toast notifications for AMS device actions
- Internationalization for remaining hardcoded UI strings

### Fixed
- Crash from re-entrant observer destruction during callback dispatch (fixes #82)
- Use-after-free when destroying widgets from event callbacks (fixes #80)
- AMS slot tray visibility behind badge/halo overlays
- AFC buffer fault warnings not clearing on recovery
- Happy Hare reason_for_pause not clearing on idle
- Icon font validation locale handling
- Focus on close/context menu buttons causing unintended list scroll
- Modal dialog bind_text subject references missing @ prefix

### Changed
- AMS detail views refactored to shared ams_unit_detail and ams_loaded_card components
- Spoolman and history panel search inputs use shared text_input clear button
- R2 release retention policy added to prune old releases

## [0.9.24] - 2026-02-15

### Fixed
- OTA updates now correctly extract the installer from release tarballs (path mismatch between packaging and extraction)
- Button visual shift on release by setting transform pivot in base button style

## [0.9.23] - 2026-02-15

### Added
- LED colors stored as human-readable #RRGGBB hex strings with automatic legacy integer migration
- ASLR auto-detection in backtrace resolver for more accurate crash report symbol resolution

### Fixed
- Crash from LVGL object user_data ownership collisions causing SIGABRT
- Crash from NULL pointer passed to lv_strdup
- Use-after-free in animation completion callbacks
- Use-after-free when replacing toast notifications during exit animation

## [0.9.22] - 2026-02-15

### Added
- Timelapse phase 2: event handling, render notifications, and video management
- AD5M ready-made firmware image as primary install option in docs

### Fixed
- **Critical**: install.sh now included in release packages, fixing "Installer not found" error during UI-initiated updates (thanks @bassco)

### Changed
- CI release pipeline refactored to matrix builds for easier platform maintenance (thanks @bassco)

## [0.9.21] - 2026-02-14

### Added
- G-code console gated behind beta features setting
- Cancel escalation system: configurable e-stop timeout with settings toggle and dropdown
- Internationalization for hardcoded settings strings

### Fixed
- Nested overlay backdrops no longer double-stack
- Crash handler and report dialog disabled in test mode to prevent test interference
- Installer now extracts install.sh from tarball to prevent stale script failures
- Operation timeout guards increased for homing, QGL, and Z-tilt commands
- Touch calibration option hidden for USB HID input devices

### Changed
- G-code console and cancel escalation documented in user guide

## [0.9.20] - 2026-02-14

This release adds multi-extruder temperature support, tool state tracking, multi-backend AMS (allowing printers with multiple filament systems), and fixes a critical installer bug that prevented Moonraker from starting on ForgeX AD5M printers after reboot.

### Added
- Multi-extruder temperature support with dynamic ExtruderInfo discovery and selection panel
- Tool state tracking (ToolState singleton) with active tool badge and tool-prefixed temperature display
- Multi-backend AMS: per-backend slot storage, event routing, backend selector UI, and multi-system detection
- ASLR-aware crash reports: ELF load base emitted for accurate symbol resolution
- AD5M boot diagnostic script for troubleshooting boot/networking issues
- Russian translation updates (thanks @kostake, @panzerhalzen)
- Telemetry Analytics Engine dashboard

### Fixed
- **Critical**: ForgeX installer logged wrapper broke S99root boot sequence, preventing Moonraker from starting after reboot (#36)
- Splash screen no longer triggers LVGL rendering while it owns the framebuffer
- Exception-safe subject updates in sensor callbacks
- UpdateQueue crash protection with try-catch in process_pending
- Notification and input shaper overlays use modal alert system instead of manual event wiring
- Invalid text_secondary design token replaced with text_muted
- LED macro preset UX improvements and stale deletion bug fix
- systemd service adds tty to SupplementaryGroups for console suppression

### Changed
- Async invoke simplified to forward directly to ui_queue_update
- LED preset labels auto-generated from macro names instead of manual naming

## [0.9.19] - 2026-02-13

### Added
- Crash reports now include fault info, CPU registers, and frame pointers for better diagnostics
- XLARGE breakpoint tier for responsive UI on larger displays
- Responsive fan card rendering with dynamic arc sizing and tiny breakpoint support
- Unified responsive icon sizing via design tokens
- Geralkom X400/X500 and Voron Micron added to printer database
- HelixScreen Discord community link in documentation

### Fixed
- Overlay close callback deferred to prevent use-after-free crash (#70)
- macOS build error caused by libhv gettid() conflict

### Changed
- 182 missing translation keys added across the UI
- Navigation bar width moved from C++ to XML for declarative layout control
- Qidi and Creality printer images updated; Qidi Q2 Pro removed

## [0.9.18] - 2026-02-13

### Added
- Actionable notifications: tapping notification history items now dispatches their associated action (e.g. navigate to update panel)
- Skipped-update notifications persist in notification history with tap-to-navigate

### Fixed
- LED macro integration: macro backend now correctly tracks LED state and handles device transitions
- Pre-rendered generic printer images updated with correct corexy model

## [0.9.17] - 2026-02-13

### Added
- Full LED control system with four backends, auto-state mapping editor, macro device configuration, and settings overlay
- Crash report dialog with automatic submission, QR code for manual upload, and local file fallback
- Layer estimation from print progress when slicer lacks SET_PRINT_STATS_INFO (#37)
- Rate limiting on crash and telemetry ingest workers

### Fixed
- Crash reporter now shows modal before TelemetryManager consumes the crash file
- LED strip auto-selection on first discovery, lazy LED reads, icon and dropdown fixes
- Installer config file operations use minimal permissions instead of broad sudo

### Changed
- Motion overlay refactored to declarative UI with homing indicators and theme colors
- LED settings layout extracted to reusable XML components
- User guide restructured into sub-pages with new screenshots

## [0.9.16] - 2026-02-12

### Added
- Printer Manager overlay accessible from home screen with tap-to-open, custom printer images, inline name editing, and capability chips
- Theme-aware markdown viewer
- Custom printer image selection with import support and list+preview layout

### Fixed
- Setup wizard now defaults IP to 127.0.0.1 for local Moonraker connections
- Whitespace in IP and port input fields no longer causes validation errors

### Changed
- All modals standardized to use the Modal system with ui_dialog
- AMS modals refactored to use modal_button_row component
- Release assets now include install.sh (thanks @Major_Buzzkill)
- Markdown submodule updated with faux bold fix

## [0.9.15] - 2026-02-12

### Fixed
- Touchscreen calibration wizard no longer appears on capacitive displays (#40)
- Calibration verify step now applies new calibration so accept/retry buttons are tappable
- Debug logging via HELIX_DEBUG=1 in env file now works correctly after sourcing order fix
- Release pipeline R2 upload failing when changelog contains special characters
- Symbol resolution script using wrong domain (releases.helixscreen.com → .org)
- User docs referencing `--help | head -1` instead of `--version` for version checks

## [0.9.14] - 2026-02-12

### Fixed
- Installer fails on systems without hexdump (e.g., Armbian) with "Cannot read binary header" error

## [0.9.13] - 2026-02-11

### Added
- Frequency response charts for input shaper calibration with shaper overlay toggles
- CSV parser for Klipper calibration frequency response data
- Filament usage tracking with live consumption during printing and slicer estimates on completion modal
- Unified error modal with declarative subjects and single suppression
- Ultrawide home panel layout for 1920x480 displays
- Internationalization support for header bar and overlay panel titles
- Demo mode for PID and input shaper calibration screenshots
- Klipper/Moonraker pre-flight check in AD5M and K1 installers

### Fixed
- getcwd errors during AD5M startup (#36)
- Installer permission denied on tar extraction cleanup (#34)
- Print tune panel layout adjusted to fit 800x480 screens
- CLI hyphen normalization for layout names

### Changed
- Input shaper graduated from beta to stable
- Width-aware Bresenham line drawing for G-code layer renderer
- Overlay content padding standardized across panels
- Action button widths use percentages instead of hardcoded pixels

## [0.9.12] - 2026-02-11

### Added
- QIDI printer support with detection heuristics and print start profile
- Snapmaker U1 cross-compile target, printer detection, and platform support
- Layout manager with auto-detection for alternative screen sizes and CLI override
- Input shaper panel redesigned with config display, pre-flight checks, per-axis results, and save button
- PID calibration: live temperature graph, progress tracking, old value deltas, abort support, and 15-minute timeout
- Multi-LED chip selection in settings replacing single dropdown
- Macro browser (gated behind beta features)

### Fixed
- Crash on shutdown from re-entrant Moonraker method callback map destruction
- Installer: BusyBox echo compatibility for ANSI colors and temp directory auto-detection
- Missing translations for telemetry, sound, PID, and timelapse strings
- Unwanted borders on navigation bar and home status card buttons
- Scroll-on-focus in plugin install modal
- Beta feature flag conflict hiding hardware check rows in advanced settings

### Changed
- PID calibration ungated from beta features — now available to all users
- Moonraker API abstraction boundary enforced — UI no longer accesses WebSocket client directly
- Test-only methods moved to friend test access pattern (cleaner production API)

## [0.9.11] - 2026-02-10

Sound system, KIAUH installer, display rotation, PID tuning, and timelapse support — plus a splash screen fix for AD5M.

### Added
- Sound system with multi-backend synthesizer engine (SDL audio, PWM sysfs, M300 G-code), JSON sound themes (minimal, retro chiptune), toggle sounds, and theme preview
- Sound settings overlay with volume slider and test beep on release
- KIAUH installer integration for one-click install from KIAUH menu
- Display rotation support for all three binaries (main, splash, watchdog)
- PID tuning calibration with fan control and material presets
- Timelapse plugin detection, install wizard, and settings UI (beta)
- Versioned config migration system
- Shadow support and consistent borders across widgets
- Platform-aware cache directory resolution for embedded targets
- Telemetry analytics pipeline with admin API, pull script, and analyzer

### Fixed
- Splash process not killed on AD5M when pre-started by init script (display flashing)
- Layer count tracking with G-code response fallback
- Print file list 15-second polling fallback for missed WebSocket updates
- Display wake from sleep on SDL click
- Translation sync with extractor false-positive cleanup
- Cross-compiled binaries now auto-stripped after linking
- Build system tracks lv_conf.h as LVGL compile prerequisite
- LayerTracker debug log spam reduced to change-only logging

### Changed
- Sound system and timelapse gated behind beta features flag
- Bug report and feature request GitHub issue templates added

## [0.9.10] - 2026-02-10

Hotfix release — gradient and flag images were broken for all users due to a missing decoder setting, and WiFi initialization caused a 5-second startup delay on NetworkManager-based systems.

### Added
- Optional bed warming step before Z-offset calibration
- Reusable multi-select checkbox widget
- Symbol maps for crash backtrace resolution
- KIAUH extension discovery tests

### Fixed
- Gradient and flag images failing to load (LV_BIN_DECODER_RAM_LOAD not enabled)
- WiFi backend now tries NetworkManager first, avoiding 5-second wpa_supplicant timeout on most systems
- Observer crash on shutdown from subject lifetime mismatch
- Connection wizard mDNS section hidden, subtitle improved

### Changed
- Project permission settings organized into .claude/settings.json

## [0.9.9] - 2026-02-09

Telemetry, security hardening, and a bundled uninstaller — plus deploy packages are now ~60% smaller.

### Added
- Anonymous opt-in telemetry with crash reporting, session recording, and auto-send scheduler
- Hardware survey enrichment for telemetry sessions (schema v2)
- Telemetry opt-in step in setup wizard with info modal
- Cloudflare Worker telemetry backend
- Bundled uninstaller with 151 shell tests
- Creality K2 added to GitHub release workflow

### Fixed
- Framebuffer garbage on home panel from missing container background
- Observer crash on quit from subject/display deinit ordering
- Stale subject pointers in ToastManager and WizardTouchCalibration on shutdown
- Print thumbnail offset and outcome overlay centering
- Confetti particle system rewritten to use native LVGL objects
- Print card thumbnail overlap — e-stop relocated to print card
- Auto-navigation to print status suppressed during setup wizard
- KIAUH extension discovery uses native import paths (fixes #30)
- Data root auto-detected from binary path with missing globals.xml abort
- NaN/Inf guards on all G-code generation paths
- Safe restart via absolute argv[0] path resolution
- Replaced system() with fork/execvp in ping_host()
- Tightened directory permissions, replaced strcpy with memcpy
- K2 musl cross-compilation LDFLAGS
- Telemetry opt-in enforced for crash events
- Telemetry enabled state synced at startup with API key auth

### Changed
- Deploy footprint reduced ~60% with asset excludes and LZ4 image compression
- Shell test gate added to release workflow

## [0.9.8] - 2026-02-09

### Added
- G-code toolpath render uses AMS/Spoolman filament colors for accurate color previews
- Reprint button shown for all terminal print states (error, cancelled, complete)
- Config symlinked into printer_data for editing via Mainsail/Fluidd file manager
- Async button timeout guard to prevent stuck UI on failed operations
- 35 new translation strings synced across all languages

### Fixed
- Slicer time estimate preserved across reprints instead of resetting to zero
- Install directory ownership for Moonraker update manager (fixes #29)
- Python 3.9 compatibility for Sonic Pad KIAUH integration (fixes #28)
- Display sleep using software overlay for unrecognized display hardware (#23)
- Z-offset controls compacted for small displays (#27)
- Print error state handled with badge, reprint button, and automatic heater shutoff
- WebSocket callbacks deferred to main thread preventing UI race conditions
- Responsive breakpoints based on screen height instead of max dimension
- Cooldown button uses TURN_OFF_HEATERS for reliable heater shutoff
- Splash screen support for ultra-wide displays
- 32-bit userspace detection on 64-bit Pi kernels
- Graph Y-axis label no longer clips top padding
- Print card info column taps now navigate to status screen
- Watchdog double-instance prevented on supervised restart
- Internal splash skipped when external splash process is running
- Resolution auto-detection enabled at startup

### Changed
- Z-offset scale layout dynamically adapts to measured label widths
- Filament panel temperature updates are targeted instead of full-refresh
- Machine limits G-code debounced to reduce unnecessary sends
- Delete button on print detail uses danger styling

## [0.9.7] - 2026-02-08

Z-offset calibration redesigned from scratch with a Prusa-style visual meter,
plus display reliability fixes and hardware detection improvements.

### Added
- Z-offset calibration overhaul: Prusa-style vertical meter with draw-in arrow animation, horizontal step buttons, auto-ranging scale, saved offset display, and auto-navigation when calibration is in progress
- Z-offset calibration strategy system for printer-specific save commands
- Automatic update notifications with dismiss support
- Sleep While Printing toggle to keep display on during prints
- Hardware detection: mainboard identification heuristic, non-printer addon exclusion, and kinematics filtering
- Calibration features gated behind beta feature flag

### Fixed
- Crash from rapid filament load/unload button presses
- Crash dialog not initializing touch calibration config
- Keyboard shortcuts firing when typing in text inputs
- Parent directory (..) not always sorted first in file browser
- Splash screen crash when prerendered assets missing
- Console bleed-through on fbdev displays
- Display not repainting fully after wake from sleep
- Moonraker updates switched from git_repo to zip type for reliability
- Thumbnail format forced to ARGB8888 for correct rendering
- Print outcome badges misaligned above thumbnail
- Scroll-on-focus causing unwanted panel jumps
- Install service filtering to only existing system groups
- Screws tilt adjust detection from configfile fallback
- Wizard saving literal 'None' instead of empty string for unselected hardware
- Mock printer kinematics matching actual printer type
- Touch calibration detection unified with headless pointer support

### Changed
- Dark mode applies live without restart
- Calibration button layout redesigned Mainsail-style
- Textarea widgets migrated to text_input component
- Redundant kinematics polling eliminated

## [0.9.6] - 2026-02-08

### Added
- Per-object G-code toolpath thumbnails in Print Objects overlay
- AFC (Armored Turtle) support: live device state, tool mapping, endless spool, per-lane reset, maintenance and LED controls, quiet mode, and mock simulation
- Active object count shown on layer progress line during printing
- Change Host modal for switching Moonraker connection in settings
- Z movement style override setting and E-Stop relocated to Motion section
- K1 dynamic linking toolchain and build target
- Creality K2 series cross-compilation target (ARM, static musl — untested, needs hardware validation)
- CDN-first installer downloads with GitHub fallback
- Multi-channel R2 update distribution with GitHub API fallback

### Fixed
- Toasts now render on top layer instead of active screen (fixes toasts hidden behind overlays)
- Print cancel timeout increased to 15s with active state observation for more reliable cancellation
- Pre-print time estimates seeded from slicer data with blended early progress
- Thread-safe slicer estimate seeding during print start
- G-code viewer cache thrash from current_object changes during exclude-object prints
- ForgeX startup framebuffer stomping by S99root init script
- Wrong-platform binary install prevented with ELF architecture validation and safe rollback
- Use-after-free crash on Print Objects overlay close
- Isometric thumbnail rendering with shared projection, depth shading, and thicker lines
- Install warning text centered in update download modal
- Missing alert_circle icon codepoint
- Settings About section consolidated with cleaner version row layout
- Z baby step icons and color swatch labels
- Exclude object mock mode: objects populated from G-code on print start with proper status dispatch

## [0.9.5] - 2026-02-07

### Added
- Exclude object support for streaming/2D mode with selection brackets and long-press interaction
- Print Objects list overlay showing defined objects during a print
- LED selection dropdown in settings for multi-LED printers
- Version number displayed on splash screen
- Beta and dev update channels with UI toggle and R2 upload script
- Beta feature wrapper component with badge indicator
- 32-bit ARM (armv7l) Raspberry Pi build target (#10)
- Auto-publish tagged releases to R2 with platform detection
- Exclude object G-code parsing and status dispatch in mock mode

### Fixed
- Use-after-free race in wpa_supplicant backend shutdown (#8)
- Deadlock in Happy Hare and ToolChanger AMS backend start (#9)
- DNS resolver fallback for static glibc builds
- Crash when navigating folders during metadata fetch in print selection
- LED detection excluding toolhead LEDs from main LED control
- WebSocket max message size increased from 1MB to 5MB (#7)
- Elapsed/remaining time display during mock printing
- Crash on window close from SDL event handling during shutdown
- Accidental scroll taps by increasing scroll limit default
- G-code parser now reads layer_height, first_layer_height, object_height from metadata
- Invalid text_secondary color token replaced with text_muted
- KIAUH metadata wrapper key and moonraker updater path (#3)
- Installer sparse checkout for updater repo (#11)
- Output_pin lights detected as LEDs with fallback to first LED (#14)
- Percentage rounding instead of truncating to fix float precision (#14)
- Z offset display sync when print tune overlay opens (#14)
- CoreXZ treated as gantry-moves-Z instead of bed-moves (#14)

### Changed
- Log levels cleaned up: INFO is concise, DEBUG is useful without per-layer/shutdown spam
- Duplicate log bugs fixed (PrintStartCollector double-completion, PluginManager double-unload)
- Settings panel version rows deduplicated
- Exclude object modal XML registration and single-select behavior

## [0.9.4] - 2026-02-07

### Added
- Pre-print time predictions based on historical heating/homing data
- Heater status text on temperature cards (Heating, Cooling, At Target)
- Slicer estimated time fallback for remaining time
- Seconds in duration display under 5 minutes

### Fixed
- Crash on 16bpp HDMI screens from forced 32-bit color format
- Elapsed time using wall-clock duration instead of print-only time
- Pre-print overlay showing when it shouldn't
- Backlight not turning off on AD5M
- Heater status colors (heating=red, added cooling state)
- AMS row hidden when no AMS connected
- Modal button alignment
- Install script version detection on Pi (#6)

## [0.9.3] - 2026-02-06

First public beta release. Core features are complete — we're looking for early
adopters to help find edge cases.

**Supported platforms:** Raspberry Pi (aarch64), FlashForge AD5M (armv7l),
Creality K1 (MIPS32)

> **Note:** K1 binaries are included but have not been tested on hardware. If you
> have a K1, we'd love your help verifying it works!

### Added
- Print start profiles with modular, JSON-driven signal matching for per-printer phase detection
- NetworkManager WiFi backend for broader Linux compatibility
- `.3mf` file support in print file browser
- Non-printable file filtering in print selection
- Beta features gating system for experimental UI (HelixPrint plugin)
- Platform detection and preset system for zero-config installs
- Settings action rows with bind_description for richer UI
- Restart logic consolidated into single `app_request_restart_service()` entry point

### Fixed
- Print start collector not restarting after a completed print
- Sequential progress regression on repeated signals during print start
- Bed mesh triple-rendering and profile row click targets
- Wizard WiFi step layout, password visibility toggle, and dropdown corruption
- Touch calibration skipped for USB HID touchscreens (HDMI displays)
- CJK glyph inclusion from C++ sources in font generation
- File ownership for non-root deploy targets
- Console cursor hidden on fbdev displays

### Changed
- Pi deploys now use `systemctl restart` instead of stop/start
- fbdev display backend for Pi (avoids DRM master contention)
- Comprehensive architectural documentation from 5-agent audit
- Troubleshooting guide updated with debug logging instructions

## [0.9.2] - 2026-02-05

Major internal release with live theming, temperature sensor support, and
extensive UI polish across all panels.

### Added
- Live theme switching without restart — change themes in settings instantly
- Dark/light gradient backgrounds and themed overlay constants
- Full-screen 3D splash images with dark/light mode support
- Temperature sensor manager for auxiliary temp sensors (chamber, enclosure, etc.)
- Responsive fan dial with knob glow effect
- Software update checker with download progress and install-during-idle safety
- Platform hook architecture for modularized installer functions
- Auto-detect Pi install path from Klipper ecosystem
- AD5M preset with auto-detection for zero-config setup
- Beta features config flag for gating experimental UI
- CJK glyph support (Chinese, Japanese, Russian) in generated fonts
- Pencil edit icons next to temperature controls
- OS version, MCU versions, and printer name in About section
- Shell tests (shellcheck, bats) gating release builds

### Fixed
- Shutdown crash: stop animations before destroying panels to prevent use-after-free
- Observer crash: reorder display/subject teardown sequence
- Stale widget pointer guards for temperature and fan updates
- Theme palette preservation across dark/light mode switches
- Button text contrast for layout=column buttons with XML children
- Navbar background not updating on theme toggle
- Dropdown corruption with `&#10;` newline entities in XML
- Wizard initialization: fan subscriptions, sensor select, toast suppression
- Kinematics detection and Z button icons for bed-moves printers
- Bed mesh data normalization and zero plane visibility
- Filament panel deferred `set_limits` to main thread
- Touch calibration target spread and full-screen capture

### Changed
- Pi builds target Debian Bullseye for wider compatibility
- Static-link OpenSSL for cross-platform SSL support
- Binaries relocated to `bin/` subdirectory in deploy packages
- Fan naming uses configured roles instead of heuristics
- HelixScreen brand theme set as default
- Installer modularized with platform dispatchers
- Release build timeout increased to 60 minutes

## [0.9.1] - 2026-02-04

Initial tagged release. Foundation for all subsequent development.

### Added
- 30 panels and 16 overlays covering full printer control workflow
- First-run setup wizard with 8-step guided configuration
- Multi-material support: AFC, Happy Hare, tool changers, ValgACE, Spoolman
- G-code preview and 3D bed mesh visualization
- Calibration tools: input shaper, mesh leveling, screws tilt, PID, firmware retraction
- Internationalization system with hot-reload language switching
- Light and dark themes with responsive 800x480+ layout
- Cross-compilation for Pi (aarch64), AD5M (armv7l), K1 (MIPS32)
- Automated GitHub Actions release pipeline
- One-liner installation script with platform auto-detection

[0.13.7]: https://github.com/prestonbrown/helixscreen/compare/v0.13.6...v0.13.7
[0.13.6]: https://github.com/prestonbrown/helixscreen/compare/v0.13.5...v0.13.6
[0.13.5]: https://github.com/prestonbrown/helixscreen/compare/v0.13.4...v0.13.5
[0.13.4]: https://github.com/prestonbrown/helixscreen/compare/v0.13.3...v0.13.4
[0.13.3]: https://github.com/prestonbrown/helixscreen/compare/v0.13.2...v0.13.3
[0.13.2]: https://github.com/prestonbrown/helixscreen/compare/v0.13.1...v0.13.2
[0.13.1]: https://github.com/prestonbrown/helixscreen/compare/v0.13.0...v0.13.1
[0.13.0]: https://github.com/prestonbrown/helixscreen/compare/v0.12.1...v0.13.0
[0.12.1]: https://github.com/prestonbrown/helixscreen/compare/v0.12.0...v0.12.1
[0.12.0]: https://github.com/prestonbrown/helixscreen/compare/v0.11.1...v0.12.0
[0.11.1]: https://github.com/prestonbrown/helixscreen/compare/v0.11.0...v0.11.1
[0.11.0]: https://github.com/prestonbrown/helixscreen/compare/v0.10.14...v0.11.0
[0.10.14]: https://github.com/prestonbrown/helixscreen/compare/v0.10.13...v0.10.14
[0.10.13]: https://github.com/prestonbrown/helixscreen/compare/v0.10.12...v0.10.13
[0.10.12]: https://github.com/prestonbrown/helixscreen/compare/v0.10.11...v0.10.12
[0.10.11]: https://github.com/prestonbrown/helixscreen/compare/v0.10.10...v0.10.11
[0.10.10]: https://github.com/prestonbrown/helixscreen/compare/v0.10.9...v0.10.10
[0.10.9]: https://github.com/prestonbrown/helixscreen/compare/v0.10.8...v0.10.9
[0.10.8]: https://github.com/prestonbrown/helixscreen/compare/v0.10.7...v0.10.8
[0.10.7]: https://github.com/prestonbrown/helixscreen/compare/v0.10.6...v0.10.7
[0.10.6]: https://github.com/prestonbrown/helixscreen/compare/v0.10.5...v0.10.6
[0.10.5]: https://github.com/prestonbrown/helixscreen/compare/v0.10.4...v0.10.5
[0.10.4]: https://github.com/prestonbrown/helixscreen/compare/v0.10.3...v0.10.4
[0.10.3]: https://github.com/prestonbrown/helixscreen/compare/v0.10.2...v0.10.3
[0.10.2]: https://github.com/prestonbrown/helixscreen/compare/v0.10.1...v0.10.2
[0.10.1]: https://github.com/prestonbrown/helixscreen/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/prestonbrown/helixscreen/compare/v0.9.24...v0.10.0
[0.9.24]: https://github.com/prestonbrown/helixscreen/compare/v0.9.23...v0.9.24
[0.9.23]: https://github.com/prestonbrown/helixscreen/compare/v0.9.22...v0.9.23
[0.9.22]: https://github.com/prestonbrown/helixscreen/compare/v0.9.21...v0.9.22
[0.9.21]: https://github.com/prestonbrown/helixscreen/compare/v0.9.20...v0.9.21
[0.9.20]: https://github.com/prestonbrown/helixscreen/compare/v0.9.19...v0.9.20
[0.9.19]: https://github.com/prestonbrown/helixscreen/compare/v0.9.18...v0.9.19
[0.9.18]: https://github.com/prestonbrown/helixscreen/compare/v0.9.17...v0.9.18
[0.9.17]: https://github.com/prestonbrown/helixscreen/compare/v0.9.16...v0.9.17
[0.9.16]: https://github.com/prestonbrown/helixscreen/compare/v0.9.15...v0.9.16
[0.9.15]: https://github.com/prestonbrown/helixscreen/compare/v0.9.14...v0.9.15
[0.9.14]: https://github.com/prestonbrown/helixscreen/compare/v0.9.13...v0.9.14
[0.9.13]: https://github.com/prestonbrown/helixscreen/compare/v0.9.12...v0.9.13
[0.9.12]: https://github.com/prestonbrown/helixscreen/compare/v0.9.11...v0.9.12
[0.9.11]: https://github.com/prestonbrown/helixscreen/compare/v0.9.10...v0.9.11
[0.9.10]: https://github.com/prestonbrown/helixscreen/compare/v0.9.9...v0.9.10
[0.9.9]: https://github.com/prestonbrown/helixscreen/compare/v0.9.8...v0.9.9
[0.9.8]: https://github.com/prestonbrown/helixscreen/compare/v0.9.7...v0.9.8
[0.9.7]: https://github.com/prestonbrown/helixscreen/compare/v0.9.6...v0.9.7
[0.9.6]: https://github.com/prestonbrown/helixscreen/compare/v0.9.5...v0.9.6
[0.9.5]: https://github.com/prestonbrown/helixscreen/compare/v0.9.4...v0.9.5
[0.9.4]: https://github.com/prestonbrown/helixscreen/compare/v0.9.3...v0.9.4
[0.9.3]: https://github.com/prestonbrown/helixscreen/compare/v0.9.2...v0.9.3
[0.9.2]: https://github.com/prestonbrown/helixscreen/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/prestonbrown/helixscreen/releases/tag/v0.9.1
