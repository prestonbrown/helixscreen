# Printing and calibration problems

A print will not start, sits on Preparing, reports the wrong layer or time, or a calibration run fails.

Back to the [troubleshooting index](../TROUBLESHOOTING.md).

---

### The file list is empty or a file is missing

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

### Tapping Start Print does nothing

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

### Preparing Print takes a long time or seems stuck

**Symptoms:**
- The screen shows "Preparing Print" for many minutes after tapping Start Print
- The pre-print progress bar sits on one step
- Other interfaces (Mainsail, Fluidd) report the printer as idle or "complete" while HelixScreen shows preparing

**What's going on:**
The window between tapping Start Print and the first layer is doing real work, homing, heating the bed to soak temperature, bed mesh, and on some printers it regularly runs five to ten minutes (a K2 Plus with Auto Bed Mesh enabled takes seven to ten). Two things make it *look* stuck when it isn't:

- Part of this work can run on the printer's host before the job is formally handed over, so other interfaces may show the printer as idle or still "complete" for the whole block. HelixScreen tracks it as preparing regardless.
- A slow step is not a stuck step. As long as the printer is still moving and narrating what it's doing, HelixScreen keeps waiting, a long bed mesh is given the time it needs rather than cut off mid-sequence.

During this window the Print Status panel and the home panel's print card show the preparing job: the current step, a progress bar, and an estimate for the whole pre-print period.

**Solutions:**

**Check the printer is actually working.** Look at the machine, is the toolhead moving, the bed heating? Or open a web interface's console and watch for ongoing output. Motion and messages mean it's working, not stuck.

**Not willing to wait? Cancel.** Cancel is always available during preparation, and it ends as a canceled start, not a failed print: the print never starts. When it takes effect depends on where the work is running:

- If HelixScreen has not handed the job to the printer yet, the cancel is immediate. A move already under way finishes first.
- If the printer is already running its start sequence (its `PRINT_START` macro), Klipper cannot interrupt that macro. HelixScreen sends the cancel to the printer and tells you so; the printer finishes its start sequence (homing, heating, probing) and stops before the first print move. The job shows as canceled once the printer confirms it. If you need it to stop right now, use the E-Stop.

