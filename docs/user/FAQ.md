# Frequently Asked Questions

Quick answers to common questions about HelixScreen.

---

## General

### What is HelixScreen?

HelixScreen is a touchscreen interface for Klipper 3D printers. It connects to your Moonraker instance and provides a modern, touch-friendly UI for controlling your printer.

**Key features:**
- 30+ panels, 20+ overlays, and a customizable multi-page widget dashboard
- 3D G-code preview, bed mesh visualization, frequency response charts
- 7 multi-material backends (AFC, Happy Hare, ACE, AD5X IFS, CFS, Snapmaker U1, tool changers) with Spoolman integration
- First-run wizard with auto-detection of 80+ printer models
- Theme editor with 18 presets (dark and light), 9 languages
- Sound system, timelapse integration, label printing, exclude objects
- Auto-detecting layout system for displays from 480x320 to 1024x600, including ultrawide and portrait
- ~15MB RAM on embedded targets — designed for the modest hardware most people already own, no desktop required

### Which printers are supported?

HelixScreen works with any Klipper-based printer running Moonraker. Tested and supported platforms:

| Platform | Status | Notes |
|----------|--------|-------|
| Voron 0.1, 2.4, Trident | **Tested** | Primary development platforms |
| Doron Velta / RatRig V-Core | **Tested** | |
| FlashForge AD5M / 5M Pro | **Tested** | Requires Forge-X or Klipper Mod firmware |
| QIDI Q2, Max 4 | **Supported** | Stock firmware works (runs standard Moonraker); community firmware like FreeDi or FreeQIDI also supported. Plus 4 uses a TJC serial display and is not supported for on-device install — only remote control via Moonraker. |
| Creality K1 / K1C / K1 Max / K1 SE | **Supported** | Requires rooting or Guilouz firmware |
| Creality K2 Pro / K2 Plus | **Tested** | Runs natively with CFS support, on stock firmware and on community builds with a rewritten CFS module — see [Filament → CFS](guide/filament.md#creality-filament-system-cfs) |
| Creality Sonic Pad | **Supported** | 32-bit ARM, dedicated build. Tested only on [SonicPad-Debian](https://github.com/Jpe230/SonicPad-Debian); stock Creality firmware untested |
| Creality Hi | **Preliminary** | Auto-detected; Cartesian bedslinger with optional CFS. Untested on our hardware. |
| Anycubic Kobra 2 Pro / Kobra 3 / 3 V2 / 3 Max / S1 / S1 Max | **Community** | Auto-detected on [Rinkhals](https://github.com/jbatonnet/Rinkhals) firmware; native ACE (`filament_hub`) supported. Untested on our hardware. |
| FlashForge AD5X | **Tested** | IFS filament system integrated |
| SOVOL SV06 / SV08 | **Tested** | Community reports welcome |
| Elegoo Centauri Carbon 1 | **Tested** | Requires [OpenCentauri COSMOS](https://docs.opencentauri.cc/klipper-conversion/cosmos/cosmos/) firmware; ships with factory white-balance calibration |
| Snapmaker U1 (SnapSwap toolchanger) | **Tested** | Native four-head support with RFID spool recognition. Needs SSH — stock firmware (1.2+) via its **Root access** option, or PAXX Extended Firmware (SSH on by default). Tested on PAXX 1.2.x–1.4.x; stock-firmware support is newly added. Reinstall after a firmware update. On PAXX firmware you can also view/control the screen remotely in Mainsail/Fluidd (the "gui" webcam) via the firmware's `web remote_screen` toggle; physical cameras work normally. |
| Artillery M1 Pro | **Tested** | |
| Zero G Mercury / Nebula / Hydra | **Tested** | Multiple variants supported |
| Other Klipper printers | **Should work** | Any printer with Moonraker API access |

> **Note:** "Tested" means the HelixScreen team has verified the platform on real hardware. "Community" means a community user has confirmed it working but we haven't tested it ourselves. "Preliminary" means support exists from the printer's published config but has not been verified on hardware. See the [Installation Guide](INSTALL.md) for platform-specific instructions, and [Supported Printers](guide/supported-printers.md) for a feature-by-feature breakdown of what works on each specially-integrated model.

### Can I run HelixScreen on a separate device instead of on my printer?

Yes. HelixScreen is a Moonraker client — it only needs network access to your printer's Moonraker instance, so it doesn't have to run on the printer itself. Install it on any supported Linux device (a spare Raspberry Pi with a touchscreen, a mini PC with an HDMI touchscreen, even your desktop) and enter your **printer's IP address** in the wizard's Moonraker step instead of `localhost`. This is ideal when the printer is on the floor and you want the screen at your desk.

Point it at Moonraker (port `7125`), not the Mainsail/Fluidd web interface — you don't need Mainsail or Fluidd on the screen device at all. See [Remote Screen Setup](INSTALL.md#remote-screen-setup-run-on-a-separate-device) for step-by-step instructions.

### Which displays are supported?

**Tested and confirmed working:**
- BTT 5" HDMI/DSI touchscreen
- BTT 7" HDMI/DSI touchscreen
- FlashForge AD5M built-in 4.3" display (800x480)

**Active testing underway:**
- FlashForge AD5X built-in 4.3" display (800x480)

**Should work but not yet tested:**
- Official Raspberry Pi 7" DSI touchscreen
- Creality K2 built-in 4.3" display (480x800 — the panel is software-rotated to landscape; running it in portrait works too, though landscape is more polished)
- Other HDMI displays
- SPI displays (with proper configuration)

**Display sizes:** HelixScreen auto-detects the best layout for your display. 800x480 and 1024x600 are fully supported. 480x320 displays will run but may have layout overlap issues — improved small-screen support is ongoing.

**Ultrawide and portrait screens both work.** The layout engine detects an ultrawide screen (wider than about 2.5:1, e.g. 1920x480) or a portrait screen (narrower than about 0.8:1, e.g. 480x800) and adapts the navigation bar and grid sizing to it. Portrait additionally has dedicated layouts for the app shell, navigation bar, print status and print tune. Panels without a dedicated layout use the adaptive fallback, so landscape remains the most polished of the three.

The part that adapts furthest is the **home dashboard**. Its widget grid is sized from the actual screen rather than a fixed table, so a 480x800 portrait panel gets a 3x6 grid and a 320x1480 one gets 2x12 — more usable cells than before. Portrait also has its own set of default widgets (Tips is left out, since it is too wide to be worth a row on a narrow grid), and buttons, inputs, and headers are sized from the screen's height, so a tall panel gets taller controls instead of cramped ones. Nothing outside the home dashboard changes.

Both keep gaining per-panel work. If you want to help, they need only XML, not C++ — see the [UI Contributor Guide](../devel/UI_CONTRIBUTOR_GUIDE.md).

You can force either mode to try it: `helix-screen --layout ultrawide` or `--layout portrait`, or set `"layout": "ultrawide"` in the `display` section of `settings.json`.

**Display rotation:** All three binaries (main, splash, watchdog) support 0°, 90°, 180°, and 270° rotation via config or command line. Rotating a portrait panel to landscape (what the Creality K2 does) is well-trodden; leaving it in portrait and using the portrait layout also works, with the caveats above.

If you test on hardware not listed above, please let us know your results!

### Which slicer should I use?

HelixScreen reads standard G-code, so most slicers work. But support is tiered:

| Slicer | Status | Notes |
|--------|--------|-------|
| **OrcaSlicer 2.3.2+** | **Primary** | The slicer we develop and test against. Best experience — including one-way (HelixScreen → OrcaSlicer) filament preset sync with HelixScreen's filament slots. Recommended for everyone. |
| Manufacturer slicers (Creality Print, FlashForge Orca, Bambu Studio, etc.) | **Supported** | Most are OrcaSlicer/PrusaSlicer forks and work well. We aim to support them. |
| PrusaSlicer / SuperSlicer | **Supported** | Fully usable, including exclude-objects and pre-print options. |
| Cura | **Not targeted** | We don't test against Cura and don't build features for it, but we don't go out of our way to break it. Output generally works; some features (exclude objects, filament sync) need extra setup or aren't available. |

For the best results — accurate metadata, thumbnails, exclude-object support, and filament syncing — use **OrcaSlicer 2.3.2 or later**.

### Why is my layer count or time remaining inaccurate?

For an exact layer count and a reliable time-remaining estimate, HelixScreen needs your slicer to report layer info to Klipper via the `SET_PRINT_STATS_INFO` command in the printed G-code. Many stock slicer profiles don't emit it, so HelixScreen falls back to estimating from progress and Z-height — close, but not exact. Adding two short lines to your slicer's custom G-code fixes it. See [Troubleshooting → Layer count is wrong, stuck at 0, or total layers missing](TROUBLESHOOTING.md#layer-count-is-wrong-stuck-at-0-or-total-layers-missing) for the exact snippets per slicer.

### How is this different from KlipperScreen and GuppyScreen?

| Feature | HelixScreen | KlipperScreen | GuppyScreen |
|---------|-------------|---------------|-------------|
| **UI Framework** | LVGL 9 XML | GTK 3 (Python) | LVGL 8 (C) |
| **Declarative UI** | Full XML | Python only | C only |
| **Disk Size** | ~75-115MB | ~50MB | ~60-80MB |
| **RAM Usage** | ~15MB (32-bit) | ~50MB | ~15-20MB |
| **Reactive Binding** | Built-in | Manual | Manual |
| **3D G-code preview** | Yes | 2D layers | No |
| **3D bed mesh** | Yes | 2D heatmap | 2D heatmap |
| **Status** | 1.0 (active) | Mature (maintenance) | Unmaintained |

**HelixScreen advantages:**
- Low memory footprint (~15MB on embedded targets vs ~50MB for KlipperScreen on the same hardware)
- Declarative XML layouts (change UI without recompiling)
- Modern reactive architecture with 7 multi-material backends
- 3D visualizations (G-code preview, bed mesh)
- 80+ printer auto-detection database
- 9 languages (English, German, Spanish, French, Italian, Japanese, Portuguese, Russian, Chinese)

---

## Installation

### What Raspberry Pi do I need?

HelixScreen is light enough that whatever Pi you already own is almost certainly fine. The Pi you've had in a drawer for years works.

| Pi Model | Supported | Notes |
|----------|-----------|-------|
| Pi 5 | ✅ | Best performance, overkill for HelixScreen alone |
| Pi 4 | ✅ | Plenty for the touchscreen UI plus Klipper/Moonraker |
| Pi 3B+ | ✅ | Works well — no need to upgrade for HelixScreen's sake |
| Pi 3B | ✅ | Usable; may feel slow on heavy 3D mesh interactions |
| Pi Zero 2 W | ✅ | Great for space-constrained setups |
| Pi Zero (original) | ❌ | Too slow |

**Memory:** 1GB is enough. HelixScreen itself uses ~15MB on 32-bit and a few times more on 64-bit Pi — the rest is for Klipper, Moonraker, and the OS.

**32-bit and 64-bit:** Both are supported. The installer automatically detects your architecture (`uname -m`) and downloads the correct binary — `aarch64` gets the 64-bit build, `armv7l` gets the 32-bit build. No manual selection needed.

### Can I run HelixScreen alongside KlipperScreen/GuppyScreen?

**Not on the same display.** Both compete for the framebuffer. The HelixScreen installer automatically disables any existing screen UI.

**MainsailOS (systemd):**
```bash
# Disable KlipperScreen
sudo systemctl stop KlipperScreen
sudo systemctl disable KlipperScreen

# Enable HelixScreen
sudo systemctl enable helixscreen
sudo systemctl start helixscreen
```

**AD5M Klipper Mod (SysV init):**
```bash
# Disable KlipperScreen
/etc/init.d/S80klipperscreen stop
chmod -x /etc/init.d/S80klipperscreen

# Enable HelixScreen
chmod +x /etc/init.d/S80helixscreen
/etc/init.d/S80helixscreen start
```

**AD5M Forge-X (SysV init):**
```bash
# Disable GuppyScreen
/etc/init.d/S60guppyscreen stop
chmod -x /etc/init.d/S60guppyscreen

# Enable HelixScreen
chmod +x /etc/init.d/S90helixscreen
/etc/init.d/S90helixscreen start
```

If you have two displays, you could theoretically run both (advanced configuration, not tested).

### Do I need to install X11 or a desktop environment?

**No.** HelixScreen renders directly to the framebuffer (fbdev) or DRM. It doesn't need:
- X11 / Xorg
- Wayland
- Desktop environment (GNOME, KDE, etc.)
- Display manager (LightDM, GDM, etc.)

This is why it uses less memory than alternatives.

### Does this work with MainsailOS, FluiddPi, or other Klipper distros?

Yes! HelixScreen works with any Klipper distribution that includes Moonraker:
- MainsailOS ✅
- FluiddPi ✅
- Custom Klipper installs ✅
- KIAUH installs ✅

The web frontend you use (Mainsail, Fluidd, etc.) doesn't matter - HelixScreen talks to Moonraker.

### What gets preserved during upgrades?

Your configuration is safe. The installer preserves:
- `settings.json` — all your settings
- `helixscreen.env` — environment variable overrides
- Custom printer images in `config/custom_images/`
- Custom theme files in `config/themes/`

Your Klipper configuration is never touched. See the [Upgrade Guide](UPGRADING.md) for instructions.

### My display is upside down or rotated wrong

Set the rotation in your config file:

```bash
# Edit settings.json and add/change in the "display" section:
"rotation": 180
```

Valid values: `0`, `90`, `180`, `270`. Restart HelixScreen after changing.

---

## Features

### Does it support multiple extruders / toolchangers?

**Yes.** Full multi-extruder and toolchanger support:
- ✅ Per-extruder temperature control with extruder selector in the Temperature panel
- ✅ Toolchanger support via [klipper-toolchanger](https://github.com/viesturz/klipper-toolchanger) — active tool badge on Home panel (T0, T1, etc.)
- ✅ Tool-prefixed temperatures on the print status overlay
- ✅ Dynamic discovery of all extruders from Klipper (extruder, extruder1, extruder2, etc.)
- ✅ Multiple filament systems can run simultaneously (e.g. toolchanger + Happy Hare)

### Can I use my webcam?

**Yes.** HelixScreen shows your webcam feed on the home dashboard and during printing. It automatically detects webcams configured in Moonraker (crowsnest, camera-streamer, etc.) and displays the MJPEG stream.

For the best camera performance on Raspberry Pi, install `libturbojpeg0`:

```bash
sudo apt install libturbojpeg0
```

This enables SIMD-accelerated (hardware-optimized) JPEG decoding, which is 3-5x faster than the built-in software decoder. HelixScreen automatically uses it if available — no configuration needed. Without it, everything still works, just with slightly higher CPU usage during camera streaming.

### Does it work with Spoolman?

**Yes.** Spoolman integration is supported:
- **Advanced panel** → **Spoolman** to browse your spool inventory
- **Settings** → **Hardware & Devices** → **Spoolman** for weight sync settings
- Assign spools to AMS slots and track filament usage

### Can I print spool labels?

**Yes.** HelixScreen supports printing physical spool labels to thermal label printers:
- **Brother QL** — via Network or Bluetooth
- **Phomemo** — via USB or Bluetooth
- **Niimbot** — via Bluetooth (B21, D11, D110)
- **MakeID** — via Bluetooth (E1, L1, M1 — 9/12/16mm continuous tape)

Labels include spool name, material, color swatch, temperatures, and a QR code. See the [Label Printing Guide](guide/label-printing.md) for setup.

### Does it support Happy Hare or AFC-Klipper?

**Yes.** Full multi-material support is available for:
- **Happy Hare** — MMU2, ERCF, 3MS, Tradrack
- **AFC-Klipper** — Box Turtle, OpenAMS and ViViD, with full lane data, a live step bar during tool changes, and more than 25 device settings and actions grouped into their own sections
- **ACE** (Anycubic ACE Pro) — supported on native Anycubic firmware (the `filament_hub` Klipper object, e.g. Kobra on Rinkhals) and on the community ValgACE/BunnyACE/DuckACE Klipper drivers
- **Tool changers** — supported

Features include visual slot configuration with tool badges, endless spool arrows, tap-to-edit popup, Spoolman integration, and material compatibility validation.

### Can I customize the home screen widgets?

**Yes!** The Home Panel displays configurable widgets — quick-access buttons for features like temperature, LED control, network status, AMS, and more.

To customize:
1. Long-press the Home panel to enter Edit Mode
2. Tap the **+** button on the navigation bar to open the Widget Catalog and add widgets
3. Drag widgets to rearrange or resize them, then tap **Done**

Some widgets (like AMS, humidity sensor, or probe) only appear if the relevant hardware is detected. See the [Home Panel guide](guide/home-panel.md#available-widgets) for the full widget list.

### Is there a quicker way to shut down the printer?

Yes. Besides the settings menu, you can put a dedicated **Shutdown/Reboot widget** right on your home screen:

1. Enter **Edit Mode** on the Home Panel (long-press an empty area)
2. Add the **Shutdown/Reboot** widget from the Widget Catalog
3. Tap it any time — a confirmation dialog appears first, so you can't shut down by accident

See the [Shutdown/Reboot Widget](guide/home-panel.md#shutdownreboot-widget) section of the Home Panel guide for details, or the **Power** widget if you'd rather toggle a PSU or smart plug.

### Can I customize the colors or layout?

**Yes!** HelixScreen includes a built-in theme editor with 18 preset themes:

1. Go to **Settings** → **Display & Sound**
2. Tap **Theme Colors** to open the theme editor
3. Choose from presets: Ayu, Catppuccin, ChatGPT, Cupertino, Dracula, Everforest, Gruvbox, Hazard, HelixScreen (default), Kanagawa, Material Design, Midnight, Nord, One Dark, Rose Pine, Solarized, Tokyo Night, or Yami
4. Toggle dark/light mode
5. Customize individual colors if desired - changes are saved to `config/themes/`

For layout customization, use Edit Mode on the Home panel — long-press the dashboard to rearrange and resize widgets.

### Does it support multiple printers?

**Yes!** You can configure multiple Klipper printers and switch between them from the navigation bar or Settings. You view one printer at a time, but switching is instant. Enable beta features first (**Settings** → **Help & About** → **About**, then tap the version button 7 times), then add printers via **Settings** → **Hardware & Devices** → **Printers**.

### Can I view print history?

**Yes.** The History panel shows past prints with statistics, thumbnails, and details. Access via the navbar or home screen.

### Can I send G-code commands directly?

**Yes.** The Console panel lets you send G-code commands and view responses. Access via **Advanced** → **Console**.

### Does it support power device control?

**Yes.** If you have Moonraker power devices configured, the Power panel lets you control them. Access via **Settings** → **System** → **Power Devices**, or **Advanced** → **Power**, or long-press the home panel power button.

### Can I view and run bed mesh?

**Yes.** The Bed Mesh panel shows a 3D visualization of your bed mesh and lets you run calibration. Access via **Controls** → **Bed Mesh**.

### Does it support input shaper?

**Yes.** The Input Shaper panel provides a full calibration workflow with frequency response charts, per-axis results, shaper comparison tables, and Save Config. Access via **Advanced** → **Input Shaper**. Requires an accelerometer configured in Klipper.

### Does it support exclude objects?

**Yes.** During a print, you can exclude objects that failed. Tap the print status area to access exclude object controls.

### Can I run macros?

**Yes.** The Macro panel shows your Klipper macros. Access via **Advanced** → **Macros**. You can also configure quick macro buttons in **Settings** → **Printing** → **Macro Buttons**.

### Can I change which macro the Load / Unload / Purge buttons run?

**Yes.** Go to **Settings > Printing > Macro Buttons** and scroll to the **Standard Macros** section. Each button has a dropdown where you can select any macro from your Klipper config, or choose **(Auto)** to let HelixScreen detect it automatically. This works with or without an AMS system — see the [Filament guide](guide/filament.md#customizing-which-macro-runs) for details.

### Why does my nozzle cool down after a filament change?

**That's deliberate.** A load or unload heats the nozzle to material temperature, and HelixScreen turns the heater back off two minutes later so it doesn't sit hot indefinitely. The delay lets you run several operations back to back, and a running print is never interfered with.

If your filament system already does its own post-operation cooldown — AFC does — turn ours off at **Settings > Safety & Notifications > Cool nozzle after filament ops** so the two aren't both driving the heater. It's a per-printer setting, so your other machines keep the built-in behavior. See [Safety settings](guide/settings/safety.md#cool-nozzle-after-filament-ops).

### Can I customize the printer image on the home screen?

**Yes.** Tap the printer image on the Home Panel to open the Printer Manager, then tap the image again to open the Printer Image picker. You have three options:

- **Auto-Detect** (default) — HelixScreen picks an image based on your printer type from Klipper
- **Shipped Images** — Choose from 25+ pre-rendered images (Voron, Creality, FlashForge, Anycubic, RatRig, etc.)
- **Custom Images** — Drop a PNG or JPEG file into `config/custom_images/` and it appears automatically the next time you open the picker. You can also import images directly from a USB drive. Files must be under 5MB and 2048x2048 pixels max.

Your selection is saved to the `display.printer_image` config key and persists across restarts. See the [Printer Manager guide](guide/home-panel.md#changing-the-printer-image) for step-by-step instructions.

### Can I rename my printer?

**Yes.** Tap the printer image on the Home Panel to open the Printer Manager. Then tap the printer name (shown with a pencil icon) to enable inline editing. Type the new name and press **Enter** to save, or **Escape** to cancel. The name syncs automatically with Mainsail and Fluidd. See the [Printer Manager guide](guide/home-panel.md#changing-the-printer-name) for details.

### What languages are supported?

HelixScreen ships with 9 languages: English, German, Spanish, French, Italian, Japanese, Portuguese, Russian, and Chinese. Change the language in **Settings** → **Display & Sound** → **Language**.

### Does HelixScreen collect any data?

**Only if you opt in.** Telemetry is off by default. When enabled, it collects anonymous usage data (display resolution, platform, print outcomes) to help improve the software. No filenames, G-code, IP addresses, or personal information is ever collected. You can view, disable, and delete your data at any time in **Settings** → **System** → **Share Usage Data**. See the [Telemetry page](TELEMETRY.md) for full details.

---

## Usage

### Can I use a USB mouse or keyboard?

**Yes.** HelixScreen automatically detects USB mice and keyboards connected at startup. Both work alongside the touchscreen — you don't have to choose one or the other. A small white cursor appears when a mouse is detected. Combo devices like the Logitech K400 (keyboard + trackpad) also work.

Devices must be plugged in before HelixScreen starts. If auto-detection doesn't find your device, set `HELIX_MOUSE_DEVICE` or `HELIX_KEYBOARD_DEVICE` in `helixscreen.env` to the device path (run `cat /proc/bus/input/devices` to find it).

### How do I calibrate my touchscreen?

If taps register in the wrong location:
1. Go to **Settings** (gear icon)
2. Tap **System**, then **Touch & Input**
3. Tap **Touch Calibration**
4. Tap the crosshairs that appear on screen
5. Calibration saves automatically when complete

Note: This option only appears on touchscreen displays, not in the desktop simulator.

### How do I change the theme or colors?

1. Go to **Settings** → **Display & Sound**
2. Tap **Theme Colors** to open the theme editor
3. Browse available presets and see live preview
4. Toggle dark/light mode
5. Tap **Apply** to save (some changes require restart)

### How do I adjust settings during a print?

Tap the **Tune** button on the print status screen to access:
- **Print Speed** (50-200%) - Adjust movement speed
- **Flow Rate** (75-125%) - Adjust extrusion rate
- **Z-Offset** - Baby stepping for first layer adjustment

Fan control is available from the home screen fan widget or controls panel.

### Does HelixScreen support firmware retraction?

**Yes**, if your printer has `[firmware_retraction]` configured in Klipper. Go to **Settings** → **Printing** → **Retraction Settings** to adjust:
- Retract length and speed
- Unretract extra length and speed
- Enable/disable firmware retraction

This option only appears if Klipper reports firmware retraction capability.

### How do I check why the UI is slow?

1. **Check your display connection:** SPI displays are significantly slower than HDMI or DSI. If possible, use an HDMI or DSI-connected display for best performance.
2. **Disable animations:** Go to **Settings** → **Display & Sound** → toggle **Animations** off
3. **Check CPU/memory via SSH:** Run `top` or `htop` to see if something else is using resources
4. **Reduce logging:** If you added `-vv` or `-vvv` to the service, remove it
5. **Heavy 3D interactions feel slow?** Bed mesh rotation and gcode preview lean on the CPU/GPU; a Pi 4 or Pi 5 is smoother than a Pi 3 or Zero, but everything else in HelixScreen works fine on the older Pi tier.

### Why does the setup wizard keep appearing?

The wizard runs when no valid configuration exists. Causes:
1. Config file missing or deleted
2. Config file has invalid JSON
3. Permissions prevent reading config

**Fix:** Check `~/helixscreen/config/settings.json` exists and is valid JSON (on installs without a Klipper ecosystem the fallback path is `/opt/helixscreen/config/settings.json`).

### How do I change the Moonraker address?

There's currently no UI to change this after initial setup. Your options:

**Edit the config file directly:**
```bash
sudo nano ~/helixscreen/config/settings.json
# (fallback path if no Klipper ecosystem: /opt/helixscreen/config/settings.json)
# Edit moonraker_host and moonraker_port in the "printer" section
sudo systemctl restart helixscreen
```

**Or re-run the setup wizard:**
```bash
# Either delete the config to trigger wizard on next start:
# (fallback path if no Klipper ecosystem: /opt/helixscreen/config/settings.json)
sudo rm ~/helixscreen/config/settings.json
sudo systemctl restart helixscreen

# Or force wizard with command-line flag:
helix-screen --wizard
```

---

## Troubleshooting

### Touch doesn't work

1. **Check input device:** `ls /dev/input/event*`
2. **Test manually:** `sudo evtest /dev/input/event0`
3. **Specify device:** Add `"touch_device": "/dev/input/event1"` to `input` section in config

### Screen is black

1. **Check service:** `sudo systemctl status helixscreen`
2. **Check logs:** `sudo journalctl -u helixscreen -n 50`
3. **Check framebuffer:** `ls /dev/fb*` or `ls /dev/dri/*`
4. **Try specifying device:** Add `"drm_device": "/dev/dri/card1"` to `display` section in config

### Can't connect to Moonraker

1. **Check Moonraker:** `sudo systemctl status moonraker`
2. **Test manually:** `curl http://localhost:7125/printer/info`
3. **Check firewall:** `sudo ufw status`
4. **Verify IP:** `hostname -I`

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for more solutions.

---

## Development & Contributing

### Is HelixScreen open source?

Yes! HelixScreen is licensed under **GPL v3**. Source code is on GitHub.

### How can I contribute?

See the [Contributing Guide](../devel/DEVELOPMENT.md#contributing) for:
- Code standards
- Development setup
- Pull request process

We welcome:
- Bug reports
- Feature suggestions
- Code contributions
- Documentation improvements

### How do I build from source?

See the [Development Guide](../devel/DEVELOPMENT.md) for build instructions, dependencies, and development setup.

### What programming language is HelixScreen?

- **C++17** for application logic
- **XML** for UI layouts (LVGL 9 declarative system)
- **Makefile** for build system
- **Bash** for scripts

---

## Getting Help

### Where can I get help?

- **[HelixScreen Discord](https://discord.gg/RZCT2StKhr)** — community support, setup help, feature discussions, and development updates
- **[GitHub Issues](https://github.com/prestonbrown/helixscreen/issues)** — bug reports and feature requests

### Where can I report bugs?

Open an issue on [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) with:
- HelixScreen version
- Hardware info (Pi model, display)
- Steps to reproduce
- Relevant log output

### Where can I request features?

Open a GitHub issue with the "enhancement" label, or suggest it in the [Discord](https://discord.gg/RZCT2StKhr).

### How do I enable debug logging?

**Easiest method:** Go to **Settings > System > Log Level** and select **Debug** from the dropdown. The change takes effect immediately — no restart needed. Set it back to **Warn** when you're done.

**Alternative (via config file):** Add `HELIX_LOG_LEVEL=debug` to your `helixscreen.env` file and restart. On Klipper-based printers the canonical path is in your `printer_data` config dir (the same place where you edit `printer.cfg` from Mainsail/Fluidd):

```bash
# MainsailOS / Pi (Klipper convention)
echo 'HELIX_LOG_LEVEL=debug' >> ~/printer_data/config/helixscreen/helixscreen.env
sudo systemctl restart helixscreen

# AD5M (Forge-X) — install dir is symlinked to printer_data convention
echo 'HELIX_LOG_LEVEL=debug' >> /opt/helixscreen/config/helixscreen.env
/etc/init.d/S90helixscreen restart
```

Available levels: `warn` (default), `info`, `debug`, `trace`. **Set back to Warn after debugging** — verbose logging impacts performance.

### Where are the logs?

The app produces **two log streams**: a structured app log (recommended starting point), and a smaller launcher/crash capture file. Where they live depends on the platform.

```bash
# Structured app log
sudo journalctl -u helixscreen -f          # MainsailOS / x86 / any systemd Pi-like setup
grep helix-screen /var/log/messages         # AD5M, Snapmaker U1 (persistent syslog)
logread | grep helix-screen                 # K1 / K1C / K2 / CC1 / AD5X (BusyBox in-RAM)

# Launcher / crash capture (SysV platforms only — systemd platforms put this in the journal)
tail -100 /opt/helixscreen/logs/launcher.log              # AD5M
tail -100 /usr/data/helixscreen/logs/launcher.log         # K1 / K1C / K2 / AD5X
tail -100 /var/log/helixscreen/launcher.log               # Snapmaker U1
tail -100 /user-resource/helixscreen/logs/launcher.log    # CC1 (COSMOS)
tail -100 /tmp/helixscreen.log                            # pre-v0.99.62 fallback
```

For a complete map of log locations and how they're wired up, see the [Logging](../devel/LOGGING.md#log-destinations--retrieval) developer doc.

---

*For more details, see:*
- *[Installation Guide](INSTALL.md)*
- *[User Guide](USER_GUIDE.md)*
- *[Configuration Reference](CONFIGURATION.md)*
- *[Troubleshooting](TROUBLESHOOTING.md)*
