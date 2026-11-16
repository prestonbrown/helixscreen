# Installing HelixScreen on the Elegoo Centauri Carbon

How to run HelixScreen on the Elegoo Centauri Carbon's built-in 4.3" touchscreen: flash the community [OpenCentauri COSMOS](https://docs.opencentauri.cc/klipper-conversion/cosmos/cosmos/) firmware, install HelixScreen on top of it, then update or remove it later.

> **Easy installer:** the [HelixScreen easy installer](https://helixscreen.org/installer/) does not handle this printer yet. See [what works today](easy-installer-unsupported.md#elegoo-centauri-carbon).

## What You Need

- An Elegoo Centauri Carbon on your network (Wi-Fi or Ethernet).
- **COSMOS 26.07.0 or newer.** Stock Elegoo firmware can't run HelixScreen (it has no SSH, Klipper or Moonraker), and COSMOS 0.0.7 and earlier lack the calibration and filament macros HelixScreen uses. HelixScreen is tested on COSMOS 26.08.0.
- A USB stick formatted FAT32, for flashing COSMOS.
- A computer with an SSH client.

## Step 1: Install COSMOS

COSMOS replaces the stock firmware with Klipper, Moonraker, Mainsail and a choice of touchscreen programs.

1. Download the latest `update.swu` from the [COSMOS releases](https://github.com/OpenCentauri/cosmos/releases).
2. Copy it to the root of the FAT32 USB stick.
3. Insert the stick and switch the printer on.
4. On the stock Elegoo screen, open the firmware update menu and apply the update.
5. **The first boot takes 5 to 10 minutes** while it reflashes the toolhead and bed boards. Leave it alone until it finishes.
6. Connect the printer to Wi-Fi from the COSMOS screen and note its IP address.

If the update fails or the printer won't boot, see the OpenCentauri [install guide](https://docs.opencentauri.cc/klipper-conversion/cosmos/install/) and its [emergency USB recovery](https://docs.opencentauri.cc/software/updates/).

**Already on COSMOS?** Check the version before installing HelixScreen:

```bash
curl -s http://<printer-ip>/printer/info | grep -o '"software_version": *"[^"]*"'
# "software_version": "Release - 26.08.0"
```

Mainsail shows the same text on its Machine page. Anything older than `Release - 26.07.0` needs the COSMOS update above first.

## Step 2: Install HelixScreen

Sign in over SSH. The default password is `OpenCentauri`; change it once you are done.

```bash
ssh root@<printer-ip>
```

Then run the installer:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

### What the installer does

- Recognizes COSMOS and installs HelixScreen to `/user-resource/helixscreen` (the rest of the COSMOS system is read-only).
- Keeps logs and caches beside it in `/user-resource/helixscreen-state`, and your settings in `/etc/klipper/config/helixscreen`, so an update that replaces the install folder leaves them alone.
- Stops the touchscreen program COSMOS was running (GrumpyScreen, AtomScreen or GuppyScreen) and hands the screen to HelixScreen.
- Makes all three of those COSMOS screen choices start HelixScreen instead. COSMOS's screen setting only accepts those three names, so this is how HelixScreen gets started at boot whichever one is selected. The originals are kept and come back when you uninstall.
- Adds the boot service `/etc/init.d/helixscreen`.

---

## Getting Started on a Centauri Carbon

HelixScreen starts as soon as the installer finishes. The Centauri Carbon release already knows this printer's heaters, fans, probe and screen colors, so the setup wizard is short:

1. **Language**, if none is set yet.
2. **Network:** pick a Wi-Fi network, or continue if the printer is already connected by Wi-Fi or Ethernet.
3. **Connection:** HelixScreen connects to the printer's own Moonraker by itself and skips this step when it can. On COSMOS, Moonraker listens on port `80`, not the usual `7125`. If the step appears, tap **Test Connection**: with the port field still on its default it tries port `80` as well. If you typed a port yourself, enter `80`.
4. **Preferences:** a few choices, such as sounds, that you can change later in Settings.
5. **Help Improve HelixScreen:** choose whether to send anonymous usage statistics. You can change this later in **Settings > System**.

Then the Home screen appears. Worth doing first:

- **Slice with the COSMOS printer profile.** Elegoo's stock start G-code calls `M729`, which COSMOS treats as an emergency stop: a file sliced with the stock profile halts the printer as soon as it starts, and Klipper needs a `FIRMWARE_RESTART`. COSMOS 26.08 shows "Use COSMOS OrcaSlicer profile (or COSMOS start/end machine gcode)"; 26.07 shows "Use COSMOS start/end machine gcode". Re-slice with the COSMOS OrcaSlicer profile, whose start G-code is one `PRINT_START EXTRUDER=... BED=... CHAMBER=...` line.
- **Check the version** in **Settings > Help & About > About**.
- **Reboot and shut down from HelixScreen**, not from Mainsail or Fluidd. HelixScreen powers the printer down itself and works on every COSMOS version; Mainsail and Fluidd ask Moonraker, which on COSMOS stable fails with a `sudo` error because COSMOS has no `sudo`.

---

## Service Control and Logs

```bash
/etc/init.d/helixscreen restart
tail -100 /user-resource/helixscreen-state/logs/helix.log    # the app's log
tail -100 /user-resource/helixscreen/logs/launcher.log       # startup and crash output
```

## Updating

- **On the touchscreen:** **Settings > Updates**. See [Updates](settings/updates.md).
- **Over SSH:**

  ```bash
  curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update
  ```

Your settings are kept. Add `--version vX.Y.Z` to install a specific version, or use `--clean` instead of `--update` to reinstall with fresh settings. See [Updating HelixScreen](../INSTALL.md#updating-helixscreen).

Check the installed version over SSH with `/user-resource/helixscreen/bin/helix-screen --version`.

## Uninstalling

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --uninstall
```

This removes HelixScreen, restores the three COSMOS screen programs and starts GrumpyScreen. COSMOS's screen setting is left as it was, so after a reboot the program you had selected starts again. While HelixScreen is installed, switching COSMOS's screen setting does not bring another program back, because all three start HelixScreen; uninstall instead.

## Notes

- **Moonruster**, a Rust replacement for Moonraker that some COSMOS users install to save memory, works with HelixScreen. It cannot load Moonraker add-ons written in Python, so the [HelixPrint plugin](helixprint-plugin.md) and Moonraker-Timelapse don't run under it. HelixScreen works without them.
- **Colors:** the Centauri Carbon release carries a white-balance correction for this printer's panel, so colors look neutral without any tuning.

**Testing on this printer?** Please report your results on [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) or [Discord](https://discord.gg/RZCT2StKhr).

## See Also

- [Supported printers](supported-printers.md#other-dedicated-builds): where the Centauri Carbon sits in the support list
- [Troubleshooting](../TROUBLESHOOTING.md): collecting logs and common problems
- [Upgrading HelixScreen](../UPGRADING.md): version pinning, resets
