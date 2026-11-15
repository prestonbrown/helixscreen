# Home Panel: Printer Manager

**Tap the Printer Image widget** to open the Printer Manager. It is the one place to see and change your printer's identity: its name, model and picture, the software versions it runs and the hardware HelixScreen found.

You can also reach it without the widget, which helps if you removed Printer Image from your home screen or your layout has no room for it:

1. Tap the **gear icon** in the navigation bar to open Settings
2. In the **PRINTER** group, tap **Printer** (the row reads *Name, image and model*)
3. The Printer Manager opens. Tap the back arrow to return to Settings

The steps below start from the printer image, but every one of them works the same from **Settings > Printer**.

### Changing the Printer Name

1. Tap the **printer image** on the Home Panel to open the Printer Manager
2. Tap the **printer name** (shown with a pencil icon). It becomes an editable text field
3. Type your new name (e.g., "Workshop Voron", "Printer #2")
4. Press **Enter** to save, or **Escape** to cancel

The name defaults to "My Printer" if left empty. It's saved to your config file and persists across restarts.

> **Auto-import:** If you've already named your printer in Mainsail or Fluidd, HelixScreen automatically picks up that name on first connection, no need to re-enter it.

> **Sync:** When you rename your printer in HelixScreen, the new name is automatically pushed to Mainsail and Fluidd so all your interfaces stay in sync.

