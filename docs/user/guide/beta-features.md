# Beta Features

HelixScreen includes several features that are functional but still being refined. These are gated behind a beta flag so they can be tested without affecting the default experience.

---

## Enabling Beta Features

**Method 1: Secret tap (recommended)**

1. Go to **Settings** → tap the **About** row to open the About overlay
2. Tap the **Current Version** button **7 times** (like enabling Android Developer Mode)
3. A countdown appears after 4 taps ("3 more taps...", "2 more taps...", etc.)
4. A toast confirms "Beta features: ON"

Repeat the same process to disable beta features.

> **Note:** Taps must be within 2 seconds of each other or the counter resets.

**Method 2: Config file**

Set `"beta_features": true` in your `settings.json`.

**Method 3: Test mode**

Beta features are always enabled when running with `--test`.

---

## Beta Feature List

When beta features are enabled, the following appear in the UI with an orange "BETA" badge and left accent border:

| Feature | Location | Description | Status |
|---------|----------|-------------|--------|
| **HelixPrint Plugin** | Advanced panel | Install/uninstall the HelixPrint Klipper plugin for advanced print start control | Functional; plugin manages bed mesh, QGL, z-tilt skipping |
| **Configure PRINT_START** | Advanced panel | Make bed mesh and QGL skippable in your print start macro | Functional; needs only Beta Features enabled — writes your Klipper config directly via Moonraker, no plugin required |
| **Plugins** | Settings panel | View installed plugins and their status | Functional; plugin system is early-stage |
| **Update Channel** | Settings panel | Switch between Stable, Beta, and Dev update channels | Functional; Beta/Dev channels may have less-tested releases |
| **Macro Browser** | Advanced panel | Browse and execute custom Klipper macros | Functional; hides system macros, confirms dangerous ones |
| **Z Calibration** | Controls panel | Quick-access `PROBE_CALIBRATE` button (probe Z endstop) — distinct from the graduated Z-Offset Calibration flow | Functional; requires probe hardware |
| **MPC Calibration** | Heater Calibration panel | Model Predictive Control calibration as an alternative to PID | Functional; requires Kalico firmware |
| **Belt Tension** | Advanced panel | Measure and compare belt path resonant frequencies for CoreXY/Cartesian | Functional; requires accelerometer; optional PWM LED strobe |
| **Multi-Printer Management** | Settings panel, Navbar, Printer Manager | Add, switch between, and manage multiple Klipper printers from one touchscreen | Functional; switch/add/delete printers with one-tap switching |

> **Graduated from beta:** the **Sound System**, PID Calibration, Input Shaper, the **Spool Wizard**, the **G-code Console**, **Probe Management**, **Z-Offset Calibration**, and **Timelapse** are now available to all users without enabling beta features.

---

## ESP32-S3 Firmware (Alpha)

Besides running on a printer's host computer (Linux, Raspberry Pi, Android), HelixScreen can be built as firmware that runs directly on ESP32-S3 display panels — the BTT K-Touch class of hardware.

**Status: alpha.** This is an early, experimental port for developers and early testers. Expect rough edges, missing features, and instability — not for daily use, and not a scheduled release. The Linux, Pi, and Android targets are unaffected and remain the production ones.

What works today:

- Boots to the Home panel; heavier screens load on first visit to fit the panel's memory
- Live printer status and bed mesh over WiFi through Moonraker
- WiFi setup on first boot: the panel broadcasts its own setup hotspot — join it from your phone to configure the network
- Over-the-air updates, with a fallback slot to recover a bad flash
- All nine languages and the printer image set, packed to fit the panel's storage

Not yet available on this target: the camera feed and QR features, the 2D G-code view, and the 3D bed mesh view.

There is no release download yet — the firmware is built from the source tree with the ESP-IDF toolchain.

---

## Update Channel Selection

When beta features are enabled, a channel selector appears in **Settings** → **About**:

| Channel | Description |
|---------|-------------|
| **Stable** | Production releases (default) |
| **Beta** | Pre-release builds for testing upcoming features |
| **Dev** | Development builds — latest code, may be unstable |

The update channel can also be set via `update.channel` in the config file (0=Stable, 1=Beta, 2=Dev).

---

**Next:** [Tips & Best Practices](tips.md) | **Prev:** [Advanced Features](advanced.md) | [Back to User Guide](../USER_GUIDE.md)
