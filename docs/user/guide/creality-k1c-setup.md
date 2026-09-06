# Creality K1C Setup Guide

Get HelixScreen running on your Creality K1C (also works for K1 and K1 Max).

**Time required:** ~20 minutes

---

## What You Need

- Creality K1C with firmware **v1.3.3.5 or later** (check: Settings on the touchscreen)
- K1C connected to your WiFi network
- A computer on the same network with an SSH client

---

## Step 1: Enable Root Access

On the K1C touchscreen:

1. Go to **Settings** > **Root account information**
2. Read the disclaimer, **wait 30 seconds**, then confirm

That's it. Root is now enabled.

> **Newer K1C units (2025 hardware revision):** Some recent K1C units ship without the **Root Account Information** option in the Settings menu. If you don't see it, your unit needs a community rooting workaround — check the Simple AF or Guilouz project pages (Step 3) for the current method.

---

## Step 2: SSH Into the Printer

From your computer:

```bash
ssh root@<PRINTER_IP>
# Password: creality_2023
```

Find the IP address on the K1C touchscreen under Settings > Network, or check your router's DHCP client list.

---

## Step 3: Install Simple AF (Recommended) or Guilouz Helper Script

HelixScreen needs Moonraker (the Klipper API server) to be running on your K1C. Stock firmware doesn't include it, so you need community firmware.

### Option A: Simple AF (Recommended)

