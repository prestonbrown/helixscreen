# HelixScreen Installation Guide

This guide walks you through installing HelixScreen on your 3D printer's touchscreen display.

**Target Audience:** Klipper users who want to use pre-built packages. If you're a developer building from source, see [DEVELOPMENT.md](../DEVELOPMENT.md).

---

## Table of Contents

- [Quick Start](#quick-start)
- [Prerequisites](#prerequisites)
- [MainsailOS Installation](#mainsailos-installation)
- [Flashforge Adventurer 5M Installation](#flashforge-adventurer-5m-installation)
- [Creality K1 Installation](#creality-k1-series-simple-af)
- [Creality K2 Series (Untested)](#creality-k2-series-untested)
- [Elegoo Centauri Carbon 1 (Testing)](#elegoo-centauri-carbon-1-testing)
- [First Boot & Setup Wizard](#first-boot--setup-wizard)
- [Display Configuration](#display-configuration)
- [Starting on Boot](#starting-on-boot)
- [Updating HelixScreen](#updating-helixscreen)
- [Uninstalling](#uninstalling)
- [Getting Help](#getting-help)

---

## Quick Start

> **⚠️ Run these commands on your printer's host, not your local computer.**
>
> SSH into your Raspberry Pi, BTT CB1/Manta, or similar host. For all-in-one printers (Creality K1, K2 series, Flashforge Adventurer 5M/Pro), SSH directly into the printer itself as root.

**Raspberry Pi (MainsailOS) or Creality K1:**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer automatically detects your platform and downloads the correct release.

**Flashforge Adventurer 5M:** The easiest option is our [ready-made firmware image](https://github.com/prestonbrown/ff5m) — just flash from a USB drive. For manual installation on existing Forge-X or Klipper Mod setups, see [Flashforge Adventurer 5M Installation](#flashforge-adventurer-5m-installation).

> **Note:** Both `bash` and `sh` work. The installer is POSIX-compatible for BusyBox environments.

**KIAUH users:** HelixScreen is available as a KIAUH extension! Run `kiauh` and find HelixScreen in the extensions menu, or use the one-liner above. See [scripts/kiauh/](https://github.com/prestonbrown/helixscreen/tree/main/scripts/kiauh) for details.

> **Pre-flight checks:** On AD5M and K1, the installer validates that Klipper and Moonraker are running before proceeding. If either is missing, you'll get a clear error message explaining what's needed.

After installation, the setup wizard will guide you through initial configuration.

> **Upgrading from an older version?** If HelixScreen keeps showing the setup wizard after an update, see [UPGRADING.md](UPGRADING.md) for how to fix configuration issues.

---

## Prerequisites

### MainsailOS (Raspberry Pi)

This covers any Klipper printer with a Raspberry Pi running MainsailOS (or similar), including SOVOL SV06, SOVOL SV08, Voron, RatRig, and other printers where Klipper runs on a separate Pi.

- **Hardware:**
  - Raspberry Pi 3, 4, or 5 (Pi 3 is minimum, Pi 4/5 recommended)
  - Raspberry Pi Zero 2 W also supported
  - Both **64-bit** and **32-bit** Raspberry Pi OS / MainsailOS supported
  - Touchscreen display (HDMI, DSI, or SPI)
  - Network connection (Ethernet or WiFi)

- **Software:**
  - MainsailOS installed and working
  - Klipper running and printing works via Mainsail web interface
  - SSH access to your Pi
  - About 100MB free disk space

> **32-bit vs 64-bit:** The installer automatically detects your OS architecture and downloads the correct binary. If you're unsure which you have, run `uname -m` — `aarch64` means 64-bit, `armv7l` means 32-bit.

### Flashforge Adventurer 5M / 5M Pro

> **Easiest option:** We provide a [ready-made firmware image](https://github.com/prestonbrown/ff5m) — a fork of Forge-X 1.4.0 with HelixScreen pre-configured. Just put it on a flash drive and install on your printer. No SSH, no manual setup. If you'd rather install HelixScreen on an existing Forge-X or Klipper Mod setup, follow the manual instructions below.

- **Hardware:**
  - Flashforge Adventurer 5M or 5M Pro
  - Stock 4.3" touchscreen (800x480)
  - Network connection

- **Software:**
  - Custom Klipper firmware: [Forge-X](https://github.com/DrA1ex/ff5m) **or** [Klipper Mod](https://github.com/xblax/flashforge_ad5m_klipper_mod)
  - SSH access to the printer (usually `root@<printer-ip>`)
  - About 100MB free disk space

> **Tested versions:** Most thoroughly tested on ForgeX 1.4.0 with Flashforge firmware 3.1.5. Other versions may work fine.

#### AD5M Firmware Variants

The installer automatically detects which firmware you're running and configures paths accordingly:

| Firmware | Replaces | Install Location | Init Script |
|----------|----------|------------------|-------------|
| **Forge-X** | GuppyScreen | `/opt/helixscreen/` | `S90helixscreen` |
| **Klipper Mod** | KlipperScreen | `/root/printer_software/helixscreen/` | `S80helixscreen` |

**Memory Savings:** On Klipper Mod, HelixScreen (~10MB) replaces KlipperScreen (~50MB), freeing ~40MB RAM on the memory-constrained AD5M.

#### Forge-X Prerequisites

**Important:** ForgeX must be installed and configured for GuppyScreen mode **before** installing HelixScreen. HelixScreen uses ForgeX's infrastructure (Klipper, Moonraker, backlight control) but replaces the GuppyScreen UI.

1. Install ForgeX following [their instructions](https://github.com/DrA1ex/ff5m)
2. Configure ForgeX with `display = 'GUPPY'` in variables.cfg
3. Verify GuppyScreen works on the touchscreen
4. Then run the HelixScreen installer

The HelixScreen installer will:
- Keep ForgeX in GUPPY display mode (required for backlight control)
- Disable GuppyScreen's init scripts (so HelixScreen takes over)
- Disable the stock Flashforge UI in auto_run.sh
- Patch ForgeX's `screen.sh` to prevent backlight dimming conflicts
- Install HelixScreen as the replacement touchscreen UI

On uninstall, all ForgeX changes are reversed and GuppyScreen is restored.

### Creality K1 Series (Simple AF)

- **Hardware:**
  - Creality K1, K1C, K1 Max, or similar
  - Stock touchscreen display
  - Network connection

- **Software:**
  - [Simple AF](https://github.com/pellcorp/creality) installed and working
  - SSH access to the printer (`root@<printer-ip>`)
  - About 100MB free disk space in `/usr/data`

The installer automatically detects Simple AF and configures paths:

| Environment | Replaces | Install Location | Init Script |
|-------------|----------|------------------|-------------|
| **Simple AF** | GuppyScreen | `/usr/data/helixscreen/` | `S99helixscreen` |

**Prerequisites:**
1. Install Simple AF following [their instructions](https://github.com/pellcorp/creality)
2. Verify GuppyScreen works on the touchscreen
3. Then run the HelixScreen installer

The HelixScreen installer will:
- Stop and disable GuppyScreen
- Install HelixScreen to `/usr/data/helixscreen/`
- Configure Moonraker update_manager for one-click updates from Fluidd/Mainsail

On uninstall, GuppyScreen is restored.

### Creality K2 Series (Untested)

> **⚠️ This platform has not been tested on real hardware.** The build target exists and produces a binary, but no one has run it on an actual K2 yet. If you have a K2 with SSH access, we'd love your help validating it!

- **Hardware:**
  - Creality K2, K2 Pro, K2 Plus, or K2 SE
  - Stock 4.3" touchscreen display (480x800)
  - Network connection

- **Software:**
  - Stock firmware with root access enabled (Settings → "Root account information")
  - SSH access (`root@<printer-ip>`, password: `creality_2024`)
  - Moonraker is included in stock firmware on port 4408

**What's different from K1:**
- ARM processor (Allwinner, not MIPS) — standard cross-compilation
- Stock Moonraker — no community firmware (Simple AF, etc.) required
- OpenWrt-based init system (procd, not SysV)
- Display may be portrait orientation (480x800) — rotation support may be needed

**Current status:**
- Build target: `make k2-docker` produces a static ARM binary
- Deploy targets exist: `make deploy-k2 K2_HOST=<ip>`
- No installer script support yet — manual deployment only
- See `docs/printer-research/CREALITY_K2_PLUS_RESEARCH.md` for full research and open questions

**If you want to help test**, run the diagnostic commands in the research doc (Section 13) and report back via [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues).

### Elegoo Centauri Carbon 1 (Testing)

> **Active testing is underway on this platform.** Prebuilt binaries are included in releases, but there is no installer script support yet — manual deployment only.

- **Hardware:**
  - Elegoo Centauri Carbon 1
  - Stock touchscreen display
  - Network connection

- **Software:**
  - Klipper + Moonraker running on the printer
  - SSH access to the printer

**Current status:**
- Build target: `make cc1-docker` produces a static armv7-a binary
- Deploy targets exist: `make deploy-cc1 CC1_HOST=<ip>`
- Prebuilt `cc1` binaries are included in GitHub releases
- No installer script support yet — manual deployment only

**If you're testing on this printer**, please report your results via [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) or [Discord](https://discord.gg/RZCT2StKhr).

---

## Raspberry Pi / MainsailOS Installation

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
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer automatically:
1. Detects your platform, architecture (32-bit or 64-bit), and Klipper ecosystem
2. Downloads the correct release
3. Stops any competing UIs (KlipperScreen, etc.)
4. Installs to `~/helixscreen` (if Klipper ecosystem detected) or `/opt/helixscreen` (fallback)
5. Configures and starts the systemd service
6. Sets up Moonraker update_manager for web UI updates

> **Install path auto-detection:** The installer checks for `~/klipper`, `~/moonraker`, `~/printer_data`, or an active `moonraker.service`. If any are found, HelixScreen installs alongside them in your home directory. Override with `INSTALL_DIR=/custom/path`.

### Step 3: Complete the Setup Wizard

After installation, HelixScreen starts automatically. The on-screen wizard guides you through:
1. WiFi configuration (if not connected via Ethernet)
2. Finding your Moonraker instance
3. Identifying your printer
4. Selecting heaters, fans, and LEDs

See [First Boot & Setup Wizard](#first-boot--setup-wizard) for details.

---

## Flashforge Adventurer 5M Installation

### Ready-Made Firmware Image (Easiest)

We maintain a [ready-made firmware image](https://github.com/prestonbrown/ff5m) — a fork of Forge-X 1.4.0 with HelixScreen pre-configured. This is the fastest way to get up and running:

1. Download the image from [github.com/prestonbrown/ff5m](https://github.com/prestonbrown/ff5m)
2. Copy it to a USB flash drive
3. Insert the flash drive into your AD5M or AD5M Pro and install

That's it — no SSH, no manual commands. HelixScreen will be ready to go after the firmware installs.

> If you already have Forge-X or Klipper Mod installed and prefer to add HelixScreen manually, continue with the instructions below.

### Manual Installation

> **Important:** Installing HelixScreen replaces your current screen UI (GuppyScreen on Forge-X, KlipperScreen on Klipper Mod). Make sure you have a backup method to access your printer (SSH, Mainsail/Fluidd web interface).

### Automated Installation (Recommended)

The AD5M uses BusyBox which doesn't support HTTPS downloads directly. This is a **two-step process**:
1. Download on your local computer (Steps 1-2)
2. SSH into the printer as root and run the installer (Step 3)

**Step 1: Download on your computer**

Go to the [latest release page](https://github.com/prestonbrown/helixscreen/releases/latest) and download:
- `helixscreen-ad5m-vX.Y.Z.tar.gz` (the AD5M release archive)
- `install.sh` (the installer script, under "Assets")

Or use the command line (replace `vX.Y.Z` with the actual version):
```bash
VERSION=vX.Y.Z  # Check latest at https://github.com/prestonbrown/helixscreen/releases/latest
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-ad5m-${VERSION}.tar.gz"
wget https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh
```

**Step 2: Copy to your printer**

```bash
# AD5M requires -O flag for scp (BusyBox lacks sftp-server)
# Note: Use /data/ not /tmp/ - AD5M's /tmp is a tiny tmpfs (~54MB)
scp -O helixscreen-ad5m-vX.Y.Z.tar.gz install.sh root@<printer-ip>:/data/
```

> **Windows users:** The `-O` flag is not supported by Windows 11's built-in OpenSSH.
> Use one of these alternatives instead:
> - **WSL** (recommended) — open a WSL terminal and run all commands as shown (Linux tools work natively)
> - **[WinSCP](https://winscp.net/)** (free, GUI) — set the protocol to **SCP**, then drag and drop files to `/data/` on the printer
> - **[PuTTY pscp](https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html)** (free, command-line):
>   `pscp helixscreen-ad5m-vX.Y.Z.tar.gz install.sh root@<printer-ip>:/data/`

**Step 3: SSH into the printer and run the installer**

```bash
# From your local computer, SSH into the printer as root
ssh root@<printer-ip>

# Now on the printer, run the installer
sh /data/install.sh --local /data/helixscreen-ad5m-vX.Y.Z.tar.gz
```

The install script automatically detects your firmware (Forge-X or Klipper Mod) and installs to the correct location.

**What the installer does on Forge-X:**
- Verifies ForgeX is installed and sets display mode to `GUPPY`
- Stops and disables GuppyScreen (`chmod -x` on init scripts)
- Disables stock Flashforge UI in `/opt/auto_run.sh`
- Patches `/opt/config/mod/.shell/screen.sh` to skip backlight commands when HelixScreen is running (prevents ForgeX's delayed_gcode from dimming the screen)
- Installs HelixScreen to `/opt/helixscreen/`
- Creates init script at `/etc/init.d/S90helixscreen`

**What the installer does on Klipper Mod:**
- Stops Xorg and KlipperScreen
- Disables their init scripts (`chmod -x`)
- Installs HelixScreen to `/root/printer_software/helixscreen/`
- Creates init script at `/etc/init.d/S80helixscreen`

### Manual Installation

<details>
<summary>Forge-X Manual Installation</summary>

```bash
# Download on your computer (replace vX.Y.Z with actual version)
VERSION=vX.Y.Z
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-ad5m-${VERSION}.tar.gz"

# Copy to printer (AD5M requires scp -O for legacy protocol)
# Note: Use /data/ not /tmp/ - AD5M's /tmp is a tiny tmpfs (~54MB)
# Windows users: use WinSCP (SCP protocol) or PuTTY's pscp instead — see note above
scp -O helixscreen-ad5m-${VERSION}.tar.gz root@<printer-ip>:/data/

# SSH into printer
ssh root@<printer-ip>

# Extract to /opt (Forge-X location)
cd /opt
gunzip -c /data/helixscreen-ad5m-*.tar.gz | tar xf -

# Stop GuppyScreen
/opt/config/mod/.root/S80guppyscreen stop 2>/dev/null || true
chmod -x /opt/config/mod/.root/S80guppyscreen

# Install init script
cp /opt/helixscreen/config/helixscreen.init /etc/init.d/S90helixscreen
chmod +x /etc/init.d/S90helixscreen

# Start HelixScreen
/etc/init.d/S90helixscreen start

# Clean up
rm /data/helixscreen-ad5m-*.tar.gz
```

</details>

<details>
<summary>Klipper Mod Manual Installation</summary>

> **Note:** Klipper Mod's `/tmp` is a small tmpfs (~54MB). The package is ~70MB, so we must use `/mnt/data` instead.

```bash
# Download on your computer (replace vX.Y.Z with actual version)
VERSION=vX.Y.Z
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-ad5m-${VERSION}.tar.gz"

# Copy to printer's data partition (NOT /tmp - it's too small!)
# Windows users: use WinSCP (SCP protocol) or PuTTY's pscp instead — see note above
scp -O helixscreen-ad5m-${VERSION}.tar.gz root@<printer-ip>:/mnt/data/

# SSH into printer
ssh root@<printer-ip>

# Extract to /root/printer_software (Klipper Mod location)
cd /root/printer_software
gunzip -c /mnt/data/helixscreen-ad5m-*.tar.gz | tar xf -

# Stop KlipperScreen
/etc/init.d/S80klipperscreen stop 2>/dev/null || true
chmod -x /etc/init.d/S80klipperscreen

# Install init script (S80 to match KlipperScreen's boot order)
cp /root/printer_software/helixscreen/config/helixscreen.init /etc/init.d/S80helixscreen
chmod +x /etc/init.d/S80helixscreen

# Update the install path in the init script
sed -i 's|DAEMON_DIR=.*|DAEMON_DIR="/root/printer_software/helixscreen"|' /etc/init.d/S80helixscreen

# Start HelixScreen
/etc/init.d/S80helixscreen start

# Clean up
rm /mnt/data/helixscreen-ad5m-*.tar.gz
```

</details>

> **Note:** AD5M runs as root, so `sudo` is not needed.
> **Note:** AD5M uses BusyBox utilities. Use `gunzip -c | tar xf -` instead of `tar -xzf`.
> **Note:** AD5M uses SysV init (BusyBox), not systemd.

### Step 4: Reboot

```bash
reboot
```

After reboot, HelixScreen will start automatically on the touchscreen.

### Step 5: Complete Setup

Use the touchscreen to complete the setup wizard. The printer should auto-detect since it's running locally.

---

## First Boot & Setup Wizard

When HelixScreen starts for the first time, a setup wizard guides you through configuration:

### Step 1: Touchscreen Calibration
Calibrate your touchscreen by tapping the targets. This ensures accurate touch input.

> **Note:** This step may be skipped automatically for known tier-1 supported printers that ship with default calibration values. You can always recalibrate later from **Settings**.

### Step 2: Language Selection
Choose your preferred language.

### Step 3: Network Setup
Connect to your wireless network or configure Ethernet. You can:
- Select from detected WiFi networks
- Enter a hidden network name manually
- Skip if using Ethernet or already connected

### Step 4: Moonraker Connection
Enter your Moonraker host. For most setups:
- **MainsailOS:** `localhost` or `127.0.0.1`
- **AD5M:** `localhost`
- **Remote printer:** Enter the IP address

The wizard will test the connection before proceeding.

### Step 5: Printer Identification
HelixScreen will try to identify your printer from its configuration. You can:
- Confirm the detected printer type
- Select from a database of 50+ printers
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

### Step 8: LED Selection (Optional)
If your printer has controllable LEDs:
- Chamber lights
- Status LEDs
- NeoPixel strips

### Step 9: Input Shaper (Optional)
Configure resonance compensation if your printer supports input shaping.

### Step 10: Hardware Summary
Review your configured hardware before completing setup.

### Completion
After the wizard, you'll be taken to the home screen. Your settings are saved automatically.

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

If using non-standard orientation, edit `/boot/config.txt`:
```ini
# Rotate display 180 degrees
lcd_rotate=2
```

### SPI Displays (Requires Configuration)

For SPI displays (like many small LCDs):

1. Enable SPI in `/boot/config.txt`
2. Install the appropriate overlay
3. Configure framebuffer settings

See the [MainsailOS display documentation](https://docs.mainsail.xyz/) for specific display setup.

### BTT Pad 7 and Similar

The BTT Pad 7 and similar "Klipper Pad" devices typically include:
- Pre-configured display output
- Touch input via USB

HelixScreen should detect and use these automatically.

### Screen Rotation

To rotate the display, add to your `helixconfig.json` (typically at `~/helixscreen/config/helixconfig.json`):

```json
{
  "display": {
    "rotate": 180
  }
}
```

Valid values: `0`, `90`, `180`, `270`

### GPU Rendering (Experimental)

By default, HelixScreen uses CPU-based software rendering (`fbdev` backend), which works on all hardware. On supported boards, you can enable GPU-accelerated rendering for lower CPU usage and smoother animations.

**Supported hardware:**
- Raspberry Pi 3B+, Pi 4, Pi 5
- BTT CB1 (and other Allwinner H616 boards)
- Display must be connected via HDMI or DSI (SPI displays are not supported)

**How to enable:**

Edit your systemd service override:
```bash
sudo systemctl edit helixscreen
```

Add the following lines:
```ini
[Service]
Environment="HELIX_DISPLAY_BACKEND=drm"
```

Then restart:
```bash
sudo systemctl restart helixscreen
```

**How to revert:**

If the display does not work after enabling GPU rendering, SSH into your Pi and remove the override:
```bash
sudo systemctl revert helixscreen
sudo systemctl restart helixscreen
```

This switches back to the default CPU rendering, which works on all displays.

> **Note:** On Raspberry Pi 5, you may also need to specify the correct display device if auto-detection picks the wrong one. Add `Environment="HELIX_DRM_DEVICE=/dev/dri/card1"` for DSI displays or `Environment="HELIX_DRM_DEVICE=/dev/dri/card2"` for HDMI. See [CONFIGURATION.md](CONFIGURATION.md#display-settings) for details.

---

## Starting on Boot

### Enable Automatic Start

The installer configures systemd to start HelixScreen on boot. Verify with:

```bash
sudo systemctl is-enabled helixscreen
# Should show: enabled
```

If not enabled:
```bash
sudo systemctl enable helixscreen
```

### Service Management

**MainsailOS (systemd):**
```bash
# Start HelixScreen
sudo systemctl start helixscreen

# Stop HelixScreen
sudo systemctl stop helixscreen

# Restart (after config changes)
sudo systemctl restart helixscreen

# View status
sudo systemctl status helixscreen

# View logs
sudo journalctl -u helixscreen -f
```

**AD5M (SysV init):**

*Forge-X:*
```bash
/etc/init.d/S90helixscreen start|stop|restart|status
cat /tmp/helixscreen.log  # View logs
```

*Klipper Mod:*
```bash
/etc/init.d/S80helixscreen start|stop|restart|status
cat /tmp/helixscreen.log  # View logs
```

**K1/Simple AF (SysV init):**
```bash
/etc/init.d/S99helixscreen start|stop|restart|status
cat /tmp/helixscreen.log  # View logs
```

### Disabling Other UIs

If you have another UI installed, disable it to avoid conflicts:

**MainsailOS (systemd):**
```bash
# Disable KlipperScreen (if installed)
sudo systemctl stop KlipperScreen
sudo systemctl disable KlipperScreen
```

**AD5M Forge-X (SysV init):**
```bash
# Disable GuppyScreen
/opt/config/mod/.root/S80guppyscreen stop
chmod -x /opt/config/mod/.root/S80guppyscreen
```

**AD5M Klipper Mod (SysV init):**
```bash
# Disable KlipperScreen
/etc/init.d/S80klipperscreen stop
chmod -x /etc/init.d/S80klipperscreen
```

**K1/Simple AF (SysV init):**
```bash
# Disable GuppyScreen
/etc/init.d/S99guppyscreen stop
chmod -x /etc/init.d/S99guppyscreen
```

> **Note:** The HelixScreen installer automatically stops and disables competing UIs.

---

## Updating HelixScreen

### Check Current Version

On the touchscreen: **Settings** → scroll down to the bottom of the page to find the version number.

Or via SSH:
```bash
# Path varies by platform:
#   Pi: ~/helixscreen/bin/helix-screen (or /opt/helixscreen if no Klipper ecosystem)
#   K1: /usr/data/helixscreen/bin/helix-screen
#   K2: /opt/helixscreen/bin/helix-screen (assumed, untested)
#   AD5M Klipper Mod: /root/printer_software/helixscreen/bin/helix-screen
~/helixscreen/bin/helix-screen --version
```

### Update from Mainsail/Fluidd Web UI (Pi Only)

If you installed via the installer script, it automatically configures Moonraker's update_manager. You can update HelixScreen with one click from the Mainsail or Fluidd web interface:

1. Open Mainsail/Fluidd in your browser
2. Navigate to **Machine** (Mainsail) or **Settings** (Fluidd)
3. Find **HelixScreen** in the update manager
4. Click **Update** when a new version is available

> **Note:** The installer adds an `[update_manager helixscreen]` section to your `moonraker.conf`. If you installed manually, see [Manual Update Manager Setup](#manual-update-manager-setup) below.

### Update Using Install Script (Recommended)

The easiest way to update is using the install script with `--update`:

**Raspberry Pi / Creality K1:**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --update
```

**Flashforge Adventurer 5M** (no HTTPS support - two-step process):
```bash
# On your computer (replace vX.Y.Z with actual version):
VERSION=vX.Y.Z  # Check latest at https://github.com/prestonbrown/helixscreen/releases/latest
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-ad5m-${VERSION}.tar.gz"
# Windows users: use WSL, WinSCP (SCP protocol), or PuTTY's pscp instead of scp -O
scp -O helixscreen-ad5m-${VERSION}.tar.gz root@<printer-ip>:/data/

# On the printer (use the bundled install.sh - no need to download it again):
# Forge-X:
/opt/helixscreen/install.sh --local /data/helixscreen-ad5m-*.tar.gz --update
# Klipper Mod:
/root/printer_software/helixscreen/install.sh --local /data/helixscreen-ad5m-*.tar.gz --update
```

This preserves your configuration and updates to the latest version.

### Update to Specific Version

**Raspberry Pi / Creality K1:**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --update --version v1.2.0
```

**Flashforge Adventurer 5M:** Download the specific version tarball from [GitHub Releases](https://github.com/prestonbrown/helixscreen/releases), then use `--local` as shown above.

### Preserving Configuration

The update process preserves your `helixconfig.json` settings. If you want to reset to defaults:

```bash
# Use your actual install path (~/helixscreen or /opt/helixscreen)
sudo rm ~/helixscreen/config/helixconfig.json
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
type: zip
channel: stable
repo: prestonbrown/helixscreen
path: ~/helixscreen
managed_services: helixscreen
persistent_files:
    config/helixconfig.json
    config/.disabled_services
```

Then restart Moonraker:
```bash
sudo systemctl restart moonraker
```

---

## Uninstalling

### Using Install Script (Recommended)

The install script with `--uninstall` removes HelixScreen and **restores your previous UI** (GuppyScreen, KlipperScreen, etc.):

**Raspberry Pi / Creality K1:**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall
```

**Flashforge Adventurer 5M** (use the bundled install.sh):
```bash
# Forge-X:
/opt/helixscreen/install.sh --uninstall
# Klipper Mod:
/root/printer_software/helixscreen/install.sh --uninstall
```

### Manual Uninstall

<details>
<summary>MainsailOS</summary>

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

<details>
<summary>AD5M Forge-X</summary>

```bash
# Stop and remove service
/etc/init.d/S90helixscreen stop
rm /etc/init.d/S90helixscreen

# Remove files
rm -rf /opt/helixscreen

# Re-enable GuppyScreen
chmod +x /opt/config/mod/.root/S80guppyscreen 2>/dev/null || true
chmod +x /opt/config/mod/.root/S35tslib 2>/dev/null || true

# Restore stock Flashforge UI in auto_run.sh (if it was disabled)
sed -i 's|^# Disabled by HelixScreen: /opt/PROGRAM/ffstartup-arm|/opt/PROGRAM/ffstartup-arm|' /opt/auto_run.sh 2>/dev/null || true

# Remove HelixScreen patch from screen.sh (restores backlight control)
# The automated uninstaller handles this; for manual removal, edit:
# /opt/config/mod/.shell/screen.sh and remove the helixscreen_active check

# Reboot to restore GuppyScreen
reboot
```

> **Note:** The automated uninstaller (`install.sh --uninstall`) handles all ForgeX restoration automatically, including unpatching `screen.sh`.
</details>

<details>
<summary>AD5M Klipper Mod</summary>

```bash
# Stop and remove service
/etc/init.d/S80helixscreen stop
rm /etc/init.d/S80helixscreen

# Remove files
rm -rf /root/printer_software/helixscreen

# Re-enable KlipperScreen
chmod +x /etc/init.d/S80klipperscreen 2>/dev/null || true

# Reboot to restore KlipperScreen
reboot
```
</details>

<details>
<summary>K1/Simple AF</summary>

```bash
# Stop and remove service
/etc/init.d/S99helixscreen stop
rm /etc/init.d/S99helixscreen

# Remove files
rm -rf /usr/data/helixscreen

# Re-enable GuppyScreen
chmod +x /etc/init.d/S99guppyscreen 2>/dev/null || true

# Reboot to restore GuppyScreen
reboot
```
</details>

---

## Getting Help

### Check Logs First

Most issues are diagnosed from the logs:

**MainsailOS (systemd):**
```bash
# View recent logs
sudo journalctl -u helixscreen -n 100

# Follow live logs
sudo journalctl -u helixscreen -f

# Filter by error/warning level
sudo journalctl -u helixscreen -p err
```

**AD5M / K1 (SysV init):**
```bash
# View log file
cat /tmp/helixscreen.log

# Follow live logs
tail -f /tmp/helixscreen.log
```

### Common Issues

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for solutions to:
- Connection problems
- Display issues
- Touch not responding
- Configuration errors

### Still Stuck?

1. Ask in the [HelixScreen Discord](https://discord.gg/RZCT2StKhr) for quick help
2. Check [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) for known problems
3. Open a new issue with:
   - Your hardware (Pi model, display type)
   - HelixScreen version
   - Relevant log output
   - Steps to reproduce

---

## Platform-Specific Notes

### Raspberry Pi 5

Pi 5 has multiple DRM devices. HelixScreen auto-detects the correct one, but if you have issues:

```json
// helixconfig.json
{
  "display": {
    "drm_device": "/dev/dri/card1"
  }
}
```

Common Pi 5 DRM devices:
- `/dev/dri/card0` - v3d (3D acceleration only, no display)
- `/dev/dri/card1` - DSI touchscreen (if connected)
- `/dev/dri/card2` - HDMI output

### Low Memory Systems (Pi 3, Pi Zero 2 W)

HelixScreen is optimized for low memory, but if you experience issues:

1. Disable other services that aren't needed
2. Reduce Moonraker's cache size
3. Consider a lighter Mainsail configuration

### Flashforge Adventurer 5M Memory Constraints

The AD5M has limited RAM (~108MB total, with only ~24MB free after Klipper, Moonraker, and screen UI). HelixScreen is built with static linking and memory optimization for this environment.

**Measured memory comparison (VmRSS):**
| Component | KlipperScreen | HelixScreen |
|-----------|---------------|-------------|
| Screen UI | ~50 MB (Python + X Server) | **~10 MB** (C++) |
| **Total** | ~50 MB | **~10 MB** |

On Klipper Mod systems, switching from KlipperScreen to HelixScreen frees approximately **40 MB** of RAM - a significant improvement on a memory-constrained device!

> **Note:** The 10 MB footprint includes the full LVGL widget tree, draw buffers for UI elements (gradients, color pickers, AMS spool icons), and runtime state for all panels. Images are loaded on-demand, not pre-cached.

If you experience memory issues:
- Reduce print history retention in Moonraker
- Avoid keeping many G-code files on the printer
- Consider disabling the camera stream if not needed

---

*Next: [User Guide](USER_GUIDE.md) - Learn how to use HelixScreen*
