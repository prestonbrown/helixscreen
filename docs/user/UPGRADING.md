# Upgrading HelixScreen

This guide helps you upgrade HelixScreen to a newer version.

> For platform-specific upgrade commands (the two-step offline process on printers without HTTPS fetch tools, the AD5X chroot path, bundled-installer locations), see your printer's install guide; start from the [router table in the Installation Guide](INSTALL.md#which-printer-are-you-installing-on).

---

## Quick Upgrade

The preferred ways to update are inside the app itself (**Settings > Updates > Check for Updates**) or the Mainsail/Fluidd update manager. From the command line instead, on any host with direct internet access:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update
```

Your settings (`settings.json`), environment overrides (`helixscreen.env`), and custom files (custom printer images, etc.) are automatically preserved across updates.

> **One thing that can change: your home screen arrangement.** If an update changes how the home grid is sized, saved widget positions no longer point at the right cells, so HelixScreen re-places every widget on the new grid. Which widgets you have and how they are configured is kept: only the positions and sizes are recomputed. See [What Happens on Upgrade](guide/home-panel.md#what-happens-on-upgrade).

Printers without direct internet access or HTTPS fetch tools (Creality K1, Adventurer 5M, Adventurer 5X) use a two-step or chroot procedure instead; each printer's install guide has the exact commands.

---

## Coming from 1.0

1.1 installs over an existing 1.0 setup as a normal update. To get it before the stable release, switch to the Beta update channel first:

1. Go to **Settings > Help & About > About** and set **Update Channel** to **Beta**
2. Tap **Check for Updates** on the same screen

The channel picker does not appear on printers whose firmware manages updates itself (Adventurer 5M and 5X on Flashforge firmware); update those over SSH as their install guides describe.

What to expect after the update:

- **Your home screen is converted, not reset.** The first time 1.1 draws the home screen, it remaps your saved tile arrangement onto the new grid. Which tiles you have, their settings and your extra pages all carry over, and tiles that sat next to each other stay next to each other. Sizes can shift a little because the new grid divides the screen differently, and a tile that no longer fits is re-placed on its own. What you see on that first draw is what gets saved.
- **Display sleep turns the backlight off.** The panel stays powered and only the backlight is cut, so waking is immediate. The panel itself is powered down at sleep only on screens whose backlight cannot be controlled. The `/display/panel_power_off` setting in `settings.json` forces a full power-down if you want one.
- **Print preparation tracking needs nothing from your config.** The screen works out the current phase (heating, homing, printing) from the printer itself, on any printer. A `PRINT_START` that already carries `HELIX:PHASE` markers from an older install keeps working, and uninstalling removes the markers and leaves a timestamped backup of each file it touches.
- **Some installs move on disk, automatically.** On the Adventurer 5M, settings, logs and caches move to `/data/.helixscreen`, out of the printer's file list. On the K2, the install moves off the small system overlay onto the user partition (`/mnt/UDISK`). Both moves happen during the update and keep everything.
- **Going back to 1.0 and returning is safe for your settings, with one exception.** A 1.0 build reads a newer settings file without rewriting it, and 1.1 picks the file back up afterwards. The exception is **Material Temperatures**: 1.1 moves your per-material temperature and preheat changes out of `settings.json` into `user_filaments.json`, which 1.0 does not read, so a 1.0 build uses its built-in values instead. Note your changes down before going back. The home screen arrangement is the one thing that can suffer: a 1.0 build saves tile positions in its own grid's units, so after returning to 1.1 the tiles may sit in the wrong spots, or, from some 1.0 builds, the home screen comes back with its default layout. Long-press the home screen to enter Edit Mode and arrange it again.

### Coming from a 1.1 beta

From this release, HelixScreen keeps no spool or lane details of its own: everything a lane shows
lives on the printer, where every screen, Mainsail and OrcaSlicer read the same thing (see
[Where a lane's details come from](guide/filament-tracking.md#where-a-lanes-details-come-from)).
That retires a few things earlier betas kept on the screen:

- **The local copy of your lanes** (used when the printer could not be reached) is gone. Lanes fill
  in from the printer as soon as HelixScreen connects.
- **Edits held while disconnected** are gone too. Editing is unavailable until the printer is
  connected and its lanes have been read, so nothing waits to be sent later.
- **`tool_spools.json`**, the per-tool spool list, and the **external spool** saved in
  `settings.json` are no longer used. A tool's spool is the spool of the lane that feeds it, and
  the external spool is kept on the printer like any lane.

What carries over: everything already on the printer. Spool links, materials and colors your
filament system holds, and the lane records earlier betas wrote, are read as they are, with no
conversion step. Happy Hare tuning values set from HelixScreen move from the screen's settings to
the printer the first time 1.1 connects. A detail that only ever lived on the screen (an edit
made while the printer was offline and never sent) does not carry over; set it again.

On an AFC printer, AFC's own **remember spool** setting now decides what a lane keeps when the
spool comes out, and an unlink only sticks if AFC drops the spool's id.

---

## If the Setup Wizard Keeps Appearing

After upgrading, if HelixScreen keeps showing the setup wizard on every boot, your configuration file format may have changed in a way that's incompatible with the new version.

### Quick Fix

The easiest solution is to delete your config file and let the wizard create a new one:

**MainsailOS (Pi):**
```bash
sudo rm ~/helixscreen/config/settings.json
sudo systemctl restart helixscreen
```
> If HelixScreen runs as root (the `/opt/helixscreen` install), the config is at `/opt/helixscreen/config/settings.json` instead.

**Adventurer 5M (Forge-X):**
```bash
rm /opt/helixscreen/config/settings.json
/etc/init.d/S90helixscreen restart
```

**Adventurer 5M (Klipper Mod):**
```bash
rm /root/printer_software/helixscreen/config/settings.json
/etc/init.d/S80helixscreen restart
```
(Klipper Mod v00.06 and newer installs to `/opt/helixscreen`, so its settings file is `/opt/helixscreen/config/settings.json`.)

**Creality K1 (Simple AF):**
```bash
rm /usr/data/helixscreen/config/settings.json
/etc/init.d/S99helixscreen restart
```

After restarting, the wizard will guide you through setup again. Your printer settings (Klipper, Moonraker) are not affected; only HelixScreen's display preferences need to be reconfigured.

### Alternative: Factory Reset from UI

If you can access the settings panel before the wizard appears:

1. Navigate to **Settings** (gear icon in sidebar)
2. Tap **System**
3. Scroll down to **Factory Reset**
4. Tap **Factory Reset** and confirm

This clears all HelixScreen settings and restarts the wizard.

---

## What's Preserved During Upgrades

The installer automatically preserves:

- **`settings.json`**: All your settings (printer connection, display preferences, sound, safety, etc.)
- **`helixscreen.env`**: Any environment variable overrides you've set
- **Custom files**: Custom printer images, user-added printer database entries, and other files in the `config/` directory

You should not need to reconfigure anything after a normal upgrade. If something does go wrong, see the troubleshooting sections below.

---

## What Settings Are Affected by a Reset

If you perform a factory reset or delete your config, you'll need to reconfigure:

- WiFi connection (if not using Ethernet)
- Moonraker connection (usually auto-detected)
- Display preferences (brightness, theme, sleep timeout)
- Sound settings
- Safety preferences (E-Stop confirmation)

Your Klipper configuration, Moonraker settings, print history, and G-code files are **not affected**; they're stored separately.

---

## Upgrade to Specific Version

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update --version v1.2.0
```

