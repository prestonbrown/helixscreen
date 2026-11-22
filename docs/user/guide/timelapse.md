# Timelapse

HelixScreen records timelapse videos through the [moonraker-timelapse](https://github.com/mainsail-crew/moonraker-timelapse) plugin on your printer. The plugin takes the pictures and makes the video. HelixScreen turns it on and off, picks the recording mode, warns you when a print would come out empty, and plays the finished videos.

This page explains the two recording modes, the one line your slicer needs for Layer mode, parking, and what the "No Timelapse Frames" warning means. To install the plugin, see [Advanced > Timelapse](advanced.md#timelapse).

## Contents

- [The two recording modes](#the-two-recording-modes)
- [Set up your slicer for Layer mode](#set-up-your-slicer-for-layer-mode)
- [Parking the head for each frame](#parking-the-head-for-each-frame)
- [The No Timelapse Frames warning](#the-no-timelapse-frames-warning)
- [Turning timelapse on and off](#turning-timelapse-on-and-off)
- [Gotchas](#gotchas)

---

## The two recording modes

Choose the mode in **Settings > Printing > Timelapse > Recording Mode**. Tap the (i) next to it for a short reminder of what each one does.

| Mode | When it takes a picture | What you need | What you get |
|------|------------------------|---------------|--------------|
| **Layer** | Once at every layer change | Your slicer must add `TIMELAPSE_TAKE_FRAME` to its layer change G-code (see below) | A smooth video where the part grows one layer per frame. With parking on, the head is out of the way in every frame |
| **Hyperlapse** | Every 30 seconds by default, on a timer | Nothing in the slicer | A video that works with any file, but each frame catches the head wherever it happens to be, so the part can jump around between frames |

**Layer mode records nothing on its own.** It takes a picture only when the print file tells it to, by running `TIMELAPSE_TAKE_FRAME`. A file sliced without that line gives a video only a few frames long, even for a print that took hours.

---

## Set up your slicer for Layer mode

Add this single line to your slicer's layer change G-code:

```
TIMELAPSE_TAKE_FRAME
```

Put it on its own line. If the box already has other lines, add it at the end. Then re-slice your files: files you sliced before the change still have no frame calls.

| Slicer | Where to add it |
|--------|-----------------|
| **OrcaSlicer** | **Printer settings > Machine G-code > Layer change G-code** |
| **PrusaSlicer** and **SuperSlicer** | **Printer Settings > Custom G-code > Before layer change G-code** |
| **Cura** | **Extensions > Post Processing > Modify G-Code > Add a script > Insert at layer change**, with **G-code to insert** set to `TIMELAPSE_TAKE_FRAME` |
| **ideaMaker** | **Advanced Settings > Gcode > Layer Change Gcode** |

In OrcaSlicer and PrusaSlicer the setting belongs to the printer profile, so every file sliced with that profile gets it. In Cura the script belongs to the current setup; check it is still listed after you switch printers.

If your printer has its own layer change macro that already runs `TIMELAPSE_TAKE_FRAME` inside it, calling that macro from the slicer works too, and HelixScreen recognizes it.

---

## Parking the head for each frame

In Layer mode the plugin can move the head out of the way for each picture, so the part is never hidden behind it. This is called parking, and it is set in the plugin's own settings, not in HelixScreen. Open the timelapse settings in Mainsail or Fluidd to turn **Park head** on and choose where the head goes (a corner, the center, or your own position).

Parking adds a short pause at every layer. On some prints that pause can leave a small mark where the nozzle stopped. If that bothers you, turn parking off or raise the retraction in the plugin's park settings.

Hyperlapse takes its pictures at random points in a layer, so parking there stops the print at random spots. Most people leave parking off for Hyperlapse, and **Use Hyperlapse** in the warning below turns it off for that print.

---

## The No Timelapse Frames warning

When you start a print with timelapse on in Layer mode, HelixScreen reads the beginning of the file. If it finds layer changes but no `TIMELAPSE_TAKE_FRAME` call (and no call to a printer macro that runs it), it stops and shows **No Timelapse Frames** before anything moves:

| Button | What happens |
|--------|--------------|
| **Use Hyperlapse** | The print starts and records in Hyperlapse mode, with head parking off if it was on. When the print ends (finished, cancelled or failed), HelixScreen puts Layer mode and parking back for you |
| **Off for This Print** | The print starts with timelapse recording off. When it ends, HelixScreen turns timelapse back on |
| **Print Anyway** | The print starts as is. Expect a video with almost no frames |
| **Cancel** | Nothing starts. Tapping outside the dialog does the same |

To stop seeing the warning, add the line to your slicer as described above and re-slice the file.

HelixScreen only reads the first part of a large file. If the first layers are very big, or the file starts with large preview images, it may not reach a layer change, and then it says nothing rather than guess. A missing warning does not prove the file is set up.

---

## Turning timelapse on and off

- **Settings > Printing > Timelapse > Enable Timelapse** turns recording on or off for every print.
- The **Timelapse** tile in the print options, on the file screen before you press Print, sets it for the print you are starting. It starts out matching the setting above.
- During a print, **More > Timelapse** on the print screen turns recording on or off from then on.

The plugin has one on/off setting for the whole printer, so each of these changes it for later prints as well.

---

## Gotchas

- **The switch back survives a restart.** If HelixScreen restarts or loses its connection during that print, it puts the settings back once it reconnects and the print is over. A setting you changed yourself in the meantime (anywhere, Mainsail and Fluidd included) is left as you set it. Picking a recording mode in **Settings > Printing > Timelapse** cancels the switch back altogether. The one case it cannot tell apart: if you set the very value the switch set (Hyperlapse, parking off, or timelapse off), HelixScreen still puts the old one back.
- **Other screens see the change.** Mainsail, Fluidd and other screens show the plugin in Hyperlapse mode while that print runs, because there is only one mode setting.
- **The print options tile is different.** Turning off the **Timelapse** tile before printing changes the printer's setting for later prints too; only **Off for This Print** in the warning comes back on by itself.
- **The plugin's Klipper macros must be loaded.** If Klipper has not loaded `timelapse.cfg`, no mode records anything. **Settings > Printing > Timelapse** shows a card with the line to add when they are missing.
- **Printers that record timelapses in their own firmware** (for example the Snapmaker U1) do not use the plugin. Their timelapse option is on the file screen, and none of this page applies to them.
