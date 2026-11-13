# Creality Filament System (CFS)

Back to [Filament Management](filament.md).

---

The CFS is an enclosed filament storage and delivery system for **Creality** printers. It ships on the **K2 series**, and is also supported on the **K1, K1C, and K1 Max** once you install Creality's official CFS upgrade kit and firmware. Each CFS unit holds 4 spools of filament, and up to 4 units can be connected (16 total slots). HelixScreen auto-detects CFS when connected, along with the set of commands your printer's firmware actually understands: there are three, and it works this out from the firmware itself rather than assuming based on your printer model.

> **A K2 with no CFS attached** is treated as the single-spool printer it is. The filament system screens, slot widgets and Bypass controls stay out of the way, the toolhead filament sensor alone says whether filament is loaded, and the Filament panel's **Load** and **Unload** run the K2's own external-spool load and its cut and retract. The pre-print checks do not ask you to turn off a bypass the printer cannot switch. The toolhead sensor appears in **Settings > Devices > Sensors**, the sensor setup wizard and the Clog Detection widget like any other filament sensor.

> **K1 series note:** CFS on the K1, K1C, and K1 Max requires Creality's official CFS upgrade firmware (v2.3.5.33 or newer). Detection is automatic; no manual configuration is needed.

> **Running community firmware?** Some K2 Plus owners replace Creality's firmware with a community build that has its own rewritten CFS module. HelixScreen recognizes those automatically and supports them fully: slots, spools, colors, humidity and temperature all display, and loading, unloading and filament changes all work from the touchscreen. Nothing to configure.
>
> On recent versions of that CFS module, each unit also shows its own temperature and humidity, a unit that stops answering is marked **Disconnected**, and Filament Mapping works: the tool-to-slot choices you make when starting a print are sent to the CFS just before it begins, for that print only. If a mapping points at a slot the CFS cannot feed from, or at a unit that is not answering, the print does not start and HelixScreen says why.

## Slot Layout

CFS uses a **TNN address format** to identify each slot:

| Unit | Slot A | Slot B | Slot C | Slot D |
|------|--------|--------|--------|--------|
| Unit 1 | T1A | T1B | T1C | T1D |
| Unit 2 | T2A | T2B | T2C | T2D |
| Unit 3 | T3A | T3B | T3C | T3D |
| Unit 4 | T4A | T4B | T4C | T4D |

Each slot displays the detected filament color, material type (PLA, PETG, ABS, etc.), brand, and remaining filament length.

## RFID Detection

CFS units have built-in RFID readers that automatically detect Creality filament spools:

- Place a spool in any slot and its material info appears within seconds
- Supported materials include Hyper PLA, Hyper PETG, Hyper ABS, CR-PLA, CR-Silk, and more
- Remaining filament length is tracked automatically
- If a spool isn't recognized, a generic entry is shown: you can identify it manually

**What HelixScreen gets from a tag.** The CFS reads the tag itself and reports a material code, a color and the remaining length; HelixScreen turns the code into the material, brand and nozzle temperature range from Creality's filament list. You can change any of these in the slot's **Edit Spool** editor, and **Reset to tag** puts the tag's values back. A change you make goes to the CFS's own record of the slot, so Creality's screen shows it too; HelixScreen has no way to write to the tag itself. Only what the CFS reports reaches HelixScreen, so extra data stored on a tag (in its spare bytes, for example) is not shown.

**Tags and Spoolman.** A tag written with a phone app such as CFTag can carry a Spoolman spool number in its serial number. When you put a spool with such a tag in a slot, HelixScreen links the slot to that Spoolman spool, just as if you had picked it with **Select Spool**, and a short notice says so. A few things to know:

