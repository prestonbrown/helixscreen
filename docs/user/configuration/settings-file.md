# The Settings File

HelixScreen keeps every setting in one file called `settings.json`. Almost everything in it can be changed from the touchscreen, so you rarely need to open it. This page explains where it lives, how it is organised, and how to recover or reset it.

[Back to the Configuration Reference](../CONFIGURATION.md)

---

## Where the file lives

`settings.json` lives in your install's `config/` folder. The install folder depends on the platform:

| Platform | Full path |
|----------|-----------|
| MainsailOS / Raspberry Pi (Klipper detected) | `~/helixscreen/config/settings.json` |
| MainsailOS / Raspberry Pi (no Klipper ecosystem) | `/opt/helixscreen/config/settings.json` |
| FlashForge AD5M (Forge-X) | `/opt/config/mod_data/helixscreen/config/settings.json` |
| FlashForge AD5M (Klipper Mod v00.06 and later) | `/opt/helixscreen/config/settings.json` |
| FlashForge AD5M (Klipper Mod v00.05 and earlier) | `/root/printer_software/helixscreen/config/settings.json` |
| FlashForge AD5M or AD5X (ZMOD) | `/srv/helixscreen/config/settings.json` |
| FlashForge Creator 5 / Creator 5 Pro (Z-Mod) | `/srv/helixscreen/config/settings.json` inside Z-Mod's system folder (`/usr/data/.mod/.zmod/srv/helixscreen/config/settings.json` from a plain SSH login) |
| Creality K1 series | `/usr/data/helixscreen/config/settings.json` |
| Creality K2 series | `/mnt/UDISK/helixscreen/config/settings.json` |
| Elegoo Centauri Carbon | `/user-resource/helixscreen/config/settings.json` |
| Snapmaker U1 | `/userdata/helixscreen/config/settings.json` |

> **On a Raspberry Pi** the installer looks for a Klipper setup (`~/klipper`, `~/moonraker` or `~/printer_data`). If it finds one, HelixScreen installs to `~/helixscreen` and also links the file at `~/printer_data/config/helixscreen/settings.json`, so you can edit it from the Mainsail or Fluidd file manager. Without one it installs to `/opt/helixscreen`. To choose the folder yourself, run the installer with `INSTALL_DIR=/path ./install.sh`.

The setup wizard creates the file on first start. Installing or updating HelixScreen carries your existing settings file forward into this folder, including one left by an older install layout.

> **Stop HelixScreen before you edit the file by hand.** The app rewrites `settings.json` while it runs and can overwrite your edit. Stop the service, edit, then start it again.

---

## How the file is organised

The file is JSON. It holds two kinds of settings:

- **Settings for this screen** sit at the top level or in named sections such as `display`, `input`, `sounds` and `safety`. They apply to whichever printer you are looking at: brightness, theme, sounds, language and so on.
- **Settings for one printer** sit under `printers`, inside an entry named after that printer. They cover everything that describes the machine: its Moonraker address, heaters, fans, lights, filament systems, home screen layout and so on. `active_printer_id` says which entry is live.

```json
{
  "config_version": 34,
  "active_printer_id": "default",
  "dark_mode": true,
  "brightness": 80,
  "display": { "sleep_sec": 3600 },
  "input": { "scroll_limit": 10 },
  "printers": {
    "show_printer_switcher": false,
    "default": {
      "printer_name": "Voron 2.4",
      "moonraker_host": "192.168.1.100",
      "moonraker_port": 7125,
      "heaters": { "bed": "heater_bed", "hotend": "extruder" },
      "fans": { "part": "fan" }
    }
  }
}
```

Each reference page says, for every setting, whether it is **per screen** or **per printer**, and shows the section it lives in. A setting listed as `printers.<id>.leds.led_on_at_start` means the `led_on_at_start` key inside `leds` inside your printer's entry, where `<id>` is the entry's name (`default` unless you added more printers).

`config_version` records the file's layout. HelixScreen upgrades older files forward when it starts, so leave this number alone.

You do not need to edit the file to manage printers. Use **Settings > Connection > Printers** to add, switch and remove them. See [Printer and Connection](network.md) for the printer switcher.

Keys that start with an underscore, such as `_brightness_comment`, appear in the shipped template as notes to the reader. HelixScreen ignores them.

---

## Starting from the template

The install ships a documented copy at `config/settings.json.template`. To start from it:

```bash
cp ~/helixscreen/config/settings.json.template ~/helixscreen/config/settings.json
```

The template is a reference. A few values in it differ from what HelixScreen uses when a key is missing; the reference pages give the value HelixScreen uses.

---

## Resetting

### Factory reset from the screen

**Settings > System > Factory Reset** clears your settings and runs the setup wizard again.

### Full reset by hand

Deleting `settings.json` alone does not start you over, because HelixScreen restores its most recent backup when the file is missing (see below). Remove the backups too. The steps, with the exact paths for each platform, are in [Troubleshooting](../TROUBLESHOOTING.md#reset-helixscreen-or-re-run-the-setup-wizard).

### Partial reset

Stop the service, edit the file, and start it again:

```bash
nano ~/helixscreen/config/settings.json
```

Deleting one key gives that setting its default again.

---

## Safety and recovery

HelixScreen protects your settings from corruption and loss.

**Atomic saves.** A save is written to a temporary file first and then renamed into place, so a power cut during a save cannot leave a half-written file.

**Corruption detection.** If `settings.json` is not valid JSON, for example after a typing slip, HelixScreen renames it to `settings.json.corrupt`, restores the most recent rolling backup (or starts with safe defaults when there is none) and logs what went wrong. The renamed file is yours to repair.

**Rolling backups.** Every successful save refreshes two backup copies:

- `/var/lib/helixscreen/settings.json.backup`, which survives reinstalling the app
- `~/.helixscreen/settings.json.backup`, as a fallback

The Android app keeps both copies in its own storage, where an app update leaves them alone.

If `settings.json` is missing at start, for example after a Moonraker update wiped it, HelixScreen restores the most recent backup.

**If your settings are lost or broken:**

1. Restart HelixScreen. It restores the rolling backup by itself.
2. If you broke the file by hand, look for `settings.json.corrupt` in the config folder and repair it.
3. To start fresh, delete `settings.json` and the backup copies, or use **Settings > System > Factory Reset**. A template copy alone is replaced from the backup on the next start.

**An old `helixconfig.json`.** Installing or updating HelixScreen renames it to `settings.json`. Until then HelixScreen still reads it when it is the link into `printer_data` that older Raspberry Pi installs made. A plain `helixconfig.json` comes from a release older than v0.99.4, which is too old to migrate, so HelixScreen starts from defaults.

---

## Safety limits are not in this file

HelixScreen checks every temperature, move and fan command against limits it reads from your printer's own Klipper configuration: each heater's `max_temp`, the axis `position_min` and `position_max`, and the printer's `max_velocity`. Generous built-in defaults apply where Klipper states nothing. There is no setting for these. If a limit is wrong for your printer, change it in `printer.cfg`, which every tool respects. Editing a curated set of them from HelixScreen is proposed in [issue #1699](https://github.com/prestonbrown/helixscreen/issues/1699).
