# Installing HelixScreen on the Snapmaker U1

The Snapmaker U1 is an all-in-one printer with a built-in touchscreen. HelixScreen replaces the stock UI and launches automatically on boot. This guide covers installation, updates, firmware upgrades with HelixScreen installed, and recovery if the screen comes up blank.

## Tested With

> **Requires SSH access.** Enable it on **stock firmware (1.2+)** via the **Root access** option in printer settings, or install [PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) (SSH on by default). PAXX is **not** required; it's just the turnkey option.
>
> **Firmware versions:** Tested on PAXX Extended Firmware **1.2.x, 1.3.x, and 1.4.x**. The stock-firmware path has not been verified end-to-end on a real stock device. After any firmware update, **reinstall HelixScreen**; the update resets the printer's system files and the stock screen will return until you reinstall (see [Upgrading the firmware](#upgrading-the-extended-firmware-with-helixscreen-installed)).

## Prerequisites

- **Hardware:**
  - Snapmaker U1
  - Built-in touchscreen display
  - Network connection
- **Software:**
  - **SSH access**: via either firmware path:
    - **Stock Snapmaker firmware (1.2+):** enable the **Root access** option in printer settings (added in V1.2.0). This turns on SSH. *(The stock-firmware path has not been verified end-to-end on a real stock device.)*
    - **[PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware):** SSH on by default. Tested on **1.2.x, 1.3.x, and 1.4.x**.
  - SSH login (`root@<printer-ip>` or `lava@<printer-ip>`, password: `snapmaker`)

## Installation

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