### Reinstall a Version with Fresh Settings

The command above keeps your existing `settings.json`. To reinstall a specific version **and** reset HelixScreen's settings to defaults at the same time, use `--clean` instead of `--update`:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --clean --yes --version v1.2.0
```

`--yes` is required because a piped command cannot ask for confirmation; if you download the script and run it interactively over SSH, you get the confirmation prompt instead. `--clean` removes HelixScreen's settings and caches, then installs the version you specified. Your Klipper config, Moonraker settings, print history, and G-code files are **not** affected.

---

## Checking Your Version

**On the touchscreen:** **Settings > Help & About > About** shows the current version

**Via SSH:** run `<install-path>/bin/helix-screen --version`. The binary's location varies by platform; each printer's install guide lists its path.

---

## Getting Help

If you encounter issues after upgrading:

1. Ask in the [HelixScreen Discord](https://discord.gg/RZCT2StKhr) for quick help
2. Check [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for common problems
3. View logs for error messages: on systemd hosts, `sudo journalctl -u helixscreen -n 50`. Log locations for the other platforms are in your printer's install guide.
4. Open an issue on [GitHub](https://github.com/prestonbrown/helixscreen/issues) with your version and any error messages

---

*Back to: [Installation Guide](INSTALL.md) | [User Guide](USER_GUIDE.md)*
