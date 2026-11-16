# Installing HelixScreen on the FlashForge Adventurer 5X (ZMOD)

How to run HelixScreen on the built-in 4.3" touchscreen of a FlashForge Adventurer 5X with the ZMOD firmware mod: turn it on, update it and remove it.

> **Easy installer:** support for the Adventurer 5X in the [HelixScreen easy installer](https://helixscreen.org/installer/) is coming. Until then, use this guide.

## What You Need

- A FlashForge Adventurer 5X on your network.
- [ZMOD](https://github.com/ghzserg/zmod) **v1.7.0 or newer**. ZMOD gives the printer Klipper, Moonraker, Fluidd or Mainsail, and SSH. If your printer is still on stock firmware, install ZMOD first by following its own instructions.

## Install

ZMOD installs and updates HelixScreen itself, through Moonraker's update manager. Open Fluidd or Mainsail in a browser (`http://<printer-ip>`) and use its console:

1. Run `ENABLE_EXTRA_PLUGINS`. This adds ZMOD's list of extra plugins to Moonraker, including a `helixscreen` update-manager entry, and reboots the printer. It does not install HelixScreen yet.
2. When the printer is back, open the update manager in Fluidd or Mainsail and update **helixscreen**. ZMOD lists it at version v0.0.1, so the current release shows as an update; applying it downloads HelixScreen into `/srv/helixscreen`.
3. Run `DISPLAY_OFF HELIX=1`. This turns the stock screen off, selects HelixScreen in its place and reboots. Do this only after step 2: with no HelixScreen files installed, the printer comes up with no screen at all.

To go back to the stock screen, run `DISPLAY_ON`.

> **Read ZMOD's warning first.** With the stock screen off, the printer no longer runs the stock screen's print start and end routines, so bed leveling, Z-offset and your slicer's start and end G-code work differently. See ZMOD's [DISPLAY_OFF documentation](https://github.com/ghzserg/zmod/blob/main/docs/en/System.md) before you switch.

---

## Getting Started on an Adventurer 5X

After the reboot, HelixScreen appears on the touchscreen and a short setup wizard runs:

1. **Language**, if none is set yet.
2. **Network:** pick a Wi-Fi network, or continue if the printer is already connected by Wi-Fi or Ethernet.
3. **Connection:** HelixScreen connects to the printer's own Moonraker by itself and skips this step. If it can't reach it yet, the step appears: the address `127.0.0.1` and port `7125` are right, so tap **Test Connection**, then continue.
4. **Printer Identification:** confirm the Adventurer 5X. HelixScreen then fills in the heaters, fans and sensors for you and skips those steps.
5. **Preferences:** a few choices, such as sounds, that you can change later in Settings.
6. **Help Improve HelixScreen:** choose whether to send anonymous usage statistics. You can change this later in **Settings > System**.

Then the Home screen appears. Worth doing first:

- **Check the version** in **Settings > Help & About > About**.
- **Look at the IFS**, the four-spool filament system, on the Filament screen. Lane colors and materials live on the printer: a change from the printer's own color menu shows in HelixScreen, and a change made in HelixScreen is written to the printer. See [Filament Management](filament.md).
- **Run a bed mesh** from **Controls > Bed Mesh** (see [Calibration](calibration.md#bed-mesh)).

---

## Working From the Command Line

Most people never need this. It is for installing a specific version, installing from a zip on the printer, or recovering from a failed update.

ZMOD runs HelixScreen inside its own system folder, `/usr/data/.mod/.zmod`. When you sign in over SSH you land outside it, so **enter it first**; the HelixScreen installer refuses to run outside it:

```bash
ssh root@<printer-ip>
chroot /usr/data/.mod/.zmod
```

Every command below runs after that `chroot` line, and every path is as seen from inside.

### Service control and logs

```bash
/etc/init.d/S80helixscreen start      # or stop, restart, status
/srv/helixscreen/bin/helix-screen --version
```

Both logs sit where ZMOD's support archive collects them:

```bash
tail -100 /opt/config/mod_data/log/helix.log        # the app's log
tail -100 /opt/config/mod_data/log/helixscreen.log  # startup and crash output
```

### Updating

The usual way is the update manager: in Mainsail, **Machine > Update Manager**, then **Update** next to helixscreen. HelixScreen's own **Settings > Updates** works too.

If the update manager fails (for example with `Error updating helixscreen: [Errno 93] Directory not empty`), or HelixScreen won't start after an update, update from SSH inside the chroot:

```bash
curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update
```

Without internet on the printer, copy `helixscreen-ad5x.zip` and `install.sh` from the [latest release](https://github.com/prestonbrown/helixscreen/releases/latest) to the printer's `/tmp`, then:

```bash
sh /tmp/install.sh --local /tmp/helixscreen-ad5x.zip --update
```

Your settings, your `helixscreen.env` and your custom files (printer images and the like) are kept. HelixScreen keeps them in `/srv/helixscreen-state`, outside the folder an update replaces.

### Uninstalling

Inside the chroot, copy the installer out of the HelixScreen folder (it will not uninstall from inside it), then run it:

```bash
cp /srv/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
```

To keep the printer's stock screen without uninstalling, `DISPLAY_ON` from the Fluidd or Mainsail console is enough.

---

## Notes

- Random solid colors while the screen sleeps are a known AD5X quirk; see [Troubleshooting](../TROUBLESHOOTING.md#random-solid-colors-during-screen-sleep-ad5x).
- The AD5X plays HelixScreen's sounds, chords and music included, through its built-in speaker; see [Sound](settings/sound.md).

## See Also

- [Supported Printers: FlashForge Adventurer 5X](supported-printers.md#flashforge-adventurer-5x)
- [Troubleshooting: update failed in Mainsail](../TROUBLESHOOTING.md#update-failed-in-mainsail--screen-wont-start-after-update-ad5x)
- [Upgrading HelixScreen](../UPGRADING.md): version pinning and reset behavior
- [Installation overview](../INSTALL.md)