> **Tip:** You can also set the name directly in the config file under the `printer_name` key, see [Configuration Reference](../CONFIGURATION.md#name).

### Correcting the Printer Model

Directly below the printer name, the **model row** shows the model HelixScreen has recorded for this printer, or nothing at all, when detection could not identify it with confidence. Tap the row to change it:

1. Tap the **printer image** on the Home Panel to open the Printer Manager
2. Tap the **model row** (marked with a pencil icon)
3. Pick the model that matches your printer from the list
4. The overlay closes and the choice is saved immediately

The list is filtered to your printer's motion type, the same way the setup wizard filters it. Picking a model loads that printer's known behavior, the print macros HelixScreen calls, which fans it treats as part cooling versus hotend, and how its calibration flows behave.

**When you need this:** automatic detection declines to guess when the evidence is inconclusive, and on printers that look identical over the network it can settle on the wrong sibling. Correcting the model here fixes both cases, no need to delete the printer and run setup again. Detection will not overwrite a model you chose yourself.

### Changing the Printer Image

1. Tap the **printer image** on the Home Panel to open the Printer Manager
2. Tap the **printer image** again (marked with a pencil badge) to open the Image Picker
3. The **Printer Image** picker shows a scrollable list on the left and a live preview on the right
4. Choose from one of three sources:
   - **Auto-Detect** (default; the row reads "Use printer type to select image"), HelixScreen selects an image based on your printer type reported by Klipper. Some printers have a second picture for optional hardware: a Creality K2 with a CFS connected shows the K2 with the CFS on top. The picture switches on its own once the CFS reports in, and back if it is disconnected. An image you pick yourself always stays in place
   - **SHIPPED IMAGES**: the printer pictures HelixScreen ships (over 80), covering Voron, Creality, FlashForge, Anycubic, RatRig, FLSUN and more
   - **CUSTOM IMAGES**: your own images (see below)
5. Tap an image to select it, your choice takes effect immediately

![Printer image picker, scrollable list on left, live preview on right](../../images/user/home-image-picker.png)

### Status Chips on the Printer Image

While the printer is working, small chips on the printer image show what each part is doing:

- **Nozzle, bed and chamber** - a temperature chip appears while the heater has a target, and stays, grayed, until a heater you turned off has cooled below 50°C. The chamber chip appears on printers with a chamber heater or a chamber temperature sensor. With only a sensor, it shows while a chamber fan is holding the chamber at a set temperature, or while the chamber is above 50°C.
- **Part fan** - shows its speed while it is running.
- **Light** - a small light bulb at the top of the picture, shown whenever your printer has a light HelixScreen can switch. It is lit while the light is on and dark while it is off.

Tap a heater chip for its temperature graph or the fan chip for the fan controls. **Tap the light bulb to switch the light on or off.** Press and hold it to open the light's page in the LEDs overlay instead.

On the most common printers the shipped pictures know where each part is, so each chip points at its part: beside the picture with a line to it on a wide tile, or on top of it on a small one. On other pictures, including your own photos, the chips sit along the edge of the picture until you tag its parts.

### Tagging the Printer's Parts

To place the chips on a picture that does not know its parts yet, or to correct a shipped picture you disagree with:

1. Open the Image Picker (see [Changing the Printer Image](#changing-the-printer-image)) and select the picture
2. Tap **Tag Parts** under the preview
3. Tap each part as you are asked: the nozzle tip, the part cooling fan, the bed's front-left corner, the bed's front-right corner, an empty spot inside the enclosure, and the light. The fan, enclosure and light can be skipped with **Skip** if your printer does not have them; **Undo** steps back one tap
4. Check where the chips will sit, then tap **Save**

Your tags are kept for that picture, as long as its size does not change: replace a custom image with a file of different dimensions and you will need to tag it again. **Reset Tags**, which appears once a picture has your own tags, puts the picture back to its shipped positions (or to chips along the edge, for a picture that has none).

### Using Custom Printer Images

You can use your own printer photo or rendering:

**Option A: Copy to the custom images folder**

| Platform | Custom images directory |
|----------|----------------------|
| MainsailOS (Pi) | `~/helixscreen/config/custom_images/` |
| AD5M Forge-X | `/opt/config/mod_data/helixscreen/config/custom_images/` |
| AD5M Klipper Mod | `/root/printer_software/helixscreen/config/custom_images/` |
| K1 Simple AF | `/usr/data/helixscreen/config/custom_images/` |

Copy a PNG, JPEG, BMP or GIF file into the directory, then open the Image Picker. HelixScreen converts the file when the picker opens, and your image appears under **CUSTOM IMAGES**. A file that is too large or has the wrong dimensions appears grayed out and cannot be selected.

**Option B: Import from a USB drive**

1. Insert a USB drive containing image files into your printer's host
2. Open the Image Picker
3. An **IMPORT FROM USB** section appears at the bottom showing images found on the drive
4. Tap an image to import it, HelixScreen copies and converts it automatically
5. Once imported, the image appears under Custom Images and the drive can be removed

**Image requirements:**
- Formats: PNG, JPEG, BMP, or GIF
- Maximum file size: 5 MB
- Maximum dimensions: 2048x2048 pixels (resize before importing if larger)
- HelixScreen automatically generates optimized display variants

**Removing a custom image:**

Delete the image files from the `custom_images/` directory via SSH:

```bash
cd ~/helixscreen/config/custom_images/   # adjust path for your platform
rm my-printer.png my-printer-300.bin my-printer-150.bin
```

If the deleted image was active, HelixScreen falls back to auto-detect.

### Software Versions

Under **SOFTWARE** the Printer Manager shows the current versions of Klipper, Moonraker and HelixScreen, with update indicators when newer versions are available.

On a printer running Kalico, the Klipper row is labeled **Kalico**.

### Capabilities

Below the software versions, the Printer Manager lists everything HelixScreen found on your printer, grouped under six headings: MOTION & LEVELING, THERMAL & AIR, FILAMENT & TOOLHEAD, PRINTING, ELECTRONICS & ACCESSORIES and ADD-ONS. Only what was detected appears; a heading with nothing under it is hidden.

Many chips carry a detail after the name, so you can check that HelixScreen identified your hardware correctly:

| Chip | Example | What the detail tells you |
|------|---------|---------------------------|
| Kinematics | CoreXY | How your printer moves, from your Klipper config |
| Build volume | 350×350×340 | The travel HelixScreen read from your stepper limits, in mm |
| Probe | Probe · Beacon | Which probe type was recognized |
| Filament system | Happy Hare · 8 | Each multi-material system and its slot count |
| Tool changer | Tool Changer · 4 Tools | Toolheads on a tool changer, or **2 Extruders** on an IDEX or other multi-extruder printer |
| MCUs | 2 MCUs | How many controller boards Klipper reports (one board shows its chip name) |
| Counts | Fans · 2, Power · 4 | How many of that item were found. Fans counts the fans you can control, other than part cooling |

A chip that says **set manually** (for example **QGL · set manually**) is one you turned on yourself with a [capability override](../CONFIGURATION.md#capability-overrides) even though HelixScreen did not detect it on the printer.

**Tap any chip** to go to the screen that uses it, such as Bed Mesh, Input Shaper, LED Control or the Power panel. Chips with no screen of their own open a short note explaining what the feature is and where HelixScreen uses it.

If something you know is on your printer is missing from this list, HelixScreen did not detect it. See [A feature is missing for hardware my printer has](../TROUBLESHOOTING.md#a-feature-is-missing-for-hardware-my-printer-has).

![Printer Manager, name, image, software versions](../../images/user/home-printer-manager.png)

## Managing Multiple Printers

One HelixScreen can know several printers and connect to one at a time. Each printer keeps its own settings, home layout and name.

The Printer Manager always has a **Manage Printers** button in its top bar (a printer icon on a portrait screen). It opens the same **Manage Printers** screen as **Settings > Connection > Printers**, where you add, switch and remove printers.

### The printer switcher in the navigation bar

Turn on **Show Printer Switcher** on the Manage Printers screen and a printer icon appears in the navigation bar, with a small dot showing the connection state. Tap it to open the **Switch Printer** menu, which lists your printers, and tap one to switch to it. The menu also has a **+ Add Printer** button. The icon is hidden while you edit the home screen.

### Adding a printer

1. Open **Settings > Connection > Printers** and tap **+ Add Printer**, or tap **+ Add Printer** in the Switch Printer menu.
2. The setup wizard starts. Enter the new printer's address, pick its hardware and finish the wizard.
3. When the wizard ends you are connected to the new printer. If you cancel the wizard, you go back to the printer you were using.

### Switching printers

Tap the printer in the Switch Printer menu, or tap it in **Settings > Connection > Printers**. The active printer is marked with a check and a connection dot. HelixScreen shows a **Switching to** card with "Loading..." and then "Connecting..." while it reloads, then lands on the new printer's home screen. If the printer you are leaving is printing, HelixScreen asks first: "[name] is still printing. The print keeps running after you switch." Tap **Switch Printer** to continue. The print is not affected.

### Removing a printer

1. Open **Settings > Connection > Printers**.
2. Tap the **trash icon** on the printer's row. The icons appear only while you have more than one printer.
3. Confirm **Remove** in the dialog that asks "Remove [name]? All settings for this printer will be deleted."

If you remove the printer you are connected to, HelixScreen switches to the first printer left in the list. You cannot remove your only printer.

> Removing a printer only removes it from HelixScreen. It does not change the printer itself, its Klipper configuration or Moonraker.

---

**Back to:** [Home Panel](home-panel.md)
