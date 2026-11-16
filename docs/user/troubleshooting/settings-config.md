# Settings, setup wizard and printer model problems

Settings will not save, the wizard keeps coming back, HelixScreen picked the wrong printer model, a feature you expect is missing, or you want to start over.

Back to the [troubleshooting index](../TROUBLESHOOTING.md).

---

### HelixScreen shows the wrong printer model

**Symptoms:**
- HelixScreen identifies the printer as the wrong model/type (for example, a Voron showing as "FlashForge Adventurer 5M Pro")
- Changing the printer image in Printer Manager changes the picture but not the model, features, calibration dialogs, and the name still follow the wrong type

**What's going on:**
- The **printer type** (the model picked during setup) drives the name, image, bed size, probe type, and preset options. The image picker in Printer Manager is cosmetic only, it never changes the type.
- **Device-specific install packages** (Creality K1, FlashForge Adventurer 5M, and similar) run a preset-mode setup that *skips printer identification entirely*: the type comes from the install package itself, not from detection. No setting can override it.
- On **generic installs**, auto-detection either picked a wrong near-relative from the database, or, when it wasn't confident enough, deliberately left the type empty for you to choose rather than guess.

**Solutions:**

**First, figure out which situation you're in.** What did you install, and what is HelixScreen running on? If a device-specific preset package doesn't match the machine (or its screen), that's the cause, detection never ran. Install the HelixScreen package built for your hardware, or use the [remote screen setup](../INSTALL.md#remote-screen-setup-run-on-a-separate-device) on a Pi/PC/tablet pointed at your printer's Moonraker, generic installs run full auto-detection.

**If the saved model is wrong (generic install), correct it in Printer Manager, nothing gets wiped:**

1. Tap the **printer image** on the Home Panel to open the Printer Manager
2. Tap the **printer model** row: the model name directly below the printer name, marked with a pencil icon
3. Pick your model from the list (Voron 2.4, Voron 0.2, Voron Trident, and Voron Switchwire are all in the database)
4. The new type applies immediately, name, image, and all the type-driven features follow it

**Let HelixScreen catch it for you.** If you'd rather not hunt through the list, just connect the printer and wait: when detection is confident the saved type is wrong, a warning names both models. Tap **Review** on it to open the **Printer type mismatch** dialog, which offers **Choose Model** (opens the model picker from the setup wizard's identity step) or **Keep current**, the right answer for a heavily modified printer that legitimately differs from its stock sibling. Closing the warning with its **X** counts as **Keep current** too. Either way the answer is remembered for that type, so the warning won't come back on every boot. When the two are the same printer with and without a firmware mod or add-on (ForgeX, k2-improvements, CFS), the warning names that difference instead, for example "Your Creality K2 Plus looks like it has k2-improvements".

Re-adding the printer through **Printer Manager > Manage Printers > Add Printer** (then deleting the old entry) and **Settings > System > Factory Reset** remain as last resorts. A factory reset re-runs the full wizard but wipes all HelixScreen settings, so use it only if you want a clean start anyway.

If your model isn't in the database, leave it on the detected/generic profile, everything still works; you can rename the printer and set any image from Printer Manager.

### The setup wizard keeps appearing

**Symptoms:**
- Wizard shows on every boot
- Settings not saved

**Causes:**
- The settings file cannot be written (permissions, a full disk or a read-only filesystem)
- The `wizard_completed` flag was never saved

**Solutions:**

**Check that the config is valid JSON** (use your actual install path):
```bash
cat ~/helixscreen/config/settings.json | jq .
```

If HelixScreen cannot save its settings, the wizard comes back on the next start. After completing the wizard, verify:
```bash
grep wizard_completed ~/helixscreen/config/settings.json
# Should show: "wizard_completed": true
```

**Check config directory is writable:**
```bash
ls -la ~/helixscreen/config/
# The helixscreen process needs write access
```

### Settings do not save

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
# Edit settings.json, path varies by platform; see:
#   "Config File Locations" in the Touch Calibration guide
sudo nano <path-to-your-settings.json>
# Save, then start the service:
sudo systemctl start helixscreen
# Check if change persisted
```

---


### A feature is missing for hardware my printer has

**Symptoms:**
- A screen or button you expect (Bed Mesh, Input Shaper, LED controls, filament sensors, a probe calibration) never appears
- A feature shows the wrong hardware, such as the wrong probe type

**Check what HelixScreen detected:**

1. Tap the **printer image** on the Home Panel to open the Printer Manager
2. Scroll to **Capabilities**. It lists everything HelixScreen found, with details such as the probe type, kinematics and number of tools

If the item is missing there, HelixScreen did not detect it, and the screens that depend on it stay hidden. Common causes:

- The Klipper section is missing or commented out in `printer.cfg`, or Klipper failed to load it. Check that Klipper is running without errors in Mainsail or Fluidd
- The hardware is set up under a section name HelixScreen does not recognize yet. Please [report it](diagnostics.md#getting-help) with your config section so it can be added
- A few capabilities (bed mesh, QGL, Z-tilt, nozzle clean, heat soak, chamber, speaker, LED lights) can be turned on or off by hand with a [capability override](../CONFIGURATION.md#capability-overrides). An override set to `disable` hides the feature even when the hardware is present

If the detail is wrong (for example **Probe · BLTouch** on a Beacon), include a screenshot of the Capabilities section in your report.

---

### Reset HelixScreen or re-run the setup wizard

Deleting `settings.json` does **not** reset HelixScreen. It keeps rolling backup copies outside the install folder and restores the newest one the next time the file is missing, so your old settings come straight back. Use one of these instead.

**Option 1: Factory Reset from the screen (easiest).** Open **Settings > System > Factory Reset** and confirm. This erases all HelixScreen settings, clears the backup copies so the old settings cannot come back, and runs the Setup Wizard on the next start. It does not touch Klipper, Moonraker or any files on the printer. See [System settings](../guide/settings/system.md#factory-reset).

**Option 2: the screen is unusable, so reinstall clean.** Run the installer with `--clean`. It removes the old installation completely, including your HelixScreen config, backups and caches, asks you to confirm, and installs fresh. Over SSH, use the command for your platform from the [installation guide](../INSTALL.md); for a one-liner from the web installer:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --clean
```

Add `--yes` to confirm without a prompt when no terminal can answer. On printers that keep a copy of the installer (K1, AD5M, K2 and similar), run that copy from `/tmp` instead, because the installer refuses to remove its own folder. For example, on a K1 run `cp /usr/data/helixscreen/install.sh /tmp/install.sh && sh /tmp/install.sh --clean`. On a K2 the copy is `/mnt/UDISK/helixscreen/install.sh`. On an Adventurer 5M it is `/opt/config/mod_data/helixscreen/install.sh` with Forge-X and `/root/printer_software/helixscreen/install.sh` with Klipper Mod. The [K1](../guide/install-k1.md), [Adventurer 5M](../guide/install-ad5m.md) and [K2](../guide/install-k2.md) guides show the download form that works on each printer.

**Option 3: only touch is broken.** A full reset is not needed to fix touch. Add `HELIX_TOUCH_CALIBRATE=1` to `helixscreen.env` and restart, then remove the line once calibration succeeds (it does not clear itself). See [Forcing Recalibration](../guide/touch-calibration.md#forcing-recalibration).

**Run the wizard without wiping anything.** Stop the service and start the app once by hand with `helix-screen --wizard`. Your settings stay as they are.
