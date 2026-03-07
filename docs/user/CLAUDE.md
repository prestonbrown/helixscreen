# docs/user/CLAUDE.md — User Documentation

These docs are **end-user facing**. They must be written for people who are NOT developers — clear language, no implementation details, no source code references.

## Style Rules

- Write for someone who just bought a Raspberry Pi and a touchscreen
- Use step-by-step instructions with exact commands
- Screenshots are better than descriptions
- Never reference source files, class names, or internal architecture
- Config examples should be copy-pasteable
- When mentioning settings, show the exact path in the UI (e.g., "Settings > Advanced > Beta Features")
- Test all instructions on a clean install before publishing

## User Docs Index

| Doc | Contents |
|-----|----------|
| `USER_GUIDE.md` | Landing page with links to all guide sub-pages |
| `guide/getting-started.md` | Navigation, touch gestures, setup wizard, keyboard |
| `guide/home-panel.md` | Home dashboard, printer manager, custom images |
| `guide/printing.md` | File selection, printing, tune overlay, Z-offset |
| `guide/temperature.md` | Nozzle/bed temperature panels, presets, graphs |
| `guide/motion.md` | Jog pad, homing, distance increments, E-stop |
| `guide/filament.md` | Extrusion, AMS, Spoolman, dryer control |
| `guide/calibration.md` | Bed mesh, screws tilt, input shaper, PID |
| `guide/touch-calibration.md` | Touch screen calibration, forcing recalibration, config reference |
| `guide/settings.md` | Settings hub page with links to sub-pages |
| `guide/settings/appearance.md` | Language, animations, 3D preview, display settings |
| `guide/settings/printer.md` | Filament sensors, AMS, Spoolman, LEDs, retraction, macros |
| `guide/settings/notifications.md` | Sound settings, print completion alerts |
| `guide/settings/motion.md` | Z movement, machine limits, E-Stop, cancel escalation |
| `guide/settings/system.md` | Network, host, printers, touch cal, hardware, plugins, telemetry, reset |
| `guide/settings/help-about.md` | Debug bundles, Discord, docs, version, updates, print hours |
| `guide/settings/led-settings.md` | LED strip selection, auto-state, macro devices, setup guides |
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
