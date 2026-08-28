# docs/user/CLAUDE.md — User Documentation

These docs are **end-user facing**. They must be written for people who are NOT developers — clear language, no implementation details, no source code references.

## Style Rules

- Write for someone who just bought a Raspberry Pi and a touchscreen
- Use step-by-step instructions with exact commands
- Screenshots are better than descriptions
- Never reference source files, class names, or internal architecture
- Config examples should be copy-pasteable
- When mentioning settings, show the exact path in the UI (e.g., "Settings > System > Touch & Input")
- Test all instructions on a clean install before publishing

## User Docs Index

| Doc | Contents |
|-----|----------|
| `USER_GUIDE.md` | Landing page with links to all guide sub-pages |
| `guide/getting-started.md` | Navigation, touch gestures, setup wizard, keyboard |
| `guide/supported-printers.md` | Per-printer feature detail for deeply-integrated models (AD5X IFS, K1/K2 CFS, QIDI Box, Snapmaker U1, ACE), generic auto-detection baseline |
| `guide/creality-k1c-setup.md` | End-to-end K1/K1C/K1 Max setup: root access, Simple AF / Guilouz firmware, HelixScreen install, updating, firmware upgrade path |
| `guide/home-panel.md` | Home dashboard, printer manager, custom images |
| `guide/printing.md` | File selection, printing, tune overlay, Z-offset |
| `guide/print-monitoring.md` | Pre-print filament checks (empty-slot block, unassigned-tool Color Mismatch, bypass exemption), camera-based failure detection (Snapmaker U1 dialog, Creality K2 AI toggle) |
| `guide/temperature.md` | Nozzle/bed temperature panels, presets, reassigning preset filament types, graphs |
| `guide/motion.md` | Jog pad, homing, distance increments, E-stop |
| `guide/filament.md` | Extrusion, AMS, CFS, Spoolman, dryer control |
| `guide/filament-tracking.md` | Filament tracking with/without Spoolman, usage estimation, connecting Spoolman, spool inventory |
| `guide/bluetooth-setup.md` | Enabling Bluetooth on Raspberry Pi and BTT Pi, UART conflicts, USB dongle setup |
| `guide/barcode-scanner.md` | USB + Bluetooth barcode scanner setup, keymaps, BT-HID bonded-only workaround |
| `guide/label-printing.md` | Label printer setup, supported printers (Brother/Phomemo/Niimbot/MakeID), Bluetooth pairing, troubleshooting |
| `guide/calibration.md` | Bed mesh, screws tilt, input shaper, PID |
| `guide/touch-calibration.md` | Touch screen calibration, forcing recalibration, config reference |
| `guide/settings.md` | Settings hub page with links to sub-pages |
| `guide/settings/display-sound.md` | Language, timezone, animations, display settings, sound settings, home widgets |
| `guide/settings/printing.md` | Toolhead, G-code preview, Z movement, machine limits, retraction, timelapse, macros |
| `guide/settings/hardware.md` | Hardware issues, printers, multi-filament systems, fans, sensors, LEDs, power devices, Spoolman |
| `guide/settings/safety.md` | E-Stop confirmation, cancel escalation, print completion alert |
| `guide/settings/system.md` | Security, network, host, touch & input link, plugins, telemetry, reset |
| `guide/settings/touch-input.md` | Touch calibration, debug viz, scroll engage distance, scroll guard |
| `guide/settings/help-about.md` | Debug bundles, Discord, docs, version, updates, print hours |
| `guide/settings/led-settings.md` | LED strip selection, auto-state, macro devices, setup guides |
| `guide/fans.md` | Discovered fans grouped by controllable vs. automatic, live speed dials, RPM readouts, per-fan renaming |
| `guide/sensors.md` | Filament and motion sensors with per-sensor role assignment, read-only sensor types, chamber heater/sensor mapping |
| `guide/security.md` | PIN screen lock, auto-lock tied to display sleep, lock-screen keypad, what a factory reset clears |
| `guide/camera.md` | Webcam viewing via the home widget and fullscreen viewer, rotation/flip, stream status, performance throttling |
| `guide/print-history.md` | History dashboard with time-range statistics and trend charts, searchable job list, per-job details, reprinting |
| `guide/advanced.md` | Console, macros, power, history, timelapse |
| `guide/beta-features.md` | Beta activation, feature list, update channels |
| `guide/tips.md` | Workflow tips, troubleshooting, panel reference |
| `INSTALL.md` | Installation guide for all platforms |
| `CONFIGURATION.md` | All settings explained with examples |
| `TROUBLESHOOTING.md` | Common problems and solutions |
| `FAQ.md` | Quick answers to common questions |
| `UPGRADING.md` | Version upgrade instructions |
| *(Plugin development guide moved to `devel/PLUGIN_DEVELOPMENT.md`)* | |
| `TESTING_INSTALLATION.md` | Post-install verification steps |
| `TELEMETRY.md` | What telemetry collects, privacy controls, opt-in/out |
| `PRIVACY_POLICY.md` | Privacy policy for telemetry data |

## When Updating User Docs

- New features: Add to the appropriate `guide/*.md` sub-page
- New settings: Add to appropriate `guide/settings/*.md` sub-page and `CONFIGURATION.md`
- New install methods/platforms: Add to `INSTALL.md`
- Known issues: Add to `TROUBLESHOOTING.md`
- After every release: Review all user docs for accuracy
