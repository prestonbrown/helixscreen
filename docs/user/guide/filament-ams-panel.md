# The Filament System Panel

This page covers the panel that draws your slots and filament path for Happy Hare, AFC, ACE, a tool changer, the Snapmaker U1 and the other multi-material systems: the slot view, the sidebar, the slot menu, the spool editor and the management screen. For the Filament panel with the Load, Unload and Purge buttons, see [Filament Management](filament.md).

---

For multi-material systems (Happy Hare, AFC-Klipper, ACE, Tool Changer, etc.). The AMS panel has two main areas: the **slot view**, which draws your spools and the filament path, and the **sidebar**, which shows what is loaded and holds the controls.

A **gear** button at the right end of the panel header opens the [Multi-Filament System Management](#multi-filament-system-management) screen. It is there whenever your printer has a filament system.

How the two are arranged depends on the shape of your screen:

- **Landscape screens** put the slot view on the left and the sidebar on the right.
- **Portrait screens** (taller than wide) stack everything in one column. From the top: the header, with the back button, your filament system's logo and its name and the gear; the loaded-filament strip and the status line, with the **Bypass** switch at the end of the status line when your system has one; the slot view and filament path at full width; and the action buttons, or the step progress while an operation runs, across the bottom.

![AMS panel on a portrait screen](../../images/user/ams-portrait.png)

## Slot View

The slot view shows all your filament slots in a visual tray layout:

- **Spool icons**: Each slot displays a 3D spool visualization with its filament color
- **Spool labels**: Above each spool. With four slots or fewer, the label shows the spool's name (for example "Bone White") over its material (PLA, PETG, ABS, etc.). When the name already says the material, as in "PLA+ Bone White", only the name is shown, and a slot with no name shows just the material. With five or more slots there is only room for the material, shortened to its first four letters. On smaller screens five or more labels alternate between two rows, so neighbors never overlap, and a thin line leads from a raised label down to its spool.
- **Status badge**: Slot number with color-coded background (green = loaded, gray = empty, red = error)
- **Slot numbers**: Slots count from 1 unless you turn on **Number slots from 0** under **Settings > Printing**, which counts slots, lanes, gates and tools from 0, so gate 0 on Happy Hare reads as Gate 0 next to T0. Only the display changes; tool names stay T0, T1 and so on.
- **Tool badge**: A small badge (T0, T1, etc.) on a slot shows which tool prints from it. When every tool feeds the slot of its own number (T0 the first slot, T1 the second, and so on), the badges would only repeat the slot numbers, so they are hidden. They appear when the tools are mapped any other way, for example after you change a slot's [Assigned Tool](#assigned-tool) or when two tools share a slot. A badge in the warning color marks a mapping you set yourself rather than the one your system reports.
- **Environment pill**: On a box that reports temperature or humidity, a pill stands at the right end of the box: a thermometer with the temperature over a water drop with the humidity. The drop fills to the humidity reading, so a wet box shows a full drop at a glance; a box whose sensor gives no reading shows an empty outline. While the box dries, the temperature line shows the heat icon and the time left. Tap the pill to open that box's drying and humidity screen (see [Filament Drying and Humidity](filament-drying.md)). Boxes with no readings have no pill, and the box takes the full width.

Below the slot grid, a **filament path diagram** shows the routing from slots through the hub/selector to the toolhead. This updates in real time during load/unload operations, including eject animations when retracting filament at the slot sensor. On the Anycubic ACE the path reads the hub and toolhead sensors the driver publishes, so a strand parked partway down the tube is drawn where it actually sits - short of the hub - rather than back at the spool.

### Reading Error States

When a slot runs into trouble, HelixScreen shows it visually so you don't have to dig through logs:

- **Error dot**: A small colored dot appears at the corner of a slot's spool when that slot reports a problem. **Red** means an error (jam, runout, hardware fault); **amber** means a warning. With animations enabled, the dot gently pulses to draw your eye.
- **Buffer-health tint**: On systems with a buffer between the slots and the toolhead (an AFC buffer - a TurtleNeck, or a pressure-sensor buffer such as an `FPS_PSF` - an OpenAMS filament pressure sensor, or Happy Hare with sync feedback), the hub on the filament path diagram changes color as the buffer drifts toward a fault: gray on target, amber as it drifts, red near an end stop.
- **Filament pressure (OpenAMS)**: On OpenAMS running without AFC, each unit's filament pressure sensor appears on the filament path as a box labeled **FPS**. The sensor measures how hard the filament is pressing (compression) only: 0% is no pressure and 100% is fully compressed. The box stays gray while the pressure regulates around its target and turns amber near the top (85% and up) or red (95% and up), or amber near zero while filament is loaded because the feed is not engaging. A sensor with no target is only tinted at those ends. Tap the box to open Buffer Status: an upright gauge that fills from the bottom up to the reading with a tick at the target, the reading against its target (for example "FPS 53%" and "target 50%"), whether it is at, above or below target (within 5 points counts as at target), and the last minute as a line with the target dashed. OpenAMS has no clog detection, so only the pressure reading is shown. AFC pressure-sensor buffers and Happy Hare keep the two-ended slider with its tight and loose wording.

**To recover:**

- Use **Reset** in the sidebar (it reads **Home** on Happy Hare). This clears the error message your system is holding onto and then puts the system back to a known-good state. It is the right first move for almost every jam or fault, including one reported against a single slot. On Happy Hare it runs `MMU_RECOVER` and then `MMU_HOME`, which unloads any loaded filament before homing, so the button is grayed out while a print is running. On AFC it runs `RESET_FAILURE` and `AFC_CLEAR_MESSAGE`, then `AFC_RESET`.
- If filament from one slot is stuck partway down the tube, tap that slot. When your system can pull it back, the slot menu's second button changes from **Unload** to **Recover**. Tap it to draw the filament back toward the slot without heating the nozzle.
- For a system-wide problem that Reset does not shift, use **Recover** on the Multi-Filament System Management screen (the gear in the header). On Happy Hare this is how you run `MMU_RECOVER` from the screen, and it moves nothing - see [Multi-Filament System Management](#multi-filament-system-management) for what it sends. When a Happy Hare error pops up on screen, its own **Recover** button sends plain `MMU_RECOVER` and lets Happy Hare work out the filament position from its sensors.

**On Happy Hare**, every fault the MMU reports opens a recovery popup showing Happy Hare's own reason, whether it happened during a print or during a load or home you started yourself. Happy Hare's own error prompt closes behind it. The popup offers:

- **Resume** - only when a print is paused, and first in the list there. Heats the nozzle first if it has cooled.
- **Recover** - lets Happy Hare work out where the filament is with its own sensors. First in the list when nothing is paused. Works on a cold nozzle.
- **Unload** - when filament is at the toolhead. Heats the nozzle first.
- **Unlock** - a last resort that clears Happy Hare's pause lock without moving anything.

## Sidebar

The sidebar shows the status of the currently loaded filament and provides quick-access controls. On a printer with more than one toolhead, a **Mounted:** line naming the tool on the carriage (for example **Mounted: Tool 2**, or **No tool mounted** when none is) tops the sidebar. On a portrait screen the loaded-filament strip and status line sit above the slot view, and the buttons sit below it.

**Currently loaded strip:**

A narrow color bar matching the loaded filament sits beside two lines of text:

- **Position and weight**: Which slot is feeding the toolhead, such as "Current: Slot 2" (or "Current: Bypass" when bypass is enabled), followed by the estimated filament remaining (e.g., "750g") when your system knows it
- **Filament name**: e.g., "Red PLA", "Prusament PETG". A long name is cut short with "..."

When your system has a buffer or flow sensor, its meters sit under the two lines on a landscape screen and to their right on a portrait screen. On smaller screens each meter shrinks to a small icon or arc beside its reading. The meters hide while a load or unload runs, so the step progress has the room.

- **Buffer slider**: When your system has a filament buffer with a proportional reading (a filament pressure sensor, or Happy Hare sync feedback), a small upright gauge appears in the strip, with the short reading and, on wider screens, its **FPS** or **Sync** label under it. For Happy Hare and AFC pressure-sensor buffers it is a slider: loose is up and tight is down; the block is gray on target, amber off target and red near an end. For an OpenAMS pressure sensor it is a fill that rises from the bottom with the pressure, with a tick at the target, gray until it nears the top or bottom end. It is the same reading the [Filament Buffer widget](home-panel.md#filament-buffer-widget) shows. Without a target a slider reading shows as a number only, and a fill keeps its gauge with no tick. On smaller screens a small up-and-down arrow icon stands in for the gauge, beside the reading.
- **Clog detection meter**: When your system has flow monitoring (encoder, FlowGuard, or AFC buffer), an arc meter shows the current reading, with the sensor named underneath. It is the same reading the [Clog Detection widget](home-panel.md#clog-detection-widget) draws as a bar on the dashboard. When the sensor is armed but has nothing to report, the arc is replaced by a check mark. On smaller screens the sensor name is left off.

**During load/unload operations**, the sidebar switches to a **step progress display** showing each stage of the operation. The exact steps come from your filament system, so they match what it actually does rather than a generic list.

On **AFC** (Box Turtle, OpenAMS):

- **Load into an empty toolhead:** Heat nozzle → Feed filament → Purge to bucket → Brush nozzle → Kick away → Load complete
- **Swap to a different slot:** Heat nozzle → Cut tip → Unload filament → Feed filament → Purge to bucket → Brush nozzle → Kick away → Load complete
- **Unload:** Heat nozzle → Cut tip → Retract filament

If AFC's `auto_home` is enabled in AFC.cfg, HelixScreen skips its home-first prompt: AFC homes the printer itself when needed.

On **Happy Hare**:

- **Load into an empty toolhead:** Heat nozzle → Select gate → Load filament → Purge → Load complete
- **Swap to a different slot:** Heat nozzle → Form tip → Cut tip → Unload → Select gate → Load filament → Purge → Load complete
- **Unload:** Heat nozzle → Form tip → Cut tip → Unload

Systems that don't publish a step list of their own get a shorter generic bar: Heat nozzle → Feed filament → Purge, with a tip step added for a swap.

Some steps only apply to how your machine is set up. A step your system never reaches stays grayed out instead of lighting up, so a bar that skips **Cut tip** or **Kick away** is normal, not a stall. Each step updates in real time so you can see exactly where the operation is. On systems that can stop an operation midway, a **Cancel** button appears beside the status line while it runs. When a button is grayed out, a line of text under the buttons can say why.

On small portrait screens there is room for the steps only as a row of dots, so the name of the current step is shown under them.

**Action buttons** (hidden while an operation is in progress) sit two to a row. On small portrait screens they sit in one row. Only the buttons your system supports appear. On a landscape screen the **Bypass** switch sits above them; on a portrait screen it sits at the end of the status line.

| Button | Action |
|--------|--------|
| **Bypass** (toggle) | Feed filament directly to the extruder, bypassing the AMS. Shown when your hardware supports bypass, or when you turn on **Enable Bypass Controls** - see [When Bypass Doesn't Appear](#when-bypass-doesnt-appear). The toggle is guarded: it can't be changed while a job holds the machine (a "Bypass cannot be changed while printing" warning appears), if a lane's filament is loaded it is unloaded first before bypass engages, and where a hardware sensor owns the bypass the toggle only reports that the sensor is in control. |
| **Load...** | Opens a picker naming each tool and the slot it loads from - the slot set as its **Assigned Tool**. Pick the tool and confirm. To load any other slot, tap that slot in the slot view and choose **Load** |
| **Unload** | Retract the currently loaded filament back to its slot. It also works when filament is stuck in the toolhead and no slot claims it, on systems that can tell. If your system needs to know which slot that filament came from and cannot tell, Unload first asks **Which lane is in the toolhead?** and lists the lanes that hold filament, even when there is only one. Pick the lane, tap **Clear extruder only** to cut and clear the extruder without pulling any lane back (the rest of that strand stays in its tube, near the toolhead), or tap **Cancel**. When no lane holds filament it asks **Clear the extruder?** instead. On the FlashForge AD5X, a lane whose load failed partway is remembered and unloaded without asking. On a printer with several toolheads it reads **Unload...** and opens the picker instead - see below |
| **Mount...** / **Park** | Tool changers. **Mount...** picks a tool and puts it on the carriage; **Park** puts the mounted tool back in its dock. On a tool changer that cannot feed filament itself, these stand where Load and Unload would be |
| **Reset** | Reset the AMS system state (useful after jams or errors). Hidden on systems that have nothing to reset |
| **Check Slots** | Happy Hare only: checks every slot's sensor. The printer parks the toolhead and moves each lane's filament to test it, so the button is grayed out while a print is running |

**Load and Unload always mean filament.** They never mount or park a tool - that is what tapping a toolhead, **Mount...** and **Park** are for. A printer that needs a tool on the carriage to feed it mounts the tool as part of the load.

**The tool picker.** Each row names a tool and, where it is not the tool's own, the slot it uses, with what that slot holds. That is the material, followed by **loaded** when its filament is at the toolhead, or **ready to load** or **empty** when a sensor in the slot says which. A slot with no sensor reading shows the material alone, because the screen does not guess. The tool you have selected, or else the one on the carriage, comes ticked when it can take the operation. A tool with no slot assigned to it is grayed out with **Assign a slot first**; set one with **Assigned Tool** in the slot's **Edit Spool** editor.

**Loading or unloading several heads at once.** On a printer with a toolhead per slot (Snapmaker U1, MedusaHC), the picker lets you tick several heads, and shows an **All** row to tick them all at once when two or more can take the operation and the printer can run them together. The prompt text the printer shows is English - every screen attached to the printer sees the same words. If a ticked head turns out not to take the operation, it is skipped with a warning naming the head and the reason, and the rest continue. A head whose own sensor reports it empty is not tickable for Unload at all: there is nothing in it for an unload to act on.

**MedusaHC.** One extruder drive serves every tool, so **Load** mounts the tool, heats it and asks you to insert the filament into that tool - the extruder grabs it, so tap **Continue** once it is in. The printer then draws it in slowly, purges and puts back whichever tool was on the carriage before (or parks, if none was). The purge is 50mm past the nozzle unless you change **Purge Length** (0-200mm) with the slider on the Multi-Filament System Management screen (the gear in the panel header). **Unload** mounts, heats, retracts until the filament is fully out, and restores the carriage the same way. Because Unload mounts the tool first and nothing senses filament in a hot end, every tool offers it, docked or mounted; it is withdrawn while the dock sensors disagree. The prompt's words are English even on a translated screen, because every client attached to the printer shows them. **Cancel** on the prompt or in the sidebar, closing the prompt without answering, or leaving it waiting for three minutes all turn the heater off and restore the carriage, with a message saying so.

Ticking several tools loads or unloads them in one run. A load asks once up front - insert a strand into every ticked tool as far as it goes - then works tool by tool, heating each next tool to its temperature while the current one runs, and puts the carriage back at the end. Stopping part way (canceling, a timeout, a failure) turns every heater off, restores the carriage, and names the tools that did finish. Neither works while a print is running or paused.

**Tool changers that cannot feed filament.** On a klipper-toolchanger with no feed command, filament goes in and out of each tool by hand. The sidebar shows **Mount...** and **Park** instead of Load and Unload, and a slot's menu shows Load and Unload grayed out with **Load and unload filament at the tool by hand**, plus **Mount** (or **Park**, on the mounted tool). On the Creator 5 with the afclite feed, Load feeds the tool and Unload stays a cut by hand.

## When Bypass Doesn't Appear

Some filament systems do not report a bypass position. On those, the Bypass toggle is hidden and no external spool appears on the filament path:

| System | Reason |
|--------|--------|
| Anycubic ACE Pro | The ACE protocol has no bypass position - unless your rig has the fifth-spool master switch (see below) |
| Snapmaker U1 | Each toolhead has its own path, so there is nothing to bypass |
| Tool changers (generic Klipper) | Each tool has its own path |
| QIDI Box | Not implemented in the QIDI backend yet |
| Happy Hare | Only when `[mmu_machine] has_bypass` is `0` |

The Creality CFS no longer appears here: it has a working external spool. See [CFS and the External Spool](filament-cfs.md#cfs-and-the-external-spool).

To show the controls anyway, turn on **Enable Bypass Controls** in Settings > Printing > Multi-Filament System Management. The setting appears only when your firmware reports no bypass.

With it on, the external spool appears on the filament path beside your slots. Tap it to set material, color, and brand, or to link a Spoolman spool.

**The ACE Pro's fifth spool is a working bypass.** The ACE protocol has no bypass position of its own, but some rigs have a master switch that turns the whole ACE path off so a fifth spool can be fed to the toolhead by hand. Where the driver publishes that switch, HelixScreen drives it as the bypass: the switch being off *is* bypass engaged. There is no firmware command for it, so the switch is thrown by macros - define `ACE_BYPASS_ON` and `ACE_BYPASS_OFF` on your printer and the Bypass toggle works, with the usual unload-first and never-during-print guards. A rig without the switch, or with only one of the two macros, reports no bypass and the table above applies.

**On Happy Hare, the bypass itself also works.** `MMU_SELECT_BYPASS` does not check `has_bypass` - it deselects the gear steppers and reports gate -2 either way. Turn the setting on if your MMU has a bypass but reports `has_bypass: 0`. That happens with `mmu_vendor: Other` (which includes a QIDI Box driven through Happy Hare) and with a type-A selector whose bypass offset is not calibrated yet.

**On the other systems, the setting changes only what HelixScreen displays.** There is no bypass command to send, so the Bypass toggle reports that the operation is not supported. Use the external spool to record the material and color you loaded by hand: [filament tracking](filament-tracking.md), spool presets, and purge temperatures all read from it. Load and unload with your own macros or from the Extrusion panel.

**On the systems where bypass genuinely engages** (AFC, AD5X IFS, Happy Hare, Creality CFS, and an ACE Pro with its fifth-spool switch), it also quiets the pre-print filament checks. Filament fed through the bypass never passes through a slot, so a print started that way would otherwise be flagged for every tool it uses. See [Pre-Print Filament Check](print-monitoring.md#printing-from-bypass). On the display-only systems above nothing is suppressed, because bypass never actually engages there.

**Always Show Bypass Spool**, in the same place, keeps the external spool on the filament path while bypass is disengaged. It applies to AFC systems only (Box Turtle, OpenAMS), which report a bypass sensor whether or not one is wired, so the spool is otherwise hidden until bypass is engaged.

## Loading and Unloading the Bypass Spool

> Added in 1.1.

**Tap the external spool** on the filament path. Alongside the spool bookkeeping (Edit Spool, Select Spool, Scan QR, Clear) the menu offers **Load** and **Unload**, which feed and retract the bypass spool itself.

Bypass load deliberately does not go through the AMS. It runs your configured **Load Filament** macro, the same one the Filament panel's Load button uses, set in Settings > Printing > Macro Buttons, or a plain feed if you have no macro configured. With bypass engaged, the normal load routine takes over. Unload does go through the AMS on backends that expose a bypass unload, because that is how the filament gets back out of the toolhead.

If bypass is not engaged when you tap **Load**, it is engaged first. On systems that require it, a lane's filament is unloaded before bypass engages, and the load starts once that finishes: the same sequence the Bypass toggle performs, so nothing is fed down the lane path by mistake.

Both entries gray out for the same reasons the Filament panel's buttons do: while a filament operation is already running, while a job holds the machine, and (for Unload) when there is nothing at the toolhead to retract.

> The same Load and Unload are on the **Filament panel**, which acts on the external spool whenever bypass is the current selection.

## Switching Toolheads from the Filament Path

On printers with a toolhead per slot (the Snapmaker U1, tool changers, and AFC units with an extruder for each lane), the filament path draws every toolhead. **Tap a toolhead that is not mounted** to switch to it. HelixScreen asks "Switch to Tool 2 (T1)?" first, unless you turned off [Confirm tool changes](settings/printing.md#confirm-tool-changes). The same rules apply as on the home screen's [Tool Switcher](home-panel.md#switching-tools): a paused print always asks, and a running print, spools on the bed, or a filament system waiting for you refuses the change with a message.

Tapping the mounted toolhead, or any slot's filament tube or spool, opens the slot menu below instead, so a tap meant for the filament never moves the toolhead.

## Slot Context Menu

**Tap any slot** to open a context menu with actions for that specific slot. It holds actions only, so it always fits the screen without scrolling. To move the selector, tap it on the filament path: its menu steps it to the next or previous gate.

| Action | Description |
|--------|-------------|
| **Load** | Feed filament from this slot to the toolhead. Disabled if the slot is empty. |
| **Unload** | Retract filament from this slot. Only available if this slot is currently loaded. Grayed out while filament sits in the toolhead that no slot claims, because the slot this menu names may not be the one holding it: use **Unload** in the filament sidebar instead. |
| **Eject** | Push filament fully out of the lane to release the spool, when the slot has filament in its lane but **not** loaded into the toolhead. Replaces the Unload button in that state. Only on backends that support per-lane eject (AFC and Happy Hare). |
| **Recover** | Pull filament that is stranded partway down the tube back toward its slot, without heating the nozzle. Takes the place of **Unload** when the system can tell that this slot's filament is stuck past the hub. |
| **Check Slot** | Happy Hare only. Check this gate's filament sensor. Grayed out while the MMU is busy. |
| **Preload** | Happy Hare only. Feed filament from the spool into its gate, ready for a later load. Grayed out during a print, while any filament is loaded, or while the MMU is busy. |
| **Edit Spool** | Open the spool editor to view or change the material, brand, color and remaining weight, and the slot's **Assigned Tool** and Endless Spool backup. See [Editing Filament Properties](#editing-filament-properties). |
| **Select Spool** | Open the spool editor on your list of Spoolman spools, to assign one to this slot. Only shown when Spoolman is configured. |
| **Scan QR Code** | Scan a filament QR code to auto-fill spool data. Only shown when Spoolman is configured. |
| **Clear Spool** | Erase everything HelixScreen and the printer's firmware remember about this slot: your saved edits, the slot's spool details, and the Spoolman link, plus the printer's own record where the firmware lets it be wiped (Happy Hare's whole gate map entry, the QIDI Box's stored slot values, the CFS's stored color and material, an ACE slot on the ACEPRO driver). What is left afterwards is only what the hardware can physically read - on the Snapmaker U1 and on ACE drivers other than ACEPRO the firmware's record cannot be wiped (it is read-only), so the slot repopulates from it, and an RFID spool's tag is read again the next time it goes in. Shown whenever the slot carries an assignment, loaded or not. Refused on the lane feeding an active, paused or preparing print - the button grays out and says why. |

> **Note:** On a Happy Hare printer that fills its gates from Spoolman (Spoolman pull mode), the firmware side is refused for both edits and Clear Spool, and the reason names Spoolman: make color, material and spool changes in Spoolman itself. Clear Spool still clears HelixScreen's own copy, so remove the spool in Spoolman to finish the job.

**Endless Spool** switches to another spool when one runs out mid-print. Creality calls it Auto-Refill, Snapmaker Auto-Replenish, zMod Infinite Spool Mode, AFC infinite spool and Happy Hare EndlessSpool; HelixScreen calls it Endless Spool everywhere except on a vendor's own toggle. On systems that support it, the slot's **Edit Spool** editor has, below the spool's details:

- **Backup Slot** (**Backup Lane** on AFC, **Backup Gate** on Happy Hare): Choose where to switch to if this spool runs out mid-print. The backup must hold a compatible material; a slot holding something else is marked **(incompatible)** and can't be picked. A slot holding the *same* material in a different grade, PLA-CF behind PLA, say, is marked **(different grade)** and *can* be picked: the swap will work, but filled filaments print slower and wear a brass nozzle, and this one happens mid-print without you there, so the label tells you before you choose it. This picker appears on **AFC (Box Turtle)** and on **single-unit Happy Hare** setups; a multi-unit Happy Hare shows its groups read-only, because the command that edits them acts on whichever unit is selected.

**Assigned Tool** - On systems where HelixScreen can set which tool a slot feeds, the editor has an **Assigned Tool** dropdown beside it: pick the tool this slot should feed. See [Assigned Tool](#assigned-tool) below.

Both are saved with the editor's **Save**, like the rest of its changes. A line under them says what a runout will do.

**The Creality CFS is different: there is nothing to pick.** Its **Auto-Refill** is managed entirely by the box's firmware. Stock CFS firmware never says which slot it would switch to, so the editor has no Backup Slot row and no backup arrows are drawn between the slots; the line at the bottom of the editor says whether Auto-Refill is on and that the firmware chooses. A community Kalico-based CFS setup that publishes its switch plan is the exception: while Auto-Refill is on, a slot with a planned backup shows the Backup Slot row **grayed out** with that slot in it, and arrows mark the plan. Either way, the box decides for itself which slot can stand in, and it only accepts one holding the **exact same material and the exact same color**. If nothing matches, or Auto-Refill is switched off, it does not switch at all: the print stays paused and HelixScreen tells you which of the two it was. Auto-Refill itself is turned on or off with the **Endless Spool** switch on the Multi-Filament System Management screen.

> **Clearing every backup at once:** To remove every Endless Spool backup in one step (back to "a runout just stops the print"), open the Multi-Filament System Management screen and tap **Reset Endless Spool**. See [Multi-Filament System Management](#multi-filament-system-management) below.

> **Assigning tools:** Which tool a slot feeds is set in two places. For one print, use the **Filament Mapping** card that appears when you select a multi-tool file: see [Filament Mapping](filament-mapping.md#filament-mapping). To change the printer's own standing assignment, pick the slot's **Assigned Tool** in its **Edit Spool** editor: see [Assigned Tool](#assigned-tool) below.

## Editing Filament Properties

Tap a slot and choose **Edit Spool** to open the spool editor. This tells HelixScreen what's loaded in the slot, which matters most on systems that can't read it themselves. The title names the slot (for example **Gate 3** or **Lane 2**). When the slot is linked to a Spoolman spool, the title also shows the Spoolman icon and the spool's number.

**What the editor shows:**

- **Summary line** - a color dot and the filament in words, for example **eSUN Silk PETG, Blue**. It follows your changes as you make them.
- **Remaining weight** - the pill on the summary line, for example **750 g of 1000 g**. Tap it and type the grams left on the spool.
- **Temperatures** - the nozzle range and bed temperature for what you've picked: a product's own range when you pick one, otherwise the material's usual range.
- **Material, Variant, Brand, Product** - four dropdowns that narrow each other down. Pick the material first (PLA, PETG, ABS, ASA, PA, TPU, PC and the rest); **Variant** offers that material's kinds (Basic, +, CF, GF, HS, Silk, Matte, Wood, Marble, Recycled and so on); **Brand** lists the makers of it, with **Generic** first; **Product** lists that brand's lines and is hidden while the brand is Generic. With Spoolman, the materials and brands of your recent spools come first in their lists. Changing the material resets the variant to Basic, and keeps the brand when it makes the new material, otherwise it goes back to Generic. The Material and Brand lists end in **Browse catalog...**, which opens the full filament catalog: search it, or type a filament it doesn't list, and the pick fills in all four.
- **Color** - a grid of color swatches, beside the dropdowns or below them depending on the screen. Tap one to use it. The rainbow swatch at the start of the row opens the full color view (so does tapping the color dot on the summary line). A ring marks the current color, on the rainbow swatch when the color is one the row doesn't show.

**The color view** has the full set of preset colors and a custom color picker (a hue and shade picker, a hex field and **Select**). Tap a preset to use it, or mix a color and tap **Select**; either takes you back to the editor. On larger screens the presets and the picker show side by side. On small screens (480x320, 480x272 and portrait 320x480) they take turns: tap the rainbow swatch at the start of the presets to switch to the picker, and **Presets** to switch back. Back returns to the editor without changing the color.

**From tag.** On systems that read the spool's RFID or NFC tag, a small **from tag** pill sits beside each value that came from the tag: on the summary line, the material, brand, product, color and the temperatures. Change a value and its pill goes away. **Reset to tag**, below the form, puts back every value the tag states.

**Clear Spool**, also below the form, takes the spool off the slot and closes the editor. It clears the slot exactly as **Clear Spool** in the slot menu does (see [Slot Context Menu](#slot-context-menu)), appears whenever the slot has a material or a linked Spoolman spool, and needs no Spoolman connection: the Spoolman spool itself is left alone.

**Spoolman** (when Spoolman is configured):

- The two round buttons in the header, left of **Save**, open your **Spoolman spool list** and **Scan QR Code** (scan a spool label to look the spool up). In the list, search for a spool and tap it: the editor comes back with that spool's brand, material, color and temperatures filled in, ready to save. The list ends in **Not in your spools? Set it up**, which goes back to the editor so you can describe the spool by hand.
- **Select Spool** in the slot's menu opens the same list directly. Tapping a spool there links it to the slot and saves straight away.
- **Grayed-out spools** in the list are already linked to another slot (or tool) and can't be picked: one physical spool can't sit in two places. Unlink it from the other slot first, or pick a different spool. The External Spool editor grays out every spool that is linked to a slot, so to link a spool to a CFS or AMS slot, edit that slot rather than the external spool.
- Linking a spool to a slot also decides which spool Spoolman deducts usage from when that slot is loaded. See [Which spool Spoolman charges](filament-tracking.md#which-spool-spoolman-charges-on-a-multi-filament-system).
- **More** (only on a slot linked to a Spoolman spool) opens the spool's **Price**, **Location**, **Lot/Batch Nr** and **Notes**, plus **Print Label** when a label printer is set up. These are saved to the spool in Spoolman.
- **Add to Spoolman + label** (only on a slot that isn't linked) creates the spool in Spoolman from what you've entered, links it to the slot, and prints its label when a label printer is set up.

**Saving:**

1. **Save** in the header lights up once something has changed, including a field under **More**.
2. If the slot is linked to a Spoolman spool and you changed its material, brand or color, HelixScreen asks **Different filament?**: **Update in Spoolman** points the spool at the matching filament in Spoolman (creating it if needed; a filament record other spools share is never edited), and **Unlink, keep on this slot** keeps your changes on the slot only and removes the Spoolman link.
3. The editor closes once the save is done. A save that is still talking to Spoolman finishes even if you leave the screen. Opening the same slot again before it finishes shows **Still saving this spool…**; try again in a moment.

Tapping Back with unsaved changes asks whether to **Save** or **Discard** them.

**When a save is only partly done**, a message says what did and didn't save:

- **Saved. Spoolman is offline, so price, location, lot and notes stay here unsaved.** The slot's own changes are saved; the **More** fields stay in the editor for when Spoolman is back.
- **This spool's details haven't loaded from Spoolman yet. Try Save again.** The **More** fields have nothing to save into yet. Nothing is lost; tap Save again in a moment.
- **Saved to Spoolman, but the spool's price, location, lot and notes didn't save.** Everything else was saved.
- **Spoolman saved the price, location, lot and notes, but not the rest. Nothing else changed on this printer.**
- **Spoolman is offline, so brand and material can't be saved.** A linked spool's brand and material come from Spoolman, so they wait until it is reachable.

**Assigned Tool** and the Endless Spool backup sit side by side under the colors, above **More**, on systems that support them. They are saved with **Save** too. See [Assigned Tool](#assigned-tool) and [Slot Context Menu](#slot-context-menu) for Endless Spool.

> **Adding brands and products from a file.** The catalog can be extended or corrected by editing `user_filaments.json` instead of using the screen. See [Editing materials and brands by hand](settings/printing.md#editing-materials-and-brands-by-hand).

> **Tip: favorite filaments.** Tap the star on any row in the filament catalog (the brand and material picker). Starred filaments lead the vendor list in a **Favorites** section and float to the top of their own vendor's list too, on every printer. Tap the star again to unstar.

> **Material names with punctuation or spaces.** On AFC and Happy Hare the material is stored
> by the firmware itself, so the name has to be something Klipper accepts. Names like `PLA+`,
> `PA6-CF`, `PETG-CF` and `Silk PLA` all save correctly. If a Spoolman spool's material name
> contains something HelixScreen cannot send (a semicolon, a quote, a backslash, or a
> non-English character), the save tells you so: everything else - color, weight, Spoolman
> link - is still saved, and renaming the material in Spoolman using letters, digits, spaces
> and `+ - _ . ( ) /` fixes it.

## Where Lane Information Comes From

Every piece of information on a lane - color, material, brand, spool name, remaining weight - can arrive from more than one place: you edit it on the screen, the printer's firmware reports it, or a linked Spoolman spool carries it. HelixScreen remembers which source said what, and the more trustworthy source for each field wins, rather than whichever wrote last:

- **A color you pick yourself beats the linked spool's.** The spool record says what the vendor sells; your pick says what is actually loaded in that lane right now. The color name you chose travels with it, so a swatch is never labeled with a different color's name.
- **A spool linked from Spoolman owns its brand, material and spool name.** While Spoolman is unreachable those fields are read-only in the editor - the picker shows the spool's own values instead of letting you type over what the server will replace anyway. The spool's remaining weight also comes from the server.
- **Your edits are marked as yours in the record the printer shares with other tools**, so Mainsail, OrcaSlicer or a macro refreshing that record does not wipe what you entered.
- **A reading that leaves a field out leaves it standing.** A status frame that says nothing about, say, brand keeps whatever another source already knew.

**Swapping a spool is noticed, even with the screen off.** Each lane remembers a fingerprint of the spool it held, so a spool swapped while HelixScreen was not running clears the old spool's edits instead of painting them onto the new one. This works on the QIDI Box, on the Creality CFS (including community Kalico-based CFS setups; see [RFID Detection](filament-cfs.md#rfid-detection) for the swaps it can't see), and - by RFID tag - on the Snapmaker U1.

## Assigned Tool

Separately from Filament Mapping, some filament systems keep their own table of which slot each tool prints from, and it stays in place from one print to the next. Where HelixScreen can write that table (such as AFC, Happy Hare and the QIDI Box), the spool editor shows an **Assigned Tool** dropdown: tap a slot, choose **Edit Spool**, pick the tool that slot should feed, and **Save**. The dropdown is left out where the system keeps no table HelixScreen can write, such as the Snapmaker U1 and the ACE. The tool badges in the slot view show the result.

Filament Mapping is chosen per print, from the file you are about to print. Assigned Tool is the printer's standing table, which a print uses when nothing maps it otherwise.

## Multi-Filament System Management

Tap the **gear** in the panel header to open **Multi-Filament System Management**, the screen for advanced controls. Its top section, **Quick Actions**, has the buttons below, and **Status** and **Details** at the bottom describe the system's state:

- **Home**: Return the AMS to its home position. Sends `MMU_HOME` on Happy Hare (which unloads any loaded filament first) and `AFC_RESET` on AFC. Unlike the sidebar's Reset, it does not clear the error message first.
- **Recover**: Attempt to recover from an error state. On Happy Hare this runs `MMU_RECOVER`, which tells Happy Hare what is really selected and loaded when its tracking has gone wrong; nothing moves. A dialog asks which gate is actually selected (it starts on the current one; you can also pick **Bypass**, or **Keep current** to leave the gate alone) and whether filament is loaded (**Detect automatically**, the default, lets Happy Hare check its sensors; or choose **Loaded** / **Unloaded**). HelixScreen then sends `MMU_RECOVER` with only what you set: `GATE=` or `BYPASS=1`, plus `LOADED=1` or `LOADED=0`. On AFC, Recover sends `AFC_RESET`, the same as Home.
- **Abort**: Cancel the current operation immediately
- **Bypass Mode**: Toggle direct-feed mode (if supported by hardware). The toggle refuses while a filament operation is running, and on systems that require it, while filament is still loaded - unload first. When a hardware sensor owns the bypass, this row becomes a read-only "Controlled by hardware sensor" indicator instead of a toggle. If your machine has no bypass according to its firmware, an **Enable Bypass Controls** toggle appears here instead - see [When Bypass Doesn't Appear](#when-bypass-doesnt-appear)
- **Always Show Bypass Spool**: Keep the external spool visible on the filament path even while bypass is disengaged (AFC systems only)
- **Keep Spool Info on Eject**: When a lane is emptied, keep its spool details so reloading the same spool after maintenance needs no re-selection (on by default). Turn it off to start fresh when a lane empties. Shown only where HelixScreen's own record is all the printer knows about a lane's spool: tool changers (including MedusaHC and INDX), OpenAMS without `[openams_spoolman]`, and the ACE without the ACEPRO driver. Elsewhere the printer's firmware holds the spool and decides what an eject keeps; on AFC the same row switches AFC's own `remember_spool` for every lane.
- **Link Spools from Tags** - When a spool whose tag names a Spoolman spool is put in a slot, link the slot to that spool (on by default). Shown on systems whose tags can carry a Spoolman spool number (Creality CFS) when Spoolman is set up. See [Tags and Spoolman](filament-cfs.md#rfid-detection).
- **Endless Spool**: Turns the firmware's own runout switching on or off, on systems whose firmware takes the switch (Happy Hare, the ACE, the Creality CFS and the Snapmaker U1; this is the Creality CFS's Auto-Refill and the U1's Auto-Replenish).
- **Reset Endless Spool**: Clear every slot's backup at once, so a runout stops the print until you set up Endless Spool again. Only appears on systems whose Endless Spool backups you can edit here (AFC, single-unit Happy Hare); hidden on CFS and AD5X, which manage it in firmware. Asks you to confirm before clearing. See [Endless Spool / Backup Slot](#slot-context-menu) above.
- **System status**: Current system state and firmware version

Below the top-level controls, **device-specific settings appear as expandable sections** that vary by hardware. Tap a section to open it; inside you'll find buttons, on/off toggles, and sliders for that group, and changes apply immediately.

**AFC (Box Turtle and friends)** exposes the richest set, organized into sections:

| Section | What's inside |
|---------|---------------|
| **Setup** | Run Calibration Wizard, Bowden Length, LED toggles (system + per-toolhead), Quiet Mode |
| **Speed** | Forward and reverse move-speed multipliers |
| **Toolhead** | Calibrate Toolhead, then the sensor-to-nozzle, unload, and post-sensor clear distances (per tool). **Calibrate Toolhead** walks you through measuring the sensor-to-nozzle and unload distances and the cutter retract, with AFC asking each question on screen. It appears only when your AFC version has that calibration |
| **Maintenance** | Test All Lanes, Change Blade, Park, Clean Brush, Reset Motor Timer |
| **Hub & Cutter** | Cutter enable, cut distance, hub bowden length, assisted retract |
| **Tip Forming** | Ramming volume, unloading start speed, cooling-tube length and retraction |
| **Purge & Wipe** | Purge enable/length, brush-wipe enable |
| **Print Behavior** | Unload at end: unload the filament when a print finishes |

**Happy Hare** groups its controls into Setup, Speed, Toolhead, Accessories and Maintenance. A few worth knowing:

- **Load Extruder** and **Unload Extruder** (Maintenance) run only the extruder part of a load or unload, for filament that is already at the toolhead. Happy Hare heats the nozzle for them. Not available during a print.
- **Motors** (Maintenance) switches the MMU's motors on or off. Switching them on does not home the MMU.
- **Refresh Spoolman** (Accessories) asks Happy Hare to re-read its spool details from Spoolman. Grayed out when Spoolman support is off in Happy Hare.

Other backends show their own (usually smaller) set of sections, or none at all.

## Tips

- When an AMS slot is actively loaded, its material information drives spool preset behavior: you'll see a spool preset button on the Filament and Temperature panels, and purge macros receive the correct temperature automatically. See [External Spool Configuration](filament.md#external-spool-configuration) for details.
- The filament path diagram at the bottom of the slot view is interactive: tap a slot's spool or tube to open its slot menu, and on printers with a toolhead per slot, tap another toolhead to [switch to it](#switching-toolheads-from-the-filament-path).
- During a load or unload, watch the step progress in the sidebar to track exactly where the operation is.
