# Install, update and printer-specific problems

HelixScreen will not start after an install or update, or a printer model (Snapmaker U1, Adventurer 5M and 5X, Creality K1) behaves differently from the rest.

Back to the [troubleshooting index](../TROUBLESHOOTING.md).

---

### Snapmaker U1: blank screen and off the network after a reboot

**Symptoms:**
- HelixScreen installed and worked, but after a reboot the screen is blank and the printer is unreachable over WiFi (Mainsail/Fluidd won't load).
- Most often seen right after upgrading the PAXX Extended Firmware, or on a fresh install of an older HelixScreen version on Extended Firmware 1.4.

**What happened:** A firmware upgrade clears HelixScreen's files (they live in a layer the upgrade resets), so it isn't installed anymore, and because HelixScreen manages the WiFi connection, nothing brings WiFi up. HelixScreen starts by itself on Extended Firmware 1.2, 1.3 and 1.4 once it is installed, so the fix is to reinstall the current version.

**Fix:** Recover over a wired connection, then reinstall:

1. Plug a **USB-Ethernet adapter** into the printer and connect it to your router. The printer gets a wired IP automatically (check your router's client list).
2. `ssh root@<wired-ip>` (password `snapmaker`).
3. Reinstall HelixScreen:
   ```bash
   curl -sSL https://releases.helixscreen.org/install.sh | sh
   reboot
   ```
4. To instead go back to the stock screen, run the uninstaller: `curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --uninstall && reboot`.

See [Snapmaker U1 install guide > Recovery](../guide/install-u1.md#recovery-screen-is-blank-or-the-printer-is-off-the-network) for the full procedure and the manual reset fallback.

### AD5X: the screen will not start after an update failed in Mainsail

**Symptoms:**
- Mainsail's Update Manager showed an error toast like `Error updating helixscreen: [Errno 93] Directory not empty: PosixPath('/srv/helixscreen')`
- After the failed update, the touchscreen stays black or shows the splash and never starts
- Logs (if reachable) show repeated `[Watchdog] execv failed:` lines that include `(No such file or directory)` and `[helix-launcher] Exiting with code 42`

**What happened:** Moonraker's in-place update wiped part of `/srv/helixscreen` (including the `bin/helix-screen` binary) before something interrupted it. The install directory now has leftover files but no working binary, so the launcher can't start anything, and a retry from Mainsail trips over those leftovers.

**Fix:** Re-run the CLI installer from inside the ZMOD chroot - it cleans up the broken state and lays down a fresh install while preserving your settings. Full procedure: [Adventurer 5X install guide > Updating](../guide/install-ad5x.md#updating).

Quick form, from a Mainsail Shell or SSH:

```bash
ssh root@<printer-ip>
chroot /usr/data/.mod/.zmod
curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update
```

The `chroot` step is required; see [Upgrading](../UPGRADING.md) for why.

### AD5X: Mainsail says Z-Mod is modified and will not update it

**Symptoms:**
- Mainsail or Fluidd lists Z-Mod as modified (dirty) and refuses to update it
- The HelixScreen installer warned that an earlier install added `[update_manager helixscreen]` to a file that belongs to the firmware mod

**What happened:** an older HelixScreen installer wrote its update entry into the `moonraker.conf` that Z-Mod keeps in its own folder. Moonraker then sees Z-Mod's files as changed and will not update Z-Mod. Current installers never edit that file: they put the entry in the user `moonraker.conf` that Z-Mod's file includes.

**Fix:** run the HelixScreen installer once more (as in the section above). It prints a `git ... checkout -- moonraker.conf` command that puts Z-Mod's file back. Run that command over SSH, then restart Moonraker. HelixScreen's own update entry stays in the user file, so updates keep working.

### The service never starts (GLIBC version not found)

**Symptoms:**
- The install completes, but the service never comes up
- Running the binary by hand prints one or more of:
  ```
  /lib/arm-linux-gnueabihf/libm.so.6: version `GLIBC_2.29' not found
  /lib/arm-linux-gnueabihf/libpthread.so.0: version `GLIBC_2.30' not found
  /usr/lib/arm-linux-gnueabihf/libstdc++.so.6: version `GLIBCXX_3.4.26' not found
  ```
- The installer may have warned about this before it finished

**Cause:** your OS is older than the build targets. The `pi` and `pi32` packages are
dynamically linked against **glibc 2.31** (Debian 11 "Bullseye"). glibc is forward- but
not backward-compatible, so those binaries run on Bullseye and anything newer, and fail
to load on anything older. Debian 10 "Buster" ships glibc 2.28.

Confirm what you have:

```bash
ldd --version | head -1          # e.g. "ldd (Debian GLIBC 2.28-10) 2.28"
cat /etc/os-release | head -2    # e.g. VERSION="10 (buster)"
uname -m                         # armv7l = 32-bit, aarch64 = 64-bit
```

**Fix, pick one:**

1. **Upgrade the OS to Bullseye or newer.** Best option on a general-purpose Pi. Current
   Raspberry Pi OS and MainsailOS images are already well past Bullseye, so a reflash
   solves it outright.

2. **Install the `cc1` package instead.** It is **statically linked**, it carries its own
   C library and does not care what glibc the host has. This is the practical answer on
   stock printer images that are pinned to Buster and cannot be upgraded. Despite the
   name it is not Creality-specific; it is a generic static armv7 build and has been run
   successfully on other armv7 hardware, including Rockchip RV1126 boards.

   ```bash
   # replace vX.Y.Z with the current release
   wget https://github.com/prestonbrown/helixscreen/releases/download/vX.Y.Z/helixscreen-cc1.zip
   ./install.sh --local helixscreen-cc1.zip
   ```

   Trade-off: a static binary is larger and does not pick up the host's own OpenSSL/system
   libraries. For a printer touchscreen that is rarely a problem.

If neither works on your hardware, open an issue with the output of all three commands
above, new armv7 platforms are worth adding support for.


---

### Mainsail or Fluidd lists an old `helix_print` update

**Symptoms:**
- The update manager in Mainsail (Machine page) or Fluidd (Settings > Software Updates) lists a `helix_print` row with a version like `v1.1.0-beta.9-461-g...`
- That row offers an update, or says it is up to date while HelixScreen itself is newer

**Why:** the HelixPrint plugin was installed with the old one-line installer, which downloaded a separate copy of the plugin into `~/helix_print` and added that row so Moonraker could update it. Current HelixScreen loads the plugin it ships with instead, and updating HelixScreen updates the plugin. The old row only tracks the separate copy, which is no longer used.

**Do not click Update on that row.** It downloads unreleased development code into the old copy and restarts Moonraker for nothing.

**Fix:** restart HelixScreen. When it starts, it switches the plugin over and removes the old row (it keeps a backup of each config file it edits next to that file, for example `moonraker.conf.bak.helix.<date>`). The row disappears after Moonraker's next restart. To do it by hand instead, run the plugin installer that came with HelixScreen:

```bash
# Use your actual install path: ~/helixscreen on most systems,
# /opt/helixscreen or /usr/data/helixscreen on some printers
~/helixscreen/moonraker-plugin/install.sh --auto
```

**Removing the old copy:** check where Moonraker loads the plugin from first:

```bash
# Moonraker's folder varies by platform too (~/moonraker on most systems)
readlink -f ~/moonraker/moonraker/components/helix_print.py
```

If the result points inside your HelixScreen folder and not into `~/helix_print`, the old copy is unused and you can delete it with `rm -rf ~/helix_print`. If it still points into `~/helix_print`, leave the folder alone and run the installer command above first.

---

### Creality K1: Creality Print can no longer find or connect to the printer

**Symptoms:**
- Creality Print stops discovering the printer on the LAN, and typing its IP directly does not work either
- The Creality Cloud app stops reaching the printer
- Port 80 on the printer is closed
- HelixScreen, Fluidd, Mainsail and Moonraker all work normally

**Cause:**
HelixScreen keeps the stock Creality backend (`master-server`, `app-server`, `web-server`) running beside the screen UI, so Creality Print and Creality Cloud work normally. If they cannot reach the printer, the backend is not running: usually the backend init script (`/etc/init.d/S99creality-backend`) failed to start. On a Simple AF install the stock stack was already disabled before HelixScreen arrived, so a lost Creality Print connection there predates HelixScreen. Full setup path and background: [K1 / K1C / K1 Max setup guide](../guide/install-k1.md).

**Fix:** update HelixScreen; the update installs and starts the backend script:

```bash
cp /usr/data/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --update
```

After the update (and after a reboot), `pidof web-server` should answer with a PID. If it does not, check `/etc/init.d/S99creality-backend` exists and start it with `/etc/init.d/S99creality-backend start`.

**If you would rather have the stock screen back:** uninstalling removes the backend script and re-enables the stock UI services it replaced:

```bash
cp /usr/data/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
```


---

### Adventurer 5M: the screen dims a few seconds after boot

**Symptoms:**
- Screen dims to ~10% brightness shortly after boot
- Happens about 3 seconds after Klipper starts

**Cause:**
ForgeX's `headless.cfg` has a `reset_screen` delayed_gcode that sets backlight to eco mode.

**Solution:**
The HelixScreen installer automatically patches /opt/config/mod/.shell/screen.sh to skip backlight commands when HelixScreen is running. If you installed manually or the patch didn't apply:

```bash
# Check if patch is present
grep helixscreen_active /opt/config/mod/.shell/screen.sh

# If not present, re-run installer or manually add after "backlight)" line:
#     if [ -f /tmp/helixscreen_active ]; then
#         exit 0
#     fi
```

### Adventurer 5M: black screen after boot

**Symptoms:**
- Display stays black
- SSH works, printer responds

**Causes:**
1. ForgeX not in GUPPY mode
2. GuppyScreen still running
3. Backlight not enabled

**Solutions:**

**Check ForgeX display mode:**
```bash
grep display /opt/config/mod_data/variables.cfg
# Should show: display = 'GUPPY'
```

**Verify GuppyScreen is disabled:**
```bash
ls -la /opt/config/mod/.root/S80guppyscreen
# Should NOT have execute permission (no 'x')
```

**Check that HelixScreen is running:**
```bash
/usr/sbin/chroot /data/.mod/.forge-x /etc/init.d/S90helixscreen status   # Forge-X
/etc/init.d/S80helixscreen status                        # Klipper Mod
tail /data/.helixscreen/logs/helix.log                   # structured app log
cat /opt/config/mod_data/helixscreen/logs/launcher.log   # Forge-X launcher capture
cat /root/printer_software/helixscreen/logs/launcher.log # Klipper Mod launcher capture
```

### Adventurer 5M: service commands

AD5M uses SysV init, not systemd. Commands are different. On Forge-X the service script is inside Forge-X's own system folder (`/data/.mod/.forge-x/etc/init.d/S90helixscreen`), not in the printer's main `/etc/init.d`, so it runs through `chroot`:

```bash
# Forge-X
/usr/sbin/chroot /data/.mod/.forge-x /etc/init.d/S90helixscreen start|stop|restart|status
cat /opt/config/mod_data/helixscreen/logs/launcher.log
tail -100 /data/.helixscreen/logs/helix.log    # structured app log

# Klipper Mod
/etc/init.d/S80helixscreen start|stop|restart|status
cat /root/printer_software/helixscreen/logs/launcher.log
tail -100 /data/.helixscreen/logs/helix.log
```

> The `launcher.log` file captures startup messages and crash output from the supervisor shell. The full structured app log (everything the app itself logs) goes to `/data/.helixscreen/logs/helix.log`, written directly to flash and rotated; `/var/log/messages` carries only the earliest startup output, before the app's own logging takes over. You usually want both when reporting an issue. 

### Adventurer 5M: SSH and SCP notes

AD5M's BusyBox has limitations:

```bash
# Use legacy SCP protocol (no sftp-server)
scp -O localfile root@<printer-ip>:/path/

# Use IP address, not hostname (mDNS may not resolve)
ssh root@192.168.1.67

# Extract zip archives with unzip
unzip archive.zip

# Alternative: use rsync if available
rsync -avz localfile root@<printer-ip>:/path/
```

#### Windows users: `scp -O` not supported

Windows 11's built-in OpenSSH does not support the `-O` flag. Use one of these alternatives:

1. **WSL (recommended)**, Open a WSL terminal (Ubuntu, Debian, etc.) and run all commands exactly as shown in the install guide. Everything works natively.

2. **WinSCP** (free GUI), Download from [winscp.net](https://winscp.net/). When connecting, set the protocol to **SCP** (not SFTP). Then drag and drop files to the printer.

3. **PuTTY pscp** (free command-line), Download from [putty.org](https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html). Use `pscp` instead of `scp -O`:
   ```
   pscp helixscreen-ad5m.zip root@<printer-ip>:/data/
   ```

### Adventurer 5M: the installer says Forge-X is not installed

**Symptoms:**
- Installer fails or skips ForgeX configuration
- HelixScreen runs but backlight doesn't work

**Solution:**
HelixScreen requires ForgeX to be installed first. Install ForgeX following [their instructions](https://github.com/DrA1ex/ff5m), verify GuppyScreen works, then run the HelixScreen installer.

### Adventurer 5M: going back to GuppyScreen

To go back to GuppyScreen:

```bash
cp /opt/config/mod_data/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --uninstall
reboot
```

The installer will not uninstall from inside its own folder, so it is copied to `/tmp` first. It puts back the display mode you had, re-enables GuppyScreen and restores Forge-X's backlight control. See [Uninstalling in the Adventurer 5M guide](../guide/install-ad5m.md#uninstalling).

---

### Checking for updates or changing the update channel

The channel (**Stable** or **Beta**) is under **Settings > Updates > Update Channel**. **Check for Updates** and **Install Update** are on the same page. See [Updates settings](../guide/settings/updates.md). Printers whose firmware manages HelixScreen show a notice there instead, and update through the firmware's own tools.
