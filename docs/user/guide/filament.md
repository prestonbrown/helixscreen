# Filament Management

---

## External Spool Configuration

If you're not using an AMS or multi-material system, you can tell HelixScreen what filament is loaded by configuring an **external spool**. Tap the spool icon on the Filament panel to set the material, color, and brand. If Spoolman is configured, you can also link to a specific Spoolman spool. To take the spool off again, open it and tap **Clear Spool** below the form. A spool whose filament carries a tuned pressure advance applies it when it becomes active; see [Pressure advance per filament](filament-tracking.md#pressure-advance-per-filament).

Once configured, the external spool information is used throughout the UI:

- **Spool preset button** — A dynamic preset button appears in the Filament panel's preset grid, between TPU and Cool Down, with your spool's material name and recommended temperatures. Tap it to pre-heat both the nozzle and bed to the correct temperatures for your loaded filament.
- **Temperature panel presets** — The Nozzle and Bed temperature panels also show a spool preset button for quick one-tap heating.
- **Purge temperature** — When you tap **Purge**, HelixScreen passes a nozzle temperature to the purge macro (as the `PURGE_TEMP` parameter): the nozzle's current target or the spool's recommended temperature, whichever is hotter. A temperature at or below your printer's minimum extrusion temperature, or above your hotend's maximum temperature, is never passed; the parameter dialog asks instead.

The spool preset button only appears when the loaded material differs from the standard presets (PLA, PETG, ABS, TPU). For standard materials, just use the built-in preset buttons. Its place in the grid stays empty when it isn't shown, so the other buttons never move.

> **Tip:** The spool preset updates automatically when you change the external spool configuration — no need to close and reopen panels.

