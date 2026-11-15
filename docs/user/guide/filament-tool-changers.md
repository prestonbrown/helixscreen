# Snapmaker U1 and MedusaHC

Printers with one toolhead or hot end per slot. Back to [Filament Management](filament.md).

---

## Choosing and picking up a tool

Which tool the Filament panel acts on, the **Pick up** and **Park** button, and what **Confirm tool changes** does are on the Filament page: see [Choosing a tool on a multi-head printer](filament.md#choosing-a-tool-on-a-multi-head-printer). The sidebar's **Mounted** line and the **Mount...** and **Park** buttons are described under [The sidebar](filament-ams-panel.md#sidebar). The temperature screen has its own tool picker (see [Temperature Control](temperature.md)).

## Other tool changers

A klipper-toolchanger printer with no feed command has no Load and Unload: filament goes in and out of each tool by hand, and the sidebar offers **Mount...** and **Park** instead. A Creator 5 with its own filament feed loads filament into the tool through Load, while Unload stays a cut by hand. Both are covered under [The sidebar](filament-ams-panel.md#sidebar), and the Creator 5's tool offset prompt is in [Calibration](calibration.md).

---

## Snapmaker U1 (SnapSwap)

The Snapmaker U1 is a **4-toolhead changer**, not a shared-path AMS. Each of its four slots has its own independent toolhead, so the topology is **parallel**: every slot keeps its own spool permanently attached to its own tool. There is no hub or selector to share between slots.

That fixed slot-to-toolhead pairing is not the same thing as which toolhead prints which part of your file. When you start a print, the printer is told which head to use for each color in the file, matching on the filament that is actually loaded. So a file sliced with its first color red will print red even if your red spool sits in the third slot: you do not have to rearrange spools to match the slicing order.

The model preview on the print screen follows the same routing: each part of the model is drawn in the color of the filament that will really print it, not in the slicer's slot order. If the preview colors look swapped compared to your slicing software, that is the preview showing you what the printer is actually going to do.

### RFID Detection

Each channel has an RFID reader that reads Snapmaker filament tags automatically:

- Material type and sub-type (e.g. "PLA SnapSpeed"), manufacturer, and brand appear per slot
- Color, recommended nozzle/bed temperatures, and spool weight come from the tag
- When you physically swap a spool, the new tag is detected and any stale metadata you'd previously entered for that slot is cleared automatically

You can still edit spool info manually from the slot context menu for spools without a tag.

**Setting the material for a spool without a tag.** The U1 keeps its own record of what each head holds, and it refuses to resume a paused print while a head the print uses has no material in that record (the printer reports `e1 not edit filament`, counting heads from 0). When you set a head's material in HelixScreen, the printer's record is updated too: on stock firmware with the printer's own filament command, and on the Extended Firmware through its filament endpoint. A vendor other than Snapmaker is recorded as "Generic", and a sub-type other than one of Snapmaker's product lines as "Basic". HelixScreen only fills a blank record or writes a material you changed yourself, and it doesn't rewrite the record when the material hasn't changed, because each write resets that head's flow calibration. For the same reason, a material or color change is refused while a print is running or paused (the editor says the printer is busy with a print), except after a runout: once the printer has forgotten the head's material, setting the new spool's material goes through, since resuming has to write it anyway. A spool link or weight change still saves. With the RFID reader turned off, HelixScreen can't tell a Snapmaker spool from any other, so a material you set on such a head replaces what its tag recorded.

### Runout and Resume

The U1 tracks filament with a motion sensor per tool. When you resume a print that paused for something other than a runout, HelixScreen first checks the printer's filament record for every head the print uses. If a head's record is blank but HelixScreen knows its material, HelixScreen fills the record in before resuming. After a runout it doesn't, because you may have loaded a different spool. If the record stays blank, the resume is refused and HelixScreen explains that the head needs its material set.

After a runout, HelixScreen has the printer refeed the head from its feeder (heating, feeding and purging, which takes about a minute and a half) before it sends Resume, so a plain Resume actually continues the print. If the motion sensor reports a runout but filament is still physically present (a stale sensor reading), HelixScreen recovers silently without prompting you.

### Toolhead Offset Calibration

The U1 measures how far each toolhead's nozzle sits from the others, so every head prints in the same place. Open the Multi-Filament System Management screen (the gear in the Filament System panel's header), then **Calibration**, then **Calibrate Toolhead Offsets**. It takes about 20 minutes and runs the printer's own calibration, the same one as the stock screen's Multi-toolhead Offset Calibration:

1. The printer preheats and homes, then for each toolhead in turn heats the nozzle, purges and wipes it, and parks it at the front.
2. For each parked toolhead, HelixScreen asks you to brush the nozzle clean with the wire brush that came with the printer, then tap **Next**. The nozzle is hot.
3. When all four are clean, take the PEI sheet off the bed and tap **Next**. The printer checks the sheet is off before probing; if it still detects the sheet, it asks again.
4. Each toolhead is probed and the offsets are saved. The final screen lists every toolhead's X, Y and Z offset. Put the PEI sheet back.

Every toolhead is purged and probed, including one with no filament loaded, exactly as on the stock screen; an empty head just turns its extruder over without pushing anything out.

Tapping **Cancel** or leaving the screen stops the calibration and turns the heaters off. If HelixScreen restarts in the middle of a calibration, it ends the printer's calibration mode and turns the heaters off when it reconnects. A step already running finishes first (a heat-up, for example), except probing, which stops straight away. If a step fails, nothing is saved and the heaters are turned off. The button is grayed out while a print is running or paused.

> **Note:** Because each toolhead is independent, the U1 has no Home, Recover, Reset or Bypass controls: those apply to shared-path AMS hardware only. Its Endless Spool is the firmware's own Auto-Replenish: the **Endless Spool** switch on the Multi-Filament System Management screen turns it on or off, and the printer picks the backup head itself.

---

## MedusaHC (Hotend Changer)

MedusaHC swaps only the **hot end** (heater, thermistor and fan) rather than a whole
toolhead. HelixScreen shows it as a tool changer with **parallel** topology: each tool is
its own independent path, with no hub or selector.

You don't need to configure anything. HelixScreen recognizes a MedusaHC automatically from
its controller (`[medusahc]`) and dock sensors (`[pin_watch io]`), whether or not you also
run the optional klipper-toolchanger. Tool changes always use your printer's own `T0`, `T1`,
... macros and `DROP_TOOL`.

### Which tool is mounted

MedusaHC has a sensor at each dock, and HelixScreen trusts those sensors over what the
toolchanger *thinks* it picked up. That matters after a failed or partial pickup: the
toolchanger will still report the tool it was told to fetch, while the docks know it never
arrived.

If the sensors can't agree on what's on the head, HelixScreen shows an error rather than
guessing. That state is deliberately not shown as "no tool", because starting a tool change
from an unknown position risks driving the carriage into a dock. Seat the hot ends by hand,
and the error clears once the sensors agree.

When a tool change fails, the error names the tool and quotes the controller's own message,
for example "Tool change to Tool 2 (T1) failed: MHC_SET: sensors did not confirm T1". A
failure that HelixScreen isn't showing as an error (the docks agree again, or it happened
before the screen connected) appears once as a message instead. If the controller itself
failed to start, you see **Tool changer error:** with its message; check `klippy.log`.

