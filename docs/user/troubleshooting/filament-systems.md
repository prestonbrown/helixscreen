# Filament system and Spoolman problems

Slots are missing or wrong, loading fails, a slicer does not see your spools, or Spoolman is empty or not charging prints.

Back to the [troubleshooting index](../TROUBLESHOOTING.md).

---

### The filament panel shows no slots (AMS, Happy Hare, AFC)

**Symptoms:**
- AMS panel shows no slots
- A "No Multi-Filament System connected" message

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

### The CFS shows no slots (Creality K2)

**Symptoms:**
- The filament panel opens but the CFS is empty, no bays, no spools
- Your CFS is populated and works fine in Fluidd or Mainsail
- The home screen's multi-filament widget is grayed out or missing

**Cause:** you are most likely running **community firmware** whose CFS module reports the CFS in a different format than Creality's. HelixScreen reads both.

**Check which one you have:**
```bash
curl -s 'http://localhost:7125/printer/objects/query?box' | grep -o 'slots\|T1'
```

- Prints `T1`: stock Creality format. The format is not your problem; work through [The filament panel shows no slots](#the-filament-panel-shows-no-slots-ams-happy-hare-afc) above.
- Prints `slots`: community format. Display, loading, unloading and filament changes all work with nothing to configure. If the panel is empty anyway, update HelixScreen (see [Updates](updates-install.md)); firmware that installed HelixScreen for you may have pinned an older version.

A K2 with no CFS attached shows no slots by design: it is a single-spool printer. See [Creality CFS](../guide/filament-cfs.md).

### OrcaSlicer does not pick up your slot colors or materials

**Symptoms:**
- Slots show the right color and material in HelixScreen, but OrcaSlicer's filament panel stays on its defaults
- OrcaSlicer can upload and start prints on the printer, so the connection itself looks fine

**Causes and fixes:**

1. **OrcaSlicer 2.4 with a blank API key.** OrcaSlicer 2.4 (reported on 2.4.2) sends a placeholder API key when the field is empty. A Moonraker that relies on `trusted_clients` rejects it with `401 Invalid API Key`, and the filament sync fails without saying so. Either upgrade to OrcaSlicer 2.5, which sends no key when the field is empty, or put your real Moonraker API key in OrcaSlicer's printer connection settings. Run this from a machine Moonraker already trusts to print the key:
   ```bash
   curl -s http://<printer-ip>:7125/access/api_key
   ```
2. **OrcaSlicer older than 2.3.2.** Older releases don't read slot info from the printer at all. Upgrade.
3. **The slot has no spool info yet.** OrcaSlicer only gets what HelixScreen has. Set the material and color under the slot's **Edit Spool** first.

A material OrcaSlicer has no preset for comes across with its color and temperatures but no preset selected. That's expected; see [Syncing with OrcaSlicer](../guide/filament.md#syncing-with-orcaslicer-232-and-later).

### Load or Unload does nothing

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

### Spoolman does not show up

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

### Spool data is out of date

**Solutions:**

**Force refresh:**
Leave the filament panel and open it again. A change made on the printer side shows up within about ten seconds.

**Check Moonraker logs:**
```bash
sudo journalctl -u moonraker | grep -i spoolman
```

### Spoolman did not deduct a print (multi-filament systems)

**Symptoms:**
- A print finished but the spool's remaining weight in Spoolman did not change
- Mainsail or Fluidd shows no active spool, or a different one

**Check, in order:**
1. **Was the slot linked before the print started?** Usage is only recorded while a spool is active, and HelixScreen makes a slot's spool active when the slot loads or when you link a spool to the loaded slot. A print that ran before the link is not deducted afterwards.
2. **Is the right spool linked to that slot?** Open the slot's **Edit Spool** editor: a linked slot shows the Spoolman icon and spool number in its title. Names don't matter; only the link does.
3. **Does your filament system set the spool itself?** AFC, Happy Hare (with its Spoolman support on) and INDX do, and HelixScreen leaves it to them.

Details: [Which spool Spoolman charges](../guide/filament-tracking.md#which-spool-spoolman-charges-on-a-multi-filament-system).

### A lane shows different details than Mainsail, Fluidd or Spoolman

HelixScreen shows what the printer holds for the lane, in this order: what your filament
system remembers (AFC, Happy Hare, the CFS, the AD5X's color menu and so on), then the
lane record stored on the printer, then Spoolman for the linked spool. So when a lane
looks wrong, the printer itself holds the wrong value, and that is where to fix it:

- **Edit it in HelixScreen** (tap the lane, **Edit Spool**). The change is written to the
  printer and every screen shows it.
- **Or fix it on the printer's side**: AFC's or Happy Hare's own commands and panels, the
  printer's color menu, or Mainsail's lane editor. HelixScreen picks up the change within
  about ten seconds.
- **Spoolman details showing for the wrong spool?** Spoolman only fills in details for the
  spool the printer says the lane is linked to. Link the right spool, and the details
  follow.

If a save shows **Not saved** under the Save button, the printer refused the change and
the lane still shows what it held before; the line says why. On an OpenAMS with
openams_spoolman, or a Happy Hare that fills its gates from Spoolman, some changes can
only be made in Spoolman, and the editor says so.

On an AFC lane with **remember spool** on, unlinking a spool may not stick: AFC keeps the
spool id through an unlink, and after a moment the lane shows it again. Turn remember
spool off for that lane, or clear the id from AFC.

### Only some of my spools show in Spoolman lists

**Symptoms:**
- Spool picker or Spoolman panel only shows a subset of your spools
- Missing spools that exist in Spoolman

**Cause:**
HelixScreen currently fetches up to 1,000 spools from Spoolman in a single request. If you have more than 1,000 spools, the rest will not appear.

**Workaround:**
Archive or delete unused spools in Spoolman to stay under 1,000 active spools.

---