<details>
<summary>Manual Install (if you prefer, or the one-liner doesn't work on your network)</summary>

**Step 1: Download the release archive**

```bash
wget https://releases.helixscreen.org/releases/v<VERSION>/helixscreen-snapmaker-u1.zip
```

**Step 2: Extract to the install directory**

```bash
mkdir -p /userdata/helixscreen && unzip -q helixscreen-snapmaker-u1.zip -d /userdata/helixscreen
```

**Step 3: Configure autostart**

```bash
bash /userdata/helixscreen/scripts/snapmaker-u1-setup-autostart.sh /userdata/helixscreen
```

This sets HelixScreen to launch on boot and disables the stock UI program (`/usr/bin/gui`) so HelixScreen owns the screen.

**Step 4: Start HelixScreen**

```bash
killall gui 2>/dev/null; /userdata/helixscreen/bin/helix-launcher.sh &
```

</details>

## What the Installer Does

- Automatically detects the Snapmaker U1 and installs to `/userdata/helixscreen/`
- Configures autostart so HelixScreen launches instead of the stock UI on boot
- Disables the stock UI program (`/usr/bin/gui`) so HelixScreen owns the screen. The stock UI program lives in a read-only part of the firmware and is only disabled, never deleted; the uninstaller re-enables it
- On stock firmware, keeps SSH working. The stock screen is what starts SSH at boot when **Root access** is on, so HelixScreen does that job instead each time it starts. The **Root access** switch itself is on the stock screen: to change it, uninstall HelixScreen, flip the switch, then reinstall

## Service Control and Logs

```bash
# Restart the service
/userdata/helixscreen/config/helixscreen.init restart

# Structured app log (persistent syslog)
grep helix-screen /var/log/messages | tail -100

# Launcher / supervisor capture (startup, crash output)
tail -100 /var/log/helixscreen/launcher.log
```

To start it by hand (for example after killing a stuck instance): `killall gui 2>/dev/null; /userdata/helixscreen/bin/helix-launcher.sh &`

## Updating

Re-run the Quick Install one-liner with `--update`; it preserves your settings:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update
```

To pin a specific version add `--version vX.Y.Z`, or swap `--update` for `--clean` to reinstall with fresh settings. See [Updating HelixScreen](../INSTALL.md#updating-helixscreen) for the universal details.

### Upgrading the Extended Firmware with HelixScreen installed

A firmware upgrade is safe to run with HelixScreen installed; it does **not** brick the printer. HelixScreen's files live in a layer that the upgrade clears, so after upgrading you simply **reinstall HelixScreen**.

Because HelixScreen replaces the stock touchscreen, the stock screen's on-device **"Local Update"** button is gone. Upgrade over the network instead:

1. In a web browser on the same network, open **`http://<printer-ip>/firmware-config`**.
2. Choose **Firmware Upgrade**, upload the new `U1_extended_<version>_upgrade.bin`, and let it complete. The printer reboots into the new firmware.
3. The stock screen comes back (HelixScreen was cleared by the upgrade). **Reinstall HelixScreen** with the [Quick Install](#quick-install-recommended) one-liner. Your settings, WiFi, and printer config are preserved (they live on a separate partition the upgrade keeps).

## Uninstalling and Reverting to Stock UI

Run the uninstaller; it re-enables the stock UI and removes HelixScreen:

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

## Recovery: screen is blank or the printer is off the network

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

## SSH refused after installing on stock firmware

On stock Snapmaker firmware, SSH only runs because the stock screen starts it at boot while **Root access** is on. Older HelixScreen versions disabled the stock screen without taking over that job, so after the next reboot `ssh` answers "Connection refused". Fluidd and Mainsail keep working, but they give you no shell.

1. **Update HelixScreen from the printer's screen**: **Settings > About**, check for updates and install. The updated HelixScreen starts SSH whenever **Root access** was on; if `ssh` still refuses afterwards, reboot the printer once.
2. **If you can't update from the screen**, reinstall the stock firmware the way Snapmaker's documentation describes. A firmware update clears HelixScreen's system changes, so the stock screen comes back. Then turn **Root access** back on in the stock screen (**Settings > Maintenance**) and reinstall HelixScreen.

## Quirks and Notes

- **A firmware update resets system files**: any firmware update (stock or PAXX) can overwrite HelixScreen and bring the stock screen back; re-run the installer afterward (see [Upgrading the firmware](#upgrading-the-extended-firmware-with-helixscreen-installed))
- **Remote screen ("gui" camera) works on PAXX firmware**: the built-in "gui" webcam in Mainsail/Fluidd shows the live HelixScreen UI, and you can tap it to control the printer remotely. Enable **Remote Screen** in the firmware settings web UI at `http://<printer-ip>/firmware-config/`; it registers the "gui" webcam and restarts HelixScreen and Moonraker for you (hand-editing the config value alone is not enough). The physical "case" camera is unaffected. Stock firmware is not yet confirmed to expose the feed. Setup steps: [Supported Printers → Snapmaker U1](supported-printers.md#snapmaker-u1-snapswap)
- **Two harmless Moonraker warnings are expected**: after install, the Mainsail/Fluidd "Moonraker warnings found" banner may show *"Unable to find DBus PolKit Interface"* and *"Unable to initialize System Update Provider for distribution: buildroot"*. Both are inherent to Moonraker on the U1's buildroot firmware (no PolKit, no OS package manager) and do **not** affect HelixScreen or printing. They are not specific to HelixScreen; installing simply restarts Moonraker, which re-surfaces them. See [Troubleshooting](../TROUBLESHOOTING.md)
- **Display resolution may need manual configuration** if the screen appears stretched or misaligned (see [Display Configuration](../INSTALL.md#display-configuration))

## See Also

- [Troubleshooting: Snapmaker U1 blank screen](../TROUBLESHOOTING.md#snapmaker-u1-blank-screen-and-off-the-network-after-a-reboot): the wired-recovery companion to the section above
- [Supported printers: Snapmaker U1 (SnapSwap)](supported-printers.md#snapmaker-u1-snapswap): what works, and remote screen setup
- [UPGRADING.md](../UPGRADING.md): version pinning, resets, migrations