### When a tool change fails during a print

The controller pauses the print. The message tells you what to do: seat the hot ends in
their docks (or the right one on the head), heat the tool it was changing to above 190°C,
then tap **Resume**. The printer then picks that tool up again, primes and cleans it, and
carries on. HelixScreen won't send Resume while that tool is still at or below 190°C, and
says so, because the printer would refuse it. This needs the PAUSE and RESUME macros from
MedusaHC's recovery guide.

### Cleaning the nozzle

**Clean nozzle**, under **Maintenance** on the Multi-Filament System Management screen, wipes the mounted tool's nozzle
on the brush with your printer's `CLEAN` macro. It needs a tool on the head, and it is not
available while a print is running.

### The filament feeder

Because only the hot end travels, the filament is held by a servo gripper on the frame
instead of by the moving toolhead. You'll find **Open feeder** and **Close feeder** under
the gear in the Filament System panel's header (the Multi-Filament System Management screen).

Use **Open feeder** to release the filament when you're clearing a jam or loading a fresh
spool by hand, then **Close feeder** to grip it again.

> **Note:** These are refused while a print is running. The gripper is the only thing
> holding your filament: releasing it mid-print drops the strand and ruins the job.

HelixScreen picks the right macro for your setup automatically.

If your setup uses different macro names (you renamed them, or you are part-way through
migrating to the Python controller), you can choose them yourself. On the same
Multi-Filament System Management screen, **Open feeder macro** and **Close feeder macro** list the macros found
on your printer. Leave them on **auto** to keep HelixScreen's automatic choice, which also
means you'll pick up the newer commands for free if you migrate later.

### Filament Mapping on MedusaHC

You can point a G-code tool number at a different physical tool, which is useful when a
slicer project expects a different tool order than your machine is loaded with. See
[Filament Mapping](filament-mapping.md#filament-mapping) for the dialog itself.

Your printer's own `T0`, `T1`, ... macros decide which hot end each tool number picks, and
nothing can remap them (klipper-toolchanger's mapping does not reach them either). So
HelixScreen rewrites the file instead. That needs the HelixPrint plugin, and any per-tool
temperatures you pass to `PRINT_START` stay as sliced. Read
[Printers that map filament by rewriting the file](filament-mapping.md#printers-that-map-filament-by-rewriting-the-file)
before your first print that maps filament; it has start G-code that heats the right tools.

### Tool offsets

The tool offset screens edit X, Y and Z for each tool in the controller's own store, and
saving keeps them across restarts. A change to the tool on the head applies at once; the
others apply the next time they are picked up.

### What HelixScreen remembers per tool

Your printer reports nothing about the filament in each hot end: no material, no color,
no brand. So whatever you set in HelixScreen *is* the record, and it's kept for you:
material, color, brand, spool name, and remaining weight, per tool.

That survives restarts and reconnects. It's stored on your printer via Moonraker rather
than only on the screen, and it uses the same records Mainsail writes, so spools you've
assigned there should show up here and vice versa.

One consequence worth knowing: nothing on a tool changer can detect that you physically
swapped a spool, so HelixScreen keeps showing what you last told it until you change it.
Edit the slot when you switch filament.

> **Note:** Like other tool changers, MedusaHC has no Bypass, Endless Spool or dryer
> controls: each tool is its own path, and those apply to shared-path AMS hardware.