[Simple AF](https://github.com/pellcorp/creality) is the easiest path and has first-class HelixScreen support.

```bash
cd /usr/data
git clone https://github.com/pellcorp/creality.git pellcorp
cd pellcorp
sh installer.sh
```

Follow the prompts. This installs Klipper, Moonraker, and GuppyScreen. Once it's working (GuppyScreen appears on the touchscreen), proceed to Step 4.

### Option B: Guilouz Helper Script

If you prefer the [Guilouz Helper Script](https://github.com/Guilouz/Creality-Helper-Script-Wiki):

```bash
git clone --depth 1 https://github.com/Guilouz/Creality-Helper-Script.git /usr/data/helper-script
sh /usr/data/helper-script/helper.sh
```

From the menu, install **Moonraker** at minimum. Optionally install Fluidd or Mainsail for web access.

> **Note:** If git clone fails with an SSL error, run `git config --global http.sslVerify false` first.

---

## Step 4: Install HelixScreen

### Option A: Direct Install (Recommended)

If your K1C has internet access, install directly on the printer via SSH:

```bash
wget -O - http://dl.helixscreen.org/install.sh | sh
```

This works because `dl.helixscreen.org` serves over plain HTTP, which BusyBox wget supports. No need to download anything on your computer first.

### Option B: Two-Step Install

If your printer doesn't have internet access, download on your computer first, then copy to the printer.

**On your computer:**

Go to the [latest release page](https://github.com/prestonbrown/helixscreen/releases/latest) and download:
- `helixscreen-k1.zip` (the K1 release archive)
- `install.sh` (the installer script, under "Assets")

Or use the command line (replace `vX.Y.Z` with the actual version):
```bash
VERSION=vX.Y.Z  # Check latest at https://github.com/prestonbrown/helixscreen/releases/latest
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-k1.zip"
wget https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh
```

**Copy to the printer and install:**

```bash
scp helixscreen-k1.zip install.sh root@<PRINTER_IP>:/usr/data/
ssh root@<PRINTER_IP>    # password: creality_2023
sh /usr/data/install.sh --local /usr/data/helixscreen-k1.zip
```

The installer automatically:
- Detects your K1C platform
- Stops GuppyScreen (or whatever screen UI is running)
- Installs to `/usr/data/helixscreen/`
- Creates the boot service (`/etc/init.d/S99helixscreen`)
- Configures Moonraker update manager for easy future updates

---

> **Installing HelixScreen stops Creality Print from reaching the printer.**
> HelixScreen and the stock Creality UI cannot share the framebuffer, so the
> installer stops the stock UI. On the K1 the same init script also runs
> `master-server`, `app-server` and `web-server`, which are the backend Creality
> Print and the Creality Cloud app connect to, so those stop too.
>
> Sending prints still works through Fluidd or Mainsail, HelixScreen's own file
> browser, or any slicer that uploads to Moonraker (OrcaSlicer, or PrusaSlicer
> with the Moonraker plugin). Creality Print can still slice — it just cannot
> upload over the network. To get the stock network stack back, uninstall
> HelixScreen (see [Uninstalling](#uninstalling)).

## Step 5: Complete Setup on the Touchscreen

HelixScreen starts automatically after install. The on-screen wizard walks you through:

1. **Language** selection
2. **Moonraker connection** — should auto-detect on `localhost:7125`
3. **Printer identification** — the K1C is in the printer database
4. **Hardware discovery** — heaters, fans, sensors, LEDs

---

## Managing HelixScreen

```bash
# Start/stop/restart
/etc/init.d/S99helixscreen start
/etc/init.d/S99helixscreen stop
/etc/init.d/S99helixscreen restart

# View logs (K1 BusyBox keeps the app log in RAM; launcher capture is on disk)
logread | grep helix-screen | tail -100              # structured app log
tail -100 /usr/data/helixscreen/logs/launcher.log    # launcher / crash capture
tail -f /usr/data/helixscreen/logs/launcher.log      # live follow launcher
# Pre-v0.99.62 installs only: tail -f /tmp/helixscreen.log
```

---

## Updating

### From Fluidd/Mainsail (easiest)

If you have Fluidd or Mainsail installed, HelixScreen appears in the update manager. One-click update.

### From SSH

```bash
# On your computer (replace vX.Y.Z with actual version):
VERSION=vX.Y.Z
wget "https://github.com/prestonbrown/helixscreen/releases/download/${VERSION}/helixscreen-k1.zip"
scp helixscreen-k1.zip root@<PRINTER_IP>:/usr/data/

# On the printer (use the bundled install.sh - no need to download it again):
/usr/data/helixscreen/install.sh --local /usr/data/helixscreen-k1.zip --update
```

---

## Uninstalling

```bash
/usr/data/helixscreen/install.sh --uninstall
```

This restores GuppyScreen automatically.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Can't SSH in | Make sure root is enabled (Step 1). Password is `creality_2023` |
| `curl` or SSL error | K1 doesn't support HTTPS downloads. Use the two-step install (Step 4) — download on your computer, then `scp` to the printer |
| Installer says "Moonraker not found" | Complete Step 3 first — Moonraker must be running |
| Blank screen after install | Check logs: `logread \| grep helix-screen \| tail -100` and `tail -100 /usr/data/helixscreen/logs/launcher.log` |
| Touch not responding | Reboot: `reboot` |
| Creality Print can't find the printer after install | Expected — the stock Creality backend is stopped so HelixScreen can use the screen. Use Fluidd/Mainsail, HelixScreen's file browser, or a Moonraker-capable slicer. To revert, uninstall HelixScreen. See [Troubleshooting](../TROUBLESHOOTING.md#creality-k1-series-issues) |

For more help: [Troubleshooting Guide](../TROUBLESHOOTING.md) | [Discord](https://discord.gg/RZCT2StKhr) | [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues)

---

## Firmware Upgrade Path

If your K1C is on older firmware, you must upgrade in order:

| Starting Version | Upgrade To |
|-----------------|------------|
| v1.2.9.14 or earlier | v1.2.9.15 → v1.2.9.22 → v1.3.0.30 → v1.3.1.4 → latest |
| v1.2.9.17 – v1.2.9.21 | v1.2.9.22 → v1.3.0.30 → v1.3.1.4 → latest |
| v1.3.0.30 | v1.3.1.4 → latest |
| v1.3.1.4+ | Latest directly |

Download firmware from Creality's site, put the `.img` on a FAT32 USB drive, insert into the printer, and follow the on-screen prompts. **Factory reset from Settings before installing rooted firmware.**

> **After factory reset**, root access must be re-enabled (Step 1) and community firmware reinstalled (Step 3).