> **Tip:** You can change which filament type a preset button controls — long-press it to pick a different material. See [Reassigning a Preset's Filament Type](temperature.md#reassigning-a-presets-filament-type).

---

## Extrusion Panel

![Extrusion Panel](../../images/user/controls-extrusion.png)

Manual filament control:

| Button | Action |
|--------|--------|
| **Extrude** | Push filament through nozzle |
| **Retract** | Pull filament back |

**Amount selector**: 5mm, 10mm, 25mm
**Speed selector**: Slow, Normal, Fast

> **Safety:** Extrusion requires the hotend to be at minimum temperature (usually 180°C for PLA, higher for other materials). If HelixScreen knows what filament is loaded — either from an [external spool](filament.md#external-spool-configuration) or an active AMS slot — it skips the cold-nozzle safety warning and auto-preheats to the correct temperature instead.

---

## Load / Unload / Purge

The Filament panel has dedicated **Load**, **Unload**, and **Purge** buttons. These run Klipper macros — HelixScreen auto-detects common names like `LOAD_FILAMENT`, `UNLOAD_FILAMENT`, and `PURGE` from your printer config.

If a load, unload or tool change needs the printer homed and it isn't, HelixScreen homes it first and then carries on, without asking. Make sure the bed is clear before you start one on a printer that has been sitting idle with its motors off. When HelixScreen can see that your multi-material system homes on its own (AFC's `auto_home` setting in `AFC.cfg`), it leaves the homing to it.

On a printer with more than one tool, the picker beside the lane bars chooses which tool these buttons act on. Its label uses your printer's own word for a tool (**Toolhead** on the Snapmaker U1), and the closed picker shows just the tool's number, or the name you gave it; open it to see the full list. When the nozzle is too cold to move filament and HelixScreen doesn't know what is loaded, a warning takes the place of the **Operations** heading until the nozzle is hot enough.

### Customizing which macro runs

You can override any of these buttons to run a different macro:

1. Go to **Settings > Printing > Macro Buttons**
2. Scroll to the **Standard Macros** section
3. Tap the dropdown for **Load Filament**, **Unload Filament**, or **Purge**
4. Select **(Auto)** to use auto-detection, or pick any macro from your Klipper config

This works whether or not you have an AMS system. If a slot is left empty (no macro detected or configured), the button is disabled.

> **With an AMS system:** by default the Load and Unload buttons drive your filament system directly — slot-based load and unload through the AMS — rather than running a macro. Pick a macro yourself and **your choice wins**: the button runs your macro and the filament system's own handling is skipped for that operation. That is the point of the override, and it is worth knowing what it means, because your macro then owns everything the built-in path would have done — on AFC, for example, `TOOL_UNLOAD` no longer runs, so parking the shuttle and marking the lane are up to your macro. Set the slot back to **(Auto)** to hand the operation back to the filament system.
>
> The Purge button always uses your configured macro.

### Manual extrude/retract

For manual control without macros, use the **Extrude** and **Retract** buttons on the extrusion widget with selectable amounts (5mm, 10mm, 25mm) and speeds.

### When no preheat happens first

Loading or unloading normally heats the nozzle to the material's temperature before the operation starts. HelixScreen skips that preheat when something else already handles the heat, so the nozzle is not heated twice:

- your filament system heats on its own when it loads,
- your configured Load/Unload macro heats the hotend itself, or
- **Allow cold load/unload** is on (Settings > Printing).

### What happens to the nozzle afterward

Loading or unloading heats the nozzle to material temperature. Two minutes after the operation finishes, HelixScreen turns the extruder heater back off — the delay lets you run several operations in a row without the nozzle cooling in between, and a running print is never touched.

If your filament system already handles this (AFC does), turn off **Settings > Printing > Cool nozzle after filament ops** so only one of them is driving the heater. It's a per-printer setting. See [Printing settings](settings/printing.md#cool-nozzle-after-filament-ops).

### Loading on a QIDI (M604)

QIDI's own load macro, `M604`, is only the first step of a wizard that QIDI's stock screen walks you through. HelixScreen runs the whole wizard, so a load from the Filament panel finishes on the screen instead of stopping with a hot nozzle and filament that nothing has gripped:

1. Tap **Load**. The printer heats the nozzle and parks over the waste chute.
2. A **Load Filament** dialog asks you to push the filament into the extruder until it stops. It is gripped as soon as the filament sensor sees it, or tap **Extrude** to feed it yourself.
3. Once it has fed, the dialog asks **Is new filament coming out of the nozzle?** Tap **Extrude more** to push more through, or **Done** when the new color runs clean.

However the dialog ends, with **Done** or **Cancel**, HelixScreen cools the nozzle and turns the filament sensor back on, which the stock macro leaves off. An unload with QIDI's `M603` turns the sensor back on afterward too. On firmware older than 01.01.02.x, which has no push-in step, the dialog opens straight on the question.

With a QIDI Box, box slots load and unload the way the stock screen does them.

---

## AMS / Multi-Material Systems

![AMS Panel](../../images/user/ams.png)

For multi-material systems (Happy Hare, AFC-Klipper, ACE, Tool Changer, etc.). The AMS panel has two main areas: the **slot view**, which draws your spools and the filament path, and the **sidebar**, which shows what is loaded and holds the controls.

How the two are arranged depends on the shape of your screen:

- **Landscape screens** put the slot view on the left and the sidebar on the right.
- **Portrait screens** (taller than wide) stack everything in one column. From the top: the header, with the back button, your filament system's logo and its name; the loaded-filament strip and the status line, with the **Bypass** switch at the end of the status line when your system has one; the slot view and filament path at full width; and the action buttons, or the step progress while an operation runs, across the bottom.

![AMS panel on a portrait screen](../../images/user/ams-portrait.png)

### Slot View

The slot view shows all your filament slots in a visual tray layout:

- **Spool icons** — Each slot displays a 3D spool visualization with its filament color
- **Spool labels**: Above each spool. With four slots or fewer, the label shows the spool's name (for example "Bone White") over its material (PLA, PETG, ABS, etc.). When the name already says the material, as in "PLA+ Bone White", only the name is shown, and a slot with no name shows just the material. With five or more slots there is only room for the material, shortened to its first four letters. On smaller screens five or more labels alternate between two rows, so neighbors never overlap, and a thin line leads from a raised label down to its spool.
- **Status badge** — Slot number with color-coded background (green = loaded, gray = empty, red = error)
- **Tool badge**: A small badge (T0, T1, etc.) on a slot shows which tool prints from it. When every tool feeds the slot of its own number (T0 the first slot, T1 the second, and so on), the badges would only repeat the slot numbers, so they are hidden. They appear when the tools are mapped any other way, for example after you change a slot's [Assigned Tool](#assigned-tool) or when two tools share a slot. A badge in the warning color marks a mapping you set yourself rather than the one your system reports.
- **Environment pill**: On a box that reports temperature or humidity, a pill stands at the right end of the box: a thermometer with the temperature over a water drop with the humidity. The drop fills to the humidity reading, so a wet box shows a full drop at a glance; a box whose sensor gives no reading shows an empty outline. While the box dries, the temperature line shows the heat icon and the time left. Tap the pill to open that box's drying and humidity screen (see [Filament Drying and Humidity](#filament-drying-and-humidity)). Boxes with no readings have no pill, and the box takes the full width.

Below the slot grid, a **filament path diagram** shows the routing from slots through the hub/selector to the toolhead. This updates in real time during load/unload operations, including eject animations when retracting filament at the slot sensor. On the Anycubic ACE the path reads the hub and toolhead sensors the driver publishes, so a strand parked partway down the tube is drawn where it actually sits - short of the hub - rather than back at the spool.

#### Reading Error States

When a slot runs into trouble, HelixScreen shows it visually so you don't have to dig through logs:

- **Error dot** — A small colored dot appears at the corner of a slot's spool when that slot reports a problem. **Red** means an error (jam, runout, hardware fault); **amber** means a warning. With animations enabled, the dot gently pulses to draw your eye.
- **Buffer-health tint**: On systems with a buffer between the slots and the toolhead (an AFC buffer - a TurtleNeck, or a pressure-sensor buffer such as an `FPS_PSF` - an OpenAMS filament pressure sensor, or Happy Hare with sync feedback), the hub on the filament path diagram changes color as the buffer drifts toward a fault: gray on target, amber as it drifts, red near an end stop.
- **Filament pressure (OpenAMS)**: On OpenAMS running without AFC, each unit's filament pressure sensor appears on the filament path as a box labeled **FPS**. The sensor measures how hard the filament is pressing (compression) only: 0% is no pressure and 100% is fully compressed. The box stays gray while the pressure regulates around its target and turns amber near the top (85% and up) or red (95% and up), or amber near zero while filament is loaded because the feed is not engaging. A sensor with no target is only tinted at those ends. Tap the box to open Buffer Status: an upright gauge that fills from the bottom up to the reading with a tick at the target, the reading against its target (for example "FPS 53%" and "target 50%"), whether it is at, above or below target (within 5 points counts as at target), and the last minute as a line with the target dashed. OpenAMS has no clog detection, so only the pressure reading is shown. AFC pressure-sensor buffers and Happy Hare keep the two-ended slider with its tight and loose wording.

**To recover:**

- Use **Reset** in the sidebar (it reads **Home** on Happy Hare). This clears the error message your system is holding onto and then puts the system back to a known-good state. It is the right first move for almost every jam or fault, including one reported against a single slot. On Happy Hare it runs `MMU_RECOVER` and then `MMU_HOME`, which unloads any loaded filament before homing, so the button is grayed out while a print is running. On AFC it runs `RESET_FAILURE` and `AFC_CLEAR_MESSAGE`, then `AFC_RESET`.
- If filament from one slot is stuck partway down the tube, tap that slot. When your system can pull it back, the slot menu's second button changes from **Unload** to **Recover**. Tap it to draw the filament back toward the slot without heating the nozzle.
- For a system-wide problem that Reset does not shift, use **Recover** in the AMS Management overlay (Settings). On Happy Hare this is how you run `MMU_RECOVER` from the screen, and it moves nothing - see [AMS Management](#ams-management-settings-overlay) for what it sends. When a Happy Hare error pops up on screen, its own **Recover** button sends plain `MMU_RECOVER` and lets Happy Hare work out the filament position from its sensors.

**On Happy Hare**, every fault the MMU reports opens a recovery popup showing Happy Hare's own reason, whether it happened during a print or during a load or home you started yourself. Happy Hare's own error prompt closes behind it. The popup offers:

- **Resume** - only when a print is paused, and first in the list there. Heats the nozzle first if it has cooled.
- **Recover** - lets Happy Hare work out where the filament is with its own sensors. First in the list when nothing is paused. Works on a cold nozzle.
- **Unload** - when filament is at the toolhead. Heats the nozzle first.
- **Unlock** - a last resort that clears Happy Hare's pause lock without moving anything.

### Sidebar

The sidebar shows the status of the currently loaded filament and provides quick-access controls. On a portrait screen the loaded-filament strip and status line sit above the slot view, and the buttons sit below it.

**Currently loaded strip:**

A narrow color bar matching the loaded filament sits beside two lines of text:

- **Position and weight**: Which slot is feeding the toolhead, such as "Current: Slot 2" (or "Current: Bypass" when bypass is enabled), followed by the estimated filament remaining (e.g., "750g") when your system knows it
- **Filament name**: e.g., "Red PLA", "Prusament PETG". A long name is cut short with "..."

When your system has a buffer or flow sensor, its meters sit under the two lines on a landscape screen and to their right on a portrait screen. On smaller screens each meter shrinks to a small icon or arc beside its reading. The meters hide while a load or unload runs, so the step progress has the room.

- **Buffer slider**: When your system has a filament buffer with a proportional reading (a filament pressure sensor, or Happy Hare sync feedback), a small upright gauge appears in the strip, with the short reading and, on wider screens, its **FPS** or **Sync** label under it. For Happy Hare and AFC pressure-sensor buffers it is a slider: loose is up and tight is down; the block is gray on target, amber off target and red near an end. For an OpenAMS pressure sensor it is a fill that rises from the bottom with the pressure, with a tick at the target, gray until it nears the top or bottom end. It is the same reading the [Filament Buffer widget](home-panel.md#filament-buffer-widget) shows. Without a target a slider reading shows as a number only, and a fill keeps its gauge with no tick. On smaller screens a small up-and-down arrow icon stands in for the gauge, beside the reading.
- **Clog detection meter** — When your system has flow monitoring (encoder, FlowGuard, or AFC buffer), an arc meter shows the current reading, with the sensor named underneath. It is the same reading the [Clog Detection widget](home-panel.md#clog-detection-widget) draws as a bar on the dashboard. When the sensor is armed but has nothing to report, the arc is replaced by a check mark. On smaller screens the sensor name is left off.

**During load/unload operations**, the sidebar switches to a **step progress display** showing each stage of the operation. The exact steps come from your filament system, so they match what it actually does rather than a generic list.

On **AFC** (Box Turtle, OpenAMS):

- **Load into an empty toolhead:** Heat nozzle → Feed filament → Purge to bucket → Brush nozzle → Kick away → Load complete
- **Swap to a different slot:** Heat nozzle → Cut tip → Unload filament → Feed filament → Purge to bucket → Brush nozzle → Kick away → Load complete
- **Unload:** Heat nozzle → Cut tip → Retract filament

If AFC's `auto_home` is enabled in AFC.cfg, HelixScreen skips its home-first prompt — AFC homes the printer itself when needed.

On **Happy Hare**:

- **Load into an empty toolhead:** Heat nozzle → Select gate → Load filament → Purge → Load complete
- **Swap to a different slot:** Heat nozzle → Form tip → Cut tip → Unload → Select gate → Load filament → Purge → Load complete
- **Unload:** Heat nozzle → Form tip → Cut tip → Unload

Systems that don't publish a step list of their own get a shorter generic bar: Heat nozzle → Feed filament → Purge, with a tip step added for a swap.

Some steps only apply to how your machine is set up. A step your system never reaches stays grayed out instead of lighting up, so a bar that skips **Cut tip** or **Kick away** is normal, not a stall. Each step updates in real time so you can see exactly where the operation is.

On small portrait screens there is room for the steps only as a row of dots, so the name of the current step is shown under them.

**Action buttons** (hidden while an operation is in progress) sit two to a row. On small portrait screens they sit in one row. Only the buttons your system supports appear. On a landscape screen the **Bypass** switch sits above them; on a portrait screen it sits at the end of the status line.

| Button | Action |
|--------|--------|
| **Bypass** (toggle) | Feed filament directly to the extruder, bypassing the AMS. Shown when your hardware supports bypass, or when you turn on **Enable Bypass Controls** - see [When Bypass Doesn't Appear](#when-bypass-doesnt-appear). The toggle is guarded: it can't be changed while a job holds the machine (a "Bypass cannot be changed while printing" warning appears), if a lane's filament is loaded it is unloaded first before bypass engages, and where a hardware sensor owns the bypass the toggle only reports that the sensor is in control. |
| **Load...** | Opens a picker naming each tool and the slot it loads from - the slot set as its **Assigned Tool**. Pick the tool and confirm. To load any other slot, tap that slot in the slot view and choose **Load** |
| **Unload** | Retract the currently loaded filament back to its slot. It also works when filament is stuck in the toolhead and no slot claims it, on systems that can tell. If your system needs to know which slot that filament came from and cannot tell, Unload first asks **Which lane is in the toolhead?** and lists the lanes that hold filament, even when there is only one. Pick the lane, tap **Clear extruder only** to cut and clear the extruder without pulling any lane back (the rest of that strand stays in its tube, near the toolhead), or tap **Cancel**. When no lane holds filament it asks **Clear the extruder?** instead. On the FlashForge AD5X, a lane whose load failed partway is remembered and unloaded without asking. On a printer with several toolheads it reads **Unload...** and opens the picker instead - see below |
| **Mount...** / **Park** | Tool changers. **Mount...** picks a tool and puts it on the carriage; **Park** puts the mounted tool back in its dock. On a tool changer that cannot feed filament itself, these stand where Load and Unload would be |
| **Reset** | Reset the AMS system state (useful after jams or errors) |
| **Check slots** | Happy Hare only: checks every slot's sensor. The printer parks the toolhead and moves each lane's filament to test it, so the button is grayed out while a print is running |
| **Settings** | Open the AMS Management overlay for advanced controls |

**Load and Unload always mean filament.** They never mount or park a tool - that is what tapping a toolhead, **Mount...** and **Park** are for. A printer that needs a tool on the carriage to feed it mounts the tool as part of the load.

**The tool picker.** Each row names a tool and, where it is not the tool's own, the slot it uses, with what that slot holds - the material and whether it is loaded, ready to load, or empty. The tool you have selected, or else the one on the carriage, comes ticked when it can take the operation. A tool with no slot assigned to it is grayed out with **Assign a slot first**; set one with **Assigned Tool** in the slot's **Edit Spool** editor.

**Loading or unloading several heads at once.** On a printer with a toolhead per slot (Snapmaker U1, MedusaHC), the picker lets you tick several heads. The prompt text the printer shows is English - every screen attached to the printer sees the same words. If a ticked head turns out not to take the operation, it is skipped with a warning naming the head and the reason, and the rest continue. A head whose own sensor reports it empty is not tickable for Unload at all: there is nothing in it for an unload to act on.

**MedusaHC.** One extruder drive serves every tool, so **Load** mounts the tool, heats it and asks you to insert the filament into that tool - the extruder grabs it, so tap **Continue** once it is in. The printer then draws it in slowly, purges and puts back whichever tool was on the carriage before (or parks, if none was). The purge is 50mm past the nozzle unless you change **Purge Length** (0-200mm) under **Purge & Wipe** in Device Operations, reached from the AMS panel's **Settings** button. **Unload** mounts, heats, retracts until the filament is fully out, and restores the carriage the same way. Because Unload mounts the tool first and nothing senses filament in a hot end, every tool offers it, docked or mounted; it is withdrawn while the dock sensors disagree. The prompt's words are English even on a translated screen, because every client attached to the printer shows them. **Cancel** on the prompt or in the sidebar, closing the prompt without answering, or leaving it waiting for three minutes all turn the heater off and restore the carriage, with a message saying so.

Ticking several tools loads or unloads them in one run. A load asks once up front - insert a strand into every ticked tool as far as it goes - then works tool by tool, heating each next tool to its temperature while the current one runs, and puts the carriage back at the end. Stopping part way (canceling, a timeout, a failure) turns every heater off, restores the carriage, and names the tools that did finish. Neither works while a print is running or paused.

**Tool changers that cannot feed filament.** On a klipper-toolchanger with no feed command, filament goes in and out of each tool by hand. The sidebar shows **Mount...** and **Park** instead of Load and Unload, and a slot's menu shows Load and Unload grayed out with **Load and unload filament at the tool by hand**, plus **Mount** (or **Park**, on the mounted tool). On the Creator 5 with the afclite feed, Load feeds the tool and Unload stays a cut by hand.

### When Bypass Doesn't Appear

Some filament systems do not report a bypass position. On those, the Bypass toggle is hidden and no external spool appears on the filament path:

| System | Reason |
|--------|--------|
| Anycubic ACE Pro | The ACE protocol has no bypass position - unless your rig has the fifth-spool master switch (see below) |
| Snapmaker U1 | Each toolhead has its own path, so there is nothing to bypass |
| Tool changers (generic Klipper) | Each tool has its own path |
| QIDI Box | Not implemented in the QIDI backend yet |
| Happy Hare | Only when `[mmu_machine] has_bypass` is `0` |

The Creality CFS no longer appears here — it has a working external spool. See [CFS and the External Spool](#cfs-and-the-external-spool).

To show the controls anyway, turn on **Enable Bypass Controls** in Settings > Printing > Multi-Filament System Management. The setting appears only when your firmware reports no bypass.

With it on, the external spool appears on the filament path beside your slots. Tap it to set material, color, and brand, or to link a Spoolman spool.

**The ACE Pro's fifth spool is a working bypass.** The ACE protocol has no bypass position of its own, but some rigs have a master switch that turns the whole ACE path off so a fifth spool can be fed to the toolhead by hand. Where the driver publishes that switch, HelixScreen drives it as the bypass: the switch being off *is* bypass engaged. There is no firmware command for it, so the switch is thrown by macros - define `ACE_BYPASS_ON` and `ACE_BYPASS_OFF` on your printer and the Bypass toggle works, with the usual unload-first and never-during-print guards. A rig without the switch, or with only one of the two macros, reports no bypass and the table above applies.

**On Happy Hare, the bypass itself also works.** `MMU_SELECT_BYPASS` does not check `has_bypass` - it deselects the gear steppers and reports gate -2 either way. Turn the setting on if your MMU has a bypass but reports `has_bypass: 0`. That happens with `mmu_vendor: Other` (which includes a QIDI Box driven through Happy Hare) and with a type-A selector whose bypass offset is not calibrated yet.

**On the other systems, the setting changes only what HelixScreen displays.** There is no bypass command to send, so the Bypass toggle reports that the operation is not supported. Use the external spool to record the material and color you loaded by hand: [filament tracking](filament-tracking.md), spool presets, and purge temperatures all read from it. Load and unload with your own macros or from the Extrusion panel.

**On the systems where bypass genuinely engages** (AFC, AD5X IFS, Happy Hare, Creality CFS, and an ACE Pro with its fifth-spool switch), it also quiets the pre-print filament checks. Filament fed through the bypass never passes through a slot, so a print started that way would otherwise be flagged for every tool it uses. See [Pre-Print Filament Check](print-monitoring.md#pre-print-filament-check). On the display-only systems above nothing is suppressed, because bypass never actually engages there.

**Always Show Bypass Spool**, in the same place, keeps the external spool on the filament path while bypass is disengaged. It applies to AFC systems only (Box Turtle, OpenAMS), which report a bypass sensor whether or not one is wired, so the spool is otherwise hidden until bypass is engaged.

### Loading and Unloading the Bypass Spool

> Added in 1.1.

**Tap the external spool** on the filament path. Alongside the spool bookkeeping (Edit Spool, Select Spool, Scan QR, Clear) the menu offers **Load** and **Unload**, which feed and retract the bypass spool itself.

Bypass load deliberately does not go through the AMS. It runs your configured **Load Filament** macro — the same one the Filament panel's Load button uses, set in Settings > Printing > Macro Buttons — or a plain feed if you have no macro configured. That is what the reporter of this behavior expected: with bypass engaged, the normal load routine takes over. Unload does go through the AMS on backends that expose a bypass unload, because that is how the filament gets back out of the toolhead.

If bypass is not engaged when you tap **Load**, it is engaged first. On systems that require it, a lane's filament is unloaded before bypass engages, and the load starts once that finishes — the same sequence the Bypass toggle performs, so nothing is fed down the lane path by mistake.

Both entries gray out for the same reasons the Filament panel's buttons do: while a filament operation is already running, while a job holds the machine, and — for Unload — when there is nothing at the toolhead to retract.

> The same Load and Unload are on the **Filament panel**, which acts on the external spool whenever bypass is the current selection.

### Switching Toolheads from the Filament Path

On printers with a toolhead per slot (the Snapmaker U1, tool changers, and AFC units with an extruder for each lane), the filament path draws every toolhead. **Tap a toolhead that is not mounted** to switch to it. HelixScreen asks "Switch to Tool 2 (T1)?" first, unless you turned off [Confirm tool changes](settings/printing.md#confirm-tool-changes). The same rules apply as on the home screen's [Tool Switcher](home-panel.md#switching-tools): a paused print always asks, and a running print, spools on the bed, or a filament system waiting for you refuses the change with a message.

Tapping the mounted toolhead, or any slot's filament tube or spool, opens the slot menu below instead, so a tap meant for the filament never moves the toolhead.

### Slot Context Menu

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

- **Backup Slot** (**Backup Lane** on AFC, **Backup Gate** on Happy Hare) — Choose where to switch to if this spool runs out mid-print. The backup must hold a compatible material; a slot holding something else is marked **(incompatible)** and can't be picked. A slot holding the *same* material in a different grade — PLA-CF behind PLA, say — is marked **(different grade)** and *can* be picked: the swap will work, but filled filaments print slower and wear a brass nozzle, and this one happens mid-print without you there, so the label tells you before you choose it. This picker appears on **AFC (Box Turtle)** and on **single-unit Happy Hare** setups; a multi-unit Happy Hare shows its groups read-only, because the command that edits them acts on whichever unit is selected.

**Assigned Tool** - On systems where HelixScreen can set which tool a slot feeds, the editor has an **Assigned Tool** dropdown beside it: pick the tool this slot should feed. See [Assigned Tool](#assigned-tool) below.

Both are saved with the editor's **Save**, like the rest of its changes. A line under them says what a runout will do.

**The Creality CFS is different: there is nothing to pick.** Its **Auto-Refill** is managed entirely by the box's firmware. Stock CFS firmware never says which slot it would switch to, so the editor has no Backup Slot row and no backup arrows are drawn between the slots; the line at the bottom of the editor says whether Auto-Refill is on and that the firmware chooses. A community Kalico-based CFS setup that publishes its switch plan is the exception: while Auto-Refill is on, a slot with a planned backup shows the Backup Slot row **grayed out** with that slot in it, and arrows mark the plan. Either way, the box decides for itself which slot can stand in, and it only accepts one holding the **exact same material and the exact same color**. If nothing matches, or Auto-Refill is switched off, it does not switch at all: the print stays paused and HelixScreen tells you which of the two it was. Auto-Refill itself can be turned on or off from the CFS device actions.

> **Clearing every backup at once:** To remove every Endless Spool backup in one step — back to "a runout just stops the print" — open the AMS Management overlay and tap **Reset Endless Spool**. See [AMS Management (Settings Overlay)](#ams-management-settings-overlay) below.

> **Assigning tools:** Which tool a slot feeds is set in two places. For one print, use the **Filament Mapping** card that appears when you select a multi-tool file: see [Filament Mapping](#filament-mapping) below. To change the printer's own standing assignment, pick the slot's **Assigned Tool** in its **Edit Spool** editor: see [Assigned Tool](#assigned-tool) below.

### Editing Filament Properties

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

### Where Lane Information Comes From

Every piece of information on a lane - color, material, brand, spool name, remaining weight - can arrive from more than one place: you edit it on the screen, the printer's firmware reports it, or a linked Spoolman spool carries it. HelixScreen remembers which source said what, and the more trustworthy source for each field wins, rather than whichever wrote last:

- **A color you pick yourself beats the linked spool's.** The spool record says what the vendor sells; your pick says what is actually loaded in that lane right now. The color name you chose travels with it, so a swatch is never labeled with a different color's name.
- **A spool linked from Spoolman owns its brand, material and spool name.** While Spoolman is unreachable those fields are read-only in the editor - the picker shows the spool's own values instead of letting you type over what the server will replace anyway. The spool's remaining weight also comes from the server.
- **Your edits are marked as yours in the record the printer shares with other tools**, so Mainsail, OrcaSlicer or a macro refreshing that record does not wipe what you entered.
- **A reading that leaves a field out leaves it standing.** A status frame that says nothing about, say, brand keeps whatever another source already knew.

**Swapping a spool is noticed, even with the screen off.** Each lane remembers a fingerprint of the spool it held, so a spool swapped while HelixScreen was not running clears the old spool's edits instead of painting them onto the new one. This works on the QIDI Box, on the Creality CFS (including community Kalico-based CFS setups; see [RFID Detection](#rfid-detection) for the swaps it can't see), and - by RFID tag - on the Snapmaker U1.

### Filament Mapping

<a id="tool-mapping"></a>

For multi-tool prints, you can control which slot feeds each tool the slicer expects (T0, T1, T2, …). The **FILAMENTS** card on the file detail screen shows one chip for each tool the print uses.

**Reading a chip.** Each chip is split in two:

- **The top band** is what the file asks for: it is filled with the file's color for that tool and labeled with the file's material (or the tool's name, such as **T2**, when the file names no material). On 800x480 screens and larger the material is in bold.
- **The bottom** is what the printer will feed: a spool dot in the color of the slot the tool is mapped to, the slot's number and a small arrow. On a filament system with one unit the number is the slot itself ("3"). With several units it reads unit, then slot: "2·1" is the first slot of the second unit. A tool with no slot shows an empty dashed spool and the arrow; an empty slot shows a red ring instead of the spool dot.
- **The outline** tells you whether the pairing is a problem:
  - **Grey**: the slot holds what the file asks for.
  - **Amber**: the material matches but the color is different. The chip only turns amber when the file says which color it wants.
  - **Red**: the tool has no slot, its slot is empty, or the slot holds a different material.

Chips are narrower on small screens. When the print uses more tools than fit in the row, a **+N** pill stands in for the rest.

**Changing one tool's slot.** Tap a chip. A list opens under it, headed with the tool and its material (for example **T2 · PLA**):

- **Auto** comes first, and lets HelixScreen pick the closest loaded match.
- Then every slot, best match first, each with its color, name, material and a short reason: **exact match**, **color differs**, **material differs** or **empty**. A slot another tool already uses adds that tool, for example "exact match, also T0". Two tools may share a slot.
- The tool's current pick is highlighted.
- **More options…** at the bottom opens the full **Filament Mapping** dialog.

Pick a slot and the chip updates straight away. If your filament system can't remap this file (see [Printers that map filament by rewriting the file](#printers-that-map-filament-by-rewriting-the-file)), the chips' arrows are hidden: tapping a chip offers to install the HelixPrint plugin when that is what's missing, and otherwise does nothing.

Tapping the card's header opens the full **Filament Mapping** dialog, with one row per tool in the file:

- **Map to closest colors with matching material** (toggle at the top) — When on, HelixScreen auto-assigns each tool to the loaded slot with the nearest color and a compatible material; the rows become read-only. When off, you assign tools yourself.
- **Manual assignment** — With the toggle off, tap a tool's row to pick which slot feeds it. The list is the same one a chip opens.
- **Mismatch warnings** — A warning icon appears on any row mapped to an empty slot or a slot whose material doesn't match what the tool needs.

Tap **Done** to keep your mapping, or **Cancel** to discard it.

**Printers with no filament system.** On a single-extruder printer without a filament system, the card still shows when the file names its material, so you can check it against what is loaded. There is one chip per tool the print uses. The first chip's bottom is a spool dot in the color of the filament loaded in the extruder, and its outline compares the file with that filament, judged by the same outline colors. Later chips show a swap icon (two arrows), because a file that changes filament mid-print (with `M600`) asks you to swap spools; they stay grey. When HelixScreen doesn't know what is loaded, the first chip shows a question mark with a grey outline. These chips have no arrow and open nothing: there is nothing to map.

> **Tip:** When you actually start the print, HelixScreen re-checks these mappings and stops with a **Check filament** dialog if any required tool points at an empty slot — unless bypass is engaged, in which case the mappings aren't feeding the print and the check is skipped. See [Print Monitoring & Failure Detection](print-monitoring.md#pre-print-filament-check).

> **Note:** On a filament system, the card only appears when the system supports Filament Mapping. On fixed 1:1 systems (Snapmaker U1, ACE) tools always map directly to their matching slot, so there's nothing to assign.

> **Note:** The card also hides itself while **bypass is engaged on a single-tool file**, because the print takes its filament from the external spool and the mapping decides nothing — showing it would offer an assignment the print ignores. The **Bypass active** note on the file detail screen appears in its place. A *multi-tool* file with bypass engaged still shows the card: those prints do use the lanes.

### Printers that map filament by rewriting the file

<a id="printers-that-remap-by-rewriting-the-file"></a>

Most filament systems carry out your mapping themselves: HelixScreen tells the printer "tool 0 comes from slot 2" and the printer does the rest. A few can't, because their firmware has no mapping table. On those, HelixScreen rewrites a copy of the G-code with the new tool numbers and prints that copy. Today that means a tool changer that swaps with its own `T<n>` macros, such as a [MedusaHC](#medusahc-hotend-changer).

This needs the **HelixPrint plugin** on your printer. Without it the mapping card offers to install it instead. A current plugin makes the rewritten copy on the printer itself; with an older one, the screen downloads the file, rewrites it and uploads the copy. With it, the finished job is listed in print history under its original name ([details](print-history.md)).

**What the rewrite changes:**

- Tool changes: `T0`, `T1`, ...
- Temperature commands for a specific tool: `M104 T1 S220`, `M109 T0 S230`
- An `INITIAL_TOOL=` or `TOOL=` value on any command line, such as `PRINT_START INITIAL_TOOL=0` or `SET_TOOL_TEMPERATURE TOOL=1`, so a start macro that picks up or primes the first tool picks up the right one

**What it does not change:** any other per-tool value you pass to your start macro, such as `EXTRUDER_TEMP=`, `EXTRUDER1_TEMP=`, `T0_TEMP=`, `TOOL_TEMP=` or `T=`, and a tool given by name rather than number (`TOOL=T0`). Those names are your own macro's, and HelixScreen can't know whether `EXTRUDER_TEMP` means "tool 0" or "the tool this print starts with", so it leaves them as sliced. If your start macro heats tools from those values, a job whose tool 0 you map to tool 1 heats tool 0 and then switches to a cold tool 1.

When the file you picked passes values like these and your picks move at least one tool to a different number, the **Filament Mapping** dialog shows a warning naming them, for example:

> Your PRINT_START line passes per-tool settings that Filament Mapping does not change (EXTRUDER_TEMP, EXTRUDER1_TEMP). A tool you map filament to may not be heated.

The warning appears and disappears as you change the picks: leave every tool on its own number and there is nothing to warn about, because the file prints as sliced. It doesn't stop you mapping filament; if your macro doesn't use those values to heat anything, you can ignore it.

**Recommended start G-code.** Do the tool heating in plain `M104`/`M109` lines, which the rewrite does change, and keep only the bed, homing and mesh in `PRINT_START`. In OrcaSlicer's **Machine start G-code** (one `M104` line per tool your printer has):

```
PRINT_START INITIAL_TOOL=[initial_tool] BED_TEMP=[bed_temperature_initial_layer_single]
{if is_extruder_used[0]}M104 T0 S{idle_temperature[0]}{endif}
{if is_extruder_used[1]}M104 T1 S{idle_temperature[1]}{endif}
{if is_extruder_used[2]}M104 T2 S{idle_temperature[2]}{endif}
{if is_extruder_used[3]}M104 T3 S{idle_temperature[3]}{endif}
M109 T[initial_tool] S{first_layer_temperature[initial_tool]}
```

Keep the `M109` for the first tool last. It sets that tool from its idle temperature up to printing temperature and waits, so it has to come after the idle lines. `INITIAL_TOOL=` and `TOOL=` are fine to keep if your macro uses them: the rewrite moves them along with everything else. OrcaSlicer adds a `T[initial_tool]` line right after the start G-code, and that line is rewritten too.

### Assigned Tool

Separately from Filament Mapping, some filament systems keep their own table of which slot each tool prints from, and it stays in place from one print to the next. Where HelixScreen can write that table (such as AFC, Happy Hare and the QIDI Box), the spool editor shows an **Assigned Tool** dropdown: tap a slot, choose **Edit Spool**, pick the tool that slot should feed, and **Save**. The dropdown is left out where the system keeps no table HelixScreen can write, such as the Snapmaker U1 and the ACE. The tool badges in the slot view show the result.

Filament Mapping is chosen per print, from the file you are about to print. Assigned Tool is the printer's standing table, which a print uses when nothing maps it otherwise.

### Syncing with OrcaSlicer (2.3.2 and later)

When you edit spool info in HelixScreen — on any supported filament system (AD5X IFS, Snapmaker U1, ACE, CFS) — that information is saved to your printer in the standard location OrcaSlicer 2.3.2 and later reads automatically. Open OrcaSlicer after editing and your slot's vendor, material, color, and temperatures show up in the filament panel with no extra setup.

**AFC (Box Turtle) and Happy Hare** work the same way automatically — your lane assignments flow through to OrcaSlicer with nothing to configure.

Either way, your printer's filament info and OrcaSlicer stay in sync. The sync is one-way (your printer → OrcaSlicer): editing in OrcaSlicer doesn't change what's loaded in your AMS.

**Precise names on-screen, matchable names in OrcaSlicer.** You can name your filament as specifically as you like — "ASA-GF", "PLA Silk", "PPS-CF" — and HelixScreen keeps showing that exact name on the printer. OrcaSlicer only recognizes broader material families, so when HelixScreen syncs it automatically translates your precise name to the closest one OrcaSlicer knows. That's why a slot showing "ASA-GF" on your printer may appear as "ASA" in OrcaSlicer. This is expected — the color and temperatures still come across correctly, and OrcaSlicer now picks a real ASA preset instead of falling back to a generic PLA one.

> **Tip:** For an unusual material OrcaSlicer doesn't recognize at all, the slot may sync with its color and temperatures but no material selected, rather than a wrong guess. Just pick the filament yourself in OrcaSlicer that one time — your printer keeps showing the precise name.

> **Requirements:** OrcaSlicer 2.3.2 or newer, connected to the same printer's Moonraker. Nothing to enable on the HelixScreen side — it's automatic.

> **OrcaSlicer 2.5:** sync works the same way, with nothing to change. 2.5 also re-reads the slots when you switch OrcaSlicer to a different printer, so each printer's filaments show up without a manual sync. When a slot's filament is a product from HelixScreen's filament catalog with its own OrcaSlicer preset, 2.5 also selects that exact preset (for example a brand's own PLA) instead of a generic one for the material. Earlier versions match by material type only.

> **Nothing comes across on OrcaSlicer 2.4?** If your Moonraker connection in OrcaSlicer has no API key, 2.4 sends a placeholder key that Moonraker rejects, so the sync silently fails. See [Troubleshooting](../TROUBLESHOOTING.md#orcaslicer-doesnt-pick-up-your-slot-colors-or-materials).

### AMS Management (Settings Overlay)

Tap **Settings** in the sidebar to open the AMS Management overlay with advanced controls:

- **Home** — Return the AMS to its home position. Sends `MMU_HOME` on Happy Hare (which unloads any loaded filament first) and `AFC_RESET` on AFC. Unlike the sidebar's Reset, it does not clear the error message first.
- **Recover** — Attempt to recover from an error state. On Happy Hare this runs `MMU_RECOVER`, which tells Happy Hare what is really selected and loaded when its tracking has gone wrong; nothing moves. A dialog asks which gate is actually selected (it starts on the current one; you can also pick **Bypass**, or **Keep current** to leave the gate alone) and whether filament is loaded (**Detect automatically**, the default, lets Happy Hare check its sensors; or choose **Loaded** / **Unloaded**). HelixScreen then sends `MMU_RECOVER` with only what you set: `GATE=` or `BYPASS=1`, plus `LOADED=1` or `LOADED=0`. On AFC, Recover sends `AFC_RESET`, the same as Home.
- **Abort** — Cancel the current operation immediately
- **Bypass Mode** — Toggle direct-feed mode (if supported by hardware). The toggle refuses while a filament operation is running, and on systems that require it, while filament is still loaded - unload first. When a hardware sensor owns the bypass, this row becomes a read-only "Controlled by hardware sensor" indicator instead of a toggle. If your machine has no bypass according to its firmware, an **Enable Bypass Controls** toggle appears here instead - see [When Bypass Doesn't Appear](#when-bypass-doesnt-appear)
- **Always Show Bypass Spool** — Keep the external spool visible on the filament path even while bypass is disengaged (AFC systems only)
- **Keep Spool Info on Eject** — When a lane is emptied, keep its spool details so reloading the same spool after maintenance needs no re-selection (on by default). Turn it off to start fresh when a lane empties. This applies only to spools you selected in HelixScreen: a spool assigned elsewhere (such as Mainsail) clears with the lane. To have every assigned spool remembered no matter where it was picked, use the firmware's own retention instead (AFC: `remember_spool` in AFC.cfg) - HelixScreen follows the spool the firmware reports. Note that when the firmware's own retention is switched on for every lane, it takes precedence: this toggle then has no effect and shows as disabled with a note explaining why. Shown on systems whose firmware tracks spool ids per lane (such as AFC and Happy Hare); systems that detect spool swaps by tag always refresh on a swap regardless of this setting.
- **Link Spools from Tags** - When a spool whose tag names a Spoolman spool is put in a slot, link the slot to that spool (on by default). Shown on systems whose tags can carry a Spoolman spool number (Creality CFS) when Spoolman is set up. See [Tags and Spoolman](#rfid-detection).
- **Reset Endless Spool** — Clear every slot's backup at once, so a runout stops the print until you set up Endless Spool again. Only appears on systems whose Endless Spool backups you can edit here (AFC, single-unit Happy Hare); hidden on CFS and AD5X, which manage it in firmware. Asks you to confirm before clearing. See [Endless Spool / Backup Slot](#slot-context-menu) above.
- **System status** — Current system state and firmware version

Below the top-level controls, **device-specific settings appear as expandable sections** that vary by hardware. Tap a section to open it; inside you'll find buttons, on/off toggles, and sliders for that group, and changes apply immediately.

**AFC (Box Turtle and friends)** exposes the richest set, organized into sections:

| Section | What's inside |
|---------|---------------|
| **Setup** | Run Calibration Wizard, Bowden Length, LED toggles (system + per-toolhead), Quiet Mode |
| **Speed Settings** | Forward and reverse move-speed multipliers |
| **Toolhead** | Calibrate Toolhead, then the sensor-to-nozzle, unload, and post-sensor clear distances (per tool). **Calibrate Toolhead** walks you through measuring the sensor-to-nozzle and unload distances and the cutter retract, with AFC asking each question on screen. It appears only when your AFC version has that calibration |
| **Maintenance** | Test All Lanes, Change Blade, Park, Clean Brush, Reset Motor Timer |
| **Hub & Cutter** | Cutter enable, cut distance, hub bowden length, assisted retract |
| **Tip Forming** | Ramming volume, unloading start speed, cooling-tube length and retraction |
| **Purge & Wipe** | Purge enable/length, brush-wipe enable |

**Happy Hare** groups its controls into Setup, Speed, Toolhead, Accessories and Maintenance. A few worth knowing:

- **Load Extruder** and **Unload Extruder** (Maintenance) run only the extruder part of a load or unload, for filament that is already at the toolhead. Happy Hare heats the nozzle for them. Not available during a print.
- **Motors** (Maintenance) switches the MMU's motors on or off. Switching them on does not home the MMU.
- **Refresh Spoolman** (Accessories) asks Happy Hare to re-read its spool details from Spoolman. Grayed out when Spoolman support is off in Happy Hare.

Other backends show their own (usually smaller) set of sections, or none at all.

### Tips

- When an AMS slot is actively loaded, its material information drives spool preset behavior — you'll see a spool preset button on the Filament and Temperature panels, and purge macros receive the correct temperature automatically. See [External Spool Configuration](#external-spool-configuration) for details.
- The filament path diagram at the bottom of the slot view is interactive: tap a slot's spool or tube to open its slot menu, and on printers with a toolhead per slot, tap another toolhead to [switch to it](#switching-toolheads-from-the-filament-path).
- During a load or unload, watch the step progress in the sidebar to track exactly where the operation is.

---

## Multiple Filament Systems

HelixScreen supports running multiple filament management backends at the same time. For example, a toolchanger printer might use both a Tool Changer backend and Happy Hare for different parts of the filament path.

When multiple backends are detected:

- A **backend selector** appears at the top of the AMS panel
- Tap to switch between systems (e.g. "Happy Hare" vs "Tool Changer")
- Each backend has its own slots and status display
- Slot assignments and controls are independent per backend

**Supported system types:**

| System | Description |
|--------|-------------|
| **CFS** | Creality Filament System (K2 series, plus K1/K1C/K1 Max with the official CFS upgrade) |
| **Happy Hare** | MMU2, ERCF, 3MS, Tradrack, EMU |
| **AFC** | Box Turtle, OpenAMS, ViViD |
| **OpenAMS** | klipper_openams without AFC. Needs a klipper_openams version that publishes its UI API; Load and Unload also need the `OPENAMS_LOAD` / `OPENAMS_UNLOAD` macros from its `oams_macros.cfg` |
| **ACE** | Anycubic ACE Pro (via ValgACE/BunnyACE/DuckACE Klipper drivers) |
| **Tool Changer** | Toolchanger-based filament routing |
| **AD5X IFS** | FlashForge Adventurer 5X Intelligent Filament Switching (requires ZMOD firmware v1.7.0 or newer) |
| **SnapSwap** | Snapmaker U1 4-toolhead changer (parallel, one independent toolhead per slot) |

Each system displays its own logo in the AMS panel header. Happy Hare and AFC show their firmware logos; specific hardware variants (ERCF, Box Turtle, ViViD, the QIDI Box, etc.) show hardware-specific logos when detected.

With a single backend there is no selector — the panel just shows that system.

---

## Creality Filament System (CFS)

The CFS is an enclosed filament storage and delivery system for **Creality** printers. It ships on the **K2 series**, and is also supported on the **K1, K1C, and K1 Max** once you install Creality's official CFS upgrade kit and firmware. Each CFS unit holds 4 spools of filament, and up to 4 units can be connected (16 total slots). HelixScreen auto-detects CFS when connected, along with the set of commands your printer's firmware actually understands — there are three, and it works this out from the firmware itself rather than assuming based on your printer model.

> **A K2 with no CFS attached** is treated as the single-spool printer it is. The filament system screens, slot widgets and Bypass controls stay out of the way, the toolhead filament sensor alone says whether filament is loaded, and the Filament panel's **Load** and **Unload** run the K2's own external-spool load and its cut and retract. The pre-print checks do not ask you to turn off a bypass the printer cannot switch. The toolhead sensor appears in **Settings > Devices > Sensors**, the sensor setup wizard and the Clog Detection widget like any other filament sensor.

> **K1 series note:** CFS on the K1, K1C, and K1 Max requires Creality's official CFS upgrade firmware (v2.3.5.33 or newer). Detection is automatic; no manual configuration is needed.

> **Running community firmware?** Some K2 Plus owners replace Creality's firmware with a community build that has its own rewritten CFS module. HelixScreen recognizes those automatically and supports them fully — slots, spools, colors, humidity and temperature all display, and loading, unloading and filament changes all work from the touchscreen. Nothing to configure.
>
> On recent versions of that CFS module, each unit also shows its own temperature and humidity, a unit that stops answering is marked **Disconnected**, and Filament Mapping works: the tool-to-slot choices you make when starting a print are sent to the CFS just before it begins, for that print only. If a mapping points at a slot the CFS cannot feed from, or at a unit that is not answering, the print does not start and HelixScreen says why.

### Slot Layout

CFS uses a **TNN address format** to identify each slot:

| Unit | Slot A | Slot B | Slot C | Slot D |
|------|--------|--------|--------|--------|
| Unit 1 | T1A | T1B | T1C | T1D |
| Unit 2 | T2A | T2B | T2C | T2D |
| Unit 3 | T3A | T3B | T3C | T3D |
| Unit 4 | T4A | T4B | T4C | T4D |

Each slot displays the detected filament color, material type (PLA, PETG, ABS, etc.), brand, and remaining filament length.

### RFID Detection

CFS units have built-in RFID readers that automatically detect Creality filament spools:

- Place a spool in any slot and its material info appears within seconds
- Supported materials include Hyper PLA, Hyper PETG, Hyper ABS, CR-PLA, CR-Silk, and more
- Remaining filament length is tracked automatically
- If a spool isn't recognized, a generic entry is shown — you can identify it manually

**What HelixScreen gets from a tag.** The CFS reads the tag itself and reports a material code, a color and the remaining length; HelixScreen turns the code into the material, brand and nozzle temperature range from Creality's filament list. You can change any of these in the slot's **Edit Spool** editor, and **Reset to tag** puts the tag's values back. A change you make goes to the CFS's own record of the slot, so Creality's screen shows it too; HelixScreen has no way to write to the tag itself. Only what the CFS reports reaches HelixScreen, so extra data stored on a tag (in its spare bytes, for example) is not shown.

**Tags and Spoolman.** A tag written with a phone app such as CFTag can carry a Spoolman spool number in its serial number. When you put a spool with such a tag in a slot, HelixScreen links the slot to that Spoolman spool, just as if you had picked it with **Select Spool**, and a short notice says so. A few things to know:

- If the number doesn't match a spool in your Spoolman, a notice says so and the slot is left as it was.
- If you pick a different spool for the slot while the same spool stays in it, your pick is kept. The tag is read again only when a spool is put in.
- After HelixScreen restarts, a spool already sitting in a slot links only a slot that isn't linked to anything. So if you unlinked such a slot by hand, a restart links it again; turn **Link Spools from Tags** off to keep it unlinked.
- A stock Creality spool has serial number 000001 on every tag, so it names no Spoolman spool. For the same reason, spool #1 in Spoolman can't be linked from a tag; pick it by hand.
- To turn this off, use **Link Spools from Tags** in the CFS settings (see [AMS Management](#ams-management-settings-overlay)).

**Putting a different spool in a slot.** HelixScreen recognizes a new spool by a change in the tag's material code or color. When either changes, it drops what it remembered about the old spool on that slot (your edits, the Spoolman link, the remaining weight), so they don't follow the wrong spool. Two cases are not noticed, and in both the slot keeps the old spool's Spoolman link and details until you change them yourself:

- **Same material and same color.** Swapping one Creality spool for another of the identical filament and color looks unchanged. Pick the new spool for that slot with **Select Spool**. A tag that carries a Spoolman spool number is the exception: the new tag's number links the slot to the new spool.
- **Spools without a tag.** There is nothing to compare, so a swap is not noticed. Update the slot by hand.

> **Tip:** If a slot shows incorrect info, remove and re-seat the spool to trigger a fresh RFID read.

### CFS Device Actions

Tap the menu icon on the CFS panel to access device actions:

| Action | What It Does |
|--------|--------------|
| **Refresh** | Re-read all RFID tags across all units — useful after swapping spools while the printer was off |
| **Auto-Refill** | Switch to a matching slot on runout. A runout pauses the print either way; with this on, the box then swaps in another slot **only** if one holds the exact same material and color, and resumes. With it off, or with no match, the print stays paused |
| **Nozzle Clean** | Trigger the nozzle cleaning routine using the CFS's built-in silicone strip |

### CFS and the External Spool

Every CFS printer has a spool holder that feeds the toolhead directly, next to the CFS, and HelixScreen's Bypass toggle drives it. How much the firmware does for you depends on which CFS firmware you have:

**Community K2 Plus firmware (Kalico port): the bypass is fully automatic.** Turn the Bypass toggle on and the printer takes over — it heats, moves to the waste bin, and waits for you to insert the filament into the holder. Feed it in, and the printer draws it to the toolhead and flushes. Turning the toggle off reverses the whole thing: the printer retracts and cuts, then asks you to pull the filament out. The external spool also appears on the filament path, and you can tap it to set material, color, and brand or link a Spoolman spool.

**Stock K1/K2 firmware (Creality's own): you feed the holder by hand.** Creality's firmware has no command that loads from the holder — even Creality's own screen leaves the feeding to you. When you turn the Bypass toggle on, HelixScreen stands the CFS down so it cannot push bay filament into the tube your spool is using, then watches the toolhead sensor: the moment it sees your filament, the external spool shows as active. Inserting and removing the filament updates this automatically. Turning the toggle off re-arms the CFS for normal printing. You can still tap the external spool to record material, color, and brand, and prints started from bypass skip the loaded-filament checks.

**Both firmwares:** the external spool is also published to your slicer. When bypass is available, the spool appears as one extra lane past the CFS's own lanes in OrcaSlicer's printer-adapter view (T4 beside T0–T3, for example), carrying the material, color, and spool info you set — so a single-tool file can be mapped straight onto it. Creality's firmware doesn't write this lane itself, so it disappears if the printer restarts until the next time you engage bypass or edit the spool.

Turning bypass on also switches the toolhead runout sensor on at the printer (stock firmware normally leaves it off outside CFS operations), so a bypass print is protected against running out mid-print. Turning bypass off restores the sensor's previous state.

> **Tip:** On stock firmware, turn bypass on *before* you feed the holder. The toolhead sensor can only attribute filament to the external spool while the bypass toggle is on — filament that appears without it is treated as unknown, not bypass.

---

## Snapmaker U1 (SnapSwap)

The Snapmaker U1 is a **4-toolhead changer**, not a shared-path AMS. Each of its four slots has its own independent toolhead, so the topology is **parallel** — every slot keeps its own spool permanently attached to its own tool. There is no hub or selector to share between slots.

That fixed slot-to-toolhead pairing is not the same thing as which toolhead prints which part of your file. When you start a print, the printer is told which head to use for each color in the file, matching on the filament that is actually loaded. So a file sliced with its first color red will print red even if your red spool sits in the third slot — you do not have to rearrange spools to match the slicing order.

The model preview on the print screen follows the same routing: each part of the model is drawn in the color of the filament that will really print it, not in the slicer's slot order. If the preview colors look swapped compared to your slicing software, that is the preview showing you what the printer is actually going to do.

### RFID Detection

Each channel has an RFID reader that reads Snapmaker filament tags automatically:

- Material type and sub-type (e.g. "PLA SnapSpeed"), manufacturer, and brand appear per slot
- Color, recommended nozzle/bed temperatures, and spool weight come from the tag
- When you physically swap a spool, the new tag is detected and any stale metadata you'd previously entered for that slot is cleared automatically

You can still edit spool info manually from the slot context menu for spools without a tag.

**Setting the material for a spool without a tag.** The U1 keeps its own record of what each head holds, and it refuses to resume a paused print while a head the print uses has no material in that record (the printer reports `e1 not edit filament`, counting heads from 0). When you set a head's material in HelixScreen, the printer's record is updated too: on stock firmware with the printer's own filament command, and on the Extended Firmware through its filament endpoint. A vendor other than Snapmaker is recorded as "Generic", and a sub-type other than one of Snapmaker's product lines as "Basic". HelixScreen only fills a blank record or writes a material you changed yourself, and it doesn't rewrite the record when the material hasn't changed, because each write resets that head's flow calibration. For the same reason, a material or color change is refused while a print is running (the editor says the printer is busy with a print); pause the print or wait for it to finish. A spool link or weight change still saves. With the RFID reader turned off, HelixScreen can't tell a Snapmaker spool from any other, so a material you set on such a head replaces what its tag recorded.

### Runout and Resume

The U1 tracks filament with a motion sensor per tool. When you resume a print that paused for something other than a runout, HelixScreen first checks the printer's filament record for every head the print uses. If a head's record is blank but HelixScreen knows its material, HelixScreen fills the record in before resuming. After a runout it doesn't, because you may have loaded a different spool. If the record stays blank, the resume is refused and HelixScreen explains that the head needs its material set.

After a runout, HelixScreen has the printer refeed the head from its feeder (heating, feeding and purging, which takes about a minute and a half) before it sends Resume, so a plain Resume actually continues the print. If the motion sensor reports a runout but filament is still physically present (a stale sensor reading), HelixScreen recovers silently without prompting you.

### Toolhead Offset Calibration

The U1 measures how far each toolhead's nozzle sits from the others, so every head prints in the same place. Open **AMS Management**, then **Calibration**, then **Calibrate Toolhead Offsets**. It takes about 20 minutes and runs the printer's own calibration, the same one as the stock screen's Multi-toolhead Offset Calibration:

1. The printer preheats and homes, then for each toolhead in turn heats the nozzle, purges and wipes it, and parks it at the front.
2. For each parked toolhead, HelixScreen asks you to brush the nozzle clean with the wire brush that came with the printer, then tap **Next**. The nozzle is hot.
3. When all four are clean, take the PEI sheet off the bed and tap **Next**. The printer checks the sheet is off before probing; if it still detects the sheet, it asks again.
4. Each toolhead is probed and the offsets are saved. The final screen lists every toolhead's X, Y and Z offset. Put the PEI sheet back.

Every toolhead is purged and probed, including one with no filament loaded, exactly as on the stock screen; an empty head just turns its extruder over without pushing anything out.

Tapping **Cancel** or leaving the screen stops the calibration and turns the heaters off. If HelixScreen restarts in the middle of a calibration, it ends the printer's calibration mode and turns the heaters off when it reconnects. A step already running finishes first (a heat-up, for example), except probing, which stops straight away. If a step fails, nothing is saved and the heaters are turned off. The button is grayed out while a print is running or paused.

> **Note:** Because each toolhead is independent, the Snapmaker backend has no Home, Recover, Reset, Bypass, or Endless Spool controls — those apply to shared-path AMS hardware only.

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

**Clean nozzle**, under **Maintenance** in Device Operations, wipes the mounted tool's nozzle
on the brush with your printer's `CLEAN` macro. It needs a tool on the head, and it is not
available while a print is running.

### The filament feeder

Because only the hot end travels, the filament is held by a servo gripper on the frame
instead of by the moving toolhead. You'll find **Open feeder** and **Close feeder** under
the AMS panel's **Settings** button, in Device Operations.

Use **Open feeder** to release the filament when you're clearing a jam or loading a fresh
spool by hand, then **Close feeder** to grip it again.

> **Note:** These are refused while a print is running. The gripper is the only thing
> holding your filament — releasing it mid-print drops the strand and ruins the job.

HelixScreen picks the right macro for your setup automatically.

If your setup uses different macro names — you've renamed them, or you're part-way through
migrating to the Python controller — you can choose them yourself. In the same Device
Operations screen, **Open feeder macro** and **Close feeder macro** list the macros found
on your printer. Leave them on **auto** to keep HelixScreen's automatic choice, which also
means you'll pick up the newer commands for free if you migrate later.

### Filament Mapping on MedusaHC

You can point a G-code tool number at a different physical tool, which is useful when a
slicer project expects a different tool order than your machine is loaded with. See
[Filament Mapping](#filament-mapping) above for the dialog itself.

Your printer's own `T0`, `T1`, ... macros decide which hot end each tool number picks, and
nothing can remap them (klipper-toolchanger's mapping does not reach them either). So
HelixScreen rewrites the file instead. That needs the HelixPrint plugin, and any per-tool
temperatures you pass to `PRINT_START` stay as sliced. Read
[Printers that map filament by rewriting the file](#printers-that-map-filament-by-rewriting-the-file)
before your first print that maps filament; it has start G-code that heats the right tools.

### Tool offsets

The tool offset screens edit X, Y and Z for each tool in the controller's own store, and
saving keeps them across restarts. A change to the tool on the head applies at once; the
others apply the next time they are picked up.

### What HelixScreen remembers per tool

Your printer reports nothing about the filament in each hot end — no material, no color,
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

---

## Spoolman Integration

Spoolman is an optional filament-inventory server. Once it's connected, the AMS panel shows saved spool data on each slot and lets you assign spools straight from your inventory:

- Spool name and material type shown per slot
- Remaining filament weight shown
- Tap a slot's **Select Spool**, or **Edit Spool** and the Spoolman button in its header, to assign a saved spool. See [Editing Filament Properties](#editing-filament-properties) and the [Slot Context Menu](#slot-context-menu)

Connecting a server, the full spool-inventory panel, the new-spool wizard, and how remaining weight is tracked all live on their own page: **[Filament Tracking & Spoolman](filament-tracking.md)**.

---

## Filament Drying and Humidity

Many filament materials absorb moisture from the air over time. Wet filament prints poorly - you may see popping, stringing, reduced layer adhesion, or a rough surface finish. Drying the filament before or during a print removes that moisture and restores print quality. Hygroscopic materials that benefit most include Nylon, PA-CF, TPU, and PETG; PLA is less sensitive but still benefits after long storage.

HelixScreen groups all of this into **boxes**. A box is one enclosure it can measure, heat, or both. What counts as a box depends on your hardware: a box can be a whole unit, a group of slots inside a unit, or a single lane with its own heater and sensor.

The home screen can show every drying cycle at a glance with the [Filament Dryer widget](home-panel.md#filament-dryer-widget).

> **A box is not a unit.** The slot range printed under the box name is the only thing that tells you which lanes that box covers. Read it before you start a dry.

### Supported Systems

| System | What you get |
|--------|--------------|
| **Anycubic ACE Pro** | Temperature, humidity, and drying (built-in heated chamber) |
| **QIDI Box** | Temperature, humidity, and drying (PTC heater; QIDI PLUS4, Q2, MAX4) |
| **Happy Hare** | Temperature and humidity wherever sensors are configured, drying wherever a filament heater is. A setup with per-lane heaters and sensors reports each lane as its own box |
| **Creality CFS** | Temperature and humidity, one box per unit. The CFS has no heater, so there is nothing to start |

AFC (Box Turtle, OpenAMS), AD5X IFS, Snapmaker U1 and tool changers report no environment data at all, so nothing appears for them.

> **QIDI Box note:** QIDI Box drying control requires recent QIDI firmware that exposes the `box_extras` Klipper plugin. On older firmware the heater still works, but the session timer isn't tracked - the heater runs until you tap Stop.

### Opening a Box

On the **Filament** panel, any box with a reading or a heater shows a small badge beside its slots: a thermometer with the temperature, and a water drop with the humidity where a sensor exists. The drop fills up to the reading: nearly empty in a dry box, nearly full in a humid one. Tap the badge. That is the whole route - there is nothing to open under Settings.

What opens depends on how many boxes that unit has:

| Boxes | What you get |
|-------|--------------|
| One | That box's screen |
| Two to four of the same kind | That box's screen, with a tab per box across the top |
| More than four, or a mix of heated and unheated boxes | A list of boxes; tap a row to open one |

Each tab carries a small icon for the box it names, so you can tell which box is heating, which is waiting and which has finished without selecting it first.

A mix always goes to the list rather than to tabs, because a tab would hide the difference between a box you can heat and one you can only watch behind a selection you have to make first.

When the screen is showing fewer boxes than the printer has, a **View all boxes** line appears above the card. Tap it for the full list across every unit, with a count of units, boxes and dryers under the title. The list itself has no controls: each row gives the box name, its slot range, its current reading and a one-word status. Tap a row to open that box, where the controls are.

### Reading a Box

The title names the box, and the line under it gives its slot range.

- **Temperature** is always shown.
- **Humidity** is shown only where that box has its own humidity sensor. A box without one reads `--`, and the **Material Comfort** strip is hidden rather than guessed at. That is deliberate: with no humidity reading, a comfort verdict would be a made-up all-clear.
- **While a cycle runs**, the temperature reads current then target, a countdown gives the time left, and a progress bar fills.

The same rule reaches the status column in the box list: a box with a heater but no humidity sensor reads `--` there instead of a status word, and a box with no heater at all reads **passive**.

A box with no heater says so on its own screen, and points you at a sealed dry box with desiccant instead.

The reverse case also shows up: a heated box with no sensor of its own still gets a badge, with a dash where the temperature would be, so its controls are still one tap away.

### Starting and Stopping a Dry

The controls are on the right of the box's screen:

1. **Pick a preset** from the dropdown. Each entry names a material with its temperature and time, like "PLA 55°C/4h", and fills in both fields below.
2. **Or set the values yourself.** Tap the temperature field or the minutes field to bring up a keypad.
3. Tap **Start Drying**.

While a cycle runs, that same button reads **Stop Drying**, and it is also the cancel - there is no separate cancel button. Stopping is safe at any point: the heater goes off and the remaining time is discarded. There is no fan control.

Three things about the fields are worth knowing:

- **The duration field is in minutes**, not hours. Only the preset labels read in hours, so a preset called "4h" puts `240` in the field. Cycles shorter than an hour are fine.
- **The temperature range is that box's range**, and it is printed in the field's own label - "Temp °C (35-65)". The keypad won't accept anything outside it. A preset whose usual temperature sits above the ceiling is shown and sent clamped, so a 70 °C preset on a 65 °C box reads 65 °C, and 65 °C is what runs.
- **Your last values come back.** HelixScreen remembers the temperature and duration you last started a dry with, per printer, and fills them in the next time you open a box.

Left alone, HelixScreen picks the preset that suits the filament in that box - the most conservative one, meaning the lowest drying temperature among the materials loaded there, so a mixed box isn't dried at the temperature its most tolerant spool would take. Type a value yourself and that stops for the rest of the visit; pick a preset by hand and it starts again.

### Choosing Lanes to Dry (Happy Hare 4)

On a Happy Hare 4 unit that has its own heater on every lane (an EMU-style build), the box's screen shows a **Dry** row of lane chips where the lane line would be, one chip per heated lane on that unit. Every chip starts selected each time you open the screen, which dries the whole unit exactly as before.

- **Tap a chip to leave that lane out** of the next dry, and tap it again to put it back. The last selected chip can't be cleared: a dry needs at least one lane.
- **Rotate** appears when the unit has an eSpooler. Turn it on and the selected spools are turned while they dry. Happy Hare only rotates a spool whose filament end has been pulled back out of the gate and fixed to the spool, and it warns in the console about any lane that isn't.
- **Stop Drying with some chips cleared** stops only the selected lanes, and the notice names them ("Stopped drying Gate 2, Gate 3"); the others keep drying. With every chip selected it stops the whole cycle.

To make room for the chips, the Material Comfort list wraps into two columns while they show, and on the smallest screens (480x272) it is hidden; the humidity reading above it stays.

Happy Hare runs one drying cycle per unit, so pick your lanes before you tap **Start Drying**. A lane can't be added to a cycle that is already running: stop it and start again with the new selection. On these units the button and the time left follow the unit's cycle, so a lane that isn't drying still shows **Stop Drying** while its neighbors are.

Each lane's part in the cycle shows on its spool on the **Filament** panel, as a small badge on the spool face:

| Badge | Meaning |
|-------|---------|
| Heat waves | Drying now |
| Clock | Waiting for a heater (Happy Hare limits how many lane heaters run at once) |
| Check mark | Finished |
| Cross | Stopped before it finished |

A lane that isn't part of a cycle has no badge. Shared-heater units, Happy Hare 3 and every other filament system don't show the chips or the badges, and dry exactly as they always have.

### Material Comfort

Where a box has a humidity sensor, a **Material Comfort** strip sits below the readouts. It lists the materials loaded in that box, each with a verdict - **OK**, **Marginal** or **Too humid** - and the humidity ceiling that material is judged against, written as "PLA: OK (< 50%)". Materials differ enormously in what they tolerate: nylon and the high-temperature engineering plastics want a far drier box than PLA does, so the same reading can be comfortable for one spool and too wet for the one beside it. If none of the box's slots report a material, the strip falls back to the materials on your [temperature presets](temperature.md#reassigning-a-presets-filament-type).

The one-word status in the box list is a second, more general scale, applied to the box rather than to any material in it. So a box can read **OK** in the list while a demanding material reads **Marginal** in the strip. For what you actually have loaded, the strip is the one to go by.

### Typical Drying Parameters

These are general-purpose starting points. Your filament manufacturer's guidance takes priority.

| Material | Temperature | Time |
|----------|-------------|------|
| PLA | 45 °C | 4–6 h |
| PETG | 55 °C | 4–6 h |
| ABS / ASA | 60 °C | 4 h |
| TPU / TPE | 50 °C | 4–8 h |
| Nylon (PA) | 60 °C | 8–12 h |
| PA-CF / PA-GF | 60 °C | 8–12 h |

Filament that has been stored open for a long time may need the longer end of the range.

Remember that the duration field takes minutes: 4 hours is `240`, 12 hours is `720`.

### Several Boxes at Once

Boxes are measured independently, but they aren't always heated independently. Some hardware runs one heater at a time. Where your filament system reports that, starting a dry on a second box doesn't begin heating it - the box waits its turn, and the screen says so:

- A banner with a clock icon reads "Waiting for *box* to finish. One heater at a time." (or just "Waiting for another box to finish." when the box holding the heater can't be named)
- That box's tab shows a clock instead of the heat symbol
- No progress bar appears until the box is actually heating

Nothing has gone wrong and there's nothing to retry. When the running cycle ends, the waiting box takes the heater.

Where the hardware has no such limit, boxes heat independently and more than one cycle can run at a time.

Whichever shape you have, the readouts, the Material Comfort strip and the drying controls all belong to the box named in the title - not to the unit, and not to the first box on the printer.

---

## See Also

- [Filament Tracking & Spoolman](filament-tracking.md) — How remaining weight is tracked, with and without Spoolman, and how to connect a Spoolman server
- [Temperature Control](temperature.md) — Preheat presets work with spool material info
- [Bluetooth Setup](bluetooth-setup.md) — Required for Bluetooth-connected AMS and label printers
- [Label Printing](label-printing.md) — Print physical spool labels with Spoolman data
- [Settings: Devices](settings/devices.md) — AMS, Spoolman, and filament sensor configuration

---

**Next:** [Filament Tracking & Spoolman](filament-tracking.md) | **Prev:** [Motion & Positioning](motion.md) | [Back to User Guide](../USER_GUIDE.md)
