# Installing HelixScreen on the Creality Sonic Pad

How to run HelixScreen on the Creality Sonic Pad, the standalone 7" touchscreen that runs Klipper: install, update and remove it.

> **SonicPad-Debian only.** HelixScreen is tested only on [SonicPad-Debian](https://github.com/Jpe230/SonicPad-Debian), which replaces Creality's stock OpenWrt image with Debian 11 (bullseye). Creality's stock Sonic Pad firmware is a cut-down OpenWrt build that lacks what the installer relies on (systemd, package tools, archive tools). On stock firmware, flash SonicPad-Debian first.

## What You Need

- A Creality Sonic Pad running [SonicPad-Debian](https://github.com/Jpe230/SonicPad-Debian), on your network.
- Klipper and Moonraker installed and working on the Pad, usually with [KIAUH](https://github.com/dw-0/kiauh).
- SSH access (`ssh sonic@<pad-ip>`) and about 100 MB free.

HelixScreen replaces the touchscreen program you use now, such as KlipperScreen.

## Install

Over SSH on the Pad:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

It asks for your sudo password when it needs it.

### What the installer does

- Recognizes the Pad's 32-bit Debian system and downloads the 32-bit Raspberry Pi-class build (`pi32`). The Pad's processor is 64-bit capable, but its system is 32-bit.
- Installs to `~/helixscreen` and sets HelixScreen up as a systemd service named `helixscreen`.
- Stops and disables the touchscreen program it replaces, so the two don't fight over the display.
- Registers HelixScreen with KIAUH, when KIAUH is installed.

---

## Getting Started on a Sonic Pad

HelixScreen starts when the installer finishes, and the full setup wizard runs, since the Pad can drive many different printers:

1. **Touch Calibration**, only if the touchscreen needs it.
2. **Language**, if none is set yet.
3. **Network:** pick a Wi-Fi network, or continue if the Pad is already connected by Wi-Fi or Ethernet.
4. **Connection:** Moonraker runs on the Pad itself, at `127.0.0.1` port `7125`. HelixScreen looks for it and fills it in; tap **Test Connection**, then continue.
5. **Printer Identification:** pick the printer the Pad drives. If HelixScreen knows that model's hardware, it fills it in and skips the next steps; otherwise you pick the heaters, fans and other hardware yourself.
6. **Preferences:** a few choices, such as sounds, that you can change later in Settings.
7. **Summary**, or **Help Improve HelixScreen** when the printer's hardware was filled in for you.

See [Getting Started](getting-started.md) for each wizard step. Worth doing first:

- **Check that brightness works** in **Settings > Display**. If the slider or screen dimming does nothing, see [Notes](#notes).
- **Check the version** in **Settings > Help & About > About**, and pick your update channel in **Settings > Updates**.

---

## Service Control and Logs

```bash
sudo systemctl restart helixscreen    # or start, stop, status
sudo journalctl -u helixscreen -f     # follow the log
```

## Updating

- **On the touchscreen:** **Settings > Updates**. See [Updates](settings/updates.md).
- **Over SSH**, with the installer HelixScreen keeps in its own folder:

  ```bash
  sh ~/helixscreen/install.sh --update
  ```

Add `--version vX.Y.Z` for a specific version. Check the installed version with `~/helixscreen/bin/helix-screen --version`.

## Uninstalling

```bash
cp ~/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
```

The installer will not uninstall from inside its own folder, so it is copied to `/tmp` first. This removes HelixScreen and brings back the touchscreen program you had before, such as KlipperScreen.

## Notes

- Creality's `display-sleep` service is stopped each time HelixScreen starts, so the two don't fight over the backlight.
- If the brightness slider or screen dimming does nothing, tell HelixScreen to use Creality's `brightness` helper: add `HELIX_BACKLIGHT_DEVICE=brightness` to `~/helixscreen/config/helixscreen.env` and restart HelixScreen. See [Troubleshooting](../TROUBLESHOOTING.md#brightness-slider-or-screen-dimming-does-nothing).

## See Also

- [Supported Printers](supported-printers.md)
- [Troubleshooting](../TROUBLESHOOTING.md)
- [Upgrading HelixScreen](../UPGRADING.md): version pinning and reset behavior
- [Installation overview](../INSTALL.md)
