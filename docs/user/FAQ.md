# Frequently Asked Questions

Quick answers to common questions about HelixScreen 1.1.

---

## General

### What is HelixScreen?

HelixScreen is a touchscreen interface for Klipper 3D printers. It connects to your printer's Moonraker and gives you a touch-friendly screen for printing, heating, moving, calibrating and managing filament.

Highlights:

- A home dashboard of widgets that you arrange yourself, across several pages
- A 3D G-code preview and a 3D bed mesh view
- Multi-filament support for AFC, Happy Hare, OpenAMS, Anycubic ACE, the FlashForge AD5X IFS, Creality CFS, the Snapmaker U1, QIDI Box, Bondtech INDX (early support), any system that publishes Lane Status, and tool changers, with Spoolman integration
- A first-run wizard that recognizes more than 100 printer models
- 18 color themes in dark and light, and 9 languages
- Sound, timelapse, label printing, exclude objects and input shaper calibration
- A layout that adapts to screens from 480x320 up to ultrawide and portrait panels
- A small memory footprint: about 15MB of RAM on printers like the Creality K1 and FlashForge AD5M, so the hardware you already own is enough

### Which printers are supported?

HelixScreen works with any Klipper printer that runs Moonraker. This table gives the status for each platform. [Supported Printers](guide/supported-printers.md) has the feature-by-feature detail.

