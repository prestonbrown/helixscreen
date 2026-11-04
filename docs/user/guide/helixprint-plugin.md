# HelixPrint Plugin

The HelixPrint plugin is a small add-on for Moonraker, the service on your printer that
HelixScreen, Mainsail and Fluidd all talk to. HelixScreen works without it. With it, the
printer itself takes on some jobs that a screen can only do while it is running and connected,
and some that a small screen cannot do well at all.

The plugin matters most when your HelixScreen is not attached to the printer: a BigTreeTech
K-Touch, a phone or tablet, or a desktop computer connected over the network. A screen like
that comes and goes, and the plugin keeps working while it is away. On a screen attached to the
printer (a Raspberry Pi touchscreen, or the printer's own display) the HelixScreen app runs
whenever the printer is on, even while the display sleeps, so the difference is smaller.

The plugin never moves, heats or starts your printer on its own. Anything it does to a print
or a spool that HelixScreen did not start is off until you turn it on.

---

## Installing the Plugin

On the screen, open **Advanced** and tap **Install HelixPrint Plugin**. The row only appears
when HelixScreen has checked your printer and found no plugin.

- **On a screen attached to the printer**, HelixScreen installs the plugin directly. It
  installs the plugin version that matches your HelixScreen release.
- **On a screen that is not attached** (K-Touch, tablet, desktop), the screen cannot reach the
  printer's files, so it shows a one-line command instead. Run that command on the printer over
  SSH. It looks like this, with your HelixScreen version at the end:

  ```bash
  curl -fsSL https://releases.helixscreen.org/plugin/remote-install.sh | sh -s -- v1.2.0
  ```

  Copy the command the screen shows rather than this example, so the plugin matches your app.
  After HelixScreen updates, run the command it shows again to bring the plugin up to date.

The installer restarts Moonraker once, which disconnects every screen and web interface for a
few seconds. Once the plugin is installed, the Advanced screen shows **HelixPrint Plugin** and
**Uninstall HelixPrint Plugin** in place of the Install row.

HelixScreen also offers the plugin itself, after setup, when it connects and finds none. The
offer waits for any print to finish and appears at most once each time HelixScreen starts.
Closing it holds the offer off for seven days. A printer that cannot tell the time, with no
clock and no network time, asks again after seven starts instead. Tick **Don't ask again** to
stop it for good; uninstalling the plugin stops it too. The Advanced row still installs the
plugin whenever you want it.

The plugin needs the standard Moonraker. Replacements that do not run Python add-ons, such as
Moonruster on the Centauri Carbon, cannot load it, and HelixScreen running on that printer does
not offer it there.

K-Touch owners: see also [Install the HelixPrint plugin on your printer](install-esp32.md#install-the-helixprint-plugin-on-your-printer).

---

## What It Adds

| Feature | Setting | Without the plugin |
|---------|---------|--------------------|
| [File analysis](#file-analysis) | none | The screen reads files itself |
| [Does the file fit this printer?](#does-the-file-fit-this-printer) | **Settings > Safety & Alerts > Hold slicer prints that do not fit** (off) | The screen still checks its own prints; slicer prints are not held |
| [Start options for prints from other apps](#start-options-for-prints-from-other-apps) | **Settings > Printing > Apply start options to prints from other apps** (off) | Only while HelixScreen is connected |
| [Faster starts for edited prints](#faster-starts-for-edited-prints) | none | The screen copies the file there and back |
| [Switch spools at each tool change](#switch-spools-at-each-tool-change) | **Settings > Devices > Spoolman > Switch spools at each tool change** (off) | Only while HelixScreen is connected, fewer printers, no per-print record |
| [Printer Events](#printer-events) | none | Each screen keeps its own record |
| [Print photos](#print-photos) | **Settings > Devices > Print photos** (on) | Only while HelixScreen is connected |
| [Storage](#storage) | **Settings > System > Storage > Automatic cleanup** (off) | Slower report, only timelapse videos can be cleaned, no automatic cleanup |
| [Saving settings to your printer's config](#saving-settings-to-your-printers-config) | none | The screen edits the file itself |
| [Faster filament updates on AD5X and ACE](#faster-filament-updates-on-ad5x-and-ace) | none | HelixScreen keeps checking as before |
| [Power-loss recovery](#power-loss-recovery) | **Settings > Printing > Power-loss recovery** | Not available |

### File analysis

Your printer reads every G-code file once, in the background, right after you upload it.
Nothing about the file or the print changes; HelixScreen just gets answers it would otherwise
have to work out itself.

- **A thumbnail for every file.** Files your slicer saved without a picture get one: a top-down
  view of the print, in its filament colors. It shows in HelixScreen's file list, and in
  Mainsail and Fluidd too.
- **Faster previews of large files.** The layer preview of a big file appears as soon as the
  file has downloaded, instead of after HelixScreen has read through all of it.
- **Filament per tool, sooner.** The amount and color of filament each tool uses are ready for
  the checks before a print starts.
- **A complete fit check.** Every move in the file is known, so the check below covers purge
  lines and wipe towers too.

**Setting:** none. It runs whenever the plugin is installed, and it only reads files.

**Without the plugin**, HelixScreen reads the files itself: thumbnails only when your slicer
saved one, and previews of large files take longer to appear.

**Attached or remote:** an attached screen gets all of the above. On the K-Touch the plugin is
the only source: it cannot read whole files itself, so thumbnails for files without one and the
filament amount per tool come only from the plugin.

A file you uploaded a moment ago may not have been read yet; HelixScreen then works it out
itself. Reading never slows a print: on a printer with a single-core processor it waits until no
print is running.

### Does the file fit this printer?

Before a print starts, HelixScreen compares the file with your printer's own limits: how far
each axis can travel (or a delta's radius), the highest temperature the nozzle and bed may be
set to, which tools exist, and the nozzle size and type and printer model your slicer profile
names. Where the printer reports the nozzle each toolhead has fitted (the Snapmaker U1 does),
the file is compared with that nozzle.

- A file that would move past the travel, heat past a limit, or use a tool the printer does
  not have is stopped with **File Does Not Fit This Printer**, naming each problem. Re-slice it with a profile made for this printer.
- A file sliced for a different nozzle size or nozzle type (High Flow or standard) gets
  the softer **Sliced for a Different Setup** warning, with **Start Anyway**. So does a file using a tool that exists only as a tool-change
  macro, since HelixScreen cannot be sure the printer lacks it.
- A file sliced for a different printer model only shows a note in the file's details. It
  never stops a print.

Everything found shows in a card at the top of the file's details, before you tap **Start
Print**. [Does the file fit this printer?](printing.md#does-the-file-fit-this-printer) in the
printing guide has the full table.

- **With the plugin**, the printer has already read the whole file, so every move is checked,
  purge line and wipe tower included, on an attached screen and a remote one alike.
- **Without it**, the screen reads the start of the file for temperatures, nozzle size and
  printer model, and takes the travel from the 3D or 2D preview once that has loaded. The
  preview covers the part itself, not the purge line or wipe tower. A file whose preview has
  not loaded yet, a screen that shows thumbnails only, and the K-Touch check everything but
  travel.

**Hold slicer prints that do not fit** (**Settings > Safety & Alerts**, off by default): when
your slicer uploads a file and asks the printer to start it in the same request (the
**Upload and Print** or **Print** button of a slicer set up with an OctoPrint-style host
upload), the printer starts it without HelixScreen ever seeing it. With this switch on, the
plugin checks the file first. If it would be stopped, the file is uploaded but not started;
start it from the screen once you have re-sliced it, or ignore it.

- Every file type Moonraker prints is held, and so is a `.gcode.3mf` the plugin converts. A
  print started separately after the upload (OrcaSlicer's own printer connection when it sends
  its own start command, Mainsail, Fluidd, the job queue) is not held: the plugin never sees a
  request to print it.
- A different nozzle size, nozzle type or printer model never holds a print.
- The slicer waits at most 45 seconds. If the plugin has not read the file by then, or cannot
  read the printer's configuration, a G-code file starts as it would without the switch, and
  the printer's Moonraker log says why. A 3MF whose conversion and check take longer is
  converted but not started, and the slicer is told so.
- While the printer is already printing, nothing waits: a file the plugin has already read is
  checked, and one it has not is queued unchecked, as it would be without the switch.
- A file that is printing, or already waits in the job queue, is never started a second time,
  even if the slicer sends it twice.
- The plugin cannot see the extra tools a filament system adds, so this switch never holds a
  print for its tool count. The screen still checks that when you start the file there.
- Without the plugin the switch is greyed out with **Requires the HelixPrint plugin** and
  **Install**, on any screen: only the plugin sees a slicer's print.

See also [Settings > Safety & Alerts](settings/safety.md#hold-slicer-prints-that-do-not-fit).

### Start options for prints from other apps

HelixScreen sends some of your print options at the start of every print it starts itself, for
example which toolheads unload their filament when the print ends (Snapmaker U1). A print
started from Mainsail, Fluidd, your slicer, or the printer's job queue would normally miss them.

**Setting:** **Settings > Printing > Apply start options to prints from other apps**. It is off
until you turn it on.

- **With the plugin**, the printer applies the options itself at every print start, whether or
  not any HelixScreen is connected.
- **Without the plugin**, or with a plugin too old for this feature or unable to run it on your
  printer, the switch still works, and HelixScreen applies the options itself whenever its app
  is running and connected to the printer. A screen attached to the printer covers almost every
  print: only one that starts while the printer is booting or HelixScreen is restarting is
  missed, so it shows no note. A remote screen covers prints only while it is connected, so there
  the switch says **Only while this app is connected** and offers **Install**, or asks you to
  update the plugin when an older one is installed.

With several HelixScreens on one printer, only one applies the options: the one attached to the
printer, or a remote one when no attached screen is connected. Prints HelixScreen starts itself,
and a print resumed after a power cut, are not affected: they already have their options.

### Faster starts for edited prints

Some print starts change the file before it prints: turning off a pre-print step the file runs
itself (such as bed mesh), adding a skip option to your start macro, or, on a few tool changers,
mapping filament to different tools. HelixScreen never edits your original file; it prints a
changed copy that keeps every other line of the file.

**With the plugin**, the copy is made on the printer itself. The screen sends only the list of
changes, so a large file starts without first being copied to the screen and back, and the
screen needs no free space for it.

**Without the plugin**, or with an older one, the screen downloads the file, changes it and
uploads the copy. The result is the same; it just takes longer for big files.

**Setting:** none. This works the same on an attached screen and a remote one; a remote screen
on a slow network gains the most.

If the file changed on the printer since HelixScreen read it (for example, you re-uploaded it
under the same name), the print is refused with an error instead of changing the wrong lines.
Select the file again and start it. Either way, a copy that would leave the printer with less
than 100 MB free is not made, and a message asks you to free some space (see
[Storage](#storage)).

### Switch spools at each tool change

Spoolman takes filament off one spool at a time: whichever spool is active. On a printer with
several tools or lanes, the active spool has to change at every tool change, or one spool is
charged for filament that came from another. Some printers already do this (AFC, Happy Hare
with its Spoolman support on, a Snapmaker U1 running the extended firmware's SpoolLink, and
printers whose tool-change macros set the spool); there this setting changes nothing.

**Setting:** **Settings > Devices > Spoolman**, in the Weight Sync group: **Switch spools at
each tool change**. It is off by default and only shows when Spoolman is connected.

- **With the plugin**, the printer follows the tool or lane that is printing and makes its
  linked spool the active one, for every print, whichever app started it, even when no screen
  is connected. It also records which spools each print used and how much: open the print from
  **Advanced > Print History > View Full History** and its details list them under **Spools** (for example
  "T0 · Polymaker PLA · 12 g"). A print resumed after a power cut keeps counting on the same
  record.
- **Without the plugin**, HelixScreen switches the spool itself when the tool changes during a
  print, but only while it is running and connected, and only for tools that print from their
  own extruder with a spool assigned to the tool. Filament systems that load lanes into one
  extruder keep working as before: the active spool follows the lane that is loaded while
  HelixScreen is connected. No per-print spool record is kept.

**Attached or remote:** on the screen attached to the printer, the switch works as described.
On a remote screen (a tablet or the K-Touch) without the plugin, the row says **Only while this
app is connected. Install the HelixPrint plugin to cover every print.** When a screen is
attached to the printer, it is the one that switches spools, and a remote screen stands by, so
the two never fight.

**Linking a spool by hand**, with or without the plugin: linking a spool to the lane that is
loaded right now makes it the active spool, unless the printer does that itself (AFC, or a U1
running SpoolLink). Linking the spool you print from outside the filament system (the external
spool) does the same, but only while that spool is what feeds the toolhead, not while a lane is
loaded. Unlinking a spool takes it back off as the active spool, if it still is.

Not yet supported: Creality CFS and Anycubic ACE lane changes. The plugin does not yet know
which lane is feeding on those.

See also [Settings > Devices > Spoolman](settings/devices.md#switch-spools-at-each-tool-change).

### Printer Events

Printer Events is a log of what happened to your printer, as opposed to what HelixScreen told
you: Klipper shutting down and the reason it gave, a lost connection to a controller board
(MCU), Klipper disconnecting and coming back, firmware restarts, a print pausing and what paused
it, filament runouts during a print, error messages from the printer, and how each print ended.

**Where:** tap the **Notifications** widget on the Home screen, then **Printer Events** at the
top. There is nothing to turn on. See [Printer Events](advanced.md#printer-events) for the view
itself.

- **With the plugin**, the printer keeps the record. It includes what happened while no
  HelixScreen was connected, and every screen connected to the printer shows the same list. The
  view says **Recorded by the HelixPrint plugin**.
- **Without the plugin**, each screen keeps its own record of what it saw while connected, so
  two screens can show different lists. The view says **Recorded by this screen while
  connected**. When a screen reconnects, it fills the gap from what the printer still remembers
  (recent errors, how recent prints ended, and recent Klipper shutdowns), marked
  **(recovered)**. Firmware restarts, a pause sent from another device, and filament runouts are
  only recorded with the plugin.

A screen attached to the printer is connected almost all the time, so its own record misses
little. A remote screen misses whatever happened while it was away, apart from what it recovers.

The K-Touch keeps no record of its own. Without the plugin its Printer Events view says
**Printer events need the HelixPrint plugin on this screen**.

### Print photos

HelixScreen can keep two camera pictures of every print: one once the second layer is done, so
you can see whether the first layers went down well, and one when the print ends, whether it
finished, was canceled or failed.

**Where to see them:** **Advanced > Print History > View Full History**, then tap a print. The picture area opens on
the end photo, with **Preview**, **Layer 2** and **End** buttons at the top to switch between
the slicer's preview and the photos. A print resumed after a power cut, or one whose slicer
does not report layers, has only the end photo.

**Setting:** **Settings > Devices > Print photos**. It is on by default and only appears when a
webcam is set up in Moonraker.

- **With the plugin**, the printer takes the photos itself: for every print, whichever app or
  slicer started it, and whether or not a screen is connected. The photos are removed when you
  delete a print from the history, from any app (after a delete in another app, the next time
  Moonraker restarts or a print ends a day or more after the last check), and the printer keeps photos of at most 500 prints (and no more than about 128 MB of
  them). If the plugin cannot reach the camera (for example, it answers only through a web
  interface on another port), it says so under **Advanced > HelixPrint Plugin**, and
  HelixScreen takes the photos itself, as it does without the plugin, until the camera answers
  again.
- **Without the plugin**, the screen takes them while it is connected to the printer. A screen
  attached to the printer gets every print except one that runs while the screen is
  restarting. A remote screen (a tablet, a desktop, a second screen) gets only prints that run
  while it is connected, and only when no screen attached to the printer is there to take
  them; its row then says **Only while this app is connected** and offers to install the
  plugin. Deleting a print in HelixScreen's history deletes its photos; deleting it in another
  app leaves them until the plugin is installed. Photos of at most 500 prints are kept.

Photos are shown turned and flipped the way your camera view is set up; the files on the
printer are kept exactly as the camera took them.

The K-Touch does not show the photos and cannot take them itself. With the plugin installed,
the printer still takes them, and other screens show them.

See also [Settings > Devices > Print photos](settings/devices.md#print-photos) and
[Print History](print-history.md#print-details--reprinting).

### Storage

**Settings > System > Storage** shows what fills your printer's storage, one line per kind of
file, with how much free space is left, and a **Clean up** button on the kinds that are safe to
delete: print copies, file analysis indexes, print photos, timelapse videos and old logs. Your
own files are never deleted from here.

- **With the plugin**, the printer measures its own storage, so every line is shown, 3MF
  conversions and power-loss recovery included. **Automatic cleanup** is available: once a
  day, while nothing prints, it deletes print copies older than a week and old logs older than
  a month. Photos, timelapse videos and your files are never deleted automatically. It is off
  until you turn it on, and if a print is running at that moment it tries again an hour later.
  Nothing a print or a waiting power-loss recovery needs is ever deleted. The plugin also
  refuses to start a changed print that would leave less than 100 MB free, whichever screen or
  app asked.
- **Without the plugin**, HelixScreen measures by listing the printer's folders, which takes
  longer on a printer with many files. 3MF conversions count under **Your files**, power-loss
  recovery is not shown, and Automatic cleanup is greyed out with **Requires the HelixPrint
  plugin** and **Install**. Only **Timelapse videos** can be cleaned up, and only when the
  timelapse folder is a folder of its own: HelixScreen cannot tell which print copy or index a
  power-loss recovery still needs, and the printer does not let it delete logs.

If a folder is set up so that cleaning it could delete your files (for example, a timelapse
folder that is also your G-code folder), the plugin refuses to clean anything and the overview
shows why; the page then only reports, and HelixScreen does not clean that folder itself
either. After a cleanup the page says how much it freed, or that some files, or none, could be
deleted.

**Attached or remote:** the page works the same on the printer's own screen and on a tablet or
computer. Either way, HelixScreen checks for space before it uploads a print copy or a file from
a USB stick, and stops with a message if the upload would leave less than 100 MB free.

See also [Settings > System > Storage](settings/system.md#storage), which lists every line and
what cleaning it does.

### Saving settings to your printer's config

**Save** on the Input Shaper screen, the PID-to-MPC switch on the PID screen, and the probe
settings rows on the Probe screen write straight to your Klipper configuration and restart the
printer's firmware so the change takes effect.

**With the plugin**, the printer makes the change itself. It finds the exact file and line where
each setting is defined, even when your config is split across several included files, and
writes the file in one step, so a power cut mid-save cannot leave it half-written. If someone
changed the same setting from another screen or from Mainsail in the meantime, the save is
refused instead of overwriting their change. Before every save it copies each file it changes
into the hidden `.helix_backups` folder in your config folder, keeping the five most recent
copies of each file with the date and time in their names.

**Without the plugin**, HelixScreen downloads the file, changes it, keeps one copy in
`.helix_backups` while the firmware restarts, and uploads the result. That copy is deleted once
Klipper comes back.

**Either way**, if Klipper does not come back after the restart, HelixScreen puts the previous
file back, restarts again and tells you the change was undone. To restore an older copy by hand,
turn on hidden files in Mainsail's or Fluidd's file manager and copy the file out of
`.helix_backups`. Troubleshooting covers this under [When the Printer Errors or Disconnects](../TROUBLESHOOTING.md#when-the-printer-errors-or-disconnects-recovery-dialog).

**Setting:** none. The plugin is used whenever it is installed, on attached and remote screens
alike.

### Faster filament updates on AD5X and ACE

On an AD5X running ZMOD, and on printers with an Anycubic ACE Pro read through its Moonraker
bridge, HelixScreen keeps the lane colors, materials and slot states current by asking the
printer again and again: every few seconds on an AD5X (every 30 seconds while printing), twice
a second for an ACE. With the plugin installed, the plugin watches that information on the
printer and tells the screen the moment it changes, so HelixScreen stops asking.

**Setting:** none. It works on its own whenever the plugin is installed. On a printer with one
of these systems, **Advanced > HelixPrint Plugin** lists **IFS updates without polling** or
**ACE updates without polling**; a printer with neither does not list them.

HelixScreen still checks once every 30 seconds while the plugin pushes, so a missed update is
never stale for long. If the plugin cannot read that information on your printer, it says so in
the overview, and HelixScreen goes back to checking as before.

**Without the plugin**, nothing changes: HelixScreen keeps checking as it always has.

**Attached or remote:** on a screen attached to the printer the difference is a little less
work for the printer's own computer (on an AD5X, that matters most while printing). On a remote
screen, such as a tablet or the K-Touch, it also means less traffic over Wi-Fi, and changes show
up without waiting for the next check.

### Power-loss recovery

On printers HelixScreen supports for it (today, the QIDI Q2), the plugin saves the print's
position about once a second, whichever app started the print, so a print can resume after a
power cut. After the power comes back, HelixScreen offers the resume and walks you through it
one confirmed step at a time: checking the part is still on the bed,
heating the bed and nozzle together, checking the nozzle height, then continuing.

Saving is on by default when the printer keeps its data on eMMC or similar storage, and off on
a printer that runs from an SD card, since it adds wear to the card. The switch is
**Settings > Printing > Power-loss recovery**; without the plugin it is greyed out, and on
other printers it does not appear. See
[Settings > Printing](settings/printing.md#power-loss-recovery-and-start-options-for-other-apps)
for the switch and [Recovering After a Power Loss](printing.md#resuming-with-the-helixprint-plugin)
for the step-by-step resume.

A resume normally loads the printer's default bed mesh profile. With a recent plugin,
**Settings > Printing > Resume reloads the saved bed mesh** adds one include line to
printer.cfg so a resume reloads the mesh the print actually started with. HelixScreen asks
before changing printer.cfg, keeps a backup beside it, and offers the firmware restart that
applies it; tap the row again to take the line out. Uninstalling the plugin removes it too.

---

## The Plugin Overview

**Advanced > HelixPrint Plugin** shows the plugin's version and every feature it runs on your
printer, each marked:

- **Active**: running.
- **Off**: available, but switched off in its setting.
- **Unavailable**: cannot run on this printer, with the reason the plugin gave.

Tap a feature that has a setting to go to the page where its switch is: Settings > Printing,
Safety & Alerts, Devices, Spoolman or System > Storage. The overview itself has no switches.
Features with no setting show their status only. The filament updates for AD5X and ACE are
listed only on a printer where they apply.

## What the Notes on a Setting Mean

A setting that depends on the plugin can show one of these under its switch:

| Note | What it means |
|------|---------------|
| **Requires the HelixPrint plugin** | The feature only works with the plugin, and your printer does not have it. The switch is greyed out; tap **Install** to add the plugin. Shown for power-loss recovery, holding slicer prints that do not fit, and automatic storage cleanup |
| **Requires a newer HelixPrint plugin** | The plugin is installed but too old for this feature. Update the plugin: on an attached screen HelixScreen keeps it matched to the app; on a remote screen, run the install command again |
| **Only while this app is connected. Install the HelixPrint plugin to cover every print.** | On a remote screen without the plugin. The switch works, but HelixScreen can only act on prints while this app is running and connected. Tap **Install** to cover every print. Shown for start options for prints from other apps, spool switching and print photos |
| **Only while this app is connected. Update the HelixPrint plugin to cover every print.** | On a remote screen whose plugin is too old for this feature, or cannot run it on your printer. The switch works the same way; run the install command again to update the plugin |

A screen attached to the printer shows none of the last two notes: it does those jobs itself,
so the switch simply works. Only a feature that works solely through the plugin, such as
power-loss recovery, is hidden when your printer cannot use it, and the overview says why.
