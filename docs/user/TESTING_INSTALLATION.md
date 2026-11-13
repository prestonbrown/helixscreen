# Checking Your Installation

Use this page after installing or updating HelixScreen, to confirm it is installed where you expect, starts on its own and talks to your printer. Testers can also use it to check a release before it is published.

HelixScreen is an add-on to a working Klipper and Moonraker setup. It does not ship an operating system image; the installer assumes Moonraker already answers.

---

## Contents

- [Before You Start](#before-you-start)
- [What the Installer Does](#what-the-installer-does)
- [Where HelixScreen Is Installed](#where-helixscreen-is-installed)
- [Check That It Runs](#check-that-it-runs)
- [Check the Logs](#check-the-logs)
- [Verification Checklist](#verification-checklist)
- [Check an Update](#check-an-update)
- [Check an Uninstall](#check-an-uninstall)
- [Platform Notes](#platform-notes)
- [Release Test Matrix](#release-test-matrix)
- [Known Issues](#known-issues)

---

## Before You Start

Log in to the machine HelixScreen runs on over SSH, and check that Moonraker answers:

```bash
curl -s http://localhost:7125/printer/info
```

On printers without `curl`, use:

```bash
wget -qO- http://localhost:7125/printer/info
```

The answer is a short block of text. Look for `"state": "ready"`. If Moonraker does not answer, fix that first; HelixScreen cannot do anything without it. On a separate screen that controls a printer over the network, run the same command with the printer's address in place of `localhost`.

---

## What the Installer Does

1. Works out which platform it is on (Raspberry Pi 64-bit or 32-bit, x86_64 PC, Adventurer 5M, Adventurer 5X, K1, K2, Centauri Carbon, Snapmaker U1 and others) and which firmware mod is installed.
2. Downloads the matching release package. Printers that cannot download over HTTPS, such as the Adventurer 5M, are given instructions to copy the package over from a computer instead.
3. Stops and disables other screen programs that would fight over the display, such as KlipperScreen, GuppyScreen or the printer's stock screen.
4. Installs into the platform's install folder, keeping your settings when it replaces an older install.
5. Sets up the service that starts HelixScreen at boot, adds HelixScreen to Moonraker's update manager, and starts it.

When it finishes, the installer prints a short summary that names the **Config** folder, and on printers where HelixScreen starts at the next reboot, the **Service** and **App log**. Keep that summary: it is the quickest answer to "where did it go?"

---

## Where HelixScreen Is Installed

These are the usual locations for a first install. An existing install always stays where it is, so a machine that was installed by an older release may differ. The installer's summary is the final word.

| Platform | Install folder | Started by |
|----------|----------------|------------|
| Raspberry Pi or other Linux board with Klipper on it | `~/helixscreen` in the home folder of the user Klipper runs as | systemd service `helixscreen` |
| Linux board with no Klipper on it (separate screen), installed as a normal user | `~/helixscreen` | systemd service `helixscreen` |
| Linux board with no Klipper on it, installed as root | `/opt/helixscreen` | systemd service `helixscreen` |
| x86_64 Linux desktop (`--desktop`) | `~/.local/share/helixscreen` | your applications menu; no service |
| Adventurer 5M, Forge-X | `/opt/config/mod_data/helixscreen` (Forge-X manages it) | Forge-X, through `S90helixscreen` inside its own system folder |
| Adventurer 5M, Forge-X, installed by an older release or with `--standalone` | `/opt/helixscreen` | `/etc/init.d/S90helixscreen` |
| Adventurer 5M, ZMOD | `/srv/helixscreen` | `/etc/init.d/S80helixscreen` |
| Adventurer 5M, Klipper Mod v00.06 and newer | `/opt/helixscreen` | `/etc/init.d/S80helixscreen` |
| Adventurer 5M, Klipper Mod v00.05 and older | `/root/printer_software/helixscreen` | `/etc/init.d/S80helixscreen` |
| Adventurer 5X and Creator 5 | `/srv/helixscreen`, or the mod's own folder where the mod manages HelixScreen | `S80helixscreen` |
| Creality K1 series | `/usr/data/helixscreen` | `/etc/init.d/S99helixscreen` |
| Creality K2 series | `/mnt/UDISK/helixscreen` | `/etc/init.d/S99helixscreen` |
| Elegoo Centauri Carbon | `/user-resource/helixscreen` | `/etc/init.d/helixscreen` |
| Snapmaker U1 | `/userdata/helixscreen` | the stock `S99screen`, which hands over to `config/helixscreen.init` inside the install folder |

HelixScreen's settings file, `settings.json`, is in the `config` folder inside the install folder.

On the Raspberry Pi, the installer picks the 64-bit or 32-bit package to match the operating system you run, not only the processor.

On an Adventurer 5M with Forge-X that still has an older install at `/opt/helixscreen`, the installer offers to keep using that folder. If you decline, it installs in Forge-X's folder and prints the commands to remove the old one. It never deletes the old install itself.

In the commands below, replace `INSTALL` with your install folder, for example `/usr/data/helixscreen`.

---

## Check That It Runs

**Version.** This works on every platform:

```bash
INSTALL/bin/helix-screen --version
```

**Service, on a Raspberry Pi or other systemd Linux:**

```bash
systemctl is-enabled helixscreen    # expect: enabled
systemctl is-active helixscreen     # expect: active
```

**Service, on a printer with an init script** (use the script from the table above):

```bash
/etc/init.d/S99helixscreen status
```

The Snapmaker U1's init script has no `status` action. On the U1, and on any printer, you can check for the running program instead:

```bash
ps | grep helix-screen | grep -v grep
```

On a full Linux system, use `ps aux` instead of `ps`.

**Reboot test.** Reboot, wait for the printer to finish starting, and check that HelixScreen appears on the screen by itself. Then repeat the checks above.

---

## Check the Logs

HelixScreen writes two logs:

- **The app log**, its normal messages. On a Raspberry Pi it goes to the system journal. On printers it goes to the system log.
- **The launcher log** (`launcher.log`), which records starts, stops and crash output. It is in `/var/log/helixscreen/` when that folder survives a reboot, and otherwise in the `logs` folder inside the install folder.

**Raspberry Pi and other systemd Linux:**

```bash
journalctl -u helixscreen -n 100 --no-pager    # last 100 lines
journalctl -u helixscreen -p err --no-pager    # errors only
journalctl -u helixscreen -f                   # follow live
```

**Adventurer 5M** (its system log is kept on disk):

```bash
grep helix-screen /var/log/messages | tail -100
tail -100 INSTALL/logs/launcher.log
```

**K1, K2 and other printers with an in-memory system log:**

```bash
logread | grep helix-screen | tail -100
tail -100 INSTALL/logs/launcher.log
```

If `INSTALL/logs/launcher.log` does not exist, look in `/var/log/helixscreen/launcher.log`.

To send the logs to the HelixScreen team, use a debug bundle instead: **Settings > Help & About > Upload Debug Bundle** (see [Help & About](guide/settings/help-about.md)).

---

## Verification Checklist

Run through this after installing on any platform.

### Basics

- [ ] **Version:** `INSTALL/bin/helix-screen --version` prints a version
- [ ] **Service enabled:** `systemctl is-enabled helixscreen` (Pi) or the init script exists (printers)
- [ ] **Running:** the process is there and does not keep restarting
- [ ] **Survives a reboot:** HelixScreen comes back on its own
- [ ] **Screen:** the interface shows on the display
- [ ] **Touch:** buttons respond to taps, and lists scroll

### Moonraker

- [ ] **Connected:** the printer picture on the Home panel shows no red "no entry" sign
- [ ] **Live data:** temperatures update by themselves
- [ ] **Commands work:** turning a fan or heater on from the screen reaches the printer

### Setup and Settings

- [ ] **Wizard on first start:** the setup wizard appears on a fresh install (see [Getting Started](guide/getting-started.md#first-time-setup))
- [ ] **Wizard completes:** every step finishes without an error
- [ ] **Settings kept:** after a reboot, your choices are still there

### Screens

- [ ] **Home:** temperatures and printer status
- [ ] **Controls:** motion, temperatures, fans
- [ ] **Temperature screens:** nozzle and bed
- [ ] **Print files:** tapping the print area on Home lists your G-code files
- [ ] **Settings:** you can change the theme and open Help & About

---

## Check an Update

Update with the installer's `--update` option. It keeps your settings.

**Printers and boards that can download over HTTPS** (Raspberry Pi, K1, K2 and most others):

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update
```

**Adventurer 5M**, which cannot download over HTTPS: copy the new package to the printer from a computer, as in [Installing on the Adventurer 5M](guide/install-ad5m.md), then run the installer that came with your current install:

```bash
INSTALL/install.sh --local /data/helixscreen-ad5m.zip --update
```

Then check:

- [ ] HelixScreen restarts by itself and the screen comes back
- [ ] `INSTALL/bin/helix-screen --version` shows the new version
- [ ] `INSTALL/config/settings.json` is still there and your settings are unchanged

Updates from the screen itself are described in [Updates](guide/settings/updates.md). Upgrade notes for each release are in [Upgrading](UPGRADING.md).

---

## Check an Uninstall

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --uninstall
```

On a printer that cannot download over HTTPS, copy the installer out of the install folder first, then run the copy:

```bash
cp INSTALL/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
```

Then check:

- [ ] The install folder is gone: `ls INSTALL` reports that it does not exist
- [ ] HelixScreen is no longer running: `ps | grep helix-screen | grep -v grep` prints nothing
- [ ] After a reboot, the screen program that was there before (KlipperScreen, GuppyScreen or the stock screen) is back

---

## Platform Notes

### Raspberry Pi and Other systemd Linux

- **Other screen programs are switched off.** The installer stops and disables screen programs such as KlipperScreen, and adds a small override that keeps them from starting while HelixScreen is installed. Check with `systemctl is-enabled KlipperScreen` (expect `disabled`). Uninstalling HelixScreen turns them back on.
- **KIAUH.** If KIAUH is installed, the installer adds a HelixScreen entry to KIAUH's extensions (in `~/kiauh/kiauh/extensions/helixscreen`). Start KIAUH with `~/kiauh/kiauh.sh` and open its Extensions menu to see it. `--skip-kiauh-registration` leaves KIAUH alone.
- **Update manager.** The installer adds an `[update_manager helixscreen]` section to `moonraker.conf`, so Mainsail and Fluidd can show HelixScreen updates.
- **Restart:** `sudo systemctl restart helixscreen`

### Adventurer 5M with Forge-X

Set Forge-X up for HelixScreen first, as [Installing on the Adventurer 5M](guide/install-ad5m.md) describes. The installer then:

- switches off Forge-X's GuppyScreen and the stock FlashForge screen
- patches Forge-X's screen script so the backlight stays at full brightness while HelixScreen runs

After a reboot, the backlight should be at full brightness, not dimmed. While HelixScreen runs, the file `/tmp/helixscreen_active` exists:

```bash
ls /tmp/helixscreen_active
```

Uninstalling brings GuppyScreen back and removes the backlight patch.

### Adventurer 5M with Klipper Mod

The installer switches off KlipperScreen and, if it was running, Xorg, so HelixScreen can draw on the screen directly. Check that their init scripts are no longer executable:

```bash
ls -la /etc/init.d/S80klipperscreen /etc/init.d/S40xorg
```

Restart with `/etc/init.d/S80helixscreen restart`.

### Creality K1 Series

On Simple AF (`/usr/data/pellcorp` exists), the installer switches off GuppyScreen. Check that its init script is no longer executable:

```bash
ls -la /etc/init.d/S99guppyscreen
```

Creality's background services keep running, so Creality Print can still reach the printer; only the stock screen is replaced. Restart with `/etc/init.d/S99helixscreen restart`.

### Other Printers

Each printer's install page covers its own checks: [K2](guide/install-k2.md), [Centauri Carbon](guide/install-cc1.md), [Snapmaker U1](guide/install-u1.md), [Adventurer 5X](guide/install-ad5x.md), [QIDI](guide/install-qidi.md), [Sonic Pad](guide/install-sonicpad.md) and the [Linux desktop](guide/install-linux-desktop.md).

---

## Release Test Matrix

For testers checking a release. The columns pick platforms that cover the different install methods (systemd on a Pi, init scripts on printers, BusyBox systems); the full platform list is in the [Installation Guide](INSTALL.md).

| Test | MainsailOS | KIAUH Pi | Manual Pi | AD5M Forge-X | AD5M Klipper Mod | K1 Simple AF |
|------|:----------:|:--------:|:---------:|:------------:|:----------------:|:------------:|
| Clean install | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Update | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Uninstall, then reinstall | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Starts after reboot | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Connects to Moonraker | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Touch works | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Screen draws | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Wizard completes | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Panels open | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Other screen program switched off | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |

For a clean install, first check that no earlier install is present: the install folder from the table above should not exist. A "Manual Pi" is any systemd Linux with Klipper and Moonraker installed by hand; check that systemd is in charge with `ps --no-headers -o comm 1`, which should print `systemd`.

---

## Known Issues

| Problem | What to do |
|---------|------------|
| The wizard appears at every start | Check that `INSTALL/config/settings.json` exists and is valid JSON |
| Taps land in the wrong place | Run **Settings > Touch & Input > Touch Calibration** (see [Touch Calibration](guide/touch-calibration.md)) |
| Raspberry Pi 5 shows a black screen | Set `"drm_device": "/dev/dri/card1"` (or `card2`) in the `display` section of `settings.json`; see [Troubleshooting](TROUBLESHOOTING.md) |
| Low memory on a Raspberry Pi 3 | Turn off services you do not need |
| Adventurer 5M with Forge-X: the screen dims after boot | Run the installer again to restore the backlight patch |
| Adventurer 5M with Forge-X: GuppyScreen comes back | Run the installer again; it switches GuppyScreen off |
| Adventurer 5M with Klipper Mod: KlipperScreen comes back | `chmod -x /etc/init.d/S80klipperscreen`, then reboot |
| K1 with Simple AF: GuppyScreen comes back | `chmod -x /etc/init.d/S99guppyscreen`, then reboot |
| K1: nothing on the screen | Check that Simple AF (or your other firmware mod) is installed and working first |

---

*Related: [Installation Guide](INSTALL.md) | [Upgrading](UPGRADING.md) | [Troubleshooting](TROUBLESHOOTING.md)*
