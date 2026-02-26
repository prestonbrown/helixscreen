# Frequently Asked Questions

Quick answers to common questions about HelixScreen.

---

## General

### What is HelixScreen?

HelixScreen is a touchscreen interface for Klipper 3D printers. It connects to your Moonraker instance and provides a modern, touch-friendly UI for controlling your printer.

**Key features:**
- 30 panels and 48 overlays covering all printer operations
- 3D G-code preview, bed mesh visualization, frequency response charts
- First-run wizard with telemetry opt-in
- Theme editor with 14 presets (dark and light)
- Sound system, timelapse integration, filament tracking
- Auto-detecting layout system for multiple display sizes
- Designed for embedded displays (low memory, no desktop required)

### Which printers are supported?

**Any printer running Klipper + Moonraker** should work. HelixScreen connects to Moonraker's API, not directly to Klipper.

**Tested and confirmed working:**
- Voron 0.1, Voron 2.4
- Doron Velta
- RatRig V-Core
- FlashForge Adventurer 5M / 5M Pro (with [Forge-X](https://github.com/DrA1ex/ff5m)) — most thoroughly tested on ForgeX 1.4.0 with FlashForge firmware 3.1.5; other versions may work fine

**Supported with auto-detection:**
- QIDI printers — detection heuristics and print start profile included

**Binaries available but untested:**
- Creality K1 series (K1, K1C, K1 Max) — requires [Simple AF](https://github.com/pellcorp/creality) community firmware
- Creality K2 series (K2, K2 Pro, K2 Plus) — stock firmware has Moonraker on port 4408, no community firmware needed
- Snapmaker U1 — cross-compile target with 480x320 display support exists but has not been tested on hardware

**Active testing underway:**
- SOVOL SV06 — uses Klipper on a Raspberry Pi; install with the [MainsailOS instructions](#raspberry-pi--mainsailos-installation)
- SOVOL SV08 — uses Klipper on a Raspberry Pi; install with the [MainsailOS instructions](#raspberry-pi--mainsailos-installation)
- Elegoo Centauri Carbon 1 — dedicated build target (`cc1`); prebuilt binaries included in releases but no installer support yet (manual deployment only)

**Should work but not yet tested:**
- Other Voron models
- Prusa (with Klipper mod)
- Creality Ender (with Klipper)
- Bambu (with Klipper mod)
- Any custom Klipper build

If you test on a printer not listed above, please let us know your results!

### Which displays are supported?

**Tested and confirmed working:**
- BTT 5" HDMI/DSI touchscreen
- BTT 7" HDMI/DSI touchscreen
- FlashForge AD5M built-in 4.3" display (800x480)

**Should work but not yet tested:**
- Official Raspberry Pi 7" DSI touchscreen
- Creality K2 built-in 4.3" display (480x800, portrait — may need rotation)
- Other HDMI displays
- SPI displays (with proper configuration)

**Display sizes:** HelixScreen auto-detects the best layout for your display. 800x480, 1024x600, and 1920x480 (ultrawide) are fully supported. 480x320 (Snapmaker U1) will run but may have layout overlap issues — improved small-screen support is ongoing.

**Display rotation:** All three binaries (main, splash, watchdog) support 0°, 90°, 180°, and 270° rotation via config or command line.

If you test on hardware not listed above, please let us know your results!

### How is this different from KlipperScreen and GuppyScreen?

| Feature | HelixScreen | KlipperScreen | GuppyScreen |
|---------|-------------|---------------|-------------|
| **UI Framework** | LVGL 9 XML | GTK 3 (Python) | LVGL 8 (C) |
| **Declarative UI** | Full XML | Python only | C only |
| **Disk Size** | ~70-80MB | ~50MB | ~60-80MB |
| **RAM Usage** | ~10MB | ~50MB | ~15-20MB |
| **Reactive Binding** | Built-in | Manual | Manual |
| **3D G-code preview** | Yes | 2D layers | No |
| **3D bed mesh** | Yes | 2D heatmap | 2D heatmap |
| **Status** | Beta | Mature (maintenance) | Unmaintained |

**HelixScreen advantages:**
- Lowest memory footprint
- Declarative XML layouts (change UI without recompiling)
- Modern reactive architecture
- 3D visualizations

### Is HelixScreen production-ready?

HelixScreen is currently in **beta** (closer to alpha). This means:

- ✅ Core features are complete and tested
- ✅ Daily use on real printers works well
- ⚠️ Some edge cases may have bugs
- ⚠️ Some advanced features still in development
- ⚠️ Breaking changes may occur between releases

We recommend it for enthusiasts comfortable with:
- SSH access to their printer
- Reading logs for troubleshooting
- Reporting bugs on GitHub

---

## Installation

### What Raspberry Pi do I need?

| Pi Model | Supported | Notes |
|----------|-----------|-------|
| Pi 5 | ✅ Recommended | Best performance |
| Pi 4 | ✅ Recommended | Great performance |
| Pi 3B+ | ✅ Minimum | Works well |
| Pi 3B | ⚠️ Usable | May be slow |
| Pi Zero 2 W | ✅ OK | Good for space-constrained setups |
| Pi Zero (original) | ❌ No | Too slow |

**Memory:** 1GB minimum, 2GB+ recommended.

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

**Coming soon.** Camera/webcam support is on the roadmap but not yet implemented.

### Does it work with Spoolman?

**Yes.** Spoolman integration is supported:
- **Advanced panel** → **Spoolman** to browse your spool inventory
- **Settings** → **Spoolman** for weight sync settings
- Assign spools to AMS slots and track filament usage

### Does it support Happy Hare or AFC-Klipper?

**Yes.** Full multi-material support is available for:
- **Happy Hare** — MMU2, ERCF, 3MS, Tradrack
- **AFC-Klipper** — Box Turtle with full data parsing, 11 device actions, per-lane reset, and mock mode
- **ValgACE** — supported
- **Tool changers** — supported

Features include visual slot configuration with tool badges, endless spool arrows, tap-to-edit popup, Spoolman integration, and material compatibility validation.

### Can I customize the home screen widgets?

**Yes!** The Home Panel displays configurable widgets — quick-access buttons for features like temperature, LED control, network status, AMS, and more.

To customize:
1. Go to **Settings** → **Home Widgets** (in the Appearance section)
2. Toggle widgets on or off
3. Long-press the drag handle to reorder

Up to 10 widgets can be shown. Some widgets (like AMS, humidity sensor, or probe) only appear if the relevant hardware is detected. See the [Home Panel guide](guide/home-panel.md#home-widgets) for the full widget list.

### Can I customize the colors or layout?

**Yes!** HelixScreen includes a built-in theme editor with 14 preset themes:

1. Go to **Settings** → **Display Settings**
2. Tap **Theme** to open the theme editor
3. Choose from presets: Nord (default), Catppuccin, Dracula, Gruvbox, Tokyo Night, One Dark, Solarized, Material Design, Rose Pine, Everforest, Kanagawa, Ayu, Yami, or ChatGPT
4. Toggle dark/light mode
5. Customize individual colors if desired - changes are saved to `config/themes/`

For layout customization, you can edit XML files in `ui_xml/` (no recompilation needed).

### Does it support multiple printers?

**Not currently.** HelixScreen connects to one Moonraker instance. Multi-printer support is on the long-term roadmap.

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

**Yes.** The Macro panel shows your Klipper macros. Access via **Advanced** → **Macros**. You can also configure quick macro buttons in **Settings** → **Macro Buttons**.

### Can I customize the printer image on the home screen?

**Yes.** Tap the printer image on the Home Panel to open the Printer Manager, then tap the image again to open the Printer Image picker. You have three options:

- **Auto-Detect** (default) — HelixScreen picks an image based on your printer type from Klipper
- **Shipped Images** — Choose from 25+ pre-rendered images (Voron, Creality, FlashForge, Anycubic, RatRig, etc.)
- **Custom Images** — Drop a PNG or JPEG file into `config/custom_images/` and it appears automatically the next time you open the picker. Files must be under 5MB and 2048x2048 pixels max.

Your selection is saved to the `display.printer_image` config key and persists across restarts.

### Can I rename my printer?

**Yes.** Tap the printer image on the Home Panel to open the Printer Manager. Then tap the printer name (shown with a pencil icon) to enable inline editing. Type the new name and press **Enter** to save, or **Escape** to cancel. The name is stored in the `printer.name` config key.

---

## Usage

### How do I calibrate my touchscreen?

If taps register in the wrong location:
1. Go to **Settings** (gear icon)
2. Scroll to **System** section
3. Tap **Touch Calibration**
4. Tap the crosshairs that appear on screen
5. Calibration saves automatically when complete

Note: This option only appears on touchscreen displays, not in the desktop simulator.

### How do I change the theme or colors?

1. Go to **Settings** → **Display Settings**
2. Tap **Theme** to open the theme editor
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

**Yes**, if your printer has `[firmware_retraction]` configured in Klipper. Go to **Settings** → **Retraction Settings** (under Printer section) to adjust:
- Retract length and speed
- Unretract extra length and speed
- Enable/disable firmware retraction

This option only appears if Klipper reports firmware retraction capability.

### How do I check why the UI is slow?

1. **Check your display connection:** SPI displays are significantly slower than HDMI or DSI. If possible, use an HDMI or DSI-connected display for best performance.
2. **Disable animations:** Go to **Settings** → toggle **Animations** off
3. **Check CPU/memory via SSH:** Run `top` or `htop` to see if something else is using resources
4. **Reduce logging:** If you added `-vv` or `-vvv` to the service, remove it
5. **Consider a faster SBC:** Pi 4 or Pi 5 will be noticeably smoother than a Pi 3 or Pi Zero

### Why does the setup wizard keep appearing?

The wizard runs when no valid configuration exists. Causes:
1. Config file missing or deleted
2. Config file has invalid JSON
3. Permissions prevent reading config

**Fix:** Check `/opt/helixscreen/config/helixconfig.json` exists and is valid JSON.

### How do I change the Moonraker address?

There's currently no UI to change this after initial setup. Your options:

**Edit the config file directly:**
```bash
sudo nano /opt/helixscreen/config/helixconfig.json
# Edit moonraker_host and moonraker_port in the "printer" section
sudo systemctl restart helixscreen
```

**Or re-run the setup wizard:**
```bash
# Either delete the config to trigger wizard on next start:
sudo rm /opt/helixscreen/config/helixconfig.json
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

See the [Contributing Guide](../DEVELOPMENT.md#contributing) for:
- Code standards
- Development setup
- Pull request process

We welcome:
- Bug reports
- Feature suggestions
- Code contributions
- Documentation improvements

### How do I build from source?

See the [Development Guide](../DEVELOPMENT.md) for build instructions, dependencies, and development setup.

### What programming language is HelixScreen?

- **C++17** for application logic
- **XML** for UI layouts (LVGL 9 declarative system)
- **Makefile** for build system
- **Bash** for scripts

---

## Getting Help

### Where can I get help?

Join the [HelixScreen Discord](https://discord.gg/RZCT2StKhr) for community support, setup help, and feature discussions.

### Where can I report bugs?

Open an issue on [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) with:
- HelixScreen version
- Hardware info (Pi model, display)
- Steps to reproduce
- Relevant log output

### Where can I request features?

Open a GitHub issue with the "enhancement" label. Describe:
- What you want
- Why it's useful
- How you imagine it working

### Is there a Discord or forum?

Currently, GitHub Issues and Discussions are the primary communication channels.

### Where are the logs?

```bash
# systemd journal (MainsailOS)
sudo journalctl -u helixscreen -f

# If using file logging
cat /var/log/helix-screen.log
# or
cat ~/.local/share/helix-screen/helix.log
```

---

*For more details, see:*
- *[Installation Guide](INSTALL.md)*
- *[User Guide](USER_GUIDE.md)*
- *[Configuration Reference](CONFIGURATION.md)*
- *[Troubleshooting](TROUBLESHOOTING.md)*
