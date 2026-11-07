# Filament Tracking & Spoolman

HelixScreen keeps track of what filament is loaded and how much of it is left — the material, brand, color, and remaining weight of each spool. This works two ways: a built-in tracker that runs on its own, and a deeper integration with [Spoolman](https://github.com/Donkie/Spoolman) if you run a Spoolman server for your filament inventory.

This page explains what gets tracked in each mode, how remaining weight is kept up to date, and how to connect Spoolman. For the hands-on filament controls (load, unload, purge, AMS slots), see [Filament Management](filament.md).

---

## The Two Modes

| | Without Spoolman | With Spoolman |
|---|---|---|
| **What you set** | Material, color, brand, remaining weight per spool — entered on the touchscreen | Same, but pulled from your Spoolman inventory when you assign a spool |
| **Where it's stored** | On the printer (survives restarts and reboots) | In your Spoolman database, shared across every tool that reads it |
| **How usage is tracked** | HelixScreen estimates it from how much filament each print extrudes | Spoolman tracks consumption itself, through Moonraker; HelixScreen displays the result |
| **Best for** | A single printer, no inventory system | Managing many spools across one or more printers |

You don't have to choose up front — everything works without Spoolman, and connecting Spoolman later just adds the inventory and shared-database features on top.

---

## Tracking Without Spoolman

Out of the box, HelixScreen tracks filament on its own. Nothing to enable.

### Telling HelixScreen what's loaded

**Single spool (no AMS):** Tap the spool icon on the Filament panel to configure the **external spool** — set the material, color, and brand. Once set, the whole UI knows your filament: preset heat buttons show the right temperature, purge heats correctly, and the Home panel's Active Spool widget shows it. See [External Spool Configuration](filament.md#external-spool-configuration).

**AMS / multi-material:** Each slot carries its own material, color, vendor, and remaining weight. On systems with **RFID** readers (Creality CFS, Snapmaker U1), spool info is detected automatically when you seat a spool. On systems without RFID, tap a slot and choose **Edit Spool** to enter it yourself. See [Editing Filament Properties](filament.md#editing-filament-properties).

This information persists on the printer — it survives restarts, and it's written where OrcaSlicer 2.3.2+ can read it, so your slicer's filament panel stays in sync. See [Syncing with OrcaSlicer](filament.md#syncing-with-orcaslicer-232-and-later).

### How remaining weight is estimated

When you set a spool's remaining weight, HelixScreen keeps it current on its own:

- When a print starts, it records the spool's current weight as a baseline.
- While printing, it watches how much filament the extruder pushes and converts that length into grams (using the material's density), lowering the remaining weight as the print runs.
- When the print pauses, finishes, or is canceled, it saves the updated weight.

This is an **estimate**, not a scale reading, so it's only as accurate as the starting weight and material you entered. For it to run, HelixScreen needs to know the material (to look up its density) and a starting weight above zero.

> **Note:** For AMS systems whose firmware already reports filament remaining on its own (some Creality CFS and Snapmaker U1 setups read it from the spool tag), HelixScreen defers to the hardware's number instead of estimating.

---

## Tracking With Spoolman

[Spoolman](https://github.com/Donkie/Spoolman) is a free, self-hosted filament inventory manager. Connect it and HelixScreen becomes a front-end to your spool library: assign real spools to the printer, browse your whole inventory on the touchscreen, and let Spoolman keep the authoritative record of how much filament each spool has left.

HelixScreen reaches Spoolman **through Moonraker** — you point Moonraker at your Spoolman server (HelixScreen sets this up for you), and Moonraker relays the requests. That means Spoolman's own consumption tracking and any other tools you connect all share the same numbers.

### Connecting Spoolman

You need a running Spoolman server on your network. Then, on the printer:

1. Open **Settings → Devices → Spoolman**.
2. On the setup screen, enter your Spoolman server's **IP address / hostname** and **port** (the default is `7912`).
3. Tap **Connect**.

HelixScreen verifies the server is reachable and configures Moonraker automatically — you don't need to edit `moonraker.conf` by hand. Once connected, the same screen shows the server URL with **Change** and **Remove** options.

Full setting-by-setting reference: [Settings → Devices → Spoolman](settings/devices.md#spoolman).

### Weight sync

With Spoolman connected, HelixScreen doesn't decrement weight itself — Spoolman does, against whichever spool is active, as the print runs. HelixScreen periodically **reads** the updated weight back so the display stays current.

- Turn on **Sync with Spoolman** under **Settings → Devices → Spoolman** to enable this polling.
- Choose a **Refresh Interval** — 30 seconds, 1 minute, 2 minutes, or 5 minutes. Shorter is more up to date but chattier on the network.
- Weights also refresh automatically whenever a print **starts, pauses, or completes**, so the numbers are fresh at the moments that matter even between polls.

- On a printer with several tools or lanes, turn on **Switch spools at each tool change** (same page) so each tool's filament is charged to that tool's spool. See [Settings → Devices → Spoolman](settings/devices.md#switch-spools-at-each-tool-change).

> **Note:** Because Spoolman owns consumption for any spool you've assigned from it, HelixScreen's own estimator stands down for those spools — you won't get two systems fighting over the same number. HelixScreen only writes to Spoolman when *you* make an explicit change (assigning a spool, editing weight, or creating a spool), plus switching the active spool when a lane loads or, with **Switch spools at each tool change** on, when the tool changes.

> **Color from the printer's own screen (AD5X):** the printer holds each lane's color
> and material, so a change made in the printer's color menu shows in HelixScreen even on
> a lane with a Spoolman spool linked. The linked spool fills in only what the printer
> does not hold, such as the brand and the remaining weight.

### Browsing and assigning spools

![Spoolman inventory panel](../../images/user/advanced-spoolman.png)

Once connected, a few things light up:

- **Spool inventory panel** — Reach it from the **Filament** nav tab. A scrollable, searchable list of every spool in your Spoolman database, each shown with a color-coded 3D spool, its weight, and remaining percentage. Search by vendor, material, or color; filter by storage location. Tap a spool for a context menu to **set it active**, edit, print a label, duplicate, or delete.
- **Assign to the printer** — Setting a spool "active" tells HelixScreen (and Spoolman) that this is the spool currently feeding the printer. The Home panel's Active Spool widget reflects it.
- **Assign to an AMS slot** - Tap a slot and choose **Select Spool**, or tap the Spoolman button in the spool editor's header, to pick from Spoolman and auto-fill that slot's brand, material, color, and temperatures. A spool already linked to another slot is grayed out in the list. See [Editing Filament Properties](filament.md#editing-filament-properties).
- **Scan to assign** — If you print QR-coded spool labels, scan one to look the spool up in Spoolman and link it, instead of picking from the list. See [Barcode Scanner](barcode-scanner.md) and [Label Printing](label-printing.md).

### Which spool Spoolman charges on a multi-filament system

Moonraker records filament usage against one spool at a time: Spoolman's **active spool**. If no spool is active while a print runs, nothing is deducted from any spool for that print. On a system with lanes (CFS, AFC, Happy Hare, QIDI Box, ACE and the others), HelixScreen sets the active spool for you:

- **When a lane becomes the loaded one** and that lane is linked to a Spoolman spool, HelixScreen makes that spool the active spool. It does this on every load, including reloading the same spool, so a spool that something else cleared in between is set again.
- **Linking a spool to the lane that is already loaded** makes it the active spool straight away. You don't need to unload and reload.
- **The link is the only thing that counts.** HelixScreen goes by which Spoolman spool you linked to the lane. Names play no part: a spool whose filament Spoolman calls "RePETG" is charged the same as one called "PETG", whatever your slicer profile is named.
- **A print started before the lane was linked is not tracked**, and neither is a print from a lane with no spool linked. Link the spool first (see [Editing Filament Properties](filament.md#editing-filament-properties)), then print.
- **Unloading does not clear the active spool.** The last spool set stays active until another one is set, either by the next load of a linked lane or by you.

Some filament systems set the active spool themselves, and HelixScreen leaves it to them: **AFC**, **Happy Hare** when its own Spoolman support is turned on, and **INDX** when its tool-change macros set the spool. On the **AD5X IFS**, HelixScreen never changes the active spool on its own; it changes only when you set it.

> **Creality CFS:** HelixScreen counts a CFS lane as loaded once the toolhead's filament sensor sees filament from it. While a load is still feeding, the Home panel's Active Spool widget shows no spool. The lane's spool is active by the time the filament reaches the toolhead.

To check which spool is active, look at the Active Spool widget on the Home panel, or at the active spool shown in Mainsail or Fluidd.

### Adding a spool

Tap **+ Add** in the Spoolman panel to create a new spool in three steps:

1. **Select Vendor** — Search your existing vendors, or tap **+ New** to add one.
2. **Select Filament** — Pick an existing filament, or tap **+ New** to create one with its material, color, temperature ranges, and weight.
3. **Spool Details** — Set the remaining weight, price, lot number, and any notes.

HelixScreen creates the vendor, filament, and spool records together in Spoolman, so the new spool is ready to assign right away.

### Pressure advance per filament

> Added in 1.1.

When you finish a [pressure advance test tower](calibration.md#pressure-advance-tuning-test-tower), **Save to Spool** stores the value on that spool's **filament** in Spoolman, in an extra field named `pressure_advance` (HelixScreen creates the field the first time). Every spool of that filament shares it.

From then on, when a spool of that filament becomes active (set active from the Spoolman panel, Spoolman's web page, or an AMS slot change), HelixScreen sets that pressure advance on the extruder the spool feeds (on a tool changer, the tool the spool is assigned to). It does this only when nothing is printing: if your slicer sets pressure advance at the start of a print, the slicer's value wins for that print.

**Getting it into OrcaSlicer.** OrcaSlicer does not read Spoolman directly. [spoolman2slicer](https://github.com/bofh69/spoolman2slicer) turns your Spoolman filaments into OrcaSlicer filament profiles, and its templates already read the `pressure_advance` field, so the value you tuned ends up in the profile. HelixScreen also keeps the latest results in Moonraker's database for a future OrcaSlicer plugin; plugins can read it today but cannot yet change a profile.

---

## Seeing How Much Is Left

However it's tracked, remaining filament shows up in the same places:

| Where | What it shows |
|---|---|
| **Active Spool widget** (Home panel) | Current spool's material, color, and remaining vs. total weight |
| **AMS sidebar** | Remaining weight of the loaded slot |
| **Edit Spool** editor | The slot's remaining weight, plus the spool's full weight when it is known, for example 750 g of 1000 g |
| **Spoolman inventory panel** | Weight and remaining percentage for every spool, with a warning icon on any spool that's running low (under 100 g) |

**Runout** — actually running out of filament mid-print — is handled by your filament sensors and multi-filament hardware, not by the weight number. The printer's firmware pauses the print; HelixScreen then guides you through reloading. On **AFC (Box Turtle)** and **Happy Hare**, a backup slot you configured can take over automatically (Endless Spool). On the **Creality CFS**, **Auto-Refill** is run by the box itself and always pauses first: it switches only when Auto-Refill is on and another slot holds the exact same material and color, and otherwise leaves the print paused for you. See [Reading Error States](filament.md#reading-error-states) and the Endless Spool notes in [Filament Management](filament.md#slot-context-menu).

> **Tip:** The remaining weight is only as good as the starting number. Whenever you load a fresh spool, set its weight, either by entering the grams on it in the Edit Spool editor (on a spool weighed for the first time, that number also becomes its full weight) or by assigning a fresh Spoolman spool, so the estimate has an accurate starting point.

---

## Which Should I Use?

- **Just want the touchscreen to know what's loaded and roughly how much is left?** The built-in tracking does that with nothing to install.
- **Juggling a lot of spools, or more than one printer?** Run Spoolman. You get a real inventory, shared across everything that talks to Moonraker, with Spoolman keeping the authoritative consumption record.

You can start without Spoolman and add it whenever you like — connecting it doesn't erase anything, it just layers the inventory features on top.

---

## See Also

- [Filament Management](filament.md) — Load/unload, AMS slots, filament drying and humidity, and the spool editors referenced above
- [Settings → Devices → Spoolman](settings/devices.md#spoolman) — Every Spoolman setting in one place
- [Barcode Scanner](barcode-scanner.md) — Scan Spoolman QR codes to identify spools
- [Label Printing](label-printing.md) — Print spool labels with a QR code linking back to Spoolman

---

**Next:** [Bluetooth Setup](bluetooth-setup.md) | **Prev:** [Filament Management](filament.md) | [Back to User Guide](../USER_GUIDE.md)