| Platform | Status | Notes |
|----------|--------|-------|
| Voron 0.1, 2.4, Trident | **Tested** | Primary development platforms |
| Doron Velta / RatRig V-Core | **Tested** | |
| FlashForge Adventurer 5M / 5M Pro | **Tested** | Needs the Forge-X, ZMOD or Klipper Mod firmware |
| FlashForge Adventurer 5X | **Tested** | IFS filament system integrated. Needs ZMOD 1.7.0 or newer |
| FlashForge Creator 5 / Creator 5 Pro | **Tested** | Four-head tool changer. Needs [Z-Mod](https://wiki.zmod.link/C5PRO/) or [Reforge](https://github.com/Klipper4FlashForge/firmware) firmware. On Z-Mod 1.7.4 and newer, each head's material and color are read from the printer itself |
| Creality K1 / K1C / K1 Max / K1 SE | **Supported** | Needs community firmware with Moonraker (Guilouz, Simple AF or Guppy Mod) and root access |
| Creality K2 / K2 Plus / K2 Pro | **Tested** | Works on stock firmware, with full CFS support. Community firmware with its own CFS module is recognized too |
| Creality Sonic Pad | **Supported** | Dedicated 32-bit build. Tested only on [SonicPad-Debian](https://github.com/Jpe230/SonicPad-Debian); stock Creality firmware is untested |
| Creality Hi | **Preliminary** | Recognized automatically as a bed-slinger with an optional CFS. Not yet checked on hardware |
| Elegoo Centauri Carbon | **Tested** | Needs [OpenCentauri COSMOS](https://docs.opencentauri.cc/klipper-conversion/cosmos/cosmos/) firmware 26.07.0 or newer |
| Snapmaker U1 (SnapSwap) | **Tested** | Needs SSH: stock firmware 1.2 or newer with its **Root access** option, or PAXX Extended Firmware. Tested on PAXX 1.2.x to 1.4.x; the stock-firmware path has not been checked on a real stock printer. Reinstall after a firmware update |
| QIDI Q2 / Plus 5 / Max 4 | **Supported** | Stock firmware works, as do FreeDi and FreeQIDI. The Plus 4 and the 3-series models (X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro) have a serial display, so on those HelixScreen can only run as a remote screen |
| Anycubic Kobra 2 Pro / Kobra 3 / 3 V2 / 3 Max / S1 / S1 Max, Sovol SV06 ACE | **Community** | Needs community firmware such as [Rinkhals](https://github.com/jbatonnet/Rinkhals). The ACE Pro is supported. Not yet tested on the team's hardware |
| Sovol SV06 / SV08 | **Tested** | |
| Artillery M1 Pro | **Tested** | |
| Zero G Mercury / Nebula / Hydra | **Tested** | Several variants supported |
| Other Klipper printers | **Should work** | Any printer HelixScreen can reach through Moonraker |

**Tested** means the HelixScreen team checked it on real hardware. **Supported** means it works with the firmware named. **Community** means a user confirmed it but the team has not tested it. **Preliminary** means support was built from the printer's published configuration and has not been checked on hardware. See the [Installation Guide](INSTALL.md) for setup on each platform.

HelixScreen also runs on a few things that are not printers:

- **BigTreeTech K-Touch** (ESP32 screen): alpha firmware that you flash from the [easy installer](guide/install-esp32.md#with-the-easy-installer) or from your browser. See [K-Touch Install](guide/install-esp32.md).
- **Android phones and tablets**: available on [Google Play](https://play.google.com/store/apps/details?id=org.helixscreen.app). See [Can I use HelixScreen on my phone or tablet?](#can-i-use-helixscreen-on-my-phone-or-tablet).
- **A Linux desktop**: runs in a window. See [Run HelixScreen in a Window on a Linux Desktop](guide/install-linux-desktop.md).

### Can I run HelixScreen on a separate device instead of on my printer?

Yes. HelixScreen only needs network access to your printer's Moonraker, so it does not have to run on the printer. Install it on a spare Raspberry Pi with a touchscreen, a mini PC, a K-Touch, an Android tablet, or [a window on your Linux desktop](guide/install-linux-desktop.md). Then enter your **printer's IP address** in the wizard's Moonraker step instead of `localhost`. This suits a printer on the floor with the screen at your desk.

Point it at Moonraker (port `7125`), not at the Mainsail or Fluidd web page. The screen device does not need Mainsail or Fluidd at all. See [Remote Screen Setup](INSTALL.md#remote-screen-setup-run-on-a-separate-device).

### Can I use HelixScreen on my phone or tablet?

Yes. The Android app is on [Google Play](https://play.google.com/store/apps/details?id=org.helixscreen.app), and you can also install the APK from the [releases page](https://github.com/prestonbrown/helixscreen/releases/latest). It needs Android 9.0 or newer and runs in landscape. It is a remote screen: it controls a printer over your network and installs nothing on the printer. An install from Google Play is signed differently from the APKs on the releases page, so moving from one to the other means uninstalling first and setting the app up again. See [Android App](INSTALL.md#android-app-experimental).

### Can I use HelixScreen on a Klipper machine that isn't a printer?

Yes. If your `[printer]` section sets `kinematics: none`, HelixScreen knows the machine has no motion system and does not warn about a missing nozzle heater. On any other machine without a nozzle heater, open **Settings > Devices > Hardware Health** (or the warning's **View** button) and tap **Ignore** on the extruder entry. It is not reported again. See [Hardware Health](guide/settings/devices.md#hardware-health).

### Which displays are supported?

**Tested and confirmed working:**

- BTT 5" and 7" HDMI/DSI touchscreens
- The FlashForge AD5M's built-in 4.3" display (800x480)

**Active testing underway:**

- The FlashForge AD5X's built-in 4.3" display (800x480)

**Should work but not yet tested:**

- The official Raspberry Pi 7" DSI touchscreen
- The Creality K2's built-in 4.3" display (480x800). The panel is rotated to landscape in software, which is the supported setup. Running it in portrait works too, but landscape is more polished.
- Other HDMI displays, and SPI displays that are set up correctly

**Screen sizes.** HelixScreen picks the best layout for your display on its own. 800x480 and 1024x600 are fully supported. 480x320 runs, but a few screens are tight there.

**Ultrawide and portrait screens work.** HelixScreen detects an ultrawide screen (wider than about 2.5:1, such as 1920x440) or a portrait screen (narrower than about 0.8:1, such as 480x800) and adapts the navigation bar and the home dashboard. The home dashboard grid is sized from the screen with square cells, so a portrait 480x800 panel gets 4 columns by 6 rows. Portrait also has its own layouts for several panels, and panels without one use an adaptive fallback, so landscape is still the most polished. See the [Home Panel guide](guide/home-panel.md#ultrawide-and-portrait-screens) and [Display](guide/settings/display.md).

**Rotation.** If your display is upside down or sideways, go to **Settings > Display > Screen Rotation**, pick Normal, 90° Clockwise, 180° Upside Down or 270° Clockwise, then restart HelixScreen. Touch follows the rotation by itself.

If you test on hardware not listed here, please tell us how it went.

### Which slicer should I use?

HelixScreen reads standard G-code, so most slicers work, with these tiers:

| Slicer | Status | Notes |
|--------|--------|-------|
| **OrcaSlicer 2.3.2 or newer** | **Primary** | The slicer we develop and test against. Best experience, including one-way filament preset sync from HelixScreen's filament slots to OrcaSlicer |
| Manufacturer slicers (Creality Print, FlashForge Orca, Bambu Studio and similar) | **Supported** | Most are OrcaSlicer or PrusaSlicer forks and work well. This is about G-code and profiles, not the vendor's own network path. On the Creality K1 series the stock backend keeps running beside HelixScreen, so Creality Print can still reach the printer |
| PrusaSlicer / SuperSlicer | **Supported** | Fully usable, including exclude objects and pre-print options |
| Cura | **Not targeted** | We do not test against it. Output generally works, but some features (exclude objects, filament sync) need extra setup or are not available |

For accurate metadata, thumbnails, exclude-object support and filament syncing, use **OrcaSlicer 2.3.2 or newer**.

### Why is my layer count or time remaining inaccurate?

For an exact layer count and a steady time-remaining estimate, your slicer has to report layer information to Klipper with the `SET_PRINT_STATS_INFO` command in the printed G-code. Many stock slicer profiles do not, so HelixScreen estimates from progress and height instead. That is close but not exact. Two short lines in your slicer's custom G-code fix it. See [Troubleshooting](TROUBLESHOOTING.md) for the snippet for each slicer.

### How is this different from KlipperScreen and GuppyScreen?

| | HelixScreen | KlipperScreen | GuppyScreen |
|---|---|---|---|
| **Toolkit** | LVGL 9, with layouts in XML | GTK 3 (Python) | LVGL 8 (C) |
| **Memory** | About 15MB on 32-bit embedded targets | About 50MB | About 15-20MB |
| **3D G-code preview** | Yes | 2D layers | No |
| **3D bed mesh** | Yes | 2D heatmap | 2D heatmap |

GrumpyScreen is an actively developed fork of GuppyScreen, built specifically for Simple AF and OpenCentauri COSMOS. Like its parent it uses LVGL 8, and it supports Spoolman and the AFC and Happy Hare multi-material systems. We have not measured its memory or disk use, so it does not have a column in the table above.

HelixScreen's layouts are XML files, so the look of the interface can change without recompiling. It also has a home screen you design yourself, a database of more than 100 printers for automatic setup, support for the filament systems listed above, and 9 languages (English, German, Spanish, French, Italian, Japanese, Portuguese, Russian and Chinese), with English in both US and UK spelling.

---

## Installation

### What Raspberry Pi do I need?

Whatever Pi you already own is almost certainly fine.

| Pi model | Supported | Notes |
|----------|-----------|-------|
| Pi 5 | Yes | Best performance, more than HelixScreen alone needs |
| Pi 4 | Yes | Plenty for the touchscreen UI plus Klipper and Moonraker |
| Pi 3B+ | Yes | Works well |
| Pi 3B | Yes | Usable; heavy 3D mesh interaction may feel slow |
| Pi Zero 2 W | Yes | Good for space-constrained setups |
| Pi Zero (original) | No | Too slow |

1GB of memory is enough. Both 32-bit and 64-bit Raspberry Pi OS work: the installer checks which one you run and downloads the matching build. You need Debian 11 (Bullseye) or newer. See [Generic Linux Install](INSTALL.md#generic-linux-install-raspberry-pi-btt-x86).

<a id="can-i-run-helixscreen-alongside-klipperscreenguppyscreen"></a>
### Can I run HelixScreen alongside KlipperScreen or GuppyScreen?

Not on the same display, because both want the screen. The installer turns off any screen program it finds. To switch back and forth by hand on a Raspberry Pi or other systemd machine:

```bash
# Turn KlipperScreen off and HelixScreen on
sudo systemctl stop KlipperScreen
sudo systemctl disable KlipperScreen
sudo systemctl enable helixscreen
sudo systemctl start helixscreen
```

On FlashForge, Creality and other printers with their own firmware, follow the install guide for your printer: it names the service to start, stop or restart. See the [Installation Guide](INSTALL.md).

<a id="do-i-need-to-install-x11-or-a-desktop-environment"></a>
### Do I need X11 or a desktop environment?

No. HelixScreen draws straight to the display through DRM or the framebuffer, so it needs no X11, Wayland, desktop environment or display manager. That is part of why it uses so little memory. The one exception is by choice: the [Linux desktop package](guide/install-linux-desktop.md) runs HelixScreen in a window on a desktop PC.

<a id="does-this-work-with-mainsailos-fluiddpi-or-other-klipper-distros"></a>
### Does this work with MainsailOS, FluiddPi or other Klipper distributions?

Yes. It works with any Klipper setup that includes Moonraker: MainsailOS, FluiddPi, KIAUH installs and custom installs. Which web interface you use does not matter, because HelixScreen talks to Moonraker.

<a id="what-gets-preserved-during-upgrades"></a>
### What is kept when I update?

The installer keeps your settings, your `helixscreen.env` overrides, your custom printer images and your custom themes. Your Klipper configuration is never touched. The first start of 1.1 also saves your 1.0 settings beside the new ones as `settings.json.pre-migration`. See the [Upgrade Guide](UPGRADING.md).

---

## Features

<a id="does-it-support-multiple-extruders--toolchangers"></a>
### Does it support multiple extruders and tool changers?

Yes. HelixScreen discovers every extruder Klipper reports and gives each its own temperature control, with a selector in the temperature screen. Tool changers work with [klipper-toolchanger](https://github.com/viesturz/klipper-toolchanger) and with the Snapmaker U1, FlashForge Creator 5 and MedusaHC. The Home screen shows which tool is active, and the print status screen shows temperatures per tool. A tool changer and a filament system such as Happy Hare can run side by side. See [Tool Changers](guide/filament-tool-changers.md).

### Can I use my webcam?

Yes. HelixScreen shows a webcam on the home dashboard and while printing. It finds webcams configured in Moonraker (crowsnest, camera-streamer and similar) and shows their stream. See the [Camera guide](guide/camera.md).

On a Raspberry Pi, installing `libturbojpeg0` makes JPEG decoding several times faster. HelixScreen uses it automatically when it is present:

```bash
sudo apt install libturbojpeg0
```

### Can I see or use the printer's screen from my browser?

Yes. Turn on **Settings > Connection > Screen Sharing > Share Screen** and the screen shows up live in your browser, and as a **HelixScreen Screen** webcam in Mainsail and Fluidd. Turn on **Allow Remote Control** as well to tap and drag it from the browser. Both are off until you turn them on at the printer. Some printers ask for a pairing code before a browser can take control; on a Raspberry Pi running MainsailOS or KIAUH, Moonraker's trusted clients decide instead. See [Screen Sharing and Remote Control](guide/screen-sharing.md).

### Does it work with Spoolman?

Yes. Open **Advanced > Spoolman** to browse your spool inventory, and **Settings > Devices > Spoolman** to connect a server and set up weight syncing. You can assign spools to filament slots and track how much filament each print uses. See [Filament Tracking & Spoolman](guide/filament-tracking.md).

### Where are my lanes' spool details stored?

On the printer, not on the screen. Your filament system keeps what it can (color and material at least), and the rest goes into a lane record on the printer that OrcaSlicer, Mainsail and any other screen read too, so every screen shows the same thing. When you save an edit, HelixScreen writes it there and waits for the printer to take it. If the printer does not take it, the editor stays open and says why. A detail that lived only on a 1.0 screen and was never sent to the printer does not carry over to 1.1, so set it again. See [Where a lane's details come from](guide/filament-tracking.md#where-a-lanes-details-come-from).

### Why didn't Spoolman deduct my print from the spool?

Spoolman records usage only against its **active spool**. On a multi-filament system (CFS, AFC, Happy Hare and others), HelixScreen makes a slot's linked spool active when that slot loads, so the slot has to be linked to the spool before the print starts. Matching uses the linked spool only, never the material name. See [Which spool Spoolman charges](guide/filament-tracking.md#which-spool-spoolman-charges-on-a-multi-filament-system).

### Why is a spool grayed out when I pick one?

It is already linked to another slot or tool. On the External Spool editor, every spool linked to a slot is grayed out. To link a spool to a CFS or AMS slot, edit that slot instead. See [Editing Filament Properties](guide/filament.md#editing-filament-properties).

### Does my K2 run its bed mesh twice?

Not normally. With **Auto Bed Mesh** on, HelixScreen sends the K2's `BED_MESH_CALIBRATE_START_PRINT` before the print, unless the file's start G-code already calls that same command. Other bed mesh commands in the file do not count. Details and limits: [Pre-Print Options](guide/printing.md#pre-print-options).

### Can I print spool labels?

Yes. HelixScreen prints spool labels to thermal label printers:

- **Brother QL**, over the network or Bluetooth
- **Phomemo**, over USB or Bluetooth
- **Niimbot**, over Bluetooth
- **MakeID**, over Bluetooth

Labels carry the spool name, material, color swatch, temperatures and a QR code. See the [Label Printing Guide](guide/label-printing.md).

<a id="does-it-support-happy-hare-or-afc-klipper"></a>
### Does it support Happy Hare, AFC and other filament systems?

Yes:

- **Happy Hare**: MMU2, ERCF, 3MS, Tradrack and others
- **AFC-Klipper**: Box Turtle, OpenAMS and ViViD, with full lane data, a live step bar during tool changes, and device settings and actions
- **OpenAMS** without AFC, through klipper_openams' own interface. Load and Unload work once its updated `OPENAMS_LOAD` and `OPENAMS_UNLOAD` macros are in your macro file. Until then Unload is hidden and Load says the macro is missing
- **Anycubic ACE Pro**, on native Anycubic firmware (such as a Kobra on Rinkhals) and on the community ValgACE, BunnyACE and DuckACE drivers
- **FlashForge AD5X IFS**, **Creality CFS**, **Snapmaker U1** and **QIDI Box**, with model-specific integration
- **Tool changers**

The multi-filament panel shows each slot with its color and material, Endless Spool arrows, Filament Mapping, a tap-to-edit popup, and checks that a material suits its slot. See [Filament](guide/filament.md) and [Supported Printers](guide/supported-printers.md#multi-material-systems-on-any-printer).

### Can I customize the home screen widgets?

Yes. The Home screen is a grid of widgets that you arrange yourself, such as temperatures, LED control, network status, filament, camera and macro buttons, spread across up to 8 pages.

1. **Long-press the widget grid** to enter Edit Mode.
2. **Tap** a widget to select it, then press it again and **drag** it to move it, or drag its edges to resize it. Drag a widget across the page border to move it to another page.
3. **Long-press an empty area**, or tap **+** on the navigation bar, to open the Widget Catalog and add something.
4. Tap a selected widget again, or long-press a widget and lift without moving, to open its menu. Choose **Remove** to take it off the grid, or **Configure** to change its settings.
5. Tap **Done** when you are finished.

How many widgets fit depends on your screen. The grid is sized from it, so a bigger screen gives you more cells rather than bigger ones. Widgets that need hardware you do not have appear in their own section of the Widget Catalog, dimmed, with the reason. They become available when the hardware is detected. See [Home Panel](guide/home-panel.md) and [Edit Mode](guide/home-edit-mode.md).

### Is there a quicker way to shut down the printer?

Yes. Besides the Advanced screen's **Shutdown** and **Reboot** rows, you can put a **Shutdown/Reboot** widget on your home screen. Enter Edit Mode, add the widget from the Widget Catalog, and tap it any time. A confirmation appears first, so you cannot shut down by accident. If you would rather switch a power supply or smart plug, use the **Power** widget. On Android the Shutdown and Reboot controls do not appear. See the [Home Panel guide](guide/home-panel.md).

### Can I customize the colors or layout?

Yes. HelixScreen has a theme editor with 18 presets:

1. Go to **Settings > Appearance**.
2. Tap **Theme Colors** to open the theme editor.
3. Pick a preset: Ayu, Catppuccin, ChatGPT, Cupertino, Dracula, Everforest, Gruvbox, Hazard, HelixScreen (the default), Kanagawa, Material Design, Midnight, Nord, One Dark, Rose Pine, Solarized, Tokyo Night or Yami.
4. Switch between dark and light mode, and change individual colors if you like.

For layout, use Edit Mode on the Home screen. **Settings > Display** also has a **UI Scale** choice, and each scale keeps its own home layout. See [Appearance](guide/settings/appearance.md).

### Does it support multiple printers?

Yes. Add printers in **Settings > Connection > Printers** and switch between them there. Turn on **Show Printer Switcher** on the same page to put a printer icon in the navigation bar. You view one printer at a time. See [Connection settings](guide/settings/connection.md#printers).

### Can I view print history?

Yes. Open **Advanced > Print History** for statistics, trend charts, a searchable job list, and per-job details such as spools used and camera photos. See [Print History](guide/print-history.md).

### Can I send G-code commands directly?

Yes. **Advanced > G-code Console** lets you send G-code and shows Klipper's replies. See [Advanced Features](guide/advanced.md#g-code-console).

### Does it support power device control?

Yes, for the power devices configured in Moonraker. Open **Settings > Devices > Power Devices** to switch each device, or add a **Power** widget to your home screen to switch a device with one tap. See [Power Device Control](guide/advanced.md#power-device-control).

<a id="can-i-view-and-run-bed-mesh"></a>
### Can I view the bed mesh and run a bed mesh calibration?

Yes. **Controls > Bed Mesh** shows your mesh in 3D and runs a calibration. **Advanced > Bed Leveling** also opens it. See [Calibration & Tuning](guide/calibration.md).

### Does it support input shaper?

Yes, if an accelerometer is configured in Klipper. **Advanced > Input Shaping** runs the whole calibration, with frequency response charts, per-axis results, a shaper comparison and a Save Config step. See [Calibration & Tuning](guide/calibration.md).

### Does it support pressure advance tuning?

Yes. **Advanced > Pressure Advance Tuning** prints a small test tower that raises pressure advance with height. You pick the height band that looked best, and HelixScreen applies the result. See [Calibration & Tuning](guide/calibration.md#pressure-advance-tuning-test-tower).

### Does it support exclude objects?

Yes. If the print contains several objects, an objects button appears on the print status screen. Open the Objects list, or tap an object in the viewer, to exclude one that failed. See [Exclude Object](guide/printing.md#exclude-object).

### Can I run macros?

Yes. **Advanced > Macro Browser** lists your Klipper macros. You can also set up quick macro buttons in **Settings > Printing > Macro Buttons**.

<a id="can-i-change-which-macro-the-load--unload--purge-buttons-run"></a>
### Can I change which macro the Load, Unload and Purge buttons run?

Yes. Go to **Settings > Printing > Macro Buttons** and scroll to **Standard Macros**. Each button has a dropdown where you can pick any macro from your Klipper config, or **(Auto)** to let HelixScreen choose. This works with or without a filament system. See the [Filament guide](guide/filament.md#customizing-which-macro-runs).

### Why does my nozzle cool down after a filament change?

That is deliberate. A load or unload heats the nozzle to the material's temperature, and HelixScreen turns the heater back off two minutes later so it does not stay hot. The delay lets you run several operations back to back, and a running print is never touched.

If your filament system does its own cooldown (AFC does), turn ours off at **Settings > Printing > Cool nozzle after filament ops** so the two do not both drive the heater. It is a per-printer setting. See [Printing settings](guide/settings/printing.md#cool-nozzle-after-filament-ops).

<a id="can-i-customize-the-printer-image-on-the-home-screen"></a>
### Can I change the printer picture on the home screen?

Yes. Tap the printer picture on the Home screen to open the Printer Manager, then tap the picture again to open the picker. You can choose:

- **Auto-Detect** (the default): HelixScreen picks a picture from your printer type.
- **Shipped images**: more than 80 pictures covering Voron, Creality, FlashForge, Anycubic, RatRig, FLSUN and more.
- **Custom images**: copy a PNG or JPEG into the `custom_images` folder in HelixScreen's config folder and it shows up the next time you open the picker. You can also import from a USB drive. Files must be under 5MB and no larger than 2048x2048 pixels.

The picker can also tag where your printer's nozzle, fan, bed and light are on the picture, so the heater and fan chips on the Home screen point at the right part. See [Printer Manager](guide/printer-manager.md).

### Can I rename my printer?

Yes. Tap the printer picture on the Home screen to open the Printer Manager, then tap the printer name (shown with a pencil icon). Type the new name and press **Enter** to save, or **Escape** to cancel. The name syncs with Mainsail and Fluidd. See [Printer Manager](guide/printer-manager.md).

### What languages are supported?

Nine: English, German, Spanish, French, Italian, Japanese, Portuguese, Russian and Chinese. English comes in US and UK (British) spelling. Change it in **Settings > Language & Time > Language**.

### Does HelixScreen collect any data?

Only if you opt in. **Share Usage Data** is off until you turn it on at **Settings > System > Share Usage Data**. When it is on, HelixScreen sends anonymous usage data such as screen resolution, platform and print outcomes. It never collects file names, G-code, IP addresses or personal information. **View Telemetry Data**, on the same page, shows exactly what would be sent. See [Telemetry & Privacy](TELEMETRY.md).

---

## Usage

### Can I use a USB mouse or keyboard?

Yes. HelixScreen detects USB mice and keyboards connected at startup, and they work alongside the touchscreen. A small cursor appears when a mouse is found. Plug the device in before HelixScreen starts. If it is not found, set `HELIX_MOUSE_DEVICE` or `HELIX_KEYBOARD_DEVICE` in `helixscreen.env` to the device path. `cat /proc/bus/input/devices` lists the devices.

### How do I calibrate my touchscreen?

If taps land in the wrong place, go to **Settings > Touch & Input > Touch Calibration** and tap the crosshairs that appear. The calibration saves when it finishes. The row appears only on touchscreens. See [Touch Calibration](guide/touch-calibration.md).

### How do I change the theme or colors?

Go to **Settings > Appearance > Theme Colors**, browse the presets with a live preview, switch between dark and light mode, and apply your choice. See [Appearance](guide/settings/appearance.md).

<a id="how-do-i-adjust-settings-during-a-print"></a>
### How do I adjust speed, flow and Z-offset during a print?

Tap **Tune** on the print status screen. It has:

- **Print Speed**, from 50 to 200%
- **Flow Rate**, from 75 to 125%
- **Z-Offset**, with baby-step buttons for the first layer
- **PA**, a live pressure advance row

Fan speed is not on the Tune screen: tap a fan chip on the print status screen for the fan controls. See [Print Tune Overlay](guide/printing.md#print-tune-overlay).

### Does HelixScreen support firmware retraction?

Yes, if your printer has `[firmware_retraction]` set up in Klipper. Go to **Settings > Printing > Retraction Settings** to adjust the retract length and speed and the unretract extra length and speed, and to turn firmware retraction on or off. The row appears only when Klipper reports the capability.

<a id="how-do-i-check-why-the-ui-is-slow"></a>
### Why is the UI slow?

1. **Check the display connection.** SPI displays are much slower than HDMI or DSI. Use HDMI or DSI if you can.
2. **Turn off animations** at **Settings > Appearance > Animations**.
3. **Check what else is using the processor.** **Settings > System > Performance** shows live processor and memory use, and the load on your printer's controller boards.
4. **Lower the log level.** If you raised it to Debug or Trace, set it back to **Info** at **Settings > System > Log Level**.
5. **Heavy 3D views** such as bed mesh rotation and the G-code preview lean on the processor. A Pi 4 or Pi 5 is smoother than a Pi 3 or Zero, but everything else works fine on the older boards.

### Why does the setup wizard keep appearing?

The wizard runs when HelixScreen finds no valid configuration: the settings file is missing, holds invalid data, or cannot be read because of permissions. Check that `settings.json` exists in HelixScreen's config folder and is valid. On a Raspberry Pi with Klipper that folder is `~/helixscreen/config/`. See [Troubleshooting](TROUBLESHOOTING.md).

### How do I change the Moonraker address?

Go to **Settings > Connection > Host** and enter the new address, or pick a printer from the **Discovered Printers** list. Tap **Test Connection**, then **Save**. HelixScreen disconnects from the current printer and connects to the new one right away.

To run the whole setup wizard again, use **Settings > System > Factory Reset**. It wipes all HelixScreen settings, after asking you to confirm. Deleting `settings.json` by hand does not work, because HelixScreen restores it from a backup on the next start. See [Troubleshooting](TROUBLESHOOTING.md) for the manual procedure.

---

<a id="touch-doesnt-work"></a>
<a id="screen-is-black"></a>
<a id="cant-connect-to-moonraker"></a>
<a id="my-display-is-upside-down-or-rotated-wrong"></a>
## Troubleshooting

[Troubleshooting](TROUBLESHOOTING.md) has the full guide. Three quick checks:

- **Touch does not work or is off:** see [Touch Calibration](guide/touch-calibration.md). A wrong-place touch is fixed at **Settings > Touch & Input > Touch Calibration**.
- **The screen is black:** check that the service is running with `sudo systemctl status helixscreen` and read the log with `sudo journalctl -u helixscreen -n 50`.
- **It cannot connect to Moonraker:** from the screen's device, run `curl http://PRINTER_IP:7125/printer/info` (use `localhost` when HelixScreen runs on the printer). If that fails, Moonraker is not reachable from there.

---

## Development & Contributing

<a id="what-programming-language-is-helixscreen"></a>
### Is HelixScreen open source?

Yes. It is licensed under GPL v3, and the source is on [GitHub](https://github.com/prestonbrown/helixscreen).

### How can I contribute?

Bug reports, feature suggestions, code and documentation are all welcome. See the [Contributing Guide](../devel/DEVELOPMENT.md#contributing). Layout work needs only XML, not C++: see the [UI Contributor Guide](../devel/UI_CONTRIBUTOR_GUIDE.md).

### How do I build from source?

See the [Development Guide](../devel/DEVELOPMENT.md).

---

## Getting Help

### Where can I get help?

- **[HelixScreen Discord](https://discord.gg/RZCT2StKhr)**: community support, setup help and development updates
- **[GitHub Issues](https://github.com/prestonbrown/helixscreen/issues)**: bug reports and feature requests

<a id="where-can-i-report-bugs"></a>
### How do I report a bug?

Open a [GitHub issue](https://github.com/prestonbrown/helixscreen/issues) with your HelixScreen version, your hardware (printer, Pi model, display), the steps to reproduce it, and a debug bundle. To make a bundle, set **Settings > System > Log Level** to **Debug**, reproduce the problem, then tap **Settings > Help & About > Upload Debug Bundle**. It gives you a short share code to paste into the issue. Set the log level back to **Info** afterwards. See [Help & About](guide/settings/help-about.md#upload-debug-bundle).

<a id="where-can-i-request-features"></a>
### How do I request a feature?

Open a GitHub issue with the "enhancement" label, or suggest it on [Discord](https://discord.gg/RZCT2StKhr).

<a id="how-do-i-enable-debug-logging"></a>
### Where are the logs?

The easiest way is a debug bundle (see above). To read the log by hand:

| Platform | Where the app log goes |
|----------|------------------------|
| Raspberry Pi, x86 and other systemd machines | `sudo journalctl -u helixscreen -f` |
| Snapmaker U1 (PAXX) | `grep helix-screen /home/lava/printer_data/logs/syslog` |
| FlashForge AD5M (Forge-X, Klipper Mod) | `/data/.helixscreen/logs/helix.log` |
| FlashForge AD5M (ZMOD), AD5X | `/opt/config/mod_data/log/helix.log` |
| FlashForge Creator 5 and Creator 5 Pro (Z-Mod) | `/opt/config/mod_data/log/helixscreen.log` |
| Creality K1 / K1C | `/usr/data/helixscreen-state/logs/helix.log` |
| Creality K2 | `/mnt/UDISK/helixscreen-state/logs/helix.log` |
| Elegoo Centauri Carbon | `/user-resource/helixscreen-state/logs/helix.log` |
| Android | `adb logcat -s HelixScreen` |

Printers that use a log file also keep a short launcher log with startup and crash output. Your printer's install guide names it, for example [Snapmaker U1](guide/install-u1.md) and [Creality K2](guide/install-k2.md).

---

*For more details, see the [Installation Guide](INSTALL.md), the [User Guide](USER_GUIDE.md), the [Configuration Reference](CONFIGURATION.md) and [Troubleshooting](TROUBLESHOOTING.md).*
