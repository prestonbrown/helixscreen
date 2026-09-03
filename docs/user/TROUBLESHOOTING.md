# Troubleshooting Guide

Solutions to common problems with HelixScreen.

---

## Table of Contents

- [Quick Debugging Guide](#quick-debugging-guide)
- [Startup Issues](#startup-issues)
- [Connection Issues](#connection-issues)
- [Display Issues](#display-issues)
- [Touch Input Issues](#touch-input-issues)
- [Print Issues](#print-issues)
- [AMS/Multi-Material Issues](#amsmulti-material-issues)
- [Spoolman Issues](#spoolman-issues)
- [Calibration Issues](#calibration-issues)
- [Performance Issues](#performance-issues)
- [Configuration Issues](#configuration-issues)
- [Flashforge Adventurer 5M Issues](#flashforge-adventurer-5m-issues)
- [Gathering Diagnostic Information](#gathering-diagnostic-information)
  - [Enabling Debug Logging](#enabling-debug-logging)
  - [Collecting Logs](#collecting-logs)
- [Getting Help](#getting-help)

---

## Quick Debugging Guide

**Before troubleshooting anything, increase log verbosity. Three taps, no SSH:**

1. **Settings → System → Log Level → Debug** (or Trace for the deepest detail)
2. Reproduce the problem
3. **Settings → Help & About → Upload Debug Bundle** — collects the verbose log + system info and gives you a short share code to paste into a bug report
4. Set Log Level back to **Warn** when done — Debug and Trace add CPU and log volume

That's the path for almost everyone. Use the alternatives below only if you can't reach Settings.

**If the UI is broken or you need persistent verbose logging across reboots** — set the level in `helixscreen.env` (the launcher reads this on every start):

```
HELIX_LOG_LEVEL=debug    # trace, debug, info, warn, error, critical, off
```

The file lives at `<install dir>/config/helixscreen.env` (or `/etc/helixscreen/helixscreen.env`). Restart the service after editing:

```bash
sudo systemctl restart helixscreen        # Raspberry Pi
/etc/init.d/S99helixscreen restart        # K1 / K2
/etc/init.d/S80helixscreen restart        # AD5M (Klipper Mod)
/etc/init.d/S90helixscreen restart        # AD5M (Forge-X)
/etc/init.d/helixscreen restart           # CC1
<install dir>/config/helixscreen.init restart   # Snapmaker U1
```

Then tail the log:

```bash
sudo journalctl -u helixscreen -f         # Raspberry Pi (systemd)
tail -f /var/log/messages | grep helix    # AD5M / non-systemd
```

**Verbosity levels:**
- `warn` — production default (errors and warnings only)
- `info` — connection events, panel changes
- `debug` — detailed state changes, API calls
- `trace` — everything including LVGL internals

> **Remember to set it back to `warn`** when done — verbose logging impacts performance and log volume.

**Last-resort CLI:** if the service won't start at all, run the binary directly to see startup output: `~/helixscreen/bin/helix-screen -vv` (stop the service first so it doesn't fight for the framebuffer).

---

## Startup Issues

### Snapmaker U1: blank screen and off the network after a reboot

**Symptoms:**
- HelixScreen installed and worked, but after a reboot the screen is blank and the printer is unreachable over WiFi (Mainsail/Fluidd won't load).
- Most often seen right after upgrading the PAXX Extended Firmware, or on a fresh install of an older HelixScreen version on Extended Firmware 1.4.

**What happened:** A firmware upgrade clears HelixScreen's files (they live in a layer the upgrade resets), so it isn't installed anymore — and because HelixScreen manages the WiFi connection, nothing brings WiFi up. Older HelixScreen builds also failed to auto-start on Extended Firmware 1.4 specifically. Current HelixScreen auto-starts correctly on 1.2/1.3/1.4; the fix is to (re)install the current version.

**Fix:** Recover over a wired connection, then reinstall:

1. Plug a **USB-Ethernet adapter** into the printer and connect it to your router. The printer gets a wired IP automatically (check your router's client list).
2. `ssh root@<wired-ip>` (password `snapmaker`).
3. Reinstall HelixScreen:
   ```bash
   curl -sSL https://releases.helixscreen.org/install.sh | sh
   reboot
   ```
4. To instead go back to the stock screen, run the uninstaller: `curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall && reboot`.

See [INSTALL.md → Recovery](INSTALL.md#recovery-screen-is-blank-or-the-printer-is-off-the-network) for the full procedure and the manual reset fallback.

### Update failed in Mainsail — screen won't start after update (AD5X)

**Symptoms:**
- Mainsail's Update Manager showed an error toast like `Error updating helixscreen: [Errno 93] Directory not empty: PosixPath('/srv/helixscreen')`
- After the failed update, the touchscreen stays black or shows the splash and never starts
- Logs (if reachable) show repeated `[Watchdog] execv failed: No such file or directory` and `[helix-launcher] Exiting with code 42`

**What happened:** Moonraker's in-place update wiped part of `/srv/helixscreen` (including the `bin/helix-screen` binary) before something interrupted it. The install directory now has leftover files but no working binary, so the launcher can't start anything, and a retry from Mainsail trips over those leftovers.

**Fix:** Re-run the CLI installer from inside the ZMOD chroot — it cleans up the broken state and lays down a fresh install while preserving your settings. Full procedure: [UPGRADING.md → Adventurer 5X (ZMOD)](UPGRADING.md#quick-upgrade).

Quick form, from a Mainsail Shell or SSH:

```bash
ssh root@<printer-ip>
chroot /usr/data/.mod/.zmod
curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update
```

The `chroot` step is required — see UPGRADING.md for why.

### Binary won't start (GLIBC version not found)

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

**Fix — pick one:**

1. **Upgrade the OS to Bullseye or newer.** Best option on a general-purpose Pi. Current
   Raspberry Pi OS and MainsailOS images are already well past Bullseye, so a reflash
   solves it outright.

2. **Install the `cc1` package instead.** It is **statically linked** — it carries its own
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
above — new armv7 platforms are worth adding support for.

### HelixScreen crashes immediately (segfault)

**Symptoms:**
- Service starts then immediately exits
- "Segmentation fault" in logs
- Black screen, no UI appears

**Common causes:**

1. **Missing or corrupt assets** (use your actual install path)
   ```bash
   # Check assets exist (Pi: ~/helixscreen or /opt/helixscreen)
   ls -la ~/helixscreen/assets/
   ls -la ~/helixscreen/assets/fonts/
   ls -la ~/helixscreen/xml/
   ```

2. **Wrong display backend for your hardware**
   ```bash
   # Check what display devices exist
   ls -la /dev/fb*      # Framebuffer devices
   ls -la /dev/dri/*    # DRM devices

   # Pi 4 typically uses /dev/dri/card1
   # Pi 5 may use /dev/dri/card1 or card2
   # Flashforge Adventurer 5M (AD5M) uses framebuffer /dev/fb0
   ```

3. **Permission issues**
   ```bash
   # Check user is in required groups
   groups
   # Should include: video, input, render
   ```

### "No display device found"

**Symptoms:**
- Log shows "No suitable DRM device found" or similar
- Service keeps restarting

**Solutions:**

**Specify the DRM device explicitly:**
```json
// ~/helixscreen/config/settings.json (or /opt/helixscreen/config/)
{
  "display": {
    "drm_device": "/dev/dri/card1"
  }
}
```

**For Pi 5, try different cards:**
- `card0` = v3d (3D acceleration only, won't work)
- `card1` = DSI touchscreen
- `card2` = HDMI via vc4

### Logs are empty or not appearing

**Symptoms:**
- `journalctl -u helixscreen` shows nothing useful
- Log file doesn't exist

**Causes:**
1. Log destination misconfigured
2. Service not actually running
3. Crash before logging initializes

**Solutions:**

**Check service status first:**
```bash
sudo systemctl status helixscreen
```

**Force console logging for debugging:**
```bash
# Run manually to see all output (use your actual install path)
sudo ~/helixscreen/bin/helix-screen -vvv
```

**Check log destination in config:**
```json
{
  "log_dest": "auto",
  "log_level": "info"
}
```
Valid `log_dest` values: `auto`, `journal`, `syslog`, `file`, `console`

---

## Connection Issues

### "Cannot connect to Moonraker"

**Symptoms:**
- Red connection indicator on home screen
- "Disconnected" status
- Cannot control printer

**Causes:**
1. Wrong IP address or port
2. Moonraker not running
3. Firewall blocking connection
4. Network issues

**Solutions:**

**Check Moonraker is running:**
```bash
sudo systemctl status moonraker
```
If stopped: `sudo systemctl start moonraker`

**Verify the IP address:**
```bash
# On the Pi running Klipper
hostname -I
```
Update config with correct IP.

**Test connection manually:**
```bash
curl http://localhost:7125/printer/info
```
Should return JSON with printer info.

**Check firewall:**
```bash
sudo ufw status
# If active, allow Moonraker:
sudo ufw allow 7125/tcp
```

---

### "Connection lost" during print

**Symptoms:**
- Disconnect toast appears
- UI shows "Disconnected"
- Print continues (Klipper handles it)

**Causes:**
1. Network instability (WiFi)
2. Moonraker timeout
3. Power management issues

**Solutions:**

**Use Ethernet if possible** - wired connections are more reliable.

**Check WiFi signal strength:**
```bash
iwconfig wlan0 | grep -i signal
```

**Disable WiFi power management:**
```bash
sudo iw wlan0 set power_save off
```

To make permanent, add to `/etc/rc.local`:
```bash
iw wlan0 set power_save off
```

**Increase Moonraker timeouts:**
```json
{
  "printer": {
    "moonraker_connection_timeout_ms": 15000,
    "moonraker_request_timeout_ms": 60000
  }
}
```
Note: Values are in milliseconds (15000ms = 15 seconds).

---

### WiFi won't connect

**Symptoms:**
- WiFi setup in wizard fails
- Network not found
- Authentication failures

**Solutions:**

**Verify WiFi is working at OS level:**
```bash
nmcli device wifi list
```

**Check NetworkManager is running:**
```bash
sudo systemctl status NetworkManager
```

**Manual WiFi connection:**
```bash
sudo nmcli device wifi connect "YourSSID" password "YourPassword"
```

**For hidden networks:**
```bash
sudo nmcli device wifi connect "HiddenSSID" password "Password" hidden yes
```

> **Note:** Older guides may reference `wpa_supplicant` directly, but MainsailOS and most modern systems use NetworkManager. Use `nmcli` commands instead.

### WiFi permission denied

**Symptoms:**
- WiFi scan shows no networks (but WiFi works from command line)
- "Permission denied" error when connecting
- WiFi worked before an update but stopped working

**Cause:**
HelixScreen needs permission (via polkit rules) to manage WiFi through NetworkManager. These rules are installed automatically, but can be missing if:
- HelixScreen was installed before NetworkManager was set up
- A self-update couldn't install permission rules (runs with restricted privileges)
- The polkit rules file was deleted or corrupted

**Solutions:**

**Re-run the installer** (recommended — installs correct polkit rules):
```bash
curl -fsSL https://releases.helixscreen.org/install.sh | bash
```

**Verify polkit rules are installed:**
```bash
# Check for HelixScreen polkit rules (one of these should exist)
ls -la /etc/polkit-1/rules.d/50-helixscreen-network.rules
ls -la /etc/polkit-1/localauthority/50-local.d/helixscreen-network.pkla
```

**Check NetworkManager permissions:**
```bash
nmcli general permissions
```
If most entries show "no" instead of "yes", polkit rules are missing.

**Manual fix** (if re-running installer isn't possible):

Create `/etc/NetworkManager/conf.d/any-user.conf`:
```ini
[main]
auth-polkit=false
```
Then restart NetworkManager:
```bash
sudo systemctl restart NetworkManager
sudo systemctl restart helixscreen
```

> **Note:** The manual fix disables permission checks for all users. The installer method is preferred as it only grants access to the HelixScreen service user.

---

### SSL/TLS certificate errors

**Symptoms:**
- Connection fails with certificate errors
- Works with HTTP but not HTTPS

**Cause:**
System time is wrong, making SSL certificates appear invalid.

**Solution:**
```bash
# Check current time
date

# Sync time manually
sudo timedatectl set-ntp true

# Or force sync
sudo systemctl restart systemd-timesyncd
```

---

### When the Printer Errors or Disconnects (Recovery Dialog)

If Klipper shuts down, hits an error, or loses contact with the printer's control board, HelixScreen automatically pops up a full-screen **recovery dialog** with a warning icon, a short explanation, and one or more recovery buttons. You don't have to go looking for it — it appears on top of whatever you were doing.

**What it looks like and when it appears:**

| Dialog title | When it appears | What it usually means |
|--------------|-----------------|-----------------------|
| **Printer Shutdown** | Klipper has entered a shutdown state | An emergency stop was triggered, a thermal runaway was detected, or a configuration problem stopped the printer. When Klipper reports a specific reason (for example "Max force exceeded"), that exact message is shown instead of the generic text. |
| **Printer Error** | Klipper has entered an error state | Usually the control board (MCU) lost its connection, or there's a configuration error. |
| **Printer Firmware Disconnected** | The printer's firmware has disconnected from the host | The host software lost its link to the printer's control board. |

**The buttons and when to use each:**

| Button | What it does | When to use it |
|--------|--------------|----------------|
| **Restart Klipper** | Performs a soft restart of Klipper — it reloads your configuration and reconnects to the printer without power-cycling anything. | The quickest first thing to try after a **Printer Shutdown** or **Printer Error**. Good when nothing physical is wrong and you just need Klipper to come back to a ready state. |
| **Firmware Restart** | Does everything Restart Klipper does, and also resets the printer's control board (MCU) firmware. | Use when a plain Restart Klipper isn't enough — for example after fixing a configuration error, or when the control board itself shut down or lost communication. |
| **Dismiss** | Closes the dialog without doing anything. The printer stays exactly as it was — still shut down, errored, or disconnected. | When you want to read logs, check wiring, or fix a config file first, and you'll restart afterward. Dismiss does **not** fix anything on its own. |

> **Note:** When the printer is fully disconnected, HelixScreen can't send restart commands to it, so only **Dismiss** is available. Once the connection comes back, the restart buttons return.

**Common causes:**

- **Thermal runaway** — a heater isn't reaching or holding the temperature Klipper expects (loose heater or thermistor, a fan blowing on the sensor, or a failing part). Klipper shuts down for safety.
- **Configuration error** — a recent edit to your `printer.cfg` has a mistake. Klipper will shut down again immediately after a restart until the config is fixed.
- **Lost control-board communication** — a USB/serial cable came loose, the board lost power, or the connection was interrupted.

**Important:** A restart only sticks if the underlying problem is resolved. If a bad config or a wiring fault caused the shutdown, Klipper will just shut down again. Fix the root cause first — correct the `printer.cfg` (through Mainsail or Fluidd), reseat cables, or check heater and thermistor wiring — then use **Firmware Restart** to bring the printer back.

---

## Display Issues

### Black screen on startup

**Symptoms:**
- Display stays black
- Service shows running but no output

**Causes:**
1. Wrong display driver
2. Permission issues
3. Display not detected

**Solutions:**

**Check service is running:**
```bash
sudo systemctl status helixscreen
sudo journalctl -u helixscreen -n 50
```

**Identify your display hardware:**
```bash
# Framebuffer devices (older displays, Flashforge AD5M)
ls -la /dev/fb*

# DRM devices (Pi 4/5, modern displays)
ls -la /dev/dri/*
```

**Check display permissions:**
```bash
# User needs video group access
groups
# Should include 'video'
sudo usermod -aG video $USER
# Log out and back in for group change to take effect
```

**For DRM displays, specify device:**
```json
{
  "display": {
    "drm_device": "/dev/dri/card1"
  }
}
```

---

### Wrong screen size or resolution

**Symptoms:**
- UI too small or too large
- Partial screen visible
- Stretched or squished display

**Solutions:**

HelixScreen auto-detects resolution from DRM and framebuffer backends. If auto-detection picks the wrong resolution, override it with the `-s` flag:

```bash
# In helixscreen.service ExecStart, add -s with a named size or WxH:
ExecStart=/opt/helixscreen/bin/helix-launcher.sh -s large
# or: -s 1024x600
# Named sizes: micro, tiny, small, medium, large, xlarge
```

Then reload:
```bash
sudo systemctl daemon-reload
sudo systemctl restart helixscreen
```

---

### Resolution stuck at the wrong size, or `-s` has no effect

**Symptoms:**
- HelixScreen shows a "Cannot set HelixScreen to selected resolution" message on startup
- Display is locked at 800x480 (or similar) even though you passed `-s large` or `-s 1024x600`
- System journal contains `[DRM Backend]` or `[fbdev Backend]` warnings about the requested size

**Cause:** HelixScreen can only use resolutions the kernel exposes to it. If the kernel display driver is misconfigured, or if a fallback driver like `simpledrm` is active, the display is locked to whatever the bootloader programmed at power-on — HelixScreen cannot override this at runtime.

**Fix on Raspberry Pi / RatOS:**

1. Open the boot config:
   ```bash
   sudo nano /boot/firmware/config.txt
   ```

2. Make sure this line is present and not commented out:
   ```
   dtoverlay=vc4-kms-v3d
   ```

3. If you are using a specific HDMI mode (e.g., 1024x600), add the appropriate `hdmi_cvt` / `hdmi_group` / `hdmi_mode` lines per the Raspberry Pi HDMI documentation.

4. Reboot.

5. After reboot, verify the real DRM driver is now active:
   ```bash
   ls /sys/class/drm/
   ```
   You should see something like `card0-HDMI-A-1`. If you still see only `card0` and `dmesg | grep -i drm` mentions `simpledrm`, the vc4 overlay did not load — double-check /boot/firmware/config.txt for typos and any conflicting `dtoverlay` lines.

**Check what modes the kernel knows about:**
```bash
cat /sys/class/drm/card0-HDMI-A-1/modes
```
This lists the resolutions the DRM driver will accept for the `-s` flag.

---

### UI elements look too large or too small

**Symptoms:**
- Buttons, text, and spacing look oversized — controls feel cramped or run off the edge of the screen
- Or the opposite: everything looks tiny with lots of empty space, and touch targets are hard to hit
- The resolution is correct (the whole screen is used), but the *scale* of the interface looks wrong

**Cause:**
HelixScreen sizes spacing and padding from the display's DPI (dots per inch). On unusual or high-density panels the auto-detected DPI can be off, so the layout is scaled too large or too small.

**Fix:** Override the DPI in your `helixscreen.env` file (typically `~/helixscreen/config/helixscreen.env`), then restart HelixScreen. The default is `160`; valid range is `50` to `500`.

- **Everything too large / cramped / overflowing** — set a **lower** DPI:
  ```
  HELIX_DPI=100
  ```
- **Everything too small / lots of empty space** — set a **higher** DPI:
  ```
  HELIX_DPI=200
  ```

Restart after editing:
```bash
sudo systemctl restart helixscreen
```

Adjust in steps (e.g. 110, 100, 90 or 160, 200, 240) until the interface looks right. Lower DPI = tighter/smaller; higher DPI = larger/roomier.

> **Tip:** If instead the *whole layout tier* is wrong — for example a compact phone-style layout on a big screen, or vice versa — the resolution rather than the DPI is being mis-detected. Force a layout size with `HELIX_SCREEN_SIZE` (named preset `micro`/`tiny`/`small`/`medium`/`large`/`xlarge`, or `WxH` like `1024x600`), which is the persistent equivalent of the `-s` flag covered in [Wrong screen size or resolution](#wrong-screen-size-or-resolution).

---

### Ultrawide or portrait screen looks stretched, cramped, or clipped

**Symptoms:**
- On a very wide screen (e.g. 1920x480), panels look stretched with large empty gaps
- On a taller-than-wide screen (e.g. 480x800), content is cramped, clipped, or runs off the bottom

**This is expected — ultrawide and portrait layouts are alpha at best.**

HelixScreen detects both orientations and adjusts the navigation bar and grid sizing. Portrait is the better covered of the two: besides the app shell and navigation bar, the home dashboard, Print Status, Print Tune, Motion, Bed Mesh, the temperature graph and the Advanced panel's E-stop bar all rearrange themselves for a tall screen. Ultrawide has a home dashboard layout and nothing else. Any panel not in that list falls back to the standard landscape layout, which is what you are seeing on it. Neither orientation has been tested much on real hardware.

**The home dashboard is the exception, in both orientations.** Its grid is sized from the actual screen so the cells come out square — a 480x800 portrait panel gets 4 columns by 6 rows, a 320x1480 one gets 4x17, a 1920x440 ultrawide gets 23x5 — and portrait and ultrawide each have their own default layout rather than a stretched landscape one. Buttons, inputs, and headers on a portrait panel are sized from the screen's height, so they come out taller rather than cramped.

**What you can do:**

- **On a portrait panel, rotate it to landscape.** This is the well-tested path and what the Creality K2 does out of the box. Set `"rotate": 90` (or `270`) in the `display` section of your config — see [Display upside down or rotated](#display-upside-down-or-rotated).
- **On an ultrawide screen,** there is no better fallback today. Reducing DPI (`HELIX_DPI`) can claw back some usable density, but the layout will still be a landscape layout stretched wide.
- **Force the standard layout** if the alpha layout is worse than the fallback: `--layout standard`, or `"layout": "standard"` in the `display` section.

Contributions are very welcome here and only need XML, not C++ — see the [UI Contributor Guide](../devel/UI_CONTRIBUTOR_GUIDE.md).

---

### A widget disappeared from the home screen

**Symptoms:**

- A widget vanished from the home screen and did not come back after a restart or an update
- **Tips** in particular is missing on a portrait-mounted screen
- You saw a message like *"'Tips' removed — grid full"* even though the grid looked far from full
- You get a *"'Fan Speeds' removed — grid full"* message on **every single launch**, on a dashboard where the grid really is full

**Cause 1 - the widget was too wide for the grid (portrait screens).** On older versions, a widget that was wider than a portrait screen's grid could not be placed anywhere, so HelixScreen switched it off - and *saved* that off state to your settings. Fixing the placement logic does not undo the saved setting, so the widget stays off until you put it back yourself. It is not lost: it is sitting in the Widget Catalog as an available widget.

**Cause 2 - the grid was genuinely full.** On older versions, a widget that fit fine but found every cell taken was also switched off, and the message came back on every launch because the switch-off usually never made it to disk. That is fixed: a widget that only lacks a free cell now keeps its **enabled** setting and simply has no position, so it places itself again as soon as a cell frees up, and the message appears only when the widget was actually on your screen and lost its spot. If you are still seeing it repeat, you are on an older version - [update HelixScreen](UPGRADING.md).

If your dashboard is full and you want a specific widget back, make room for it: remove a widget you care less about, or move it to a second page (see [Multiple Pages](guide/home-panel.md#multiple-pages)).

**Tips** is the one that hits this most, because it is a wide widget and a portrait grid is narrow. On the shortest portrait screens Tips is also left out of the default layout on purpose, so if Tips is the only thing missing, that may be the default rather than a widget that got dropped.

**Fix — put back a single widget:**

1. **Long-press the widget grid** to enter Edit Mode
2. **Long-press an empty area** of the grid — the Widget Catalog opens
3. Tap the widget you want. Widgets already on your dashboard are dimmed and labelled "Placed"; the ones you lost will not be
4. Tap **Done** to leave Edit Mode

**Fix — put back everything at once:**

Enter Edit Mode and tap **Reset**. This restores the default layout *and* the default set of enabled widgets, and because the defaults are now portrait-aware you get the corrected portrait layout. Your per-widget settings (display mode preferences and so on) are preserved.

Reset is not free, though: it collapses **all pages back to a single page**, so any extra pages you created are removed, and every widget position and size goes back to default. If you have a layout you like and only lost one or two widgets, re-add them from the catalog instead.

See [Home Panel](guide/home-panel.md) for the full Edit Mode walkthrough.

---

### Display upside down or rotated

**Symptoms:**
- Content displayed at wrong angle
- Touch offset from visual

**Automatic detection:**

HelixScreen automatically detects display orientation on first boot using the kernel's panel orientation setting. If your display is physically mounted upside down and the kernel knows about it (via `panel_orientation=upside_down` in the kernel command line), HelixScreen detects this and applies the correct rotation automatically — both the splash screen and the main UI will appear right-side up.

On framebuffer displays only (e.g., AD5M, Allwinner-based devices — **not** Raspberry Pi), an interactive rotation wizard runs on first boot: it cycles through 0°, 90°, 180°, and 270° — tap the screen when the text appears right-side up, then tap again to confirm. This wizard is not available on Raspberry Pi or other DRM-based displays — use the kernel `panel_orientation` parameter or manual config instead.

The detected rotation is saved to the config file and applied on all subsequent boots.

**Setting panel orientation in the kernel (Raspberry Pi):**

Edit /boot/firmware/cmdline.txt and add a `video=` parameter for your display connector:

```
video=DSI-1:panel_orientation=upside_down
```

Valid orientations: `normal`, `upside_down`, `left_side_up`, `right_side_up`. HelixScreen reads this on startup and applies the corresponding rotation.

**Manual rotation:**

Edit your config file (typically `~/helixscreen/config/settings.json` or `/opt/helixscreen/config/settings.json`):

```json
{
  "display": {
    "rotate": 180
  }
}
```

Valid values: `0`, `90`, `180`, `270`. Restart HelixScreen after changing this value. Touch coordinates are automatically adjusted to match — no separate touch configuration is needed.

**How rotation works under the hood:**

When you set a rotation value, HelixScreen checks whether your display hardware supports rotating the image directly (hardware rotation). Most embedded displays — including DSI screens on Raspberry Pi — do not support hardware rotation for 180°.

When hardware rotation is not available, HelixScreen automatically switches from the GPU-accelerated DRM backend to the framebuffer (fbdev) backend, which handles software rotation flicker-free. This happens transparently — you don't need to configure anything. You'll see this in the logs:

```
DRM lacks hardware rotation for 180°, falling back to fbdev (flicker-free software rotation)
```

The fbdev backend with software rotation works well for normal UI usage. If you notice any issues, you can also force the fbdev backend manually:

```bash
sudo systemctl edit helixscreen
```

Add:
```ini
[Service]
Environment="HELIX_DISPLAY_BACKEND=fbdev"
```

Then restart: `sudo systemctl restart helixscreen`

**To re-run automatic detection:**

Remove the `rotate` and `rotation_probed` keys from your config file's `display` section, then restart HelixScreen:

```json
{
  "display": {
    // remove "rotate" and "rotation_probed" from here
  }
}
```

> **Note:** Old configs may have `"display_rotate": 180` at the root level. This is automatically migrated to the new format on startup.

---

### Colors are wrong (red and blue swapped)

**Symptoms:**
- Red images appear blue and blue images appear red
- Green colors display correctly
- UI elements with red/blue tints look "off"

**Cause:**
Your display's framebuffer uses BGR pixel order instead of RGB, but the kernel driver reports the wrong format. This is common on some Allwinner SoCs (H616, R818, etc.) with RGB parallel (40-pin) display interfaces.

HelixScreen auto-detects BGR layout from the kernel's `fb_var_screeninfo`, but some display drivers report incorrect pixel offsets. You can verify with:

```bash
fbset -i -fb /dev/fb0
```

Look at the `rgba` line. If it shows `8/16,8/8,8/0,0/24` (red at offset 16), the kernel claims RGB. If your colors are swapped despite this, the kernel is wrong and you need a manual override.

**Solution:**

Add this line to your `helixscreen.env` file (typically `~/helixscreen/config/helixscreen.env`):

```bash
HELIX_COLOR_SWAP_RB=1
```

Then restart HelixScreen:
```bash
sudo systemctl restart helixscreen
```

To disable the swap (if auto-detection is wrong in the other direction), use `HELIX_COLOR_SWAP_RB=0`.

**Verifying the fix:**
After restart, flags on the language selection screen should show correct colors (e.g., the French flag should be blue-white-red, not red-white-blue).

---

### Random solid colors during screen sleep (AD5X)

**Symptoms:**
- After the screen goes to sleep (idle timeout), the display shows a solid red, green, or blue fill instead of a black/off screen
- Color may differ each time the screen sleeps
- Touching the screen wakes it normally and the UI returns fine
- Only affects the FlashForge Adventurer 5X

**Cause:**
A driver-level quirk in the AD5X's Allwinner display pipeline. When HelixScreen tells the screen to sleep, the display controller on some AD5X units emits a solid primary-color fill instead of a blank frame. This is not a HelixScreen rendering bug — it does not affect printing, connectivity, or anything else. We have not yet been able to reproduce it reliably on our hardware, so there is no code fix available today.

**Workaround 1 — Disable screen sleep (simplest):**

1. Tap the gear icon to open **Settings**
2. Open **Display**
3. Set **Sleep** to **Never**

The screen will stay on continuously. Power the touchscreen off at the wall if you want it dark when not in use.

**Workaround 2 — Keep backlight on during sleep (preserves sleep logic):**

This keeps the normal sleep timeout but prevents the backlight from being cut, which avoids the color fill. The screen stays lit showing the last-drawn frame.

1. SSH into your printer (or open a shell on the AD5X directly)
2. Edit `settings.json`. On the AD5X (ZMOD firmware) the install lives under the ZMOD data directory, not `~/helixscreen` — the config file is at something like `/usr/data/.mod/.zmod/srv/helixscreen/config/settings.json` (or `/srv/helixscreen/config/settings.json` from inside the ZMOD chroot).
3. Find the `"display"` section and set:

   ```json
   "sleep_backlight_off": false
   ```

4. Save the file and restart HelixScreen. The AD5X uses ZMOD's SysV init, not systemd:

   ```bash
   /etc/init.d/S80helixscreen restart
   ```

   (or restart via the ZMOD launcher / Mainsail if you manage it that way)

Caveat: the panel stays fully lit 24/7 with this option. If long-term backlight wear is a concern, prefer Workaround 1 and manually power off the screen when not needed.

**Helping us fix it:**
If you are experiencing this and are willing to help, please send a debug bundle from **Settings → Help & About → Upload Debug Bundle**. Include a note that mentions the sleep color issue so we can correlate configs and logs.

---

### Brightness slider or screen dimming does nothing

**Symptoms:**
- Moving the brightness slider in Settings has no visible effect
- Auto-dim (screen sleep) never dims or blanks the backlight
- Brightness works in another UI (KlipperScreen, stock screen) but not HelixScreen

**Cause:**
HelixScreen didn't find a backlight control method it can drive on your hardware. It tries several methods automatically, but some panels — notably certain Creality Sonic Pad firmware variants — expose the backlight through a control path HelixScreen doesn't pick by default.

**Fix:** Force a specific backlight method by adding one line to your `helixscreen.env` file (typically `~/helixscreen/config/helixscreen.env`), then restart HelixScreen.

**Creality Sonic Pad** — if the slider and auto-dim don't respond even though the screen otherwise works, use Creality's `brightness` helper:
```
HELIX_BACKLIGHT_DEVICE=brightness
```

**Other hardware** — try each of these in turn, restarting after each, until the slider works:
```
HELIX_BACKLIGHT_DEVICE=sysfs       # standard Linux backlight (Raspberry Pi and most SBCs)
HELIX_BACKLIGHT_DEVICE=allwinner   # Allwinner-based panels (AD5M and similar)
```

**External monitor with its own brightness buttons** — if HelixScreen shouldn't touch the backlight at all (e.g. an HDMI monitor you dim with its own controls), disable control entirely:
```
HELIX_BACKLIGHT_DEVICE=none
```

Restart after editing:
```bash
sudo systemctl restart helixscreen
```
(Use your platform's restart command — see [Quick Debugging Guide](#quick-debugging-guide) for the SysV-init variants.)

> **Note:** Only the values `sysfs`, `allwinner`, `brightness`, and `none` are recognized. Any other value (including a `/sys/class/backlight/...` path) is ignored and HelixScreen falls back to auto-detection.

---

### 5GHz WiFi networks not showing

**Symptoms:**
- Only 2.4GHz WiFi networks appear in the network list
- 5GHz networks visible in KlipperScreen or other tools but not in HelixScreen
- WiFi adapter supports 5GHz (e.g., AP6256) but only 2.4GHz networks shown

**Cause:**
HelixScreen displays all networks returned by the WiFi subsystem (`wpa_supplicant` or `NetworkManager`). If 5GHz networks are missing, the issue is typically in the underlying WiFi configuration rather than HelixScreen itself.

**Solutions:**

**Check if your WiFi adapter sees 5GHz networks at the OS level:**
```bash
# For NetworkManager systems:
nmcli device wifi list

# For wpa_supplicant systems:
sudo wpa_cli scan
sudo wpa_cli scan_results
```

If 5GHz networks don't appear here either, the issue is in the WiFi driver or configuration.

**Verify 5GHz support is detected:**
```bash
iw phy phy0 info | grep -A 20 "Frequencies"
```
Look for frequencies above 5000 MHz (e.g., 5180, 5240).

**Check wpa_supplicant configuration:**
If your `wpa_supplicant.conf` has a `freq_list=` parameter that only lists 2.4GHz frequencies, 5GHz networks won't be scanned. Remove the `freq_list` line or add 5GHz frequencies.

**Check country code is set:**
5GHz channels require a regulatory domain. Without it, the kernel may block 5GHz scanning:
```bash
sudo iw reg get
```
If it shows "country 00", set your country code:
```bash
sudo iw reg set US   # Replace US with your country code
```

To make permanent, add `country=US` to `/etc/wpa_supplicant/wpa_supplicant.conf` or set `wifi.powersave = 2` in NetworkManager.

**After changing WiFi hardware** (e.g., swapping from AP6212 to AP6256), a reboot is recommended to ensure the correct driver and firmware are loaded.

---

## Touch Input Issues

### Touch not responding

**Symptoms:**
- Display works but touch doesn't
- No response to taps

**Causes:**
1. Touch device not detected
2. Wrong input device
3. Permission issues

**Solutions:**

**Check touch device exists:**
```bash
ls /dev/input/event*
cat /proc/bus/input/devices | grep -A5 -i touch
```

**Test touch input:**
```bash
# Install evtest if needed: sudo apt install evtest
sudo evtest /dev/input/event0
# Tap screen and watch for events
```

**Specify touch device in config:**

Edit `settings.json` in your install's `config/` directory (see [Config File Locations](guide/touch-calibration.md#config-file-locations) for the path on your platform). **Stop the service before editing** so the daemon doesn't overwrite your changes.

```json
{
  "input": {
    "touch_device": "/dev/input/event1"
  }
}
```

**Check permissions:**
```bash
ls -la /dev/input/event*
# User needs input group
sudo usermod -aG input $USER
# Log out and back in
```

---

### Touch Feel — Which Setting Do I Tune?

Three separate settings control the feel of taps vs. scrolls. Match the symptom to the right knob before changing anything — they have different effects and tuning the wrong one makes things worse.

| Symptom | What's happening | Setting to change | Direction |
|---|---|---|---|
| Stationary taps register as swipes/scrolls | Touch controller drifts a few pixels while finger is still, crossing the scroll threshold | `scroll_limit` | **Raise** (e.g., 15–20) |
| You scroll a list and a button in it fires mid-gesture | Finger released before moving far enough to commit to scroll, so the press becomes a click | `scroll_limit` | **Lower** (e.g., 5) |
| You scroll, lift your finger, and a button fires right as you lift | Touch controller reports release→re-press on lift-off | `scroll_guard` | **Set to `true`** |
| Lists feel sluggish — long coast after a flick | Scroll momentum decays too slowly | `scroll_throw` | **Raise** (e.g., 35) |
| Short flicks never travel far enough — list barely moves | Momentum decays too fast | `scroll_throw` | **Lower** (e.g., 15) |

All three live under `input` in `settings.json` (path varies by platform — see [Config File Locations](guide/touch-calibration.md#config-file-locations)). See [CONFIGURATION.md § Input Configuration](CONFIGURATION.md#input-settings) for the full reference.

> **Stop the service before editing `settings.json`** — the daemon rewrites the file periodically and your edits can be clobbered. Stop, edit, start.
>
> **Want to try a value before committing it?** All three are sliders under **Settings → System → Touch & Input** on the printer itself, so you can feel the change immediately and keep it only if it helps. `scroll_guard` and `scroll_limit` apply straight away; the panel prompts for a restart where one is needed.

FlashForge AD5M and AD5X presets ship with `scroll_guard: true` out of the box. Other platforms default to `false`.

---

### Taps Register as Swipes

**Symptoms:**
- Tapping buttons doesn't work — the screen scrolls instead
- Most or all taps are interpreted as swipe/scroll gestures
- Buttons only work when tapped very quickly and precisely

**Cause:** Noisy touch controller (common with Goodix GT9xx and similar capacitive controllers) reports jittery coordinates even when the finger is stationary. The small coordinate changes exceed LVGL's scroll detection threshold.

**Solution:** Raise `scroll_limit`, the distance a finger has to travel before a press is treated as a scroll. Putting it above the controller's drift means a stationary tap stays a tap:

```json
{
  "input": {
    "scroll_limit": 18
  }
}
```

Or set it from the printer itself: **Settings → System → Touch & Input → Scroll Engage Distance**.

Raising it too far makes real scrolls feel unresponsive, so move in steps of a few pixels.

### Unintended Clicks While Scrolling

**Symptoms:**
- You drag a list to scroll and a button in the middle of it fires instead
- The list jumps a little, but the click on whatever was under your finger still goes through
- Happens with short or slow swipes more than long, fast flicks

**Cause:** LVGL treats the first few pixels of finger movement as a "press" on the widget under your finger. Only after you've moved past `scroll_limit` pixels does it cancel the press and commit to a scroll. If you release before reaching that threshold, the press becomes a click.

**Solution:** Lower `scroll_limit` so scrolling engages sooner.

```json
{
  "input": {
    "scroll_limit": 5
  }
}
```

Default is 10; the UI accepts values from 1 to 20. Going too low will make intentional taps feel twitchy — any slight finger wobble will start a scroll — so settle on the lowest value that feels correct, not the smallest possible.

Note that this is a separate problem from the phantom click *after* a scroll (see below) and from taps being misread as swipes (see above).

### Accidental Button Presses After Scrolling

**Symptoms:**
- After scrolling a list, a button press fires when you lift your finger
- Unwanted actions triggered at the end of a scroll gesture

**Cause:** Some capacitive touch controllers generate a phantom "clicked" event when the finger is released after scrolling. Common on FlashForge AD5M/AD5X displays.

**Solution:** Enable the scroll guard, which ignores taps for 80 ms after a scroll ends:

```json
{
  "input": {
    "scroll_guard": true
  }
}
```

This is enabled by default on AD5M and AD5X via their hardware presets. If you see this on other hardware, enable it manually (or test with `HELIX_SCROLL_GUARD=1 helix-screen`).

**Still getting phantom clicks with the guard enabled?** The 80 ms cooldown works for most capacitive controllers but some need longer. Raise `scroll_guard_cooldown_ms` (range 20–500):

```json
{
  "input": {
    "scroll_guard": true,
    "scroll_guard_cooldown_ms": 150
  }
}
```

Or test temporarily with `HELIX_SCROLL_GUARD_COOLDOWN_MS=150 helix-screen`. Try 120, 150, then 200; stop at the smallest value that eliminates the phantom tap, since going higher will start swallowing legitimate taps that closely follow a scroll.

---

### Touch Input is Inaccurate

If taps are landing in the wrong place on screen:

1. **Visualize touch points:** Enable touch debug to draw a ripple at every touch point — easiest way to see if taps are landing where you think they are.

   **Persistent (recommended):** Add this line to `helixscreen.env` (path under [Config File Locations](guide/touch-calibration.md#config-file-locations)) and restart the service:
   ```
   HELIX_DEBUG_TOUCH=1
   ```
   Remove the line and restart when you're done.

   **One-shot from SSH** (stop the service first so it doesn't fight for the touch device):
   ```bash
   helix-screen --debug-touches
   ```
2. **Recalibrate from the UI:** Go to **Settings > System > Touch Calibration**.
3. **If the option isn't visible:** Your screen may not normally need calibration. SSH in, **stop the service**, then run:
   ```bash
   sudo systemctl stop helixscreen
   helix-screen --calibrate-touch
   # When done, start the service again:
   sudo systemctl start helixscreen
   ```
4. **If the screen is too broken to navigate (recommended path):** SSH in and set `HELIX_TOUCH_CALIBRATE=1` in your `helixscreen.env`, then restart the service. Remove the line once calibration succeeds — the env var does not self-clear.

For the full menu of options (env var, config-file `force_calibration`, manual CLI, factory reset) plus exact config-file paths for every platform, see the [Touch Calibration Guide § Forcing Recalibration](guide/touch-calibration.md#forcing-recalibration).

> **Don't hand-edit `settings.json` while the service is running** — the daemon rewrites the file periodically and your edits can be clobbered. Stop the service first, edit, then start it again.

---

### Touch is offset from visual elements

**Symptoms:**
- Touch registers in wrong location
- Have to tap above/below intended target
- Touch accuracy varies across the screen

**Causes:**
- Rotation mismatch between display and touch
- Uncalibrated touch screen

**Solutions:**

**1. Ensure rotation is set correctly:**

The `display.rotate` setting affects both display AND touch automatically. Make sure it matches your physical display orientation:

```json
{
  "display": {
    "rotate": 180
  }
}
```

Restart HelixScreen after changing. Touch coordinates rotate automatically to match — you should not need any separate touch axis configuration.

**2. Run touch calibration:**

1. Go to **Settings** (gear icon in sidebar)
2. Tap **Touch Calibration**
3. Tap the crosshairs that appear accurately
4. Calibration saves automatically

> **Note:** Touch Calibration option only appears on actual touchscreen hardware, not in desktop/SDL mode.

**3. Visualize touch points to diagnose:**

Enable `--debug-touches` to see exactly where touches register, then compare with where you're tapping:
```bash
helix-screen --debug-touches
```
Or set `HELIX_DEBUG_TOUCH=1` in your environment for persistent debugging.

**4. If calibration doesn't help:**

Try different `rotate` values (0, 90, 180, 270) until touch aligns with visuals. Or remove the rotation config entirely and restart to re-trigger automatic detection (see "Display upside down or rotated" above).

### Calibration doesn't help — touches still wildly off

**Symptoms:**
- Calibration wizard completes but touches still land far from where you tap
- Accuracy varies wildly across different screen regions
- Recalibrating multiple times doesn't improve things

**Cause:**
Some touchscreen controllers report X/Y axes that don't match the display orientation. The calibration math tries to compensate but produces a numerically unstable matrix — it technically "works" at the calibration points but falls apart everywhere else.

This is common on devices where the touch controller is mounted at a different orientation than the display panel (e.g., some Sonic Pad configurations).

**Solutions:**

**1. Recalibrate (recommended):**

HelixScreen automatically detects swapped touch axes during calibration and corrects them. Just recalibrate:
```bash
# Settings > System > Touch & Input > Touch Calibration
```

**2. Manual axis swap (fallback):**

If auto-detection doesn't resolve it, set the axis swap environment variable, then recalibrate:
```bash
# Add to your helixscreen.env:
HELIX_TOUCH_SWAP_AXES=1
```
Then restart HelixScreen and run the calibration wizard again. The swap is applied before calibration, so the resulting matrix will be clean and stable.

---

## Print Issues

### Files not appearing

**Symptoms:**
- Print Select shows empty
- "No files found"
- Known files missing

**Causes:**
1. Moonraker file access issue
2. Wrong file path
3. USB not mounted

**Solutions:**

**Check Moonraker file access:**
```bash
curl http://localhost:7125/server/files/list
```

**Verify gcodes directory:**
```bash
ls -la ~/printer_data/gcodes/
```

**For USB drives, check mount:**
```bash
mount | grep media
ls /media/usb/
```

---

### Print won't start

**Symptoms:**
- Tap Start but nothing happens
- Error message about prerequisites

**Causes:**
1. Klipper not ready
2. Temperature safety checks
3. Homing required

**Solutions:**

**Check Klipper state:**
```bash
curl http://localhost:7125/printer/info | jq '.result.state'
# Should be "ready"
```

**If "error" state, check Klipper logs:**
```bash
tail -50 ~/printer_data/logs/klippy.log
```

**Restart Klipper:**
```bash
sudo systemctl restart klipper
```

---

### Preparing takes long or seems stuck

**Symptoms:**
- The screen shows "Preparing Print" for many minutes after tapping Start Print
- The pre-print progress bar sits on one step
- Other interfaces (Mainsail, Fluidd) report the printer as idle or "complete" while HelixScreen shows preparing

**What's going on:**
The window between tapping Start Print and the first layer is doing real work — homing, heating the bed to soak temperature, bed mesh — and on some printers it regularly runs five to ten minutes (a K2 Plus with Auto Bed Mesh enabled takes seven to ten). Two things make it *look* stuck when it isn't:

- Part of this work can run on the printer's host before the job is formally handed over, so other interfaces may show the printer as idle or still "complete" for the whole block. HelixScreen tracks it as preparing regardless.
- A slow step is not a stuck step. As long as the printer is still moving and narrating what it's doing, HelixScreen keeps waiting — a long bed mesh is given the time it needs rather than cut off mid-sequence.

During this window the Print Status panel and the home panel's print card show the preparing job: the current step, a progress bar, and an estimate for the whole pre-print period.

**Solutions:**

**Check the printer is actually working.** Look at the machine — is the toolhead moving, the bed heating? Or open a web interface's console and watch for ongoing output. Motion and messages mean it's working, not stuck.

**Not willing to wait? Cancel — it's clean.** Cancel is always available during preparation, and cancelling there is a clean cancel, not a failed print: the print never starts, and if the printer is mid-way through a motion it finishes that move first. Then start again with the slow step switched off — see [Pre-Print Options](guide/printing.md#pre-print-options).

**If the printer has gone quiet** — no motion, no console output, for a good while — it may genuinely be stuck. Collect a [debug bundle](#collecting-logs) and check the Klipper log for the macro that was running.

---

### Can't pause or cancel print

**Symptoms:**
- Buttons don't respond
- Print continues despite tapping Cancel

**Causes:**
1. Connection issue during print
2. Klipper busy processing

**Solutions:**

**For emergency, use the E-Stop button** - it appears in the header of most panels while a print is active, as well as on the home screen.

**Check connection status** - if disconnected, wait for reconnection.

**Via terminal:**
```bash
curl -X POST http://localhost:7125/printer/print/cancel
```

---

### Power cut out during a print

**Symptoms:**
- After power comes back and HelixScreen reconnects, a dialog asks: **Resume interrupted print?**
- The body names the file when the printer reported it ("The printer lost power while printing <name>.") or describes it generically

**What's going on:**
On printers whose firmware saves recovery data when power drops mid-print — the Creality models with recovery support (K1 family, K2, Ender 3 V3 and siblings) and the Snapmaker U1 — that data survives the reboot. When HelixScreen connects and finds it, it asks what you want to do rather than deciding for you. Creality printers get an extra-honest wording ("The resumed layer may not line up exactly.") because their recovery re-homes without re-probing the bed — that is a real property of the resume, not a malfunction.

**Your choices:**

| Choice | What happens |
|--------|--------------|
| **Resume** | The printer continues the interrupted print from where its firmware saved its progress. On Creality, check the first resumed layer before walking away — it can sit a millimetre or two off. |
| **Discard** | The recovery data is cleared and nothing is printed. The printer is back to a clean slate; start whatever you like. |
| Dismiss the dialog (tap outside it) | Nothing is decided: the recovery data is kept, and you are asked again the next time HelixScreen connects. |

The offer never appears on top of a print you have already started — if you tap Start Print before answering, the question waits until that job is done. On printers without firmware-side recovery data there is nothing to find, so the dialog never appears there; a power cut simply means starting the print again.

---

### Layer count is wrong, stuck at 0, or total layers missing

**Symptoms:**
- The layer counter on the Print Status panel doesn't match the real layer being printed
- Total layers shows as missing, 0, or a placeholder
- The current layer looks like a rough guess rather than an exact count

**Cause:**
For an exact layer count, HelixScreen reads the current and total layer directly from Klipper. Those values are only available if your slicer tells Klipper about them by emitting the `SET_PRINT_STATS_INFO` command in the printed G-code. Many stock slicer profiles don't do this. When the values are absent, HelixScreen falls back to *estimating* the layer from print progress and Z-height, which is close but not always exact.

**Fix — have your slicer emit the layer info:**

In **PrusaSlicer, SuperSlicer, or OrcaSlicer**, add these to your printer's custom G-code (Printer Settings → Custom G-code):

- **Start G-code** — report the total layer count once, at the start:
  ```
  SET_PRINT_STATS_INFO TOTAL_LAYER=[total_layer_count]
  ```
- **Before layer change G-code** (some slicers call it "Layer change G-code") — report each new layer as it begins:
  ```
  SET_PRINT_STATS_INFO CURRENT_LAYER={layer_num + 1}
  ```

Re-slice your file after adding these — the commands are baked into the G-code at slice time, so existing files won't have them. Once present, HelixScreen shows the exact layer and total for every print.

> **Cura users:** Cura doesn't expose these layer placeholders directly and needs a post-processing script to inject `SET_PRINT_STATS_INFO`. See the Klipper community docs and forums for a Cura post-processing plugin that adds it.

> **Note:** Some noise in the layer count during the print-start phase (bed mesh, purge/prime line, Z-hop) is normal. HelixScreen holds the layer at 0 until real printing begins, so it will not jump ahead before the first layer. The estimate only matters mid-print when the slicer macros above are missing.

### Time remaining is inaccurate or slow to settle

**Symptoms:**
- The estimated time remaining is off, especially early in a print
- The ETA jumps around or takes a while to steady out during the first layers
- Time remaining doesn't track the slicer's estimate

**Cause:**
Time remaining is most accurate when your slicer reports layer information to Klipper via `SET_PRINT_STATS_INFO` (the same commands as the layer-count fix above). Without it, HelixScreen estimates progress from other signals, which are noisier at the very start of a print before enough data has accumulated.

**Fix:**
Add the `SET_PRINT_STATS_INFO` commands described in [Layer count is wrong, stuck at 0, or total layers missing](#layer-count-is-wrong-stuck-at-0-or-total-layers-missing) above, then re-slice. Expect some ETA drift during the print-start phase (bed mesh, priming) even with the macros in place — the estimate tightens up once printing is underway.

---

## AMS/Multi-Material Issues

### AMS slots not detected

**Symptoms:**
- AMS panel shows no slots
- "No AMS detected" message

**Causes:**
1. Backend not configured in Klipper
2. Wrong backend type detected
3. Backend not initialized

**Solutions:**

**Verify backend is running:**
```bash
# For Happy Hare - check if mmu object exists
curl -s http://localhost:7125/printer/objects/list | grep -i mmu

# For AFC-Klipper - check if AFC object exists
curl -s http://localhost:7125/printer/objects/list | grep -i afc
```

**Check Klipper logs:**
```bash
grep -i "mmu\|afc\|ams" ~/printer_data/logs/klippy.log | tail -20
```

**Restart services:**
```bash
sudo systemctl restart klipper
sudo systemctl restart moonraker
sudo systemctl restart helixscreen
```

### CFS shows no slots (Creality K2)

**Symptoms:**
- The filament panel opens but the CFS is empty — no bays, no spools
- Your CFS is populated and works fine in Fluidd or Mainsail
- The home screen's multi-filament widget is greyed out or missing

**Cause:** you're most likely running **community firmware** whose CFS module was rewritten from scratch. It reports the CFS in a completely different format than Creality's, and HelixScreen 0.99.106 and earlier only understood Creality's.

**Check which one you have:**
```bash
curl -s 'http://localhost:7125/printer/objects/query?box' | grep -o 'slots\|T1'
```

- Prints `T1` → stock Creality format. This isn't your problem; work through *AMS slots not detected* above.
- Prints `slots` → community format. **Update HelixScreen to any release newer than 0.99.106** and your slots will appear.

Once updated, the community format is fully supported — display, loading, unloading and filament changes all work, with nothing to configure.

> **If your firmware installed HelixScreen for you**, its installer may pin an older version than the one that added this support. Updating HelixScreen through Fluidd or Mainsail (rather than reinstalling the firmware) picks up the newer release.

### Load/Unload fails

**Symptoms:**
- Load command sent but no filament movement
- Error messages in notification history

**Solutions:**

**Check filament path:**
Ensure no physical obstructions and buffer tubes are connected.

**Verify homing:**
Run home operation first - many load/unload macros require homing.

**Check temperatures:**
Some backends require extruder at temperature before operations.

---

## Spoolman Issues

### Spoolman not showing

**Symptoms:**
- No Spoolman option in AMS panel
- Spool picker not available

**Causes:**
1. Spoolman not configured in Moonraker
2. Spoolman service not running
3. Connection timeout

**Solutions:**

**Check Spoolman configuration** in `moonraker.conf`:
```ini
[spoolman]
server: http://localhost:7912
```

**Verify Spoolman is running:**
```bash
curl http://localhost:7912/api/v1/health
```

**Restart services:**
```bash
sudo systemctl restart spoolman
sudo systemctl restart moonraker
```

### Spool data not syncing

**Solutions:**

**Force refresh:**
Navigate away from and back to the AMS panel to trigger refresh.

**Check Moonraker logs:**
```bash
sudo journalctl -u moonraker | grep -i spoolman
```

### Only some spools showing in Spoolman lists

**Symptoms:**
- Spool picker or Spoolman panel only shows a subset of your spools
- Missing spools that exist in Spoolman

**Cause:**
HelixScreen currently fetches up to 1,000 spools from Spoolman in a single request. If you have more than 1,000 spools, the rest will not appear.

**Workaround:**
Archive or delete unused spools in Spoolman to stay under 1,000 active spools.

---

## Calibration Issues

### Input Shaper measurement fails

**Symptoms:**
- Measurement starts but errors out
- "ADXL not found" error

**Causes:**
1. Accelerometer not connected
2. SPI/I2C configuration issue
3. Klipper input_shaper section missing

**Solutions:**

**Verify ADXL connection via Klipper console:**
```bash
# In Mainsail/Fluidd console, or via:
curl -X POST http://localhost:7125/printer/gcode/script \
  -d '{"script": "ACCELEROMETER_QUERY"}'
```
Should return acceleration values, not an error.

**Check Klipper config** for `[adxl345]` or `[lis2dw12]` section.

**Re-run calibration** after fixing hardware issues.

### Screws tilt shows wrong adjustments

**Symptoms:**
- Adjustment values seem incorrect
- Bed gets worse after adjustments

**Solutions:**

**Verify screw positions** in `printer.cfg`:
```ini
[screws_tilt_adjust]
screw1: 30,30       # Front-left
screw1_name: front left
```

**Check probe accuracy:**
```bash
# In Klipper console:
PROBE_ACCURACY
```
Standard deviation should be < 0.01mm.

---

## Performance Issues

### UI feels slow or laggy

**Symptoms:**
- Delayed response to touches
- Choppy scrolling
- Slow panel transitions

**Diagnosis:**

```bash
# Check CPU and memory
top -b -n 1 | head -20

# Check if swapping (very slow on SD card)
free -h

# Check HelixScreen memory usage
ps aux | grep helix-screen
```

**Common causes and fixes:**

| Cause | Fix |
|-------|-----|
| Debug mode in production | Remove `-vv`/`-vvv` from service, don't use `--test` |
| Animations on slow hardware | Settings → Display & Sound → disable Animations |
| Too many G-code files | Large directories with thumbnails use more RAM |
| Other processes hogging CPU | Check `top` for culprits |
| Swapping to SD card | Reduce memory usage or add swap to USB |
| Hardware issues | Settings → Hardware & Devices → Hardware Issues - check for problems |

**To disable verbose logging:**

Edit the service override:
```bash
sudo systemctl edit --force helixscreen
# Remove any -vv or -vvv flags
sudo systemctl daemon-reload
sudo systemctl restart helixscreen
```

---

### High memory usage

**Symptoms:**
- Out of memory errors
- System becomes unresponsive
- Crashes during complex operations

**Solutions:**

**Check memory usage:**
```bash
free -h
```

**Reduce Moonraker cache** in `moonraker.conf`:
```ini
[file_manager]
queue_gcode_uploads: False
```

**Limit print history:**
```ini
[history]
max_job_count: 100
```

---

## Configuration Issues

### Wrong printer model identified

**Symptoms:**
- HelixScreen identifies the printer as the wrong model/type (for example, a Voron showing as "FlashForge Adventurer 5M Pro")
- Changing the printer image in Printer Manager changes the picture but not the model — features, calibration dialogs, and the name still follow the wrong type

**What's going on:**
- The **printer type** (the model picked during setup) drives the name, image, bed size, probe type, and preset options. The image picker in Printer Manager is cosmetic only — it never changes the type.
- **Device-specific install packages** (Creality K1, FlashForge Adventurer 5M, and similar) run a preset-mode setup that *skips printer identification entirely*: the type comes from the install package itself, not from detection. No setting can override it.
- On **generic installs**, auto-detection either picked a wrong near-relative from the database, or — when it wasn't confident enough — deliberately left the type empty for you to choose rather than guess.

**Solutions:**

**First, figure out which situation you're in.** What did you install, and what is HelixScreen running on? If a device-specific preset package doesn't match the machine (or its screen), that's the cause — detection never ran. Install the HelixScreen package built for your hardware, or use the [remote screen setup](INSTALL.md#remote-screen-setup-run-on-a-separate-device) on a Pi/PC/tablet pointed at your printer's Moonraker — generic installs run full auto-detection.

**If the saved model is wrong (generic install), correct it in Printer Manager — nothing gets wiped:**

1. Tap the **printer image** on the Home Panel to open the Printer Manager
2. Tap the **printer model** row — the model name directly below the printer name, marked with a pencil icon ("Printer model (click to correct)")
3. Pick your model from the list (Voron 2.4, Voron 0.2, Voron Trident, and Voron Switchwire are all in the database)
4. The new type applies immediately — name, image, and all the type-driven features follow it

**Let HelixScreen catch it for you.** If you'd rather not hunt through the list, just connect the printer and wait: when detection is confident the saved type is wrong, a **Printer type mismatch** dialog names both models and offers **Choose Model** (opens the model picker from the setup wizard's identity step) or **Keep current** — the right answer for a heavily modified printer that legitimately differs from its stock sibling. Picking **Keep current** is remembered for that type; the prompt won't nag on every boot.

Re-adding the printer through **Printer Manager > Manage Printers > + Add Printer** (then deleting the old entry) and **Settings > System > Factory Reset** remain as last resorts — the factory reset re-runs the full wizard but wipes all HelixScreen settings, so use it only if you want a clean start anyway.

If your model isn't in the database, leave it on the detected/generic profile — everything still works; you can rename the printer and set any image from Printer Manager.

### First-run wizard keeps appearing

**Symptoms:**
- Wizard shows on every boot
- Settings not saved

**Causes:**
- Config file missing, invalid, or not writable
- `wizard_completed` flag not set

**Solutions:**

**Check config exists and is valid JSON** (use your actual install path):
```bash
cat ~/helixscreen/config/settings.json | jq .
```

If the file is missing or invalid, the wizard will run. After completing the wizard, verify:
```bash
grep wizard_completed ~/helixscreen/config/settings.json
# Should show: "wizard_completed": true
```

**Check config directory is writable:**
```bash
ls -la ~/helixscreen/config/
# The helixscreen process needs write access
```

**Create fresh config from template:**
```bash
cp ~/helixscreen/config/settings.json.template \
   ~/helixscreen/config/settings.json
```

> **Note:** Copying the template creates a valid config but with `wizard_completed: false`, so the wizard will still run once to configure your printer.

---

### Settings not saving

**Symptoms:**
- Changes revert after restart
- Config file unchanged

**Solutions:**

**Check config directory is writable:**
```bash
# Test write access (use your actual install path)
touch ~/helixscreen/config/test && rm ~/helixscreen/config/test
echo "Write OK"
```

**Check disk space:**
```bash
df -h
```

**Check for filesystem errors:**
```bash
dmesg | grep -i "read.only\|error\|fault"
```

**Try manual edit to verify:**
```bash
# Stop the service first so it doesn't overwrite your edit:
sudo systemctl stop helixscreen
# Edit settings.json — path varies by platform; see:
#   docs/user/guide/touch-calibration.md § Config File Locations
sudo nano <path-to-your-settings.json>
# Save, then start the service:
sudo systemctl start helixscreen
# Check if change persisted
```

---

### Wrong printer detected

**Symptoms:**
- Wizard shows wrong printer model
- Wizard asks you to pick a model instead of choosing one automatically
- Features missing or wrong

**What's going on:**
Auto-detection only commits to a model when it is confident enough. Below that bar it deliberately leaves the type empty and asks you to pick — that's detection declining to guess, not a failure. When it *is* confident it can still land on a near-relative of your actual machine.

**Solutions:**

**In the wizard:** pick your model by hand at the **Printer Setup: Identity** step. The full database is there.

**After setup:** if the wrong model got saved, correct it from Printer Manager — tap the printer image on the Home Panel, then the **printer model** row underneath the printer name, and pick the right model. It applies immediately, with nothing wiped. On the next connect, HelixScreen may also flag the mismatch itself and offer **Choose Model** — see [Wrong printer model identified](#wrong-printer-model-identified) above for that flow.

---

## Flashforge Adventurer 5M Issues

The Flashforge Adventurer 5M (AD5M) has unique characteristics due to its embedded Linux environment and ForgeX/Klipper Mod firmware.

### Screen dims after a few seconds

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

### Black screen after boot

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

**Check HelixScreen is running:**
```bash
/etc/init.d/S90helixscreen status
cat /opt/helixscreen/logs/launcher.log    # AD5M launcher capture
```

### Service commands (SysV init)

AD5M uses SysV init, not systemd. Commands are different:

```bash
# Forge-X
/etc/init.d/S90helixscreen start|stop|restart|status
cat /opt/helixscreen/logs/launcher.log
grep helix-screen /var/log/messages | tail -100    # structured app log

# Klipper Mod
/etc/init.d/S80helixscreen start|stop|restart|status
cat /opt/helixscreen/logs/launcher.log
grep helix-screen /var/log/messages | tail -100
```

> The `launcher.log` file captures startup messages and crash output from the supervisor shell. The full structured app log (everything the app itself logs) goes to the system log (`/var/log/messages`). You usually want both when reporting an issue. On pre-v0.99.62 installs the launcher log lived at `/tmp/helixscreen.log` — check that path if `launcher.log` doesn't exist.

### SSH/SCP notes

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

1. **WSL (recommended)** — Open a WSL terminal (Ubuntu, Debian, etc.) and run all commands exactly as shown in the install guide. Everything works natively.

2. **WinSCP** (free GUI) — Download from [winscp.net](https://winscp.net/). When connecting, set the protocol to **SCP** (not SFTP). Then drag and drop files to the printer.

3. **PuTTY pscp** (free command-line) — Download from [putty.org](https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html). Use `pscp` instead of `scp -O`:
   ```
   pscp helixscreen-ad5m.zip root@<printer-ip>:/data/
   ```

### ForgeX not installed

**Symptoms:**
- Installer fails or skips ForgeX configuration
- HelixScreen runs but backlight doesn't work

**Solution:**
HelixScreen requires ForgeX to be installed first. Install ForgeX following [their instructions](https://github.com/DrA1ex/ff5m), verify GuppyScreen works, then run the HelixScreen installer.

### Restoring GuppyScreen

To go back to GuppyScreen:

```bash
# Automated (recommended)
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | bash -s -- --uninstall

# Manual
/etc/init.d/S90helixscreen stop
rm /etc/init.d/S90helixscreen
rm -rf /opt/helixscreen
chmod +x /opt/config/mod/.root/S80guppyscreen
chmod +x /opt/config/mod/.root/S35tslib
reboot
```

---

## Gathering Diagnostic Information

When reporting issues, gather this information. **Most importantly, enable debug logging first** so the logs contain enough detail to diagnose the problem.

### Enabling Debug Logging

By default, HelixScreen only logs warnings and errors. To capture useful diagnostic information, you need to temporarily enable debug-level logging, reproduce the problem, then collect the logs.

**Quickest method:** Go to **Settings > System > Log Level** and select **Debug**. This takes effect immediately with no restart needed. Remember to set it back to **Warn** when done.

**Verbosity levels:**
| Flag | Level | What it captures |
|------|-------|-----------------|
| *(none)* | WARN | Errors and warnings only (production default) |
| `-v` | INFO | Connection events, panel changes, milestones |
| `-vv` | DEBUG | State changes, API calls, component init (**use this for bug reports**) |
| `-vvv` | TRACE | Everything including LVGL internals (very verbose, rarely needed) |

#### MainsailOS / Raspberry Pi (systemd)

**Option A: Temporary override (recommended)**
```bash
# Create a service override that adds debug logging
sudo systemctl edit --force helixscreen
```

Add these lines (replace the path with your actual install location):
```ini
[Service]
ExecStart=
ExecStart=/home/biqu/helixscreen/bin/helix-launcher.sh --debug
```

Then restart:
```bash
sudo systemctl daemon-reload
sudo systemctl restart helixscreen
```

**Option B: One-shot manual run**

Stop the service and run manually with console output:
```bash
sudo systemctl stop helixscreen
cd ~/helixscreen   # or /opt/helixscreen
sudo ./bin/helix-launcher.sh --debug --log-dest=console
# Reproduce the issue, then Ctrl+C to stop
```

**Option C: Environment variable**

Add to the service file:
```ini
[Service]
Environment="HELIX_LOG_LEVEL=debug"
```

#### Flashforge Adventurer 5M / Forge-X (SysV init)

```bash
# Stop the running service
/etc/init.d/S90helixscreen stop   # or S80helixscreen for Klipper Mod

# Run manually with debug output
cd /opt/helixscreen
./bin/helix-launcher.sh --debug --log-dest=console 2>&1 | tee /tmp/helix-debug.log
# Reproduce the issue, then Ctrl+C to stop

# Restart the service normally when done
/etc/init.d/S90helixscreen start
```

#### After collecting logs

**Remove the debug override** to restore normal performance:

```bash
# MainsailOS: remove the override
sudo systemctl revert helixscreen   # or: sudo rm /etc/systemd/system/helixscreen.service.d/override.conf
sudo systemctl daemon-reload
sudo systemctl restart helixscreen
```

> **Important:** Debug logging increases CPU usage and log volume. Don't leave it enabled in production.

### System Information

```bash
# HelixScreen version (use your actual install path)
~/helixscreen/bin/helix-screen --version

# OS version
cat /etc/os-release

# Hardware
cat /proc/cpuinfo | grep Model
free -h
```

### Collecting Logs

**MainsailOS (systemd):**
```bash
# Recent logs (last 200 lines, with timestamps)
sudo journalctl -u helixscreen -n 200 --no-pager -o short-iso

# Logs since last restart
sudo journalctl -u helixscreen --since "$(systemctl show helixscreen --property=ActiveEnterTimestamp --value)" --no-pager

# Errors only (useful for a quick summary)
sudo journalctl -u helixscreen -p err --no-pager

# Follow live (while reproducing the issue)
sudo journalctl -u helixscreen -f
```

**Flashforge Adventurer 5M (SysV init, BusyBox syslog):**

Two streams to collect — both are needed when reporting an issue:

```bash
# 1) Structured app log (everything from spdlog: connection events, errors, etc.)
grep helix-screen /var/log/messages | tail -200

# 2) Launcher / supervisor capture (startup banner, crash output, glibc abort messages)
tail -200 /opt/helixscreen/logs/launcher.log    # pre-v0.99.62 installs: /tmp/helixscreen.log

# Follow the system log live while reproducing the issue
tail -f /var/log/messages | grep helix-screen
```

**Creality K1 / K1C / K2 (BusyBox in-memory syslog):**
```bash
# Structured app log — held in a RAM ring buffer, vanishes on reboot
logread | grep helix-screen | tail -200

# Launcher / supervisor capture
tail -200 /usr/data/helixscreen/logs/launcher.log
```

**Flashforge AD5X (ZMOD MIPS):**
```bash
# Structured app log — the one you almost always want
tail -200 /opt/config/mod_data/log/helix.log

# Launcher / crash-stderr capture, written by ghzserg's S80helixscreen
tail -200 /opt/config/mod_data/log/helixscreen.log

# Same two files, alternate path spelling (/opt/config is a bind-mount)
tail -200 /usr/data/config/mod_data/log/helix.log

# Older installs, before the app log moved under /opt/config
tail -200 /data/helixscreen/logs/helix.log
tail -200 /srv/helixscreen/logs/launcher.log

logread | grep helix-screen | tail -200
```

**Snapmaker U1:**
```bash
grep helix-screen /var/log/messages | tail -200
tail -200 /var/log/helixscreen/launcher.log
```

**Elegoo Centauri Carbon (COSMOS):**
```bash
logread | grep helix-screen | tail -200
tail -200 /user-resource/helixscreen/logs/launcher.log
```

### Configuration

```bash
# Current config (sanitize API keys before sharing!)
# Pi: ~/helixscreen/config/ or /opt/helixscreen/config/
cat ~/helixscreen/config/settings.json
```

### Display Information

```bash
# Framebuffer
ls -la /dev/fb*

# DRM devices
ls -la /dev/dri/

# Input devices
ls -la /dev/input/
cat /proc/bus/input/devices
```

---

## Getting Help

### Check Existing Resources

1. **This troubleshooting guide** - search for your symptoms
2. **[FAQ](FAQ.md)** - common questions
3. **[GitHub Issues](https://github.com/prestonbrown/helixscreen/issues)** - known problems
4. **[HelixScreen Discord](https://discord.gg/RZCT2StKhr)** - ask the community for help

### Opening a New Issue

If you can't find a solution, open a GitHub issue with:

**Required Information:**
- HelixScreen version (`helix-screen --version`)
- Hardware (Pi model, display type)
- What you expected to happen
- What actually happened
- Steps to reproduce

**Helpful Additions:**
- Debug log output ([enable debug logging first](#enabling-debug-logging), then reproduce the issue)
- Screenshots if visual issue
- Config file (remove API keys/sensitive data)

**Example Issue Format:**

```markdown
## Environment
- HelixScreen version: 0.99.111
- Hardware: Raspberry Pi 4 4GB
- Display: Official 7" touchscreen
- OS: MainsailOS 1.2.0

## Problem
Cannot connect to printer after WiFi change.

## Expected
Should connect to Moonraker on 192.168.1.100

## Actual
Shows "Connection failed" error

## Steps to Reproduce
1. Change WiFi network
2. Update config with new IP
3. Restart helixscreen service
4. See error

## Logs
```
[error] [Moonraker] Connection refused: 192.168.1.100:7125
```

## Configuration
```json
{
  "printer": {
    "moonraker_host": "192.168.1.100",
    "moonraker_port": 7125
  }
}
```
```

---

*Back to: [User Guide](USER_GUIDE.md) | [Installation](INSTALL.md)*
