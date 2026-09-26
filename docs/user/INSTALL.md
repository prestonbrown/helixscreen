# HelixScreen Installation Guide

This guide walks you through installing HelixScreen on your 3D printer's touchscreen display.

**Target Audience:** Klipper users who want to use pre-built packages. If you're a developer building from source, see [DEVELOPMENT.md](../devel/DEVELOPMENT.md).

---

## Table of Contents

- [Quick Start](#quick-start)
- [Which printer are you installing on?](#which-printer-are-you-installing-on)
- [Remote Screen Setup (Run on a Separate Device)](#remote-screen-setup-run-on-a-separate-device)
- [Android App (Experimental)](#android-app-experimental)
- [Generic Linux Install (Raspberry Pi, BTT, x86)](#generic-linux-install-raspberry-pi-btt-x86)
- [First Boot & Setup Wizard](#first-boot--setup-wizard)
- [Display Configuration](#display-configuration)
- [Updating HelixScreen](#updating-helixscreen)
- [Uninstalling](#uninstalling)
- [Getting Help](#getting-help)

---

## Quick Start

> **⚠️ Run these commands on your printer's host, not your local computer.**
>
> SSH into your Raspberry Pi, BTT CB1/CB2/Manta, or similar host. For all-in-one printers (Creality K1, K2 series, Flashforge Adventurer 5M/Pro), SSH directly into the printer itself as root.

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

The installer automatically detects your platform and downloads the correct release.

> **Note:** Both `bash` and `sh` work. The installer is POSIX-compatible for BusyBox environments.

**KIAUH users:** HelixScreen is available as a KIAUH extension! Run `kiauh` and find HelixScreen in the extensions menu, or use the one-liner above. See [scripts/kiauh/](https://github.com/prestonbrown/helixscreen/tree/main/scripts/kiauh) for details.

After installation, the setup wizard will guide you through initial configuration.

> **Upgrading from an older version?** If HelixScreen keeps showing the setup wizard after an update, see [UPGRADING.md](UPGRADING.md) for how to fix configuration issues.

---

## Which printer are you installing on?

The one-liner above works on every supported platform, but each printer family has quirks: firmware prerequisites, different install locations, its own service and update commands. The guide for your printer has all of that:

| Printer | Install guide |
|---------|---------------|
| Generic Linux - Raspberry Pi, BTT CB1/CB2/Manta, x86 | This page - see [Generic Linux Install](#generic-linux-install-raspberry-pi-btt-x86) below |
| Creality K1 / K1C / K1 Max | [Creality K1C Setup Guide](guide/creality-k1c-setup.md) - root access plus Simple AF or Guilouz firmware |
| Creality K2 / K2 Plus / K2 Pro | [K2 Series Install](guide/install-k2.md) - works on stock firmware, no custom firmware needed |
| Flashforge Adventurer 5M / 5M Pro | [Adventurer 5M Install](guide/install-ad5m.md) - Forge-X or Klipper Mod, plus a ready-made firmware image |
| FlashForge Adventurer 5X | [Adventurer 5X Install (ZMOD)](guide/install-ad5x.md) - the ZMOD firmware mod manages install and updates |
| FlashForge Creator 5 / Creator 5 Pro | [Z-Mod wiki](https://wiki.zmod.link/C5PRO/) - Z-Mod installs and updates HelixScreen itself; on [Reforge](https://github.com/Klipper4FlashForge/firmware), HelixScreen ships with the firmware |
| Elegoo Centauri Carbon | [Centauri Carbon Install](guide/install-cc1.md) - requires the OpenCentauri COSMOS firmware, 26.07.0 or newer |
| Creality Sonic Pad | [Sonic Pad Install](guide/install-sonicpad.md) - requires the SonicPad-Debian firmware |
| Snapmaker U1 | [Snapmaker U1 Install](guide/install-u1.md) - stock firmware 1.2+ with Root access, or PAXX Extended Firmware |

---

## Remote Screen Setup (Run on a Separate Device)

HelixScreen does **not** have to run on your printer. You can install it on any supported Linux device and have it drive a display while it talks to your printer's Moonraker over the network.

This is the setup to choose when:

- Your printer has no built-in screen, like a Voron, RatRig, or any Klipper printer whose host has no panel of its own
- The printer lives somewhere you don't: another room, a garage, a workshop
- You run more than one printer and want a single screen for all of them: with [multi-printer support](guide/beta-features.md) (beta) enabled, the printer manager switches between every printer you've added
- Your printer's stock panel can't be replaced (some QIDI models)

Common screen devices:

- A spare Raspberry Pi (3/4/5, Zero 2 W, CM4) with a touchscreen
- A repurposed Klipper pad or a small self-built touchscreen PC
- A mini PC or x86 box with an HDMI touchscreen
- Your desktop, running the app in a window (macOS or Linux) for monitoring

**How it works:** HelixScreen is a Moonraker client. It only needs network access to your printer's Moonraker instance (port `7125` by default); it does **not** need to run on the same machine as Klipper.

**Steps:**

1. Install HelixScreen on the device that will drive the display, using the [Quick Start](#quick-start) one-liner or the platform section that matches that device (e.g. a Raspberry Pi uses the [generic Linux](#generic-linux-install-raspberry-pi-btt-x86) steps). Install it on the *screen* device, not the printer.
2. Make sure the device is on the same network as your printer and can reach it: from the device, `ping <printer-ip>` should succeed.
3. On first boot, the setup wizard reaches [Step 4: Moonraker Connection](#step-4-moonraker-connection). Enter your **printer's IP address** (not `localhost`), for example `192.168.1.50`. Leave the port at the default `7125` unless you've changed it.
4. The wizard tests the connection, then discovers your printer's capabilities as usual.

> **Point it at Moonraker, not Mainsail/Fluidd.** HelixScreen connects to Moonraker's API (port `7125`), not the Mainsail/Fluidd web interface. You do not need Mainsail or Fluidd installed on the screen device at all.

To change the host later, go to **Settings > System > Host**, or edit `moonraker_host` in `settings.json`.

> **Note:** A remote screen controls the printer the same as an on-printer screen would. Features that require running *on the printer* (for example, HelixScreen taking over the printer's own physical panel, or on-device WiFi configuration in the wizard) don't apply to a remote install, but all printing, monitoring, and control features work normally.

---

## Android App (Experimental)

HelixScreen also runs on an Android phone or tablet. It is the same remote client described above, just on a device you already own: it talks to your printer's Moonraker over the network and does not install anything on the printer.

> **This is experimental.** It works, but it has had less real-world use than the Linux builds. Expect rough edges and please report them.

**What you need:**

- Android 9.0 or newer
- The phone or tablet on the same network as your printer
- Your printer's IP address

**Which file to download.** Grab it from the [latest release](https://github.com/prestonbrown/helixscreen/releases/latest):

| File | Use it for |
|------|-----------|
| `helixscreen-android-arm64-v<VERSION>.apk` | Essentially every modern phone and tablet. **Start here** |
| `helixscreen-android-x86_64-v<VERSION>.apk` | Emulators and x86 Chromebooks |
| `helixscreen-android-universal-v<VERSION>.apk` | Works everywhere, but a larger download. Use it if arm64 refuses to install |

Ignore the `.aab` file on the release page. That one is only for publishing to Google Play and will not install on a device.

**Installing it.**

1. Download the APK on the device, or transfer it there.
2. Open it. Android will warn that it came from outside the Play Store and offer to let your browser or file manager install apps. Allow it for that app, then confirm the install.
3. If you prefer a cable, `adb install helixscreen-android-arm64-v<VERSION>.apk` from a computer works too.

**First run.** The setup wizard appears exactly as it does elsewhere. At [Step 4: Moonraker Connection](#step-4-moonraker-connection), enter your **printer's IP address** (for example `192.168.1.50`) and leave the port at `7125`.

**Good to know:**

- **It runs in landscape.** Turn the device sideways, or let auto-rotate handle it.
- **It asks for very little.** Network access, and permission to keep the screen awake so a print you are watching does not black out. No location, storage, contacts, or camera.
- **Updating means downloading the new APK** and installing over the old one. There is no in-app updater on Android yet, and the app will not update itself.
- **Foldables and unusual screen shapes** can show layout quirks when the device folds or resizes. Reports with a screenshot are welcome.
- **It cannot do the printer-side things.** Anything that requires running *on the printer*, like taking over the printer's own panel or configuring the printer's WiFi during the wizard, does not apply here. Printing, monitoring, and control all work normally.
- **Power controls are hidden.** An Android app can't power off or reboot the device it runs on, so the shutdown widget never appears on the Home panel (it shows as "Not available on Android" in the widget catalog) and the **POWER** section of the Advanced panel is hidden too. Moonraker power *devices* (smart plugs and the like) are unaffected.

**Coming to Google Play.** Play Store distribution is in progress. When it lands, installs from Play will be signed differently from these APKs, which means moving from a sideloaded install to the Play version will require uninstalling first and setting the app up again. Sideloading will keep working either way.

---

## Generic Linux Install (Raspberry Pi, BTT, x86)

This covers any Klipper printer with a Raspberry Pi running MainsailOS (or similar), including SOVOL SV06, SOVOL SV08, Voron, RatRig, and other printers where Klipper runs on a separate Pi. Also works on x86 Linux PCs (e.g., mini ITX) running Debian/Ubuntu with Klipper and a touchscreen, and on BTT CB1/CB2/Manta host boards.

### Prerequisites

- **Hardware:**
  - Raspberry Pi 3, 4, or 5, or a BTT CB1/CB2: any of them work. Pi 3 / Zero 2 W is plenty for HelixScreen; Pi 4/5 only matters if your overall Klipper setup wants more headroom for cameras, slicing, etc.
  - Both **64-bit** and **32-bit** Raspberry Pi OS / MainsailOS supported
  - Touchscreen display (HDMI, DSI, or SPI)
  - Network connection (Ethernet or WiFi)

- **Software:**
  - MainsailOS installed and working
  - Klipper running and printing works via Mainsail web interface
  - SSH access to your Pi
  - **Debian 11 (Bullseye) or newer**: glibc 2.31+. See the OS version note below.
  - About 100MB free disk space

> **32-bit vs 64-bit:** The installer automatically detects your OS architecture and downloads the correct binary. If you're unsure which you have, run `uname -m`: `aarch64` means 64-bit, `armv7l` means 32-bit.

> **OS version: Bullseye or newer.** The `pi` and `pi32` packages are dynamically linked
> against **glibc 2.31**, the version in Debian 11 (Bullseye). They will not start on an
> older release. Debian 10 (Buster) ships glibc 2.28, which is too old; the binary fails
> at load with `version 'GLIBC_2.29' not found` or similar. Check yours with:
>
> ```bash
> ldd --version | head -1        # glibc version
> cat /etc/os-release | head -2  # distro release
> ```
>
> This matters mainly on **stock printer images**, several of which still ship Buster even
> though current Raspberry Pi OS and MainsailOS are well past it. If you are on one of
> those and cannot upgrade the OS, install the **`cc1` package instead**; it is statically
> linked and carries its own C library, so it runs on old armv7 systems regardless of what
> glibc they have. It has been used successfully this way on non-Creality armv7 hardware
> (e.g. Rockchip RV1126 boards). See [TROUBLESHOOTING.md](TROUBLESHOOTING.md#binary-wont-start-glibc-version-not-found).

### Step 1: Connect to Your Pi

Open a terminal and SSH into your Raspberry Pi:

```bash
ssh pi@mainsailos.local
# Or use your Pi's IP address:
ssh pi@192.168.1.xxx
```

Default password is usually `raspberry` unless you changed it.

### Step 2: Run the Installer

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

The installer automatically:
1. Detects your platform, architecture (32-bit or 64-bit), and Klipper ecosystem
2. Downloads the correct release
3. Stops any competing UIs (KlipperScreen, etc.)
4. Installs to `~/helixscreen` when run as a normal user, or `/opt/helixscreen` when run as root
5. Configures and starts the systemd service
6. Sets up Moonraker update_manager for web UI updates

> **Where does it install?** The installer puts HelixScreen in your home directory when it runs as a normal user (with or without a Klipper ecosystem alongside), and in `/opt` when it runs as root. Override with `INSTALL_DIR=/custom/path/helixscreen` (the directory name must contain `helixscreen`).

### Step 3: Complete the Setup Wizard

After installation, HelixScreen starts automatically. The on-screen wizard guides you through:
1. WiFi configuration (if not connected via Ethernet)
2. Finding your Moonraker instance
3. Identifying your printer
4. Selecting heaters, fans, and LEDs

See [First Boot & Setup Wizard](#first-boot--setup-wizard) for details.

### Service Management

The installer configures systemd to start HelixScreen on boot. Verify with:

```bash
sudo systemctl is-enabled helixscreen
# Should show: enabled
```

If not enabled:
```bash
sudo systemctl enable helixscreen
```

Day-to-day control:
```bash
sudo systemctl start helixscreen      # start
sudo systemctl stop helixscreen       # stop
sudo systemctl restart helixscreen    # restart (after config changes)
sudo systemctl status helixscreen     # status
sudo journalctl -u helixscreen -f     # follow live logs
```

> **Note:** The installer automatically stops and disables competing UIs. To disable KlipperScreen by hand:
> ```bash
> sudo systemctl stop KlipperScreen
> sudo systemctl disable KlipperScreen
> ```
> Printers with SysV init (K1, K2, AD5M, AD5X, CC1, Snapmaker U1) use their init script instead; see your printer's install guide.

### Raspberry Pi 5

Pi 5 has multiple DRM devices. HelixScreen auto-detects the correct one, but if you have issues:

```json
// settings.json
{
  "display": {
    "drm_device": "/dev/dri/card1"
  }
}
```

Common Pi 5 DRM devices:
- `/dev/dri/card0`: v3d (3D acceleration only, no display)
- `/dev/dri/card1`: DSI touchscreen (if connected)
- `/dev/dri/card2`: HDMI output

### Camera Streaming Performance

If you use a webcam with HelixScreen, install `libturbojpeg0` for faster camera feed rendering:

```bash
sudo apt install libturbojpeg0
```

The installer attempts this automatically, but it's listed here in case your Pi was offline during installation. HelixScreen detects and uses it automatically for 3-5x faster JPEG decoding via hardware SIMD acceleration.

### Low Memory Systems (Pi 3, Pi Zero 2 W)

HelixScreen is optimized for low memory, but if the host is still tight:

1. Camera streaming is usually the biggest memory consumer on a small Pi; disable it if you don't need it
2. Reduce Moonraker's print history retention
3. Disable other services you don't need

---

## First Boot & Setup Wizard

When HelixScreen starts for the first time, a setup wizard guides you through configuration:

### Step 1: Touchscreen Calibration
Calibrate your touchscreen by tapping the targets. This ensures accurate touch input.

> **Note:** This step is skipped automatically when your touchscreen doesn't need calibration: most capacitive and USB touchscreens are factory-calibrated. Only resistive panels (and panels reporting broken coordinate ranges) get this step. You can always recalibrate later from **Settings**.

### Step 2: Language Selection
Choose your preferred language.

### Step 3: Network Setup
Connect to your wireless network or configure Ethernet. You can:
- Select from detected WiFi networks
- Skip if using Ethernet or already connected

> **Note:** Hidden networks are not listed. Skip this step and join later from **Settings > System > Network Settings**, which can add a network by name.

### Step 4: Moonraker Connection
Enter your Moonraker host. For most setups:
- **MainsailOS:** `localhost` or `127.0.0.1`
- **AD5M:** `localhost`
- **Remote printer:** Enter the IP address

The wizard will test the connection before proceeding.

### Step 5: Printer Identification
HelixScreen will try to identify your printer from its configuration. You can:
- Confirm the detected printer type
- Select from a database of 90+ printers
- Enter custom settings

### Step 6: Heater Selection
Choose which heaters to display and control:
- Hotend/nozzle heater
- Bed heater
- Chamber heater (if available)

### Step 7: Fan Selection
Select your cooling fans:
- Part cooling fan
- Hotend fan
- Other auxiliary fans

### Step 8: AMS Identification (If Detected)
If a multi-filament system (AMS, CFS, IFS, ACE, and similar) is detected, confirm which lanes or slots exist and what is loaded in them.

### Step 9: LED Selection (Optional)
If your printer has controllable LEDs:
- Chamber lights
- Status LEDs
- NeoPixel strips

### Step 10: Filament Sensor (Optional)
If standalone filament sensors are present, choose what each one does (runout detection, motion detection).

### Step 11: Input Shaper (Optional)
Configure resonance compensation if your printer supports input shaping.

### Step 12: Hardware Summary
Review your configured hardware before completing setup.

### Completion
After the wizard, you'll be taken to the home screen. Your settings are saved automatically.

> **Note:** On printers whose install package ships pre-configured hardware (K2, AD5M, and similar), the hardware steps and the summary are collapsed, and a one-time telemetry opt-in screen appears instead.

---

## Display Configuration

### HDMI Displays (Plug and Play)

Most HDMI touchscreens work automatically. If touch input isn't working:

1. Check that the USB cable from the display is connected to your Pi
2. Verify the display appears in `/dev/input/`:
   ```bash
   ls /dev/input/event*
   ```

### Official Raspberry Pi Touchscreen (DSI)

The official 7" Pi touchscreen is detected automatically via DSI connector.

If using non-standard orientation, edit /boot/config.txt:
```ini
# Rotate display 180 degrees
lcd_rotate=2
```

### SPI Displays (Requires Configuration)

For SPI displays (like many small LCDs):

1. Enable SPI in /boot/config.txt
2. Install the appropriate overlay
3. Configure framebuffer settings

See the [MainsailOS display documentation](https://docs.mainsail.xyz/) for specific display setup.

### BTT Pad 7 and Similar All-in-One Pads

The BTT Pad 7 and similar "Klipper Pad" devices are complete units, with the single-board computer and touchscreen integrated in one housing. Display output and USB touch input come pre-configured, and HelixScreen should detect and use them automatically.

### Screen Rotation

**Prefer rotating the display itself when it can.** If your monitor has a rotation option (usually a button or an OSD menu), use it. On a Pi with a DSI panel, the kernel can rotate the panel in hardware: add `video=DSI-1:panel_orientation=upside_down` to /boot/firmware/cmdline.txt (HelixScreen detects this automatically on first boot; see [TROUBLESHOOTING: display upside down or rotated](TROUBLESHOOTING.md#display-upside-down-or-rotated)). Rotation done by the display is free. HelixScreen's `rotate` setting below is software rotation: it costs CPU on every frame, and on the Pi it also switches the display to the framebuffer backend (see the note on backends below). Use it when your display cannot rotate itself.

To rotate in software (e.g., a screen mounted upside-down that has no rotation of its own), add to your `settings.json` (typically at `~/helixscreen/config/settings.json`):

```json
{
  "display": {
    "rotate": 180
  }
}
```

Valid values: `0`, `90`, `180`, `270`. Restart HelixScreen after changing.

Touch coordinates are automatically adjusted to match the rotation: no separate touch configuration is needed.

**Rotation and display backends:** When rotation is configured on Raspberry Pi, HelixScreen checks whether your display hardware supports rotating the image directly. Most DSI/HDMI displays on Pi do not support hardware rotation. In that case, HelixScreen automatically switches from the DRM (GPU) backend to the framebuffer backend, which handles software rotation without any screen flicker. This switch is transparent: no manual configuration needed.

If you experience any display issues with rotation, you can also force the framebuffer backend manually by setting `HELIX_DISPLAY_BACKEND=fbdev` (see below).

### Display Backends: DRM vs Framebuffer

By default, HelixScreen uses the DRM/KMS backend when available. DRM presents each frame with a vsynced page flip instead of a plain memory copy, which avoids tearing; rendering itself is CPU-based on both backends. On boards where DRM is not supported, it falls back to the framebuffer (`fbdev` backend), which copies each frame directly with no vsync.

**When rotation is configured**, HelixScreen may automatically switch to the fbdev backend if the display hardware doesn't support hardware rotation. This is normal and provides flicker-free rotation.

**Supported DRM hardware:**
- Raspberry Pi 3B+, Pi 4, Pi 5
- BTT CB1, CB2 (and other Allwinner H616/H618 boards)
- Display must be connected via HDMI or DSI (SPI displays are not supported)

**To force a specific backend:**

Edit your systemd service override:
```bash
sudo systemctl edit helixscreen
```

Add the following lines:
```ini
[Service]
Environment="HELIX_DISPLAY_BACKEND=fbdev"
```

Then restart:
```bash
sudo systemctl restart helixscreen
```

Valid backends: `drm` (vsynced, avoids tearing), `fbdev` (maximum compatibility).

**How to revert to auto-detection:**

Remove the override and restart:
```bash
sudo systemctl revert helixscreen
sudo systemctl restart helixscreen
```

> **Note:** On Raspberry Pi 5, you may also need to specify the correct display device if auto-detection picks the wrong one. Add `Environment="HELIX_DRM_DEVICE=/dev/dri/card1"` for DSI displays or `Environment="HELIX_DRM_DEVICE=/dev/dri/card2"` for HDMI. See [CONFIGURATION.md](CONFIGURATION.md#display-settings) for details.

---

## Updating HelixScreen

> Platform-specific update commands live in your printer's install guide: the two-step offline process on printers without HTTPS fetch tools (K1, Adventurer 5M), the AD5X chroot path, bundled-installer locations. Start from [Which printer are you installing on?](#which-printer-are-you-installing-on).

Three ways to update, in order of preference: in the app itself, from the Mainsail/Fluidd update manager, or from the command line.

### In the App Itself (Preferred)

The app can update itself: **Settings > Help & About > About > Check for Updates**. It shows the new version, downloads it with a progress bar, and installs it; a **Retry** button appears if the download fails. No SSH and no web browser needed. The **Update Channel** row beside it picks Stable or Beta.

This option is hidden where something else manages updates for you, such as on the Snapmaker U1, whose firmware handles HelixScreen updates itself. On Android, the install step opens the Play Store instead of downloading in-app. See [Checking for Updates](guide/settings/help-about.md#checking-for-updates) for the full walkthrough.

### Check Current Version

On the touchscreen: **Settings > Help & About > About** shows the current version.

Or via SSH (path shown for a generic Linux install; other platforms' paths are in their guides):
```bash
~/helixscreen/bin/helix-screen --version
```

### Update from Mainsail/Fluidd Web UI

If you installed via the installer script, it automatically configures Moonraker's update_manager. You can update HelixScreen with one click from the Mainsail or Fluidd web interface:

1. Open Mainsail/Fluidd in your browser
2. Navigate to **Machine** (Mainsail) or **Settings** (Fluidd)
3. Find **HelixScreen** in the update manager
4. Click **Update** when a new version is available

> **Note:** The installer adds an `[update_manager helixscreen]` section to your `moonraker.conf`. If you installed manually, see [Manual Update Manager Setup](#manual-update-manager-setup) below.

### Update Using the Install Script (Command Line)

From the command line, over SSH, run the installer with `--update`:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update
```

This preserves your configuration and updates to the latest version. Printers without direct internet access use a two-step process instead; see your printer's install guide.

### Update to Specific Version

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update --version v1.2.0
```

To reinstall a specific version with a **fresh settings.json** (instead of keeping your existing settings), swap `--update` for `--clean`:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --clean --yes --version v1.2.0
```

### Preserving Configuration

The update process preserves your `settings.json` settings. If you want to reset to defaults, use the `--clean` flag; it removes your HelixScreen settings and caches everywhere they live, then does a fresh install:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --clean --yes
```

`--yes` skips the confirmation prompt, which a piped command cannot show; run the downloaded script interactively over SSH and you get the prompt instead. Your Klipper config, Moonraker settings, print history, and G-code files are **not** touched: only HelixScreen's own settings.

To reset settings **and** pin a specific version in one step, combine `--clean` with `--version`:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --clean --yes --version v1.2.0
```

If you'd rather delete the settings file by hand instead of reinstalling:

```bash
# Use your actual install path (~/helixscreen or /opt/helixscreen)
sudo rm ~/helixscreen/config/settings.json
sudo systemctl restart helixscreen
```

### Manual Update Manager Setup

If you installed manually or the installer couldn't find your `moonraker.conf`, add this to enable web UI updates:

```ini
# Add to moonraker.conf
# NOTE: The 'path' varies by platform:
#   Pi: ~/helixscreen (or /opt/helixscreen if no Klipper ecosystem)
#   K1/Simple AF: /usr/data/helixscreen
#   AD5M Klipper Mod: /root/printer_software/helixscreen
[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: ~/helixscreen
```

> **Important:** Do not add `install_script`, `managed_services`, or `persistent_files`
> to this section; these options are not supported with `type: web` and Moonraker will
> log warnings about unparsed config options. Service restart after updates is handled
> automatically by a systemd path unit installed during setup.

Then restart Moonraker:
```bash
sudo systemctl restart moonraker
```

---

## Uninstalling

### Using Install Script (Recommended)

The install script with `--uninstall` removes HelixScreen and **restores your previous UI** (KlipperScreen, the stock printer screen, etc.):

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --uninstall
```

Platform-specific details are in your printer's install guide: the bundled installer's location on printers without HTTPS fetch tools, manual revert steps, what gets restored on each firmware.

### Manual Uninstall

<details>
<summary>Generic Linux (systemd)</summary>

```bash
# Stop and disable service
sudo systemctl stop helixscreen
sudo systemctl disable helixscreen

# Remove service file
sudo rm /etc/systemd/system/helixscreen.service
sudo systemctl daemon-reload

# Remove installation (check your actual path)
sudo rm -rf ~/helixscreen
# Or if installed to /opt:
sudo rm -rf /opt/helixscreen
```
</details>

---

## Getting Help

### Check Logs First

Most issues are diagnosed from the logs. On systemd hosts (Raspberry Pi, BTT, x86, Sonic Pad):

```bash
# View recent logs
sudo journalctl -u helixscreen -n 100

# Follow live logs
sudo journalctl -u helixscreen -f

# Filter by error/warning level
sudo journalctl -u helixscreen -p err
```

Log locations for the other platforms (K1, K2, AD5M, AD5X, CC1, Snapmaker U1) are in your printer's install guide: each has a launcher/crash log plus a platform-specific app log.

### Capturing Logs for a Bug Report

A problem reproduced at a higher log level gives far more to work with:

1. Set **Settings > System > Log Level** to **Debug**, or **Trace** for touch or display issues (Trace is very verbose). It takes effect immediately; no restart is needed.
2. Perform the action that misbehaves.
3. Send a debug bundle: **Settings > Help & About > Upload Debug Bundle** collects the logs (including everything since startup), strips personal data, and gives you a share code to include in your report. See [Debug Bundles](guide/settings/help-about.md#debug-bundles).
4. Turn the log level back to where it was. Debug and trace generate a lot of output; journald rotates and caps itself on systemd hosts, but the printer-hosted platforms write plain log files that nothing rotates.

### Common Issues

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for solutions to:
- Connection problems
- Display issues
- Touch not responding
- Configuration errors

### Still Stuck?

1. Ask in the [HelixScreen Discord](https://discord.gg/RZCT2StKhr) for quick help
1. Check [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) for known problems
1. Open a new issue with:
    1. Your hardware (Pi model, display type)
    1. HelixScreen version
    1. Relevant log output
    1. Steps to reproduce

---

*Next: [User Guide](USER_GUIDE.md) (learn how to use HelixScreen)*