- If the number doesn't match a spool in your Spoolman, a notice says so and the slot is left as it was.
- If you pick a different spool for the slot while the same spool stays in it, your pick is kept. The tag is read again only when a spool is put in.
- After HelixScreen restarts, a spool already sitting in a slot links only a slot that isn't linked to anything. So if you unlinked such a slot by hand, a restart links it again; turn **Link Spools from Tags** off to keep it unlinked.
- A stock Creality spool has serial number 000001 on every tag, so it names no Spoolman spool. For the same reason, spool #1 in Spoolman can't be linked from a tag; pick it by hand.
- To turn this off, use **Link Spools from Tags** in the CFS settings (see [Multi-Filament System Management](filament-ams-panel.md#multi-filament-system-management)).

**Putting a different spool in a slot.** HelixScreen recognizes a new spool by a change in the tag's material code or color. When either changes, it drops what it remembered about the old spool on that slot (your edits, the Spoolman link, the remaining weight), so they don't follow the wrong spool. Two cases are not noticed, and in both the slot keeps the old spool's Spoolman link and details until you change them yourself:

- **Same material and same color.** Swapping one Creality spool for another of the identical filament and color looks unchanged. Pick the new spool for that slot with **Select Spool**. A tag that carries a Spoolman spool number is the exception: the new tag's number links the slot to the new spool.
- **Spools without a tag.** There is nothing to compare, so a swap is not noticed. Update the slot by hand.

> **Tip:** If a slot shows incorrect info, remove and re-seat the spool to trigger a fresh RFID read.

## CFS Device Actions

Tap the gear in the Filament System panel's header to open the Multi-Filament System Management screen. The CFS's own actions are in its **Maintenance** section:

| Action | What It Does |
|--------|--------------|
| **Refresh RFID** | Re-read the spool tags and remaining length in every unit, which is useful after swapping spools while the printer was off |
| **Clean Nozzle** | Wipe the nozzle on the CFS's silicone cleaning strip |
| **Communication Test** | Test the link between the printer and the CFS units |

On a K1 with the CFS upgrade, a **Calibration** section adds **Calibrate Cutter** and **Calibrate Purge Chute**.

**Auto-Refill** is the **Endless Spool** switch on the same screen. A runout pauses the print either way; with it on, the box then swaps in another slot **only** if one holds the exact same material and color, and resumes. With it off, or with no match, the print stays paused.

## CFS and the External Spool

Every CFS printer has a spool holder that feeds the toolhead directly, next to the CFS, and HelixScreen's Bypass toggle drives it. How much the firmware does for you depends on which CFS firmware you have:

**Community K2 Plus firmware (Kalico port): the bypass is fully automatic.** Turn the Bypass toggle on and the printer takes over: it heats, moves to the waste bin, and waits for you to insert the filament into the holder. Feed it in, and the printer draws it to the toolhead and flushes. Turning the toggle off reverses the whole thing: the printer retracts and cuts, then asks you to pull the filament out. The external spool also appears on the filament path, and you can tap it to set material, color, and brand or link a Spoolman spool.

**Stock K1/K2 firmware (Creality's own): you feed the holder by hand.** Creality's firmware has no command that loads from the holder: even Creality's own screen leaves the feeding to you. When you turn the Bypass toggle on, HelixScreen stands the CFS down so it cannot push bay filament into the tube your spool is using, then watches the toolhead sensor: the moment it sees your filament, the external spool shows as active. Inserting and removing the filament updates this automatically. Turning the toggle off re-arms the CFS for normal printing. You can still tap the external spool to record material, color, and brand, and prints started from bypass skip the loaded-filament checks.

**Both firmwares:** the external spool is also published to your slicer. It appears as one extra lane past the CFS's own lanes in OrcaSlicer's printer-adapter view (T4 beside T0–T3, for example), carrying the material, color, and spool info you set, so a single-tool file can be mapped straight onto it.

Turning bypass on also switches the toolhead runout sensor on at the printer (stock firmware normally leaves it off outside CFS operations), so a bypass print is protected against running out mid-print. Turning bypass off restores the sensor's previous state.

> **Tip:** On stock firmware, turn bypass on *before* you feed the holder. The toolhead sensor can only attribute filament to the external spool while the bypass toggle is on: filament that appears without it is treated as unknown, not bypass.

