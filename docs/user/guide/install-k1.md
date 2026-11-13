# Installing HelixScreen on the Creality K1 Series

How to put HelixScreen on the built-in screen of a Creality K1, K1C or K1 Max: get root access, add community firmware that brings Moonraker, install HelixScreen, then update or remove it later. HelixScreen replaces the printer's screen program only. Creality's backend services keep running, so Creality Print and Creality Cloud still reach the printer.

**Time required:** about 20 minutes, most of it the community firmware install.

---

## What You Need

- A K1, K1C or K1 Max on your network, with firmware **1.3.3.5 or newer** (check it on the printer's screen under **Settings**). Older firmware has to be upgraded first; see [Firmware Upgrade Path](#firmware-upgrade-path).
- A computer on the same network with an SSH client (Terminal on macOS and Linux, PowerShell or PuTTY on Windows).
- The printer's IP address. It is on the printer's screen under **Settings > Network**, or in your router's list of connected devices.

## Two Ways to Install

- **The [HelixScreen easy installer](https://helixscreen.org/installer/)**, a desktop app. Today it installs on a K1 that already runs Simple AF or is rooted with Moonraker, and it updates an existing install. The stock-firmware path (it turns on root access and installs Simple AF for you) is coming; until it ships, use the steps on this page. See [With the Easy Installer](#with-the-easy-installer).
- **By hand over SSH**, Steps 1 to 4 below. This works on every K1.

---

## Step 1: Turn On Root Access

On the printer's touchscreen:

1. Go to **Settings > Root account information**.
2. Read the notice, wait for the 30-second countdown, then confirm.

Root access is now on.

> **No "Root account information" entry?** Some 2025 K1C units ship without it. Those need a community rooting method first; the Simple AF and Guilouz project pages (Step 3) describe the current one.

## Step 2: Sign In Over SSH

From your computer:

```bash
ssh root@<PRINTER_IP>
```

The password is `creality_2023`.

## Step 3: Install Community Firmware

HelixScreen needs Moonraker, the Klipper API server, and Creality's stock firmware does not ship it. Install one of these community firmwares. If one is already on your printer, skip to Step 4.

### Option A: Simple AF (recommended)

[Simple AF](https://github.com/pellcorp/creality) installs Klipper, Moonraker and a web interface. It installs only on a printer fresh from a factory reset.

```bash
cd /usr/data
git clone https://github.com/pellcorp/creality.git pellcorp
cd pellcorp
sh installer.sh
```

Follow its prompts. When its own screen (GuppyScreen) appears on the touchscreen, go on to Step 4.

### Option B: Guilouz Helper Script

```bash
git clone --depth 1 https://github.com/Guilouz/Creality-Helper-Script.git /usr/data/helper-script
sh /usr/data/helper-script/helper.sh
```

From its menu, install **Moonraker** at least. Fluidd or Mainsail are optional and give you a web interface.

> **`git clone` fails with an SSL error?** Run `git config --global http.sslVerify false` first, then try again. This turns off certificate checking for git on the printer.

## Step 4: Install HelixScreen

### Option A: Direct install (the printer has internet access)

Still signed in over SSH, run:

```bash
wget -O - http://dl.helixscreen.org/install.sh | sh
```

The K1's `wget` can't download over HTTPS, so this address uses plain HTTP. The installer finds the right release on its own.

### Option B: Two-step install (no internet on the printer)

On your computer, open the [latest release page](https://github.com/prestonbrown/helixscreen/releases/latest) and download `helixscreen-k1.zip` and `install.sh` from its Assets. Then copy both to the printer and run the installer:

```bash
scp helixscreen-k1.zip install.sh root@<PRINTER_IP>:/usr/data/
ssh root@<PRINTER_IP>
sh /usr/data/install.sh --local /usr/data/helixscreen-k1.zip
```

### What the installer does

- Recognizes the K1 and which community firmware it runs.
- Stops the screen program that was running (Creality's own screen, or GuppyScreen) and keeps it from starting at boot.
- Keeps Creality's backend services running and starts them at every boot, so Creality Print and Creality Cloud keep working. On Simple AF those services were already removed by Simple AF.
- Keeps SSH working: on stock firmware, SSH was started by the screen program the installer turns off, so the installer starts it on its own.
- Installs HelixScreen to `/usr/data/helixscreen` with the boot service `/etc/init.d/S99helixscreen`, and adds HelixScreen to Moonraker's update manager so Fluidd or Mainsail can update it.

The installer warns if Moonraker does not seem to be running. If you see that warning, finish Step 3 first.

---

## With the Easy Installer

The [easy installer](https://helixscreen.org/installer/) recognizes the model by the name the printer reports (`CR-K1`, `K1C` or `CR-K1 Max`) and stops on anything else. It signs in as `root` with the password `creality_2023` (if you changed it, type yours when asked) and picks a path:

- **Moonraker is already running** (Simple AF or the Helper Script): it leaves your firmware mod alone and installs HelixScreen into `/usr/data/helixscreen`, exactly as Step 4 does.
- **HelixScreen is already installed:** it offers an update on the channel you are on, and never installs an older version over a newer one.
- **Stock Creality software** (once that path ships): it asks you to turn on root access (Step 1), checks the model, the firmware version and that the printer is fresh from a factory reset, installs Simple AF, then installs HelixScreen. Simple AF's install is the long step; do not switch the printer off while it runs.

| What it says | What to do |
|--------------|------------|
| No root account entry in Settings | Your unit needs a community rooting method first (see Step 1) |
| Firmware older than 1.3.3.5 | Update the printer first; see [Firmware Upgrade Path](#firmware-upgrade-path) |
| Something from an earlier mod is on the printer | Factory reset the printer from its Settings, then run the installer again |
| Another model than K1, K1C or K1 Max | This page does not cover it; see [Supported Printers](supported-printers.md) |
| It can't sign in | Check that root access is on (Step 1), and the password |

---

## Getting Started on a K1

HelixScreen starts by itself when the install finishes; a reboot is sometimes needed before the screen changes. The first time, a short setup wizard runs:

1. **Language**, if none is set yet.
2. **Network:** pick a Wi-Fi network, or continue if the printer is already connected by Wi-Fi or Ethernet.
3. **Connection:** HelixScreen connects to the printer's own Moonraker by itself and skips this step. If it can't reach it yet, the step appears: the address `127.0.0.1` and port `7125` are right, so tap **Test Connection**, then continue.
4. **Printer Identification:** confirm your model (the K1, K1C and K1 Max are all in HelixScreen's printer list). HelixScreen then fills in the heaters, fans and sensors for you and skips those steps.
5. **Preferences:** a few choices, such as sounds, that you can change later in Settings.
6. **Help Improve HelixScreen:** choose whether to send anonymous usage statistics. You can change this later in **Settings > System**.

Then the Home screen appears. Worth doing first:

- **Check the version** in **Settings > Help & About > About**, and pick your update channel in **Settings > Updates**.
- **Run a bed mesh** from **Controls > Bed Mesh** (see [Calibration](calibration.md#bed-mesh)).
- **Have a CFS?** HelixScreen shows it on the Filament screen; see [Filament Management](filament.md).

---

## Managing HelixScreen

```bash
/etc/init.d/S99helixscreen start      # or stop, restart, status
/usr/data/helixscreen/bin/helix-screen --version
```

Logs, which you need when reporting a problem:

```bash
tail -100 /usr/data/helixscreen-state/logs/helix.log   # the app's log
tail -100 /usr/data/helixscreen/logs/launcher.log      # startup and crash output
```

The app's log is kept outside the HelixScreen folder, so an update does not delete it.

## Updating

- **On the touchscreen:** **Settings > Updates**. See [Updates](settings/updates.md).
- **From Fluidd or Mainsail**, if you have one: HelixScreen is listed in the update manager.
- **Over SSH**, with the installer HelixScreen keeps in its own folder:

  ```bash
  sh /usr/data/helixscreen/install.sh --update
  ```

  Without internet on the printer, copy a newer `helixscreen-k1.zip` to `/usr/data/` as in Step 4 Option B, then run `sh /usr/data/helixscreen/install.sh --local /usr/data/helixscreen-k1.zip --update`.

Your settings are kept. See [Upgrading](../UPGRADING.md) for specific versions and fresh-settings reinstalls.

## Uninstalling

```bash
cp /usr/data/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
```

The installer will not uninstall from inside its own folder, so it is copied to `/tmp` first. This removes HelixScreen and brings back the screen program the printer had before: Creality's own screen, or GuppyScreen.

The easy installer cannot remove HelixScreen yet; use the command above.

**Back to Creality's own software:** factory reset the printer from its Settings, or flash Creality's recovery firmware (see below). A factory reset turns root access off again, so repeat Step 1 before installing anything else.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Can't sign in over SSH | Make sure root access is on (Step 1). The password is `creality_2023` |
| `wget` or `curl` fails with an SSL error | The K1 can't download over HTTPS. Use the `http://` address in Step 4, or the two-step install |
| The installer says Moonraker "does not appear to be running" | Finish Step 3 first |
| Blank screen after installing | Reboot once. If it stays blank, collect both logs (see [Managing HelixScreen](#managing-helixscreen)) and `logread \| grep helix-screen \| tail -100` |
| Touch does not respond | Reboot the printer |
| Creality Print can't find the printer | Update HelixScreen and reboot; see [Troubleshooting](../TROUBLESHOOTING.md#creality-print-can-no-longer-find-or-connect-to-the-printer) |

More help: [Troubleshooting Guide](../TROUBLESHOOTING.md), [Discord](https://discord.gg/RZCT2StKhr), [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues).

---

## Firmware Upgrade Path

Very old K1 firmware has to be upgraded in steps:

| Starting version | Upgrade through |
|------------------|-----------------|
| 1.2.9.14 or earlier | 1.2.9.15, 1.2.9.22, 1.3.0.30, 1.3.1.4, then the latest |
| 1.2.9.17 to 1.2.9.21 | 1.2.9.22, 1.3.0.30, 1.3.1.4, then the latest |
| 1.3.0.30 | 1.3.1.4, then the latest |
| 1.3.1.4 or newer | The latest directly |

Download each firmware from Creality's site, put the `.img` file on a FAT32 USB drive, insert it into the printer and follow the prompts on its screen. Factory reset the printer from its Settings before installing community firmware, then turn root access on again (Step 1).

## See Also

- [Supported Printers: Creality K1](supported-printers.md#creality-k1--k1c--k1-max--k1-se): what HelixScreen does on the K1, including the CFS
- [Installation overview](../INSTALL.md): all other printers
