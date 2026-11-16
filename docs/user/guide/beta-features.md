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

When beta features are enabled, the following appear in the UI:

| Feature | Location | Description | Status |
|---------|----------|-------------|--------|
| **Dev Update Channel** | Settings > Updates | Adds **Dev** to the Update Channel selector, which otherwise offers Stable and Beta to everyone | Functional; needs `dev_url` set in `/var/lib/helixscreen/update_urls.json` (root-owned) |

> **Graduated from beta:** the **Sound System**, PID Calibration, Input Shaper, the **Spool Wizard**, the **G-code Console**, **Probe Management**, **Z-Offset Calibration**, **Timelapse**, the **Macro Browser**, **MPC Calibration**, **Belt Tension**, **Pressure Advance Tuning** (with the print Tune overlay's PA row and saving pressure advance to Spoolman), **Tool Offsets**, **Pause at a Layer** and **Choosing Lanes to Dry** are available to all users without enabling beta features. The **HelixPrint plugin** install prompt and install/uninstall rows, the **Z Calibration** button, and **Multi-Printer Management** are likewise available to everyone.

---

## ESP32-S3 Firmware (Alpha)

Besides running on a printer's host computer (Linux, Raspberry Pi, Android), HelixScreen can be built as firmware that runs directly on ESP32-S3 display panels — the BTT K-Touch class of hardware.

**Status: alpha.** This is an early, experimental port for developers and early testers. Expect rough edges, missing features, and instability: not for daily use. The Linux, Pi, and Android targets are unaffected and remain the production ones.

What works today:

- Boots to the Home panel; heavier screens load on first visit to fit the panel's memory
- Live printer status and bed mesh over WiFi through Moonraker
- WiFi setup on first boot: the panel broadcasts its own setup hotspot — join it from your phone to configure the network
- Updating over USB that keeps your WiFi and settings. There are no over-the-air updates yet
- Seven languages (English, German, French, Spanish, Russian, Portuguese, Italian); Chinese and Japanese need a font the panel has no room for
- Printer pictures for the DIY and Klipper-converted machines an add-on panel usually drives (Voron, RatRig, Sovol SV08, Zero G and others); other printers show a generic picture
- Notification history: tap the bell to see past alerts
- Temperature graphs that start from the printer's recent history

Not yet available on this target: the camera feed and QR features, the 2D G-code view, and the 3D bed mesh view.

Release builds (1.1 betas and newer) include a ready-to-flash firmware zip. Installing, first-boot setup, updating and recovery are covered in [K-Touch Install](install-esp32.md).

---

## Update Channel Selection

The channel selector in **Settings** → **About** offers **Stable** and **Beta** on
every install. Enabling beta features adds a third entry:

| Channel | Description | Needs beta features |
|---------|-------------|---------------------|
| **Stable** | Production releases (default) | No |
| **Beta** | Pre-release builds for testing upcoming features | No |
| **Dev** | Development builds — latest code, may be unstable | Yes |

The update channel can also be set via `update.channel` in the config file (0=Stable, 1=Beta, 2=Dev).
A stored Dev falls back to Stable while beta features are off; the stored value is
kept, so turning beta back on restores Dev.

---

**Next:** [Tips & Best Practices](tips.md) | **Prev:** [Advanced Features](advanced.md) | [Back to User Guide](../USER_GUIDE.md)
