# Installing HelixScreen on the Flashforge Adventurer 5M / 5M Pro

How to run HelixScreen on the built-in 4.3" touchscreen of a Flashforge Adventurer 5M or 5M Pro (AD5M): pick a firmware mod, install HelixScreen, then update or remove it later. HelixScreen replaces the screen your mod uses: Feather, GuppyScreen or the stock screen on Forge-X, KlipperScreen on Klipper Mod, the stock screen on ZMOD.

> **Easy installer:** the [HelixScreen easy installer](https://helixscreen.org/installer/) can update HelixScreen on a Forge-X Adventurer 5M from a desktop app. Installing the firmware mod and a first install are done with this guide.

---

## What You Need

- A Flashforge Adventurer 5M or 5M Pro on your network.
- A firmware mod that gives the printer Klipper, Moonraker and root access. Stock Flashforge firmware cannot run HelixScreen.
- For Forge-X and Klipper Mod: a computer with an SSH client, and about 100 MB free on the printer.

| Firmware mod | How HelixScreen goes on | Notes |
|--------------|-------------------------|-------|
| [Forge-X](https://github.com/DrA1ex/ff5m) (**recommended**) | The HelixScreen installer, over SSH | The setup HelixScreen is tested on most |
| [ZMOD](https://github.com/ghzserg/zmod) | ZMOD turns it on, from the Fluidd or Mainsail console | No SSH needed |
| [Klipper Mod](https://github.com/xblax/flashforge_ad5m_klipper_mod) | The HelixScreen installer, over SSH | Still works, but no longer developed |

## Step 1: Install a Firmware Mod

Each mod installs the way stock firmware updates do: a file on a FAT32 USB stick, inserted before you switch the printer on. Each mod lists the stock firmware versions it supports, so check yours first (on the printer: **Settings > About**).

- **Forge-X** supports stock firmware 2.6.5 through 5.1.x. Follow the [Forge-X install guide](https://github.com/DrA1ex/ff5m/blob/main/docs/INSTALL.md), including its notes on downgrading and the `-Factory` image if your version needs one. Any of Forge-X's display modes is fine; the HelixScreen installer takes the display over from whichever one you use.
- **ZMOD** supports stock firmware 2.7.5 and newer. Follow the [ZMOD install guide](https://wiki.zmod.link/Setup/), then go to [ZMOD: Turn HelixScreen On](#zmod-turn-helixscreen-on).

> **Running the old `prestonbrown/ff5m` image?** It is Forge-X with HelixScreen already installed, and it is no longer maintained. HelixScreen keeps working and updates normally. For newer Forge-X versions, follow [upstream Forge-X](https://github.com/DrA1ex/ff5m)'s update instructions.

Keep a second way into the printer, such as SSH or Fluidd or Mainsail in a browser, before you replace its screen.

---

## ZMOD: Turn HelixScreen On

ZMOD installs and updates HelixScreen itself, through Moonraker's update manager. Open Fluidd or Mainsail in a browser (`http://<printer-ip>`) and use its console:

1. Run `ENABLE_EXTRA_PLUGINS`. This adds ZMOD's list of extra plugins to Moonraker, including a `helixscreen` update-manager entry, and reboots the printer. It does not install HelixScreen yet.
2. When the printer is back, open the update manager in Fluidd or Mainsail and update **helixscreen**. ZMOD lists it at version v0.0.1, so the current release shows as an update; applying it downloads HelixScreen into `/srv/helixscreen`.
3. Run `DISPLAY_OFF HELIX=1`. This turns the stock screen off, selects HelixScreen in its place and reboots. Do this only after step 2: with no HelixScreen files installed, the printer comes up with no screen at all.

From then on, update HelixScreen through the same update manager entry. ZMOD manages HelixScreen, so update and remove it through ZMOD rather than with the installer commands on this page. To go back to the stock screen, run `DISPLAY_ON`.

> **Read ZMOD's warning first.** With the stock screen off, the printer no longer runs the stock screen's print start and end routines, so bed leveling, Z-offset and your slicer's start and end G-code work differently. See ZMOD's [DISPLAY_OFF documentation](https://github.com/ghzserg/zmod/blob/main/docs/en/System.md) before you switch.

---

## Forge-X and Klipper Mod: Install HelixScreen

Sign in to the printer over SSH as `root` (`ssh root@<printer-ip>`). The AD5M runs everything as root, so there is no `sudo`.

### Option A: Direct install (the printer has internet access)

```bash
wget -O - http://dl.helixscreen.org/install.sh | sh
```

The AD5M's `wget` can't download over HTTPS (it answers `not an http or ftp url`), so this address uses plain HTTP. The installer then fetches the right release over plain HTTP as well, on its own.

### Option B: Two-step install

1. On your computer, open the [latest release page](https://github.com/prestonbrown/helixscreen/releases/latest) and download `helixscreen-ad5m.zip` and `install.sh` from its Assets.
2. Copy both to the printer's `/data/` folder (its `/tmp` is too small for the download):

   ```bash
   scp -O helixscreen-ad5m.zip install.sh root@<printer-ip>:/data/
   ```

   The AD5M needs `-O`. Windows 11's built-in `scp` does not support it; use **WSL** and run the command as shown, **[WinSCP](https://winscp.net/)** with the protocol set to **SCP**, or PuTTY's `pscp helixscreen-ad5m.zip install.sh root@<printer-ip>:/data/`.
3. On the printer:

   ```bash
   sh /data/install.sh --local /data/helixscreen-ad5m.zip
   ```

### What the installer does

The installer recognizes the firmware mod and checks that Klipper and Moonraker are running. It shows what it is about to change and asks you to confirm before it changes anything.

**On Forge-X:**

- Switches Forge-X's display mode to `HEADLESS`, so Forge-X starts no screen of its own, and remembers the mode you had (Stock, Feather or Guppy) so uninstalling can put it back.
- Turns off GuppyScreen, its touch layer and the stock Flashforge screen, and answers Forge-X's "Try the new Feather screen" offer so it stops appearing.
- Stops Forge-X's screen settings from dimming the backlight while HelixScreen runs.
- Installs HelixScreen to `/opt/config/mod_data/helixscreen`, next to the Forge-X folder rather than inside it, so a Forge-X update or reset leaves it alone. Forge-X starts it at boot.
- If an older HelixScreen install sits at `/opt/helixscreen`, the installer asks whether to keep using that folder. If you answer no, it installs to the new folder, leaves the old one untouched and prints the commands to move your settings across and remove the old copy.

**On Klipper Mod:**

- Stops Xorg and KlipperScreen and keeps them from starting at boot.
- Installs HelixScreen to `/root/printer_software/helixscreen` (Klipper Mod v00.05 and older) or `/opt/helixscreen` (v00.06 and newer), with the boot service `/etc/init.d/S80helixscreen`.

Uninstalling reverses all of this; see [Uninstalling](#uninstalling).

When the installer finishes, reboot the printer:

```bash
reboot
```

---

## Getting Started on an Adventurer 5M

After the reboot, HelixScreen appears on the touchscreen. The AD5M release already knows this printer's heaters, fans and touchscreen, so the setup wizard is short:

1. **Language**, if none is set yet.
2. **Network:** pick a Wi-Fi network, or continue if the printer is already connected by Wi-Fi or Ethernet.
3. **Connection:** HelixScreen connects to the printer's own Moonraker by itself and skips this step. If it can't reach it yet, the step appears: the address `127.0.0.1` and port `7125` are right, so tap **Test Connection**, then continue.
4. **Preferences:** a few choices, such as sounds, that you can change later in Settings.
5. **Help Improve HelixScreen:** choose whether to send anonymous usage statistics. You can change this later in **Settings > System**.

Then the Home screen appears. Worth doing first:

- **Check the version** in **Settings > Help & About > About**, and pick your update channel in **Settings > Updates**.
- **Run a bed mesh** from **Controls > Bed Mesh** (see [Calibration](calibration.md#bed-mesh)).
- **Expect a quiet start.** HelixScreen plays no startup music on the AD5M: on its single processor core, music has interrupted prints.

---

## Service Control and Logs

**Forge-X:** Forge-X starts HelixScreen at boot, so rebooting the printer restarts it. The service script lives inside Forge-X's own system folder, at `/data/.mod/.forge-x/etc/init.d/S90helixscreen`; there is none in the printer's main `/etc/init.d`. Run it through `chroot` from SSH:

```bash
/usr/sbin/chroot /data/.mod/.forge-x /etc/init.d/S90helixscreen status    # or restart
```

`status` answers `helixscreen is running (PID ...)`. A printer that kept an older install at `/opt/helixscreen` uses `/etc/init.d/S90helixscreen start|stop|restart|status` instead.

**Klipper Mod:**

```bash
/etc/init.d/S80helixscreen start      # or stop, restart, status
```

Two logs; collect both when reporting a problem:

| Log | Where |
|-----|-------|
| The app's log | `/data/.helixscreen/logs/helix.log` (Forge-X and Klipper Mod) |
| Startup and crash output | `logs/launcher.log` inside the HelixScreen folder: `/opt/config/mod_data/helixscreen/logs/launcher.log` on Forge-X |

```bash
tail -100 /data/.helixscreen/logs/helix.log
tail -100 /opt/config/mod_data/helixscreen/logs/launcher.log    # Forge-X
```

HelixScreen keeps its logs and caches in `/data/.helixscreen`. The dot keeps that folder out of the printer's file list. An update from an older version moves the old `/data/helixscreen` folder there on its own.

---

## Updating

**Check your version** on the touchscreen in **Settings > Help & About > About**.

- **On the touchscreen:** **Settings > Updates**. See [Updates](settings/updates.md).
- **Over SSH**, with the installer HelixScreen keeps in its own folder (Forge-X shown; on Klipper Mod use `/root/printer_software/helixscreen` or `/opt/helixscreen`):

  ```bash
  sh /opt/config/mod_data/helixscreen/install.sh --update
  ```

  This works even though the AD5M cannot download over HTTPS: the installer says `HTTPS not available, trying HTTP fallback...` and fetches the release and its checksum over plain HTTP from `dl.helixscreen.org`.

  Without internet on the printer, copy a newer `helixscreen-ad5m.zip` to `/data/` as in Option B, then run the same installer with `--local /data/helixscreen-ad5m.zip --update`.

Your settings are kept. See [Upgrading](../UPGRADING.md) for installing a specific version or reinstalling with fresh settings.

> **Forge-X:** the installer refuses `--auto-update`, which would let Moonraker replace the whole HelixScreen folder and lose your settings. Update with the commands above.

---

## Uninstalling

> **On ZMOD:** run `DISPLAY_ON` to switch back to the stock screen (see [ZMOD: Turn HelixScreen On](#zmod-turn-helixscreen-on)). The commands below are for Forge-X and Klipper Mod.

Copy the installer out of the HelixScreen folder (it will not uninstall from inside it), then run it:

```bash
# Forge-X:
cp /opt/config/mod_data/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
# Klipper Mod (use /opt/helixscreen on v00.06 and newer):
cp /root/printer_software/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
```

On Forge-X this puts back the display mode you had before (Stock, Feather or Guppy), re-enables GuppyScreen and the stock screen, and restores Forge-X's backlight control. On Klipper Mod it brings KlipperScreen back. Reboot afterwards.

---

## Notes

- **Memory.** The AD5M has little RAM (about 108 MB). HelixScreen is built for that, and on Klipper Mod, replacing KlipperScreen and Xorg frees memory for Klipper. If you run short, keep fewer G-code files on the printer, keep less print history in Moonraker, and turn off the camera stream if you don't use it.
- **The screen dims a few seconds after boot.** Forge-X's backlight handling was not switched off. Run the installer again with `--update`; see [Troubleshooting: screen dims after a few seconds](../TROUBLESHOOTING.md#screen-dims-after-a-few-seconds).
- **The installer stops with "ForgeX display state disagrees".** Forge-X's two display settings don't match yet, usually because a display change is waiting for a reboot. Reboot the printer and run the installer again.

## See Also

- [Supported Printers: Adventurer 5M / 5M Pro](supported-printers.md#flashforge-adventurer-5m--5m-pro): what works on the AD5M
- [Troubleshooting: Flashforge Adventurer 5M](../TROUBLESHOOTING.md#flashforge-adventurer-5m-issues)
- [Upgrading HelixScreen](../UPGRADING.md)
- [Installation overview](../INSTALL.md)
