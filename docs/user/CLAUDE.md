# docs/user/CLAUDE.md — User Documentation

These docs are **end-user facing**. They must be written for people who are NOT developers — clear language, no implementation details, no source code references.

## Style Rules

- Write for someone who just bought a Raspberry Pi and a touchscreen
- Use step-by-step instructions with exact commands
- Screenshots are better than descriptions
- Never reference source files, class names, or internal architecture
- Config examples should be copy-pasteable
- When mentioning settings, show the exact path in the UI (e.g., "Settings > Touch & Input > Scroll Guard")
- Test all instructions on a clean install before publishing

## User Docs Index

| Doc | Contents |
|-----|----------|
| `USER_GUIDE.md` | Landing page with links to all guide sub-pages |
| `guide/getting-started.md` | Navigation, touch gestures, setup wizard, keyboard |
| `guide/supported-printers.md` | Per-printer feature detail for deeply-integrated models (AD5X IFS, K1/K2 CFS, QIDI Box, Snapmaker U1, ACE), generic auto-detection baseline |
| `guide/install-k1.md` | End-to-end K1/K1C/K1 Max setup: root access, Simple AF / Guilouz firmware, HelixScreen install, updating, firmware upgrade path (`guide/creality-k1c-setup.md` is a redirect stub to it) |
| `guide/install-ad5m.md` | End-to-end Adventurer 5M / 5M Pro setup: Forge-X / ZMOD / Klipper Mod install, service control, updating, uninstall |
| `guide/install-ad5x.md` | Adventurer 5X (ZMOD) setup: ZMOD prerequisite, chroot install, updating, uninstall |
| `guide/install-cc1.md` | Centauri Carbon setup: COSMOS firmware flash, HelixScreen install, gui-switcher, updating, uninstall |
| `guide/install-k2.md` | End-to-end K2 / K2 Plus / K2 Pro setup: stock-firmware install, service control, updating, uninstall |
| `guide/install-sonicpad.md` | Sonic Pad setup: SonicPad-Debian requirement, install, updating, uninstall |
| `guide/install-u1.md` | Snapmaker U1 setup: SSH paths, install, firmware upgrades, blank-screen recovery, uninstall |
| `guide/install-linux-desktop.md` | x86_64 Linux desktop package: the app in a window, per-user install with `--desktop`, connecting to a remote printer, window size, updating, uninstall, running from the tarball |
| `guide/install-esp32.md` | BigTreeTech K-Touch (ESP32-S3) alpha firmware: browser flasher, esptool install, first-boot hotspot setup, updating, recovery, other ESP32-S3 panels |
| `guide/home-panel.md` | Home dashboard, printer manager, custom images |
| `guide/printing.md` | File selection, printing, the file-fits-printer check, the door-open warning, pausing at a layer, tune overlay, Z-offset, recovering after a power loss (firmware recovery and the plugin's guided resume) |
| `guide/print-monitoring.md` | Pre-print filament checks (empty-slot block, unassigned-tool Color Mismatch, bypass exemption), camera-based failure detection (Snapmaker U1 dialog, Creality K2 AI toggle) |
| `guide/temperature.md` | Nozzle/bed temperature panels, presets, reassigning preset filament types, graphs, drying filament on the bed, chamber heater diagnostics card |
| `guide/chamber-heater.md` | Add-on chamber heater setup (BIGTREETECH Panda Breath): network step, stock vs DragonBreath firmware, the Klipper module and config each needs, the Snapmaker U1 menu shortcut, troubleshooting |
| `guide/motion.md` | Jog/Move/Bed tabs, jog pad, bed map, tap-to-move coordinates, bed position grid, lifting over a print left on the bed (Cleared), park, motors off, homing, E-stop |
| `guide/filament.md` | Extrusion, the QIDI M604 load wizard, AMS, CFS (and a K2 with none), Spoolman, filament drying and humidity by box |
| `guide/filament-tracking.md` | Filament tracking with/without Spoolman, usage estimation, connecting Spoolman, spool inventory |
| `guide/bluetooth-setup.md` | Enabling Bluetooth on Raspberry Pi and BTT Pi, UART conflicts, USB dongle setup |
| `guide/barcode-scanner.md` | USB + Bluetooth barcode scanner setup, keymaps, BT-HID bonded-only workaround |
| `guide/label-printing.md` | Label printer setup, supported printers (Brother/Phomemo/Niimbot/MakeID), Bluetooth pairing, troubleshooting |
| `guide/calibration.md` | Bed mesh, screws tilt, input shaper and its live chart, probe, Z-offset, tool offsets (including the Creator 5's printer-side prompt), vertical artifact compensation, belt tension check and tuning, pressure advance, PID/MPC |
| `guide/touch-calibration.md` | Touch screen calibration, forcing recalibration, config reference |
| `guide/settings.md` | Settings hub: the three groups (Screen, Printer, HelixScreen) and their thirteen pages |
| `guide/settings/display.md` | Screen rotation, UI scale, brightness, screen dim, display sleep, screensaver, sleep while printing |
| `guide/settings/appearance.md` | Dark mode, themes and the theme editor, Glass Effects, animations, widget labels, printer visuals (toolhead style, G-code preview, Z movement, bed mesh render) |
| `guide/settings/touch-input.md` | Touch calibration, debug viz, scroll engage distance, long press, home screen editing, scroll guard, scroll buttons, Android keyboard and navigation bar |
| `guide/settings/sound.md` | Sounds, volume, UI sounds, sound themes, output device, custom themes, supported hardware, troubleshooting |
| `guide/settings/printing.md` | Machine limits, motion, retraction, enclosure, multi-filament systems, material temperatures, cold load/unload, post-filament cooldown, tool-change confirmation, timelapse, macro buttons |
| `guide/settings/devices.md` | Hardware health, camera, print photos, fans, sensors, LEDs, power devices, Spoolman (with switching spools at each tool change) |
| `guide/settings/led-settings.md` | LED strip selection, auto-state, macro devices, setup guides |
| `guide/settings/safety.md` | Safety & Alerts: E-Stop confirmation, cancel escalation, macro confirmation, spaghetti detection, holding slicer prints that do not fit, print completion alert, on-screen alerts |
| `guide/settings/connection.md` | Network settings, printers, Moonraker host, screen sharing rows (details in guide/screen-sharing.md) |
| `guide/settings/language-time.md` | Language, timezone, time format |
| `guide/settings/system.md` | Security, storage (usage by kind, cleanup, automatic cleanup), performance, telemetry, log level, restart, factory reset |
| `guide/settings/updates.md` | Update channel (and its sync with moonraker.conf), check and install, firmware-managed and can't-update notices |
| `guide/settings/help-about.md` | Welcome tour, debug bundles, Discord, docs, About (version, system info, print hours, beta features) |
| `guide/fans.md` | Discovered fans grouped by controllable vs. automatic, live speed dials, RPM readouts, chamber air modes, per-fan renaming |
| `guide/sensors.md` | Filament and motion sensors with per-sensor role assignment, door/button inputs and the door-open warning, read-only sensor types, chamber heater/sensor mapping |
| `guide/security.md` | PIN screen lock, auto-lock tied to display sleep, lock-screen keypad, what a factory reset clears |
| `guide/screen-sharing.md` | Screen sharing and remote control: Settings rows, opening it in a browser or Mainsail/Fluidd, code pairing vs Moonraker-decided access per printer, the remote-control icon, cost, troubleshooting |
| `guide/camera.md` | Webcam viewing via the home widget and fullscreen viewer, rotation/flip, stream status, performance throttling |
| `guide/print-history.md` | History dashboard with time-range statistics and trend charts, searchable job list, per-job details (spools used, camera photos), reprinting |
| `guide/advanced.md` | Console, macros, power, history, timelapse |
| `guide/helixprint-plugin.md` | The HelixPrint plugin: what it is, installing it (on the printer's screen or by SSH command from a remote one), a feature table, each feature (file analysis, the fit check and holding slicer prints, start options, edited prints, spool switching, Printer Events, print photos, storage, config saves, AD5X/ACE updates, power-loss recovery) with and without it on attached vs remote screens, the Advanced overview, what "Requires the HelixPrint plugin" / "Requires a newer HelixPrint plugin" / "Only while this app is connected" mean |
| `guide/beta-features.md` | Beta activation, feature list, update channels |
| `guide/tips.md` | Workflow tips, troubleshooting, panel reference |
| `INSTALL.md` | Installation guide for all platforms |
| `CONFIGURATION.md` | All settings explained with examples |
| `TROUBLESHOOTING.md` | Common problems and solutions |
| `FAQ.md` | Quick answers to common questions |
| `UPGRADING.md` | Version upgrade instructions |
| `TESTING_INSTALLATION.md` | Post-install verification steps |
| `TELEMETRY.md` | What telemetry collects, privacy controls, opt-in/out |
| `PRIVACY_POLICY.md` | Privacy policy for telemetry data |

## When Updating User Docs

- New features: Add to the appropriate `guide/*.md` sub-page
- New settings: Add to appropriate `guide/settings/*.md` sub-page and `CONFIGURATION.md`
- New platforms: give them their own `guide/install-*.md`; universal install methods go in `INSTALL.md`
- Known issues: Add to `TROUBLESHOOTING.md`
- After every release: Review all user docs for accuracy
