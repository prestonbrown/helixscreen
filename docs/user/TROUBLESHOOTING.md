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
- [Adventurer 5M Issues](#adventurer-5m-issues)
- [Gathering Diagnostic Information](#gathering-diagnostic-information)
  - [Enabling Debug Logging](#enabling-debug-logging)
  - [Collecting Logs](#collecting-logs)
- [Getting Help](#getting-help)

---

## Quick Debugging Guide

**Before troubleshooting anything, increase log verbosity:**

```bash
# MainsailOS - edit service temporarily
sudo systemctl edit --force helixscreen
# Add these lines (use your actual install path):
[Service]
ExecStart=
ExecStart=~/helixscreen/bin/helix-launcher.sh -vv

# Then restart
sudo systemctl daemon-reload
sudo systemctl restart helixscreen

# Watch logs
sudo journalctl -u helixscreen -f
```

**Verbosity levels:**
- No flag: WARN only (production default)
- `-v`: INFO - connection events, panel changes
- `-vv`: DEBUG - detailed state changes, API calls
- `-vvv`: TRACE - everything including LVGL internals

**Remember to remove `-vv` after debugging** - verbose logging impacts performance.

---

## Startup Issues

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
   # AD5M uses framebuffer /dev/fb0
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
// ~/helixscreen/config/helixconfig.json (or /opt/helixscreen/config/)
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
// /opt/helixscreen/config/helixconfig.json
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
# Framebuffer devices (older displays, AD5M)
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
// /opt/helixscreen/config/helixconfig.json
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

HelixScreen auto-detects resolution from DRM and framebuffer backends. If auto-detection returns wrong values, override via command line:

```bash
# In helixscreen.service ExecStart, add flags:
ExecStart=/opt/helixscreen/bin/helix-launcher.sh --width 800 --height 480
```

Then reload:
```bash
sudo systemctl daemon-reload
sudo systemctl restart helixscreen
```

---

### Display upside down or rotated

**Symptoms:**
- Content displayed at wrong angle
- Touch offset from visual

**Automatic detection (first boot):**

On first boot, HelixScreen automatically detects your display orientation. It cycles through 0°, 90°, 180°, and 270° — tap the screen when the text appears right-side up, then tap again to confirm. The rotation is saved automatically and touch coordinates adjust to match.

If the automatic detection didn't run (e.g., you upgraded from an older version), you can trigger it by removing the rotation settings from your config file — see "Manual rotation" below.

**Manual rotation:**

Edit your config file (typically `~/helixscreen/config/helixconfig.json` or `/opt/helixscreen/config/helixconfig.json`):

```json
{
  "display": {
    "rotate": 180
  }
}
```

Valid values: `0`, `90`, `180`, `270`. Restart HelixScreen after changing this value. Touch coordinates are automatically adjusted to match — no separate touch configuration is needed.

**To re-run automatic detection:**

Remove the `rotate` and `rotation_probed` keys from your config file's `display` section, then restart HelixScreen:

```json
{
  "display": {
    // remove "rotate" and "rotation_probed" from here
  }
}
```

**For DSI displays on Pi, you may also need `/boot/config.txt`:**
```ini
lcd_rotate=2
```

> **Note:** Old configs may have `"display_rotate": 180` at the root level. This is automatically migrated to the new format on startup.

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
```json
// /opt/helixscreen/config/helixconfig.json
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

### Taps Register as Swipes

**Symptoms:**
- Tapping buttons doesn't work — the screen scrolls instead
- Most or all taps are interpreted as swipe/scroll gestures
- Buttons only work when tapped very quickly and precisely

**Cause:** Noisy touch controller (common with Goodix GT9xx and similar capacitive controllers) reports jittery coordinates even when the finger is stationary. The small coordinate changes exceed LVGL's scroll detection threshold.

**Solution:** HelixScreen includes a jitter filter (enabled by default, 15px dead zone) that suppresses this noise. If taps still register as swipes, increase the threshold:

```json
// /opt/helixscreen/config/helixconfig.json
{
  "input": {
    "jitter_threshold": 25
  }
}
```

Or test temporarily with an environment variable:
```bash
HELIX_TOUCH_JITTER=25 helix-screen
```

Set to `0` to disable the filter if it interferes with intentional touch gestures.

---

### Touch Input is Inaccurate

If taps are landing in the wrong place on screen:

1. **Visualize touch points:** To see exactly where the system registers your taps, enable debug touch visualization:
   ```bash
   helix-screen --debug-touches
   ```
   This draws a ripple effect at each touch point, making it easy to see if touches are offset.
2. **Recalibrate:** Go to **Settings > System > Touch Calibration**
3. **If the option isn't visible:** Your screen may not normally need calibration. SSH in and run:
   ```bash
   helix-screen --calibrate-touch
   ```
4. **If the screen is too broken to navigate:** SSH in and either:
   - Run `helix-screen --calibrate-touch`, or
   - Edit your config file: set `"force_calibration": true` in the `input` section and restart HelixScreen

See the full [Touch Calibration Guide](guide/touch-calibration.md) for details.

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
Or set `HELIX_DEBUG_TOUCHES=1` in your environment for persistent debugging.

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

**1. Update to the latest version (recommended):**

HelixScreen v0.9+ automatically detects swapped touch axes during calibration and corrects them. Update and recalibrate:
```bash
# Update HelixScreen, then recalibrate:
# Settings > System > Recalibrate Touch
```

**2. Manual workaround (older versions):**

Set the axis swap environment variable, then recalibrate:
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
| Animations on slow hardware | Settings → Display → disable Animations |
| Too many G-code files | Large directories with thumbnails use more RAM |
| Other processes hogging CPU | Check `top` for culprits |
| Swapping to SD card | Reduce memory usage or add swap to USB |
| Hardware issues | Settings → Hardware Health - check for problems |

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
cat ~/helixscreen/config/helixconfig.json | jq .
```

If the file is missing or invalid, the wizard will run. After completing the wizard, verify:
```bash
grep wizard_completed ~/helixscreen/config/helixconfig.json
# Should show: "wizard_completed": true
```

**Check config directory is writable:**
```bash
ls -la ~/helixscreen/config/
# The helixscreen process needs write access
```

**Create fresh config from template:**
```bash
cp ~/helixscreen/config/helixconfig.json.template \
   ~/helixscreen/config/helixconfig.json
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
sudo nano /opt/helixscreen/config/helixconfig.json
# Make change, save, restart
sudo systemctl restart helixscreen
# Check if change persisted
```

---

### Wrong printer detected

**Symptoms:**
- Wizard shows wrong printer model
- Features missing or wrong

**Solutions:**

**Re-run wizard:**
1. Delete config: `rm ~/helixscreen/config/helixconfig.json`
2. Restart: `sudo systemctl restart helixscreen`
3. Manually select correct printer in wizard

**Manual configuration:**
Edit `~/helixscreen/config/helixconfig.json` to set correct printer type and features.

---

## Adventurer 5M Issues

The AD5M has unique characteristics due to its embedded Linux environment and ForgeX/Klipper Mod firmware.

### Screen dims after a few seconds

**Symptoms:**
- Screen dims to ~10% brightness shortly after boot
- Happens about 3 seconds after Klipper starts

**Cause:**
ForgeX's `headless.cfg` has a `reset_screen` delayed_gcode that sets backlight to eco mode.

**Solution:**
The HelixScreen installer automatically patches `/opt/config/mod/.shell/screen.sh` to skip backlight commands when HelixScreen is running. If you installed manually or the patch didn't apply:

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
cat /tmp/helixscreen.log
```

### Service commands (SysV init)

AD5M uses SysV init, not systemd. Commands are different:

```bash
# Forge-X
/etc/init.d/S90helixscreen start|stop|restart|status
cat /tmp/helixscreen.log

# Klipper Mod
/etc/init.d/S80helixscreen start|stop|restart|status
cat /tmp/helixscreen.log
```

### SSH/SCP notes

AD5M's BusyBox has limitations:

```bash
# Use legacy SCP protocol (no sftp-server)
scp -O localfile root@<printer-ip>:/path/

# Use IP address, not hostname (mDNS may not resolve)
ssh root@192.168.1.67

# BusyBox tar doesn't support -z flag
gunzip -c archive.tar.gz | tar xf -

# Alternative: use rsync if available
rsync -avz localfile root@<printer-ip>:/path/
```

#### Windows users: `scp -O` not supported

Windows 11's built-in OpenSSH does not support the `-O` flag. Use one of these alternatives:

1. **WSL (recommended)** — Open a WSL terminal (Ubuntu, Debian, etc.) and run all commands exactly as shown in the install guide. Everything works natively.

2. **WinSCP** (free GUI) — Download from [winscp.net](https://winscp.net/). When connecting, set the protocol to **SCP** (not SFTP). Then drag and drop files to the printer.

3. **PuTTY pscp** (free command-line) — Download from [putty.org](https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html). Use `pscp` instead of `scp -O`:
   ```
   pscp helixscreen-ad5m-vX.Y.Z.tar.gz root@<printer-ip>:/data/
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

By default, HelixScreen only logs warnings and errors. To capture useful diagnostic information, you need to temporarily enable debug-level logging (`-vv`), reproduce the problem, then collect the logs.

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

#### Adventurer 5M / Forge-X (SysV init)

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

**Adventurer 5M (SysV init):**
```bash
# Full log file
cat /tmp/helixscreen.log

# Last 200 lines
tail -200 /tmp/helixscreen.log

# Follow live (while reproducing the issue)
tail -f /tmp/helixscreen.log
```

### Configuration

```bash
# Current config (sanitize API keys before sharing!)
# Pi: ~/helixscreen/config/ or /opt/helixscreen/config/
cat ~/helixscreen/config/helixconfig.json
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
- HelixScreen version: 1.0.0
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
