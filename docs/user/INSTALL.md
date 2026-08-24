# HelixScreen Installation Guide

This guide walks you through installing HelixScreen on your 3D printer's touchscreen display.

**Target Audience:** Klipper users who want to use pre-built packages. If you're a developer building from source, see [DEVELOPMENT.md](../devel/DEVELOPMENT.md).

---

## Table of Contents

- [Quick Start](#quick-start)
- [Remote Screen Setup (Run on a Separate Device)](#remote-screen-setup-run-on-a-separate-device)
- [Android App (Experimental)](#android-app-experimental)
- [Prerequisites](#prerequisites)
- [MainsailOS Installation](#raspberry-pi--mainsailos-installation)
- [Flashforge Adventurer 5M Installation](#flashforge-adventurer-5m-installation)
- [Creality K1 Installation](#creality-k1-series)
- [Creality K2 Series](#creality-k2-series)
- [FlashForge Adventurer 5X](#flashforge-adventurer-5x)
- [Elegoo Centauri Carbon 1](#elegoo-centauri-carbon)
- [Creality Sonic Pad](#creality-sonic-pad)
- [Snapmaker U1](#snapmaker-u1)
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
> SSH into your Raspberry Pi, BTT CB1/CB2/Manta, or similar host. For all-in-one printers (Creality K1, K2 series, Flashforge Adventurer 5M/Pro), SSH directly into the printer itself as root.

**Raspberry Pi (MainsailOS):**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer automatically detects your platform and downloads the correct release.

**Creality K1/K1C/K1 Max:** Run directly on the printer via SSH:
```bash
wget -O - http://dl.helixscreen.org/install.sh | sh
```
No SSL required — uses plain HTTP. See [Creality K1 Series](#creality-k1-series) for details.

**Flashforge Adventurer 5M:** The easiest option is our [ready-made firmware image](https://github.com/prestonbrown/ff5m) — just flash from a USB drive. For manual installation on existing Forge-X or Klipper Mod setups, see [Flashforge Adventurer 5M Installation](#flashforge-adventurer-5m-installation).

**Flashforge Adventurer 5X:** Install [ZMOD](https://github.com/ghzserg/zmod), which manages HelixScreen installation and updates. See [FlashForge Adventurer 5X](#flashforge-adventurer-5x).

**Snapmaker U1:** Run directly on the printer via SSH (stock firmware with **Root access** enabled, or [PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) which enables SSH by default):
```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh
```
See [Snapmaker U1](#snapmaker-u1) for details.

> **Note:** Both `bash` and `sh` work. The installer is POSIX-compatible for BusyBox environments.

**KIAUH users:** HelixScreen is available as a KIAUH extension! Run `kiauh` and find HelixScreen in the extensions menu, or use the one-liner above. See [scripts/kiauh/](https://github.com/prestonbrown/helixscreen/tree/main/scripts/kiauh) for details.

> **Pre-flight checks:** On AD5M and K1, the installer validates that Klipper and Moonraker are running before proceeding. If either is missing, you'll get a clear error message explaining what's needed.

After installation, the setup wizard will guide you through initial configuration.

> **Upgrading from an older version?** If HelixScreen keeps showing the setup wizard after an update, see [UPGRADING.md](UPGRADING.md) for how to fix configuration issues.

---

## Remote Screen Setup (Run on a Separate Device)

HelixScreen does **not** have to run on your printer. You can install it on any supported Linux device and have it drive a display while it talks to your printer's Moonraker over the network. This is ideal when the printer sits on the floor and you want the screen at your desk, or when your printer's stock panel can't be replaced (some QIDI models).

Common setups:
- A spare Raspberry Pi (3/4/5, Zero 2 W, CM4) with a touchscreen, sitting at your desk
- A mini PC or x86 box with an HDMI touchscreen
- Your desktop, running the app in a window (macOS or Linux) for monitoring

**How it works:** HelixScreen is a Moonraker client. It only needs network access to your printer's Moonraker instance (port `7125` by default) — it does **not** need to run on the same machine as Klipper.

**Steps:**

1. Install HelixScreen on the device that will drive the display, using the [Quick Start](#quick-start) one-liner or the platform section that matches that device (e.g. a Raspberry Pi uses the [MainsailOS](#raspberry-pi--mainsailos-installation) steps). Install it on the *screen* device, not the printer.
2. Make sure the device is on the same network as your printer and can reach it — from the device, `ping <printer-ip>` should succeed.
3. On first boot, the setup wizard reaches [Step 4: Moonraker Connection](#step-4-moonraker-connection). Enter your **printer's IP address** (not `localhost`), for example `192.168.1.50`. Leave the port at the default `7125` unless you've changed it.
4. The wizard tests the connection, then discovers your printer's capabilities as usual.

> **Point it at Moonraker, not Mainsail/Fluidd.** HelixScreen connects to Moonraker's API (port `7125`), not the Mainsail/Fluidd web interface. You do not need Mainsail or Fluidd installed on the screen device at all.

To change the host later, go to **Settings > System > Host**, or edit `moonraker_host` in `settings.json`.

> **Note:** A remote screen controls the printer the same as an on-printer screen would. Features that require running *on the printer* (for example, HelixScreen taking over the printer's own physical panel, or on-device WiFi configuration in the wizard) don't apply to a remote install — but all printing, monitoring, and control features work normally.

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
- **It cannot do the printer-side things.** Anything that requires running *on* the printer, like taking over the printer's own panel or configuring the printer's WiFi during the wizard, does not apply here. Printing, monitoring, and control all work normally.
- **Power controls are hidden.** An Android app can't power off or reboot the device it runs on, so the shutdown widget never appears on the Home panel (it shows as "Not available on Android" in the widget catalog) and the **POWER** section of the Advanced panel is hidden too. Moonraker power *devices* (smart plugs and the like) are unaffected.

**Coming to Google Play.** Play Store distribution is in progress. When it lands, installs from Play will be signed differently from these APKs, which means moving from a sideloaded install to the Play version will require uninstalling first and setting the app up again. Sideloading will keep working either way.

---

## Prerequisites

### MainsailOS (Raspberry Pi)

This covers any Klipper printer with a Raspberry Pi running MainsailOS (or similar), including SOVOL SV06, SOVOL SV08, Voron, RatRig, and other printers where Klipper runs on a separate Pi. Also works on x86 Linux PCs (e.g., mini ITX) running Debian/Ubuntu with Klipper and a touchscreen.

- **Hardware:**
  - Raspberry Pi 3, 4, or 5 — any of them work. Pi 3 / Zero 2 W is plenty for HelixScreen; Pi 4/5 only matters if your overall Klipper setup wants more headroom for cameras, slicing, etc.
  - Both **64-bit** and **32-bit** Raspberry Pi OS / MainsailOS supported
  - Touchscreen display (HDMI, DSI, or SPI)
  - Network connection (Ethernet or WiFi)

- **Software:**
  - MainsailOS installed and working
  - Klipper running and printing works via Mainsail web interface
  - SSH access to your Pi
  - **Debian 11 (Bullseye) or newer** — glibc 2.31+. See the OS version note below.
  - About 100MB free disk space

> **32-bit vs 64-bit:** The installer automatically detects your OS architecture and downloads the correct binary. If you're unsure which you have, run `uname -m` — `aarch64` means 64-bit, `armv7l` means 32-bit.

> **OS version — Bullseye or newer.** The `pi` and `pi32` packages are dynamically linked
> against **glibc 2.31**, the version in Debian 11 (Bullseye). They will not start on an
> older release. Debian 10 (Buster) ships glibc 2.28, which is too old — the binary fails
> at load with `version 'GLIBC_2.29' not found` or similar. Check yours with:
>
> ```bash
> ldd --version | head -1        # glibc version
> cat /etc/os-release | head -2  # distro release
> ```
>
> This matters mainly on **stock printer images**, several of which still ship Buster even
> though current Raspberry Pi OS and MainsailOS are well past it. If you are on one of
> those and cannot upgrade the OS, install the **`cc1` package instead** — it is statically
> linked and carries its own C library, so it runs on old armv7 systems regardless of what
> glibc they have. It has been used successfully this way on non-Creality armv7 hardware
> (e.g. Rockchip RV1126 boards). See [TROUBLESHOOTING.md](TROUBLESHOOTING.md#binary-wont-start-glibc-version-not-found).

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

**Memory Savings:** On Klipper Mod, HelixScreen (~15MB) replaces KlipperScreen (~50MB), freeing ~35MB RAM on the memory-constrained AD5M.

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

### Creality K1 Series

Creality K1, K1C, and K1 Max. Requires rooting and community firmware (for Moonraker).

See the **[Creality K1C Setup Guide](guide/creality-k1c-setup.md)** for complete instructions — covers rooting, firmware options, and HelixScreen installation.

**Quick version** (if you already have root + Moonraker running):

### One-Liner Install (Recommended)

If your K1 has internet access, install directly on the printer:

```bash
wget -O - http://dl.helixscreen.org/install.sh | sh
```

This works because `dl.helixscreen.org` serves over plain HTTP, which BusyBox wget supports.

### Two-Step Install (No Internet on Printer)

If your printer doesn't have internet access, download on another computer first:

**Step 1: Download on your computer**

Go to the [latest release page](https://github.com/prestonbrown/helixscreen/releases/latest) and download:
- `helixscreen-k1.zip` (the K1 release archive)
- `install.sh` (the installer script, under "Assets")

Or use the command line (replace `vX.Y.Z` with the actual version):
```bash
VERSION=vX.Y.Z  # Check latest at https://github.com/prestonbrown/helixscreen/releases/latest
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-k1.zip"
wget https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh
```

**Step 2: Copy to your printer and install**

```bash
scp helixscreen-k1.zip install.sh root@<printer-ip>:/usr/data/
ssh root@<printer-ip>   # password: creality_2023
sh /usr/data/install.sh --local /usr/data/helixscreen-k1.zip
```

Installs to `/usr/data/helixscreen/`, boot service at `/etc/init.d/S99helixscreen`.

### Creality K2 Series

- **Hardware:**
  - Creality K2, K2 Plus, or K2 Pro
  - Stock 4.3" touchscreen display (480x800)
  - Network connection

- **Software:**
  - Stock firmware with root access enabled (Settings → "Root account information")
  - SSH access (`root@<printer-ip>`, password: `creality_2024`)
  - Moonraker is included in stock firmware on port 4408

**Install:**
```bash
python3 -c "import urllib.request as u;u.urlretrieve('https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh','/tmp/install.sh')" && sh /tmp/install.sh
```

> **Why not `wget`?** Recent K2 firmware (Tina/OpenWrt) ships neither `wget` nor `curl` on the
> `PATH` — even the BusyBox `wget` applet has been compiled out. Every K2 does include `python3`
> (Klipper and Moonraker need it) with working SSL, so the command above uses Python to fetch the
> installer over HTTPS; the installer then uses Python for the rest of the download and extraction.
> If your firmware still has `wget` (older builds did), `wget -O - http://dl.helixscreen.org/install.sh | sh` also works.

**What's different from K1:**
- ARM processor (Allwinner, not MIPS) — standard cross-compilation
- Stock Moonraker — no community firmware required
- OpenWrt-based init system (procd, not SysV)
- CFS (Creality Filament System) support for RS-485 filament management

### FlashForge Adventurer 5X

> **Tested and working.** Prebuilt binaries are included in releases. Installation is handled through the ZMOD firmware modification.

- **Hardware:**
  - FlashForge Adventurer 5X
  - Built-in 4.3" touchscreen (800x480)
  - Network connection

- **Software:**
  - [ZMOD](https://github.com/ghzserg/zmod) firmware modification installed
  - ZMOD provides Klipper, Moonraker, and SSH access

**Current status:**
- Dedicated build target: `ad5x` with its own toolchain and release binary
- Prebuilt `ad5x` binaries are included in GitHub releases
- ZMOD manages installation and updates via Moonraker update manager
- **Auto-detection:** HelixScreen automatically detects ZMOD firmware (by recognizing ZMOD-specific Klipper device names) and applies ZMOD-optimized presets for display, input, and fan configuration. No manual configuration needed.
- IFS (4-channel filament system) supported — see [Filament Management](guide/filament.md)

#### Manual install from the command line (advanced)

Most users never need this — ZMOD handles initial install and ongoing updates through Moonraker's update manager and that path "just works." Use the manual route only if you're pinning a specific version, working from a `--local` zip, or recovering from a failed update.

ZMOD installs HelixScreen into a chroot rooted at `/usr/data/.mod/.zmod/`. When you SSH into the printer you land in the host filesystem, *not* the chroot — so a plain `curl … | sh` writes into the squashfs base view that HelixScreen never sees. The installer detects this and refuses to run with a friendly message; the fix is to enter the chroot first:

```bash
ssh root@<printer-ip>
chroot /usr/data/.mod/.zmod
# now you're in the same view HelixScreen runs from:
curl -fsSL https://releases.helixscreen.org/install.sh | sh
```

The same applies to the uninstaller — run `sh /tmp/install.sh --uninstall` from inside the chroot.

For **upgrades** (including recovery from a failed Mainsail update) see [UPGRADING.md → Adventurer 5X (ZMOD)](UPGRADING.md#quick-upgrade).

### Elegoo Centauri Carbon

> **Tested and working.** Prebuilt binaries ship in releases and the installer has auto-detection support. Requires the community [OpenCentauri COSMOS firmware](https://docs.opencentauri.cc/klipper-conversion/cosmos/cosmos/) — stock Elegoo firmware is not supported (no SSH, no Klipper, no Moonraker).

- **Hardware:**
  - Elegoo Centauri Carbon (4.3" 480×272 touchscreen, Allwinner R528, armv7l)
  - Network connection (WiFi or Ethernet)

- **Software:**
  - [OpenCentauri COSMOS firmware](https://github.com/OpenCentauri/cosmos/releases) installed (replaces stock Elegoo firmware; ships Klipper + Moonraker + grumpyscreen/atomscreen/guppyscreen)
  - SSH access: `root` / default password `OpenCentauri` (change it after install)

#### Step 1: Install COSMOS firmware

OpenCentauri COSMOS is a full firmware replacement for the Centauri Carbon. It ships with Klipper, Moonraker, Mainsail, and a `gui-switcher` that lets you pick which touch UI to run.

1. Download the latest `update.swu` from https://github.com/OpenCentauri/cosmos/releases
2. Copy it to the root of a FAT32-formatted USB stick
3. Insert the USB stick into the printer, power on
4. From the stock Elegoo UI, navigate to the firmware-update menu and apply the update
5. **First boot takes 5–10 minutes** while it reflashes the toolhead and bed boards — be patient
6. After reboot, connect to WiFi from the COSMOS UI and note the printer's IP address

If the update fails or the device won't boot, consult the OpenCentauri [install guide](https://docs.opencentauri.cc/klipper-conversion/cosmos/install/) and [emergency USB recovery](https://docs.opencentauri.cc/software/updates/) docs.

#### Step 2: Install HelixScreen

SSH into the printer (replace `<ip>` with your printer's IP):

```bash
ssh root@<ip>
# Default password: OpenCentauri
```

Then run the installer:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer auto-detects COSMOS, installs HelixScreen to `/user-resource/helixscreen/`, and registers it with `gui-switcher` as the selected touch UI. It stops the currently active UI (grumpyscreen, atomscreen, or guppyscreen) and starts HelixScreen in its place.

#### Step 3: Switch back to another UI (optional)

COSMOS's `config-manager` tool lets you switch between installed UIs without uninstalling HelixScreen:

```bash
config-manager ui screen_ui grumpyscreen   # or atomscreen, guppyscreen, helixscreen
/etc/init.d/gui-switcher restart
```

**Notes:**
- Moonraker on COSMOS listens on port `80` directly (no nginx); HelixScreen's `cc1` preset is configured for this
- Install directory: `/user-resource/helixscreen/` (`/` is read-only squashfs on COSMOS)
- Init script: `/etc/init.d/helixscreen` (LSB-style, PIDFILE=`/var/run/gui.pid` for gui-switcher compatibility)
- The `cc1` preset ships with **factory white-balance calibration** (per-channel panel gain) so colors look neutral out of the box on the Centauri Carbon's 4.3" panel — no manual tuning needed
- COSMOS's `config-manager` has a fixed allowlist for the `screen_ui` slot. The installer handles this automatically via an init-script wrapper so HelixScreen can be selected without patching COSMOS itself; the uninstaller fully reverses it

**If you're testing on this printer**, please report your results via [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) or [Discord](https://discord.gg/RZCT2StKhr).

### Creality Sonic Pad

The Creality Sonic Pad is a standalone 7" touchscreen that can run Klipper. It uses a 32-bit ARM userspace (armhf) despite having a 64-bit capable processor (Allwinner H616).

> **Tested firmware: [SonicPad-Debian](https://github.com/Jpe230/SonicPad-Debian) only.**
> This is the only Sonic Pad firmware HelixScreen has been tested on. It replaces
> Creality's stock OpenWrt image with Debian 11 (bullseye), which is what gives the
> Pad a normal systemd + GNU userspace for HelixScreen to install into.
>
> HelixScreen is **not** tested on Creality's stock Sonic Pad firmware. The stock
> image is a heavily cut-down OpenWrt build, so the installer's assumptions about
> systemd, package tooling, and archive utilities do not hold there. If you are on
> stock firmware, flash SonicPad-Debian first.

HelixScreen requires Klipper and Moonraker to already be installed and working on the Sonic Pad. This is typically done via [KIAUH](https://github.com/dw-0/kiauh) or a similar tool. HelixScreen replaces whatever touchscreen UI you're currently using (e.g., KlipperScreen).

- **Hardware:**
  - Creality Sonic Pad (7" 1024x600 capacitive touchscreen)
  - Network connection (Ethernet)

- **Software:**
  - [SonicPad-Debian](https://github.com/Jpe230/SonicPad-Debian) (Debian 11 bullseye) — see the note above
  - Klipper and Moonraker installed and working (via KIAUH or similar)
  - SSH access (`sonic@<pad-ip>`)
  - About 100MB free disk space

**Installation:**

The standard installer works on Sonic Pad:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer detects the Sonic Pad as a 32-bit ARM platform and downloads the `pi32` release binary. HelixScreen installs to `~/helixscreen/` and runs as a systemd service.

**Notes:**
- The Sonic Pad has a Goodix GT9xx touchscreen controller — the touch calibration wizard runs automatically on first boot if needed
- Moonraker runs on `localhost:7125` (default)
- The `display-sleep` service is automatically stopped to prevent backlight conflicts

### Snapmaker U1

The Snapmaker U1 is an all-in-one printer with a built-in touchscreen. HelixScreen replaces the stock UI and launches automatically on boot.

- **Hardware:**
  - Snapmaker U1
  - Built-in touchscreen display
  - Network connection

- **Software:**
  - **SSH access** — via either firmware path:
    - **Stock Snapmaker firmware (1.2+):** enable the **Root access** option in printer settings (added in V1.2.0). This turns on SSH. *(Stock-firmware support is newly added and not yet verified end-to-end on a stock device — see note below.)*
    - **[PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware):** SSH on by default. Tested on **1.2.x, 1.3.x, and 1.4.x**.
  - SSH login (`root@<printer-ip>` or `lava@<printer-ip>`, password: `snapmaker`)

**Notes:**
- **Reinstall after a firmware update** — any firmware update (stock or PAXX) resets system files and can overwrite HelixScreen; re-run the installer afterward.
- **Remote screen ("gui" camera) works on PAXX firmware** — the built-in "gui" webcam in Mainsail/Fluidd shows the live HelixScreen UI, and you can tap it to control the printer remotely. Enable **Remote Screen** in the firmware settings web UI at `http://<printer-ip>/firmware-config/` — it registers the "gui" webcam and restarts HelixScreen and Moonraker for you (hand-editing the config value alone is not enough). The physical "case" camera is unaffected. Stock firmware is not yet confirmed to expose the feed. Setup steps: [Supported Printers → Snapmaker U1](guide/supported-printers.md#snapmaker-u1-snapswap).
- **Two harmless Moonraker warnings are expected** — after install, the Mainsail/Fluidd "Moonraker warnings found" banner may show *"Unable to find DBus PolKit Interface"* and *"Unable to initialize System Update Provider for distribution: buildroot"*. Both are inherent to Moonraker on the U1's buildroot firmware (no PolKit, no OS package manager) and do **not** affect HelixScreen or printing. They are not specific to HelixScreen — installing simply restarts Moonraker, which re-surfaces them. See [Troubleshooting](TROUBLESHOOTING.md).

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
- `helixscreen-ad5m.zip` (the AD5M release archive)
- `install.sh` (the installer script, under "Assets")

Or use the command line (replace `vX.Y.Z` with the actual version):
```bash
VERSION=vX.Y.Z  # Check latest at https://github.com/prestonbrown/helixscreen/releases/latest
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-ad5m.zip"
wget https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh
```

**Step 2: Copy to your printer**

```bash
# AD5M requires -O flag for scp (BusyBox lacks sftp-server)
# Note: Use /data/ not /tmp/ - AD5M's /tmp is a tiny tmpfs (~54MB)
scp -O helixscreen-ad5m.zip install.sh root@<printer-ip>:/data/
```

> **Windows users:** The `-O` flag is not supported by Windows 11's built-in OpenSSH.
> Use one of these alternatives instead:
> - **WSL** (recommended) — open a WSL terminal and run all commands as shown (Linux tools work natively)
> - **[WinSCP](https://winscp.net/)** (free, GUI) — set the protocol to **SCP**, then drag and drop files to `/data/` on the printer
> - **[PuTTY pscp](https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html)** (free, command-line):
>   `pscp helixscreen-ad5m.zip install.sh root@<printer-ip>:/data/`

**Step 3: SSH into the printer and run the installer**

```bash
# From your local computer, SSH into the printer as root
ssh root@<printer-ip>

# Now on the printer, run the installer
sh /data/install.sh --local /data/helixscreen-ad5m.zip
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
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-ad5m.zip"

# Copy to printer (AD5M requires scp -O for legacy protocol)
# Note: Use /data/ not /tmp/ - AD5M's /tmp is a tiny tmpfs (~54MB)
# Windows users: use WinSCP (SCP protocol) or PuTTY's pscp instead — see note above
scp -O helixscreen-ad5m.zip root@<printer-ip>:/data/

# SSH into printer
ssh root@<printer-ip>

# Extract to /opt (Forge-X location)
cd /opt
unzip -q /data/helixscreen-ad5m.zip

# Stop GuppyScreen
/opt/config/mod/.root/S80guppyscreen stop 2>/dev/null || true
chmod -x /opt/config/mod/.root/S80guppyscreen

# Install init script
cp /opt/helixscreen/config/helixscreen.init /etc/init.d/S90helixscreen
chmod +x /etc/init.d/S90helixscreen

# Start HelixScreen
/etc/init.d/S90helixscreen start

# Clean up
rm /data/helixscreen-ad5m.zip
```

</details>

<details>
<summary>Klipper Mod Manual Installation</summary>

> **Note:** Klipper Mod's `/tmp` is a small tmpfs (~54MB). The package is ~70MB, so we must use `/mnt/data` instead.

```bash
# Download on your computer (replace vX.Y.Z with actual version)
VERSION=vX.Y.Z
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-ad5m.zip"

# Copy to printer's data partition (NOT /tmp - it's too small!)
# Windows users: use WinSCP (SCP protocol) or PuTTY's pscp instead — see note above
scp -O helixscreen-ad5m.zip root@<printer-ip>:/mnt/data/

# SSH into printer
ssh root@<printer-ip>

# Extract to /root/printer_software (Klipper Mod location)
cd /root/printer_software
unzip -q /mnt/data/helixscreen-ad5m.zip

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
rm /mnt/data/helixscreen-ad5m.zip
```

</details>

> **Note:** AD5M runs as root, so `sudo` is not needed.
> **Note:** AD5M uses BusyBox utilities. Use `unzip` to extract `.zip` archives.
> **Note:** AD5M uses SysV init (BusyBox), not systemd.

### Step 4: Reboot

```bash
reboot
```

After reboot, HelixScreen will start automatically on the touchscreen.

### Step 5: Complete Setup

Use the touchscreen to complete the setup wizard. The printer should auto-detect since it's running locally.

---

## Snapmaker U1 Installation

> **Requires SSH access.** Enable it on **stock firmware (1.2+)** via the **Root access** option in printer settings, or install [PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) (SSH on by default). PAXX is **not** required — it's just the turnkey option.
>
> **Firmware versions:** Tested on PAXX Extended Firmware **1.2.x, 1.3.x, and 1.4.x**. Stock-firmware support is newly added and not yet verified end-to-end on a stock device. After any firmware update, **reinstall HelixScreen** — the update resets the printer's system files and the stock screen will return until you reinstall (see [Upgrading the firmware](#upgrading-the-extended-firmware-with-helixscreen-installed)).

SSH into the printer:

```bash
ssh root@<printer-ip>
# or: ssh lava@<printer-ip>
# password: snapmaker
```

### Quick Install (Recommended)

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

The installer automatically detects the Snapmaker U1 and installs to `/userdata/helixscreen/`. It configures autostart so HelixScreen launches instead of the stock UI on boot.

### Manual Install

If you prefer to install manually or the one-liner doesn't work on your network:

**Step 1: Download the release archive**

```bash
wget https://releases.helixscreen.org/stable/helixscreen-snapmaker-u1.zip
```

**Step 2: Extract to the install directory**

```bash
mkdir -p /userdata/helixscreen && unzip -q helixscreen-snapmaker-u1.zip -d /userdata/helixscreen
```

**Step 3: Configure autostart**

```bash
bash /userdata/helixscreen/scripts/snapmaker-u1-setup-autostart.sh /userdata/helixscreen
```

This sets HelixScreen to launch on boot and disables the stock UI program (`/usr/bin/gui`) so HelixScreen owns the screen. (The stock UI program lives in a read-only part of the firmware and is only disabled, never deleted — the uninstaller re-enables it.)

**Step 4: Start HelixScreen**

```bash
killall gui 2>/dev/null; /userdata/helixscreen/bin/helix-launcher.sh &
```

### Reverting to Stock UI

Run the uninstaller — it re-enables the stock UI and removes HelixScreen:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall
reboot
```

If you can't run the uninstaller, revert manually. HelixScreen *disables* the stock UI program rather than deleting it, so re-enable it and remove HelixScreen's files:

```bash
chmod +x /usr/bin/gui          # re-enable the stock UI program
rm -rf /userdata/helixscreen   # remove HelixScreen
reboot
```

**Notes:**
- SSH is required — enable stock firmware's **Root access** option, or use PAXX Extended Firmware (SSH on by default). PAXX is not required.
- Display resolution may need manual configuration if the screen appears stretched or misaligned (see [Display Configuration](#display-configuration))
- A firmware update resets the printer's system files and brings the stock screen back — reinstall HelixScreen afterward

### Upgrading the Extended Firmware with HelixScreen installed

A firmware upgrade is safe to run with HelixScreen installed — it does **not** brick the printer. HelixScreen's files live in a layer that the upgrade clears, so after upgrading you simply **reinstall HelixScreen**.

Because HelixScreen replaces the stock touchscreen, the stock screen's on-device **"Local Update"** button is gone. Upgrade over the network instead:

1. In a web browser on the same network, open **`http://<printer-ip>/firmware-config`**.
2. Choose **Firmware Upgrade**, upload the new `U1_extended_<version>_upgrade.bin`, and let it complete. The printer reboots into the new firmware.
3. The stock screen comes back (HelixScreen was cleared by the upgrade). **Reinstall HelixScreen** with the [Quick Install](#quick-install-recommended) one-liner. Your settings, WiFi, and printer config are preserved (they live on a separate partition the upgrade keeps).

### Recovery: screen is blank or the printer is off the network

If something goes wrong and the printer comes up with a blank screen and is unreachable over WiFi, recover over a wired connection:

1. Plug a **USB-Ethernet adapter** into the printer and connect it to your router. The printer auto-configures the wired link and gets an IP from your router (check the router's client list).
2. SSH in over that wired IP: `ssh root@<wired-ip>` (password `snapmaker`).
3. Run the uninstaller to return to the stock UI, then reboot:
   ```bash
   curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall
   reboot
   ```
4. Once the stock screen is back and the printer is on WiFi again, you can reinstall HelixScreen.

> If the uninstaller can't run, reset HelixScreen's persistence flag and files manually, then reboot: `rm -f /oem/.debug && rm -rf /oem/overlay/* && rm -rf /userdata/helixscreen && sync && reboot`. This returns the printer to a clean stock state.

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

### BTT Pad 7, CB2, and Similar

The BTT Pad 7, CB2, and similar "Klipper Pad" devices typically include:
- Pre-configured display output
- Touch input via USB

HelixScreen should detect and use these automatically.

### Screen Rotation

To rotate the display (e.g., if your screen is mounted upside-down), add to your `settings.json` (typically at `~/helixscreen/config/settings.json`):

```json
{
  "display": {
    "rotate": 180
  }
}
```

Valid values: `0`, `90`, `180`, `270`. Restart HelixScreen after changing.

Touch coordinates are automatically adjusted to match the rotation — no separate touch configuration is needed.

**Rotation and display backends:** When rotation is configured on Raspberry Pi, HelixScreen checks whether your display hardware supports rotating the image directly. Most DSI/HDMI displays on Pi do not support hardware rotation. In that case, HelixScreen automatically switches from the DRM (GPU) backend to the framebuffer backend, which handles software rotation without any screen flicker. This switch is transparent — no manual configuration needed.

If you experience any display issues with rotation, you can also force the framebuffer backend manually by setting `HELIX_DISPLAY_BACKEND=fbdev` (see below).

### GPU Rendering

By default, HelixScreen uses GPU-accelerated rendering via DRM/KMS when available. On boards where DRM is not supported, it falls back to CPU-based software rendering (`fbdev` backend).

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

Valid backends: `drm` (GPU-accelerated), `fbdev` (CPU rendering, maximum compatibility).

**How to revert to auto-detection:**

Remove the override and restart:
```bash
sudo systemctl revert helixscreen
sudo systemctl restart helixscreen
```

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
tail -100 /opt/helixscreen/logs/launcher.log       # launcher / crash capture
grep helix-screen /var/log/messages | tail -100    # structured app log
```

*Klipper Mod:*
```bash
/etc/init.d/S80helixscreen start|stop|restart|status
tail -100 /opt/helixscreen/logs/launcher.log
grep helix-screen /var/log/messages | tail -100
```

**K1/Simple AF (SysV init, BusyBox syslog in RAM):**
```bash
/etc/init.d/S99helixscreen start|stop|restart|status
tail -100 /usr/data/helixscreen/logs/launcher.log    # launcher / crash capture
logread | grep helix-screen | tail -100              # structured app log
```

> If `launcher.log` doesn't exist, you're on a pre-v0.99.62 install; the log lived at `/tmp/helixscreen.log` then.

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
#   K2: /opt/helixscreen/bin/helix-screen
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

**Raspberry Pi:**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --update
```

**Creality K1** (no HTTPS support - two-step process):
```bash
# On your computer (replace vX.Y.Z with actual version):
VERSION=vX.Y.Z  # Check latest at https://github.com/prestonbrown/helixscreen/releases/latest
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-k1.zip"
scp helixscreen-k1.zip root@<printer-ip>:/usr/data/

# On the printer (use the bundled install.sh - no need to download it again):
/usr/data/helixscreen/install.sh --local /usr/data/helixscreen-k1.zip --update
```

**Flashforge Adventurer 5M** (no HTTPS support - two-step process):
```bash
# On your computer (replace vX.Y.Z with actual version):
VERSION=vX.Y.Z  # Check latest at https://github.com/prestonbrown/helixscreen/releases/latest
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-ad5m.zip"
# Windows users: use WSL, WinSCP (SCP protocol), or PuTTY's pscp instead of scp -O
scp -O helixscreen-ad5m.zip root@<printer-ip>:/data/

# On the printer (use the bundled install.sh - no need to download it again):
# Forge-X:
/opt/helixscreen/install.sh --local /data/helixscreen-ad5m.zip --update
# Klipper Mod:
/root/printer_software/helixscreen/install.sh --local /data/helixscreen-ad5m.zip --update
```

This preserves your configuration and updates to the latest version.

### Update to Specific Version

**Raspberry Pi:**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --update --version v1.2.0
```

**Creality K1 / Flashforge Adventurer 5M:** Download the specific version archive from [GitHub Releases](https://github.com/prestonbrown/helixscreen/releases), then use `--local` as shown above.

To reinstall a specific version with a **fresh settings.json** (instead of keeping your existing settings), swap `--update` for `--clean`:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --clean --version v1.2.0
```

### Preserving Configuration

The update process preserves your `settings.json` settings. If you want to reset to defaults, use the `--clean` flag — it removes your HelixScreen settings and caches everywhere they live, then does a fresh install:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --clean
```

`--clean` asks for confirmation before wiping anything. Your Klipper config, Moonraker settings, print history, and G-code files are **not** touched — only HelixScreen's own settings.

To reset settings **and** pin a specific version in one step, combine `--clean` with `--version`:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --clean --version v1.2.0
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
> to this section — these options are not supported with `type: web` and Moonraker will
> log warnings about unparsed config options. Service restart after updates is handled
> automatically by a systemd path unit installed during setup.

Then restart Moonraker:
```bash
sudo systemctl restart moonraker
```

---

## Uninstalling

### Using Install Script (Recommended)

The install script with `--uninstall` removes HelixScreen and **restores your previous UI** (GuppyScreen, KlipperScreen, etc.):

**Raspberry Pi:**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall
```

**Creality K1** (use the bundled install.sh):
```bash
/usr/data/helixscreen/install.sh --uninstall
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

**AD5M / K1 / K2 / AD5X / CC1 / Snapmaker U1 (SysV init):**

There are two log streams; collect both when reporting an issue.

```bash
# 1) Structured app log — where to look depends on your platform's syslog:
#    - BusyBox in-memory syslog (K1, K2, CC1, AD5X):
logread | grep helix-screen | tail -100
#    - Persistent /var/log/messages (AD5M, Snapmaker U1):
grep helix-screen /var/log/messages | tail -100

# 2) Launcher / supervisor capture (startup, crash output) — path varies:
#    - AD5M:           /opt/helixscreen/logs/launcher.log
#    - K1 / K1C / K2 / AD5X: /usr/data/helixscreen/logs/launcher.log
#    - Snapmaker U1:   /var/log/helixscreen/launcher.log
#    - CC1 (COSMOS):   /user-resource/helixscreen/logs/launcher.log
#    - Pre-v0.99.62 installs (legacy): /tmp/helixscreen.log
tail -100 <path>
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
// settings.json
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

### Camera Streaming Performance

If you use a webcam with HelixScreen, install `libturbojpeg0` for faster camera feed rendering:

```bash
sudo apt install libturbojpeg0
```

The installer attempts this automatically, but it's listed here in case your Pi was offline during installation. HelixScreen detects and uses it automatically for 3-5x faster JPEG decoding via hardware SIMD acceleration.

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
