# Installing HelixScreen on the Creality K2 Series

How to run HelixScreen on the touchscreen of a Creality K2, K2 Plus or K2 Pro, on Creality's stock firmware: install, update and remove it. No community firmware is needed; the stock firmware already includes Moonraker.

> **Easy installer:** the [HelixScreen easy installer](https://helixscreen.org/installer/) can update HelixScreen on a K2 from a desktop app. A first install is done with this guide.

## What You Need

- A K2, K2 Plus or K2 Pro on your network.
- Root access, turned on from the printer's own screen: **Settings > Root account information**.
- A computer with an SSH client. The login is `root`, password `creality_2024`.

## Read This First

- **The Creality Cloud app and its camera stream stop working** while HelixScreen is installed. HelixScreen replaces Creality's screen program, which also runs the cloud connection. The printer's local network status connection keeps working.
- **HelixScreen takes over the chamber camera** for its own stream, unless Moonraker already lists a working camera that HelixScreen did not set up.
- **Creality's AI failure detection is replaced.** It runs inside the same screen program. On printers with Creality's detection camera, HelixScreen runs its own spaghetti detection instead, using Creality's detection model on the printer; it can pause the print or only warn. Set it up in **Settings > Safety & Alerts > Spaghetti Detection** ([Safety & Alerts](settings/safety.md#spaghetti-detection)).

Uninstalling brings all three back.

## Install

Sign in over SSH (`ssh root@<printer-ip>`) and run:

```bash
python3 -c "import urllib.request as u;u.urlretrieve('https://releases.helixscreen.org/install.sh','/tmp/install.sh')" && sh /tmp/install.sh
```

Recent K2 firmware ships neither `wget` nor `curl`, but every K2 has Python, which can download over HTTPS. The command uses Python to fetch the installer, and the installer then uses Python for the download too. If your firmware still has `wget`, `wget -O - http://dl.helixscreen.org/install.sh | sh` works as well.

### What the installer does

- Installs HelixScreen to `/mnt/UDISK/helixscreen` on the large user partition, with its logs and caches in `/mnt/UDISK/helixscreen-state`, your settings in `/mnt/UDISK/printer_data/config/helixscreen`, and the boot service `/etc/init.d/S99helixscreen`.
- Stops Creality's screen program (`/etc/init.d/app`) and keeps it from starting at boot. Creality's `web-server` is brought back every time HelixScreen starts, so the printer's local status connection keeps answering.
- At boot, waits up to two minutes for Moonraker, then starts the screen anyway and keeps reconnecting.

> **Moving from an older install.** Older releases installed to `/opt/helixscreen`, on the small system partition. The first install or update with this release, including one from **Settings > Updates**, moves HelixScreen to `/mnt/UDISK/helixscreen`. Only your settings (`settings.json`) and `helixscreen.env` come along. Anything else you added to the old `config/` folder, such as custom printer images or your own themes, does not, and the old folder is deleted once the new install works, so copy those files out first.

---

## Getting Started on a K2

The setup wizard starts on the touchscreen when the installer finishes. The K2 release already knows this printer's hardware, so the wizard is short:

1. **Language**, if none is set yet.
2. **Network:** pick a Wi-Fi network, or continue if the printer is already connected by Wi-Fi or Ethernet.
3. **Connection:** HelixScreen connects to the printer's own Moonraker by itself and skips this step. If it can't reach it yet, the step appears: the address `127.0.0.1` and port `7125` are right, so tap **Test Connection**, then continue.
4. **Preferences:** a few choices, such as sounds, that you can change later in Settings.
5. **Help Improve HelixScreen:** choose whether to send anonymous usage statistics. You can change this later in **Settings > System**.

Then the Home screen appears. Worth doing first:

- **Check the CFS.** With a CFS attached, its spools show on the Filament screen and the printer picture shows the CFS on top. Without one, the K2 is a single-spool printer and Load and Unload use the printer's own external-spool routines. See [Filament Management](filament.md).
- **Decide on spaghetti detection** in **Settings > Safety & Alerts** (see Read This First above).
- **Check the version** in **Settings > Help & About > About**, and pick your update channel in **Settings > Updates**.

The K2's screen is a portrait panel turned to landscape in software, which is the supported setup. On this slower processor, HelixScreen may tone down animations and the 3D bed mesh view.

---

## Service Control and Logs

```bash
/etc/init.d/S99helixscreen start      # or stop, restart, status
/mnt/UDISK/helixscreen/bin/helix-screen --version
```

Collect both logs when reporting a problem:

```bash
tail -100 /mnt/UDISK/helixscreen-state/logs/helix.log     # the app's log
tail -100 /mnt/UDISK/helixscreen/logs/launcher.log        # startup and crash output
logread | grep helix-screen | tail -100                   # anything in the system log
```

On firmware whose `/var/log` survives a reboot, the startup log is at `/var/log/helixscreen/launcher.log` instead.

## Updating

- **On the touchscreen:** **Settings > Updates**. See [Updates](settings/updates.md).
- **Over SSH**, with the installer HelixScreen keeps in its own folder:

  ```bash
  sh /mnt/UDISK/helixscreen/install.sh --update
  ```

Add `--version vX.Y.Z` for a specific version (see the [releases page](https://github.com/prestonbrown/helixscreen/releases/latest)). Your settings are kept; see [Upgrading](../UPGRADING.md) for what `--update` keeps and what `--clean` resets.

## Uninstalling

```bash
cp /mnt/UDISK/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
```

The installer will not uninstall from inside its own folder, so it is copied to `/tmp` first. This removes HelixScreen, turns Creality's screen program back on and starts it, which brings back Creality's own AI failure detection, and gives the chamber camera back to Creality's camera program.

## See Also

- [Supported Printers: Creality K2](supported-printers.md#creality-k2--k2-plus--k2-pro): CFS details
- [Troubleshooting: CFS shows no slots](../TROUBLESHOOTING.md#cfs-shows-no-slots-creality-k2)
- [Upgrading HelixScreen](../UPGRADING.md): version pinning, factory reset
- [Installation overview](../INSTALL.md)