Then start again with the slow step switched off, see [Pre-Print Options](../guide/printing.md#pre-print-options).

**If the printer has gone quiet**, no motion, no console output, for a good while, it may genuinely be stuck. Collect a [debug bundle](diagnostics.md#collecting-logs) and check the Klipper log for the macro that was running.

---

### Pause or Cancel does not respond

**Symptoms:**
- Buttons don't respond
- Print continues despite tapping Cancel

**Causes:**
1. Connection issue during print
2. Klipper busy processing

**Solutions:**

**In an emergency, use the E-Stop button.** While a print is starting or running, a red E-Stop button sits on the navigation bar on every screen.

**Check connection status** - if disconnected, wait for reconnection.

**Via terminal:**
```bash
curl -X POST http://localhost:7125/printer/print/cancel
```

---

### Snapmaker U1: Resume says Filament not set

**Symptoms:**
- After a pause (a runout, say), tapping Resume shows **Filament not set**: "Slot 2 has no filament type set, so the printer will not resume."
- The print stays paused

**What's going on:**
The U1 will not resume while a toolhead the print uses has no material recorded for it. Snapmaker RFID spools fill that in by themselves; a spool without a tag leaves it empty. When you have set that slot's material in HelixScreen, HelixScreen writes it into the printer's record before resuming, except after a runout, when you may have loaded a different spool. So this message means either the print paused on a runout or neither the printer nor HelixScreen knows what is loaded there.

**Solution:**
1. Open the filament system screen and tap the slot the message names
2. Tap **Edit Spool** and set the material (and color, if you like), then save
3. Tap **Resume** again

If the printer refuses the resume for some other reason, HelixScreen shows the printer's own message as "Failed to resume: ..." instead.

---

### Resume interrupted print? appears after a power cut

**Symptoms:**
- After power comes back and HelixScreen reconnects, a dialog asks: **Resume interrupted print?**
- The body names the file when the printer reported it ("The printer lost power while printing <name>.") or describes it generically

**What's going on:**
On printers whose firmware saves recovery data when power drops mid-print, that data survives the reboot. That covers the Creality models with recovery support (K1 family, K2, Ender 3 V3 and siblings) and the Snapmaker U1. When HelixScreen connects and finds it, it asks what you want to do rather than deciding for you. Creality printers get an extra-honest wording ("The resumed layer may not line up exactly.") because their recovery re-homes without re-probing the bed, that is a real property of the resume, not a malfunction.

**Your choices:**

| Choice | What happens |
|--------|--------------|
| **Resume** | The printer continues the interrupted print from where its firmware saved its progress. On Creality, check the first resumed layer before walking away, it can sit a millimeter or two off. |
| **Discard** | The recovery data is cleared and nothing is printed. The printer is back to a clean slate; start whatever you like. |
| Dismiss the dialog (tap outside it) | Nothing is decided: the recovery data is kept, and you are asked again the next time HelixScreen connects. |

The offer never appears on top of a print you have already started, if you tap Start Print before answering, the question waits until that job is done. On printers without firmware-side recovery data there is nothing to find, so the dialog never appears there; a power cut simply means starting the print again. The [HelixPrint plugin](../guide/helixprint-plugin.md) can offer a guided resume on some of those; see [Printing](../guide/printing.md).

---

### The layer count is wrong, stuck at 0, or the total is missing

**Symptoms:**
- The layer counter on the Print Status panel doesn't match the real layer being printed
- Total layers shows as missing, 0, or a placeholder
- The current layer looks like a rough guess rather than an exact count

**Cause:**
For an exact layer count, HelixScreen reads the current and total layer directly from Klipper. Those values are only available if your slicer tells Klipper about them by emitting the `SET_PRINT_STATS_INFO` command in the printed G-code. Many stock slicer profiles don't do this. When the values are absent, HelixScreen falls back to *estimating* the layer from print progress and Z-height, which is close but not always exact.

**Fix, have your slicer emit the layer info:**

In **PrusaSlicer, SuperSlicer, or OrcaSlicer**, add these to your printer's custom G-code (Printer Settings > Custom G-code):

- **Start G-code**, report the total layer count once, at the start:
  ```
  SET_PRINT_STATS_INFO TOTAL_LAYER=[total_layer_count]
  ```
- **Before layer change G-code** (some slicers call it "Layer change G-code"), report each new layer as it begins:
  ```
  SET_PRINT_STATS_INFO CURRENT_LAYER={layer_num + 1}
  ```

Re-slice your file after adding these, the commands are baked into the G-code at slice time, so existing files won't have them. Once present, HelixScreen shows the exact layer and total for every print.

> **Cura users:** Cura doesn't expose these layer placeholders directly and needs a post-processing script to inject `SET_PRINT_STATS_INFO`. See the Klipper community docs and forums for a Cura post-processing plugin that adds it.

> **Note:** Some noise in the layer count during the print-start phase (bed mesh, purge/prime line, Z-hop) is normal. HelixScreen holds the layer at 0 until real printing begins, so it will not jump ahead before the first layer. The estimate only matters mid-print when the slicer macros above are missing.

> **Z-hops do not move the estimate.** When the layer is worked out from the nozzle height, a new layer shows only once the nozzle has stayed at that height for about a second and a half of printing, so the quick lift of a Z-hop on every travel move does not bump the count up and back. The counter steps to each new layer that much after the layer change.

### Time remaining is inaccurate or slow to settle

**Symptoms:**
- The estimated time remaining is off, especially early in a print
- The ETA jumps around or takes a while to steady out during the first layers
- Time remaining doesn't track the slicer's estimate

**Cause:**
Time remaining is most accurate when your slicer reports layer information to Klipper via `SET_PRINT_STATS_INFO` (the same commands as the layer-count fix above). Without it, HelixScreen estimates progress from other signals, which are noisier at the very start of a print before enough data has accumulated.

**Fix:**
Add the `SET_PRINT_STATS_INFO` commands described in [The layer count is wrong, stuck at 0, or the total is missing](#the-layer-count-is-wrong-stuck-at-0-or-the-total-is-missing) above, then re-slice. Expect some ETA drift during the print-start phase (bed mesh, priming) even with the macros in place, the estimate tightens up once printing is underway.

---

### The input shaper measurement fails

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
  -H "Content-Type: application/json" \
  -d '{"script": "ACCELEROMETER_QUERY"}'
```
Should return acceleration values, not an error.

**Check Klipper config** for `[adxl345]` or `[lis2dw12]` section.

**Re-run calibration** after fixing hardware issues.

### Screws tilt shows the wrong adjustments

**Symptoms:**
- Adjustment values seem incorrect
- Bed gets worse after adjustments

**Solutions:**

**Check the turn direction.** Turn one screw the way its arrow points and re-probe. If that corner moves further from the reference, the `screw_thread` in your `[screws_tilt_adjust]` section names the wrong direction for your knobs: switch it between `CW-M3` and `CCW-M3` (or the M4/M5 equivalents). Tighten/Loosen does not depend on this setting; it always comes from the probed heights.

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
