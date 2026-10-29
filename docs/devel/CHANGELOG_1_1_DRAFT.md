# CHANGELOG draft - 1.1

Working draft of the release notes for 1.1. Lives here rather than in
`CHANGELOG.md` so the release tooling owns that file uncontested; at release,
this becomes the `## [1.1.0]` entry more or less verbatim. The heading has to be
the exact string in `VERSION.txt` at that point, because
`make release-gate` refuses a version with no section and the release workflow
looks the section up by it for the update notes and the Discord post. The Play
Store text for 1.1.0 is separate: `android/whatsnew/<version>.txt`, written in
the release commit.

Each `1.1.0-beta.N` tag from the trunk gets its own short `CHANGELOG.md` entry for
what that build changed. This file is the cumulative story of the release, not a
running log of the betas.

**Scope:** everything on `main` that is not in the 1.0 release. Work that shipped
in 0.99.112 and earlier belongs to the release it shipped in and is deliberately
absent.

**Keeping it current:** the trunk moves. Last reviewed end to end against `main` at
`2ad32dc6e`, with the `1.1.0-beta.1` and `1.1.0-beta.2` entries of `CHANGELOG.md` folded in.
To see what has landed since:

```bash
git log --no-merges --oneline 2ad32dc6e..main --not release/1.0
```

---

## [1.1.0] - UNRELEASED

**Upgrading from 1.0?**

- **Your home screen is converted, not reset.** The home screen moves to a square-cell
  grid, and your layout comes with it. Positions are converted onto the new grid in
  proportion: a widget that filled the left third of the screen still fills the left third,
  and two widgets that were touching stay touching. Sizes can shift slightly, because the
  new grid does not divide the screen the same way and each widget lands on the nearest size
  it is allowed to hold. A widget that genuinely will not fit is re-placed automatically,
  and only that widget. Which widgets you have, their per-widget settings, and your extra
  pages all survive. The conversion happens the first time the home screen is drawn after
  the update, so what you see on that first boot is what gets saved.
- **Going back to 1.0 costs you the home layout.** A 1.0 build saves home positions in its
  own grid's units, so when you return to 1.1 the home screen comes back rearranged, or,
  from some 1.0 builds, reset to its defaults. Arrange it again in edit mode. Everything
  else survives the round trip.
- **Your 1.0 settings are kept** (#1305). The first start of 1.1 saves your old
  `settings.json` beside the new one as `settings.json.pre-migration`, before converting it.
  HelixScreen never reads that copy back; it is there if you want your 1.0 settings.
- **Settings pages moved; your settings did not** (#1023). Nothing is reset. The table under
  Changed shows where each page went.
- **Print Completion Alert now uses its documented default.** A printer that never changed
  the setting showed only the short notification; it now shows the full-screen summary. A
  choice you made yourself is kept.
- **Some installs move on disk, automatically.** On the AD5M, HelixScreen's own files move
  to `/data/.helixscreen` so they stop showing up in the print file list. On the K2, the
  install moves off the 240MB system overlay onto the user partition. Both are migrated in
  place by the update.
- **`helixscreen.env` values are taken literally** (#1682). The file is no longer run
  through the shell, so `$VAR`, `${VAR}`, `$(command)` and backticks are not expanded. A
  line using them is skipped with a warning in the log, so write the final value itself.
  Only the settings documented for this file are accepted; anything else is skipped and
  logged. A few values are checked before they are used: the log file has to be a `.log`
  under `/tmp`, `/var/log` or the install folder, and `HELIX_NICE` has to be 0 to 19. An env
  file that the web interface can edit, like the one the Snapmaker U1 keeps in
  `printer_data`, is honoured.
- **A refused `helixscreen.env` says so on screen** (#1712). When the file cannot be trusted
  (wrong owner, or still writable by others after the automatic repair) the launcher used
  to fall back to defaults with nothing on the display. Now a startup warning says what is
  wrong, names the one command that fixes it, and stays on screen until closed; lines the
  launcher skips (a setting this file may not change, shell syntax in the value, a bad
  quote) are listed with the key and the reason each was ignored.
- **A custom update server moves out of `settings.json`** (#1718). `settings.json` can be
  edited from the web interface, so it no longer chooses where updates are downloaded from.
  To point a printer at your own server, put `r2_url` or `dev_url` in
  `/var/lib/helixscreen/update_urls.json`, a file only root or HelixScreen's own user may
  own. The old keys in `settings.json` are ignored, and the log says where to move them. A
  custom `log_path` has to sit under `/tmp`, `/var/log` or HelixScreen's own folders.

### Added

**Calibration**

- **Pressure advance tuning on a live test print** (#982) - Advanced > Calibration > Pressure
  Advance Tuning prints a small test tower and raises pressure advance with height; tap - or
  + at any moment to take over, then pick the height band that looked best from what the
  printer reported. The result is applied, can be saved to the spool's Spoolman filament
  (applied again whenever that filament becomes active, never mid-print), and is kept in
  Moonraker's database for slicer plugins. The print Tune overlay also gains a live
  pressure advance row.
- **Bed screw adjustment shows the tilt** - the bed diagram is shaded by height in the
  bed mesh colors, with a line giving how far apart the highest and lowest corners are.

**Home screen**

- **Notification history without the bell** - removing the Notifications widget from the
  home screen no longer cuts you off from notification history and Printer Events:
  **Advanced > Notification History** appears whenever no bell is on any home page.
- **The home screen is one square-cell grid on every panel** (#1126, #1559) - both axes now
  divide the panel by the same per-breakpoint track size, so a cell is square everywhere and
  a rotated panel transposes its grid exactly. The per-layout-type branches, the fixed count
  table and the separate width and height targets are gone. Tracks are half cells, which is
  three times the placement resolution the old grid had, so the compromises it forced are
  gone too: tips is no longer suppressed in portrait, and bed temperature ships enabled
  everywhere instead of losing a coin toss against AMS.
- **Widgets can be sized and dropped at half-cell resolution** - drag and resize round to a
  per-widget step: one track for a widget whose content is continuous along that axis
  (charts, aspect-fit frames, wrapping text, scrolling strips, stacked readouts), a whole
  cell for the centred icon-and-label tiles, where an in-between size buys only whitespace
  and costs a fussier drag. Every shipped layout was re-authored against the new grid:
  default, portrait, landscape, the three printer preset seeds, plus a new ultrawide variant
  for the 1480x320 and 1920x440 panels that used to draw into the left fifth of the screen.
- **Edit mode shows the lattice a widget can snap to** - whole-cell boundaries are always
  drawn; the half-cell boundaries between them appear smaller and fainter only while a widget
  that supports them is selected, and only on the axes it supports. A visible dot is a legal
  drop target.
- **Edit mode swipes between pages, and widgets move across them** (#1638) - swiping flips
  pages in edit mode as it does outside it; it pauses while your finger is on the selected
  widget, while you drag or resize one, and while the widget catalog is open. Drag a widget over
  the page border, or hold it at the edge of the widget area, and the next page slides in with
  the widget still under your finger. Pressing a widget that is not
  selected only selects it, so a swipe that starts on a widget still flips the page: press the
  selected widget again, or hold a widget, to pick it up. The long press that enters edit mode
  only selects.
- **The widget catalog opens by category, with search** (#1016) - thirty-seven widgets in one
  flat scroll had stopped being a list you read and become one you hunt through, worst on the
  480px panels. It now opens on five category rows and dives into one, with the same back
  button and slide the settings sub-pages use. A search box matches names and descriptions,
  and widgets this printer cannot use are listed in their own section with the reason. Widget
  names and descriptions are properly translated for the first time: 18 of 37 names and every
  description were invisible to the string extractor and only appeared in English. 74 new keys
  across all nine languages.
- **UI Scale setting, with a layout per scale** (#1484) - Settings > Display gains a UI Scale
  choice: Automatic, which shows the scale it picked, or a fixed 100 to 200%. Each scale keeps
  its own home arrangement, so trying a different scale and coming back restores the layout
  you had, which 1.0 could not do. On very high-density displays such as phones, Automatic
  now scales by the screen's real pixel density instead of drawing everything a third of its
  intended size.
- **1080p and wider panels get their own grid tier** - XXLarge was pinned onto XLarge, so a
  1080p panel drew the same physical widget size as a 720p one while fonts and icons scaled
  up 1.6x around it. Hence 128px glyphs and text running out of its box. Navigation width
  gained matching rungs.
- **Navigation buttons read by shape, not just colour** - inactive buttons use outline icons
  and the active one is larger and filled, which survives washed-out panels and colour-blind
  eyes. (thanks @just-trey)
- **Printer Manager lists everything HelixScreen detected** - the Capabilities section grows
  from 18 chips to over 40, grouped as Motion & Leveling, Thermal & Air, Filament & Toolhead,
  Printing, Electronics & Accessories and Add-ons. Chips carry details so you can confirm
  detection at a glance (Probe · Beacon, CoreXY, 350×350×340, Tool Changer · 4 Tools, 2 MCUs),
  capabilities forced on in settings say "set manually", and every chip is tappable: it opens
  the screen that uses it, or a short note on what it is. A Kalico printer's software row
  says Kalico.

**Home screen widgets**

- **A new screen starts with the printer's own home layout** - when a printer runs HelixScreen on
  its own display, other screens that connect to it copy that home layout the first time (your
  macro, LED, fan, sensor and camera picks included), and the Widget Catalog has a "Copy layout
  from printer" button to do it again.

- **The home screen's printer picture shows what the printer is doing (#1397)** - while the
  nozzle, bed or chamber heats, and until a heater that was turned off drops below 50°C, a
  temperature chip sits on the picture; the part fan shows its speed and the light shows
  when it is on. Tap a chip for that heater's graph, the fan controls or the light controls.
  On the 13 most common printers (K1, K1C, K1 Max, K2 Plus, Adventurer 5M Pro, AD5X,
  Creator 5 Pro, Qidi Q2, Snapmaker U1, SV08, Voron 0, Trident and V2) each chip points at
  its part: beside the picture with a line to it on a wide tile, on top of it on a small
  one. Other printers show the chips along the picture's edge - until you tag them: Tag
  parts in the printer image picker asks for six taps on the picture (nozzle tip, part fan,
  both front corners of the bed, a spot inside the enclosure, the light), shows the chips
  where they will sit, and saves. That works for your own photo too, and re-tags a shipped
  picture you disagree with; Reset tags puts it back.
- **The printer picture shows an attached CFS** - a Creality K2 with a CFS connected shows
  the K2 with the CFS on top, switching on its own when the CFS reports in or drops out. A
  picture you picked yourself stays in place.
- **A K2 without a CFS is a single-spool printer (#1904)** - with no CFS attached, the
  multi-filament panels, slot widgets and bypass controls stay out of the way, and the
  Filament panel's Load and Unload run the K2's own external-spool load and its cut and
  retract. The external spool row says what is loaded. The toolhead filament sensor shows
  up in Settings > Sensors and can watch for runout. A K2 that has a CFS no longer looks
  single-spool while its CFS is still answering at boot (#1933).
- **Icon tiles resize** - the single-icon tiles (power, lock, shutdown, firmware restart,
  LED, network and the rest) scale with their tile instead of staying one fixed size, and
  can be resized for the first time. On most panels they take half-cell steps in width; on
  the smallest panels, and for the Power tile, they keep whole cells. None goes below one
  whole cell, where the icon, its caption and the clock face would clip. A widget refuses a
  size it cannot draw, and edit mode shows that size in red.
- **The filament sensor tile on the home screen is tappable** - a tap now opens the tile's
  modal, and Load, Unload, Purge, Resume and Cancel Print all work from it, sharing the same
  dispatch the runout guidance dialog uses. Which sensor the tile watches is picked in edit
  mode.
- **The clog meter is a horizontal scale, two cells wide (#1017)** - at one cell the arc, its
  value and its mode text stacked into a box narrower than the words. It is now drawn as a
  scale with a label at each end: TANGLE/CLOG for FlowGuard's symmetrical range, SAFE/FAULT
  or a detection length for the linear ones, so a reading says which fault it is heading
  toward, not just how far. The arc stays where it fits, in the AMS sidebar and on the
  loaded-spool card.
- **Tapping the FlowGuard tile shows the reading, not just the mode** - the modal used to
  carry strictly less information than the tile that opened it: one row saying
  Automatic/Manual/Off, with no value, no threshold and no peak. It embeds the same bar now.
- **The temperature and fan stacks adapt to their shape** - given height, they put each icon
  above its reading; kept compact, they sit side by side.
- **The nozzle temperatures tile fits any size** (#1613) - it picks its font by the tile's
  height and its labels by width, shortening labels before shrinking numbers.
- **Choose which camera a widget shows** (#1487) - a camera widget's settings list Automatic
  plus every webcam the printer has, noting snapshot-only feeds and cameras that were ruled
  out. The choice is remembered by camera name, so it survives an address change, and falls
  back to Automatic if that camera goes away.
- **A Speed / Flow widget** - the speed and flow overrides on the home screen, tinted when
  either is off 100%. During a print it adds the live toolhead speed and volumetric flow,
  following the percent or mm/s choice the print screen uses. At its largest size it gains
  - and + buttons in 5% steps while printing, and a long press on a value puts it back to
  100%. Tap anywhere else on the tile to open Tune.
- **A Printer Message widget** (#1125) - the line your macros show with `M117` or
  `SET_DISPLAY_TEXT` ("Heating bed...", "Homing..."), the message a stock printer LCD keeps
  on screen. It shows with or without a print running and reads "No message" when there is
  none. At half a row tall it is one scrolling line; taller sizes wrap. Display only.
- **A Z-Offset widget** - the live Z-offset on the home screen. It shows how far you have moved
  since the last save and turns the warning color while there is something to save. At 2x1 it
  gains a Save button, and at 2x2 up and down baby-step buttons with the same remembered step
  size as Tune. Tap anywhere else to open Tune. Printers whose firmware keeps the offset itself
  never show Save.
- **A Filament Dryer widget** - every way your printer can dry filament in one tile: heated
  filament boxes, a chamber heater's drying cycle, and drying on the heated bed. Idle it shows
  a heated box's humidity, while drying the time left inside a progress ring, and on a 2x2 tile one row
  per dryer. A tap opens that dryer's own screen. It never starts or stops a cycle.
- **A humidity droplet replaces the water icon** - the home Humidity tile, the AMS overview card,
  the environment pill, the environment screen and each lane's readout draw a drop filled to the
  humidity reading. The water line sits at the reading's share of the drop's height, with two
  waves of different size in two tones taken from the theme's info colour. The surface drifts
  gently when animations are on and holds still when they are off. A sensor with no reading shows
  an empty outline.
  The Filament Dryer widget's idle humidity carries the same drop, filled to the wettest box.
- **A Host Health widget** - CPU load and temperature, memory and the busiest controller board,
  with a trace of recent load. It turns amber or red at 70% and 90% load, at 70 and 80 C, on CPU
  throttling, on rising MCU retransmits and on under 500 MB free for G-code files. A tap opens
  Settings > System > Performance. Only offered once Moonraker reports performance data.
- **A Toolhead widget you can move the toolhead from** - X, Y and Z, shown as `--` until each
  axis is homed, with a plate view and a crosshair for the nozzle from 1.5x1 up. Once the printer
  is homed and idle, tap the plate to pick a spot and confirm with Move here, or drag to move the
  toolhead live, just like the Motion screen's Bed tab; while it can move, a hold or drag on the
  plate never enters edit mode or swipes the page. At 2x2 a Home All button appears while an axis is not homed, and
  during a print the active tool and live speed take its place while the plate shows the head over
  the print. A 2x3 tile adds three preset buttons (Front, Center and Park by default; any Move-tab
  position, Park, Z +10, Z -10 or a macro, set from the gear in edit mode). A tap anywhere else
  opens Motion.
- **A Bed Mesh widget** - the active mesh as a heatmap, with a flatness reading tinted green
  below 0.15 mm, amber below 0.30 mm and red above. Wider tiles add the profile name, lowest and
  highest point and grid size, then a color scale and the profile count, and a 3x3 tile draws the
  mesh in 3D on screens that can. With no mesh loaded and nothing printing, a 2x2 tile offers
  Calibrate and Load profile. Only offered on printers with bed mesh support.
- **An Exclude Objects widget** - during a multi-object print, how many objects are still
  printing and how many are skipped, with a small map from 0.5x1 up: the object printing now
  in the accent color, skipped ones faded and hatched, over the bed's 50 mm grid. Wider tiles
  name the current object, and a 2x2 tile adds Skip objects. A tap opens the full objects map
  in Print Status, so skipping keeps its confirmation and undo. Only offered on printers with
  `[exclude_object]`.
- **Scheduled pauses are marked on the progress indicator (#1509)** - filament changes (`M600`),
  explicit `PAUSE`/`M601`, and slicer pause-at-layer stops each draw a tick where they sit in
  the print, on both the home print-status card's bar and arc and the print panel's bar. The
  tick's position follows the same axis the bar fills on: a file sliced with `M73` progress
  lines places ticks at the slicer's own percentages, anything else at file position. A print
  whose gcode was never fetched (an external start during the setup wizard, or a file too
  large to preview) simply shows no ticks.
- **Every light gets the same controls, and each light button picks its own light (#1130)** - the
  LED screen, now called LEDs, has a tab per light with a dot showing whether it is on and in
  what color. Each tab shows only what that light can do: power and brightness, white tones and
  colors, effects or presets, or On and Off for macro lights. Home light buttons each control one
  light or All lights, chosen from the gear in edit mode; a wide button has a › that opens that
  light's tab. Automatic LED Control has its own "Applies to" list. If you had picked some but not
  all of your lights for the light button, that choice now drives Automatic LED Control and your
  light buttons start out on the chamber light; set them from the gear.

**Motion**

- **The Motion screen is organized into Jog and Move tabs (#865)** - a labeled rail on the
  left in landscape, icon pills in the header in portrait, and it opens on Jog every time.
  The position readout that used to sit in a card beside the jog pad moved into the header,
  so it stays visible on both tabs and the jog pad takes the width the card used (the
  separate "Act:" line is gone; see the swap icon below). An axis that is not homed shows its
  position greyed out.
- **The Move tab sends the toolhead to named bed positions** - a 3x3 grid laid out like the
  bed seen from above (Rear at the top, Front at the bottom), placed on the print plate
  from the `[bed_mesh]` probing area rather than the full axis travel, so no preset aims
  at a purge area or tool dock past the plate; outer positions sit 10% in from the plate
  edges; on a delta the eight outer positions sit
  on a circle instead. A move lifts the nozzle to the Bed Map Clearance first when it is lower,
  and above your last print while that may still be on the bed. The positions are disabled until
  every axis is homed, and the whole tab while a print runs or is paused, or while the printer is
  not ready. The
  positions and Park grey out while the toolhead is moving, so a second tap cannot land
  mid-move. Park and Motors Off sit under the grid.
- **Park parks the toolhead** - it runs the printer's own parking macro when one is
  detected (`PARK`, `PARK_TOOLHEAD` or `TOOLHEAD_PARK`), otherwise lifts the nozzle 10mm
  (or enough to clear a print left on the bed) and parks over the rear of the plate, never past
  it. It is disabled until the printer is homed. Point it at a different macro in
  Settings > Printing > Macro Buttons.
- **Tap a coordinate in the header to move there** - a number pad opens for that axis, and
  a value outside the printer's range is refused with the allowed range while nothing
  moves; a coordinate is disabled while its axis is not homed. A Target / Actual chip beside the
  coordinates names which position is shown and toggles it, lights up for Actual, and is
  remembered per printer.
- **The Bed tab: tap or drag on a map of the plate to move there** - the plate is drawn from
  above with a 50 mm grid, and the head follows a drag. A move lifts the nozzle to the Bed Map
  Clearance (Settings > Printing > Motion) first when it is lower. The plate is disabled until
  every axis is homed, with a Home button in its middle.
- **Nothing on the Motion screen homes by itself** - the coordinates, the Move tab's positions,
  Park and the Bed tab's plate wait until the printer is homed, and only a Home button homes it,
  so a stray tap on an unhomed printer never starts a homing run.
- **Moves lift clear of a print left on the bed** - after a print ends (finished, canceled or
  stopped with an error, from any app), moves across the plate lift above its height plus the
  clearance, and the readout says why ("will lift to 77.6mm: last print is 72.6mm tall"). The
  print's objects are outlined on the plate, and a Cleared button there ends it once you have
  removed the part; the next print ends it too. A part too tall to clear refuses the move with a
  notice. It survives a restart and is kept per printer. Homing does not clear it.
- **Hold to repeat on the jog pad and Z buttons** - after about 0.4s the move repeats
  roughly every 0.15s for as long as you hold; a quick tap is still exactly one move.
- **Jog limits are quiet, and the edge is visible before you press** - holding into a limit
  simply stops; a fresh press that cannot move at all says which axis is at its limit and
  what the limit is; a partial move happens silently. The Z buttons grey out at their limit
  (on printers whose bed moves in Z, that is the pair that would move the bed past it), and the
  limits account for the printer's G-code offset.
- **Motors Off has a second home and a print guard** - it sits on the Move tab as well as
  the Controls panel, is disabled while a print runs or is paused, and if a print starts
  while its confirmation dialog is open, confirming only tells you the motors stay on and
  does nothing. E-stop remains the way to halt motion during a print.
- **Motion settings** - jog speeds and step distances are yours to set, under Settings >
  Printing > Motion or from the cog on the Motion screen's header. Separate XY and Z speed
  sliders, and all six step distances (the jog pad's inner and outer rings and the two Z
  buttons) take any value you type. Speeds are capped at the printer's own speed limit,
  including one the printer reports after you set them, and Reset Distances asks before
  wiping your values.
- **Pick a Jog-tab layout (#865)** - Settings > Printing > Motion > Layout offers the jog
  pad, Arrows (an XY cross, a Z pair, home buttons and one step selector, the Mainsail
  arrangement), Arrows + Home (a home key in the centre of each cross, the Fluidd
  arrangement) and Axis Rows (a row of minus and plus keys per axis at the jog mode's two
  distances). The arrow layouts' step selector offers every distance you set, so changing
  Turbo to 25 and 100 puts 25 and 100 on it. Home keys stay solid until their axis is homed.

**Tool changers and multi-toolhead printers**

- **IDEX and dual-extruder printers get the multi-tool screens** (#1350) - a printer is now
  recognised as multi-tool by counting its hot ends, so IDEX machines, dual extruders and
  tool changers that do not run klipper-toolchanger get the tool selector and filament
  screens instead of the single-extruder ones, and `T0`/`T1` tool switching works on them.
  Built from test configurations; not yet verified on these machines.
- **Tool Offsets screen with automatic calibration** (#1480) - per-tool X, Y and Z offsets in
  one place, and a one-tap calibration that runs your printer's own routine, follows it
  live, and saves every axis with a single restart. Where that routine calibrates every
  tool, the manual paper test is hidden. (thanks @Monstrofil)
- **Per-tool Z offset from Print Tune** - Print Tune has a Global / T0..Tn selector, so each
  tool keeps its own first-layer height, and the save button appears whenever any tool's
  offset is unsaved and writes it permanently. It used to vanish after the next Klipper
  restart.
- **Tap a toolhead on the filament path to switch to it** - on the Snapmaker U1, tool
  changers, and AFC or Happy Hare in toolchanger mode, tapping a head that is not mounted
  offers to switch to it; a tube, a spool or the mounted head still opens the slot menu.
  A new **Confirm tool changes** setting (Settings > Printing, on by default, shown only on
  multi-tool printers) decides whether that tap, the home Tool Switcher and the Filament
  screen's tool list ask first. A paused print always asks, and all three refuse the same
  way while printing, with spools on the bed, or while the filament system needs attention.
- **Snapmaker U1: load or unload several toolheads at once** - a picker lists every head
  with what it holds, pre-ticks loaded heads for an unload, and says which heads cannot take
  the operation and why. The printer's own batch command does the work, preheating the next
  head while the current one finishes, and the header names the head being worked on.

**Filament systems**

- **Your edits, the printer and Spoolman stop overwriting each other** (#1649, #1653) - every
  piece of lane information now has a known source, and the more trustworthy source wins
  instead of whichever wrote last. A colour you pick for a lane beats the linked spool's and
  keeps its colour name. A spool linked from Spoolman owns its brand, material and name
  (read-only while Spoolman is unreachable), and its remaining weight comes from the server.
  Your edits on AFC, Happy Hare and ACE are marked as yours in the printer's shared lane
  record, so Mainsail or Orca refreshing it does not wipe them, and on AFC a lane record from
  an older plugin build does not replace them either. A reading from one source that leaves
  a field out no longer blanks what another source knows. When a printer repeats an edit you
  made back to the screen, it is still read as your edit, not as the printer's own data, so
  clearing it brings back what the printer really reports (#1633). When another tool writes
  a lane after you did, the newer edit wins, whichever side made it (#1632).
- **A new spool is judged by what the hardware read** (#1710) - when a spool goes into a
  slot, a different tag, or a different material or colour read off the spool, clears the
  old spool's details; a matching read keeps them, and a spool inserted while the box is busy
  is judged only after its tag has been read. When the hardware can't tell (an untagged
  spool, or a slot the firmware only remembers), your details stay and a small "Same spool?"
  notice offers Clear. It is not shown on the lane feeding a print, or for a slot with
  nothing to clear.
- **Spool swaps are noticed, even with the screen off** - each lane remembers a fingerprint
  of the spool it held, so a spool swapped while HelixScreen was not running clears the old
  spool's edits instead of painting them onto the new one. Works on QIDI Box, the Snapmaker
  U1 (by the spool's RFID tag) and Creality CFS, including Kalico-based CFS setups.
- **A spool's tag can link its slot to Spoolman** - when a tag names a Spoolman spool, putting
  that spool in a slot links the slot to it, exactly as picking it by hand would. A tag that
  names a spool Spoolman doesn't have gets a short notice and leaves the slot alone, and a
  spool you pick while the same tag stays seated is kept. On by default; **Link Spools from
  Tags** in the filament system's settings turns it off. Creality CFS reads the Spoolman id
  that tag writers such as CFTag put in the tag's serial number.
- **QIDI Box lanes behave like every other system** (#1632) - slot presence, swap detection,
  edits that persist and repaint, and a Clear Spool that clears the Box's own records.
- **Humidity and drying per box** - the filament environment screen follows the way your
  hardware actually encloses filament: one heated enclosure over four slots, a sealed box
  per lane, or one box per unit. Each box shows its temperature and humidity and names the
  slots it covers, and its dryer is set from the numeric keypad within what that box
  accepts. A single box opens straight to its details, a few identical ones get tabs, and
  boxes that can do different things are listed side by side. When the dryer can only heat
  some boxes at a time, the waiting ones say they are queued and which box is using the
  heater.
- **Happy Hare 4: dry only the lanes you choose** (#1026) - on a unit with a heater per lane,
  the drying screen shows a chip per lane, all selected to start. Clear some to dry just the
  rest, turn on Rotate to have an eSpooler turn the spools while they dry, and Stop with a
  selection stops only those lanes. Each spool on the Filament panel shows a small badge while
  it is part of a cycle: drying, waiting for a heater, finished or stopped.
- **Favourite filaments** (#1100) - star any row in the filament catalog. Favourites lead the
  vendor list and float to the top of their vendor's view, on every printer.
- **Load and Unload for the external spool** (#1486) - the bypass spool's menu on the
  filament system screen now has its own Load and Unload, engaging bypass first when needed,
  so you no longer have to go to the Filament screen for it.
- **Happy Hare: tell the MMU what is really loaded** - Recover in the AMS Management overlay
  opens Recover State: pick the gate that is really selected (or Bypass) and whether filament is
  loaded, or let Happy Hare detect it. Nothing moves; Happy Hare only corrects its tracking, and
  no tool is remapped. The slot menu gains **Preload** (spool to gate, greyed out during a print
  or while filament is loaded), the Maintenance section gains **Load Extruder** and **Unload
  Extruder** for filament already at the toolhead (refused during a print), and Accessories
  gains **Refresh Spoolman**.
- **ACE shows where the filament actually is** (#1677, #1678) - the path reads the ACE's hub
  and toolhead sensors, so a strand parked short of the hub is drawn there rather than back
  at the spool, and on a Kobra S1 an idle slot the driver reports as parked is drawn staged
  at the hub. An ACE Pro with a fifth spool on its bypass switch gets a working bypass
  control.
- **A new Filament Buffer widget shows where your filament buffer sits** (#1724) - an upright
  slider with loose filament up and tight down, beside a big reading that is grey on target,
  amber as it drifts and red near an end. At 2x1 it adds the target, the last minute as a
  trace and "Running tight", "Running loose" or "Balanced"; tap it for Buffer Status, which
  updates live. It works with OpenAMS filament pressure sensors, AFC buffers with a pressure
  sensor (`FPS_PSF`, not yet verified on hardware: the only AFC rig on hand is a switched
  TurtleNeck, which is unaffected) and Happy Hare sync feedback. The loaded-spool card gets
  the same small slider, and OpenAMS shows each unit's FPS as a live-tinted box on the
  filament path. A sensor with no target shows the pressure as text only.
- **AD5X tool remapping uses the IFS's own commands** - the screen reads and writes the
  printer's tool map through `IFS_MAP_TOOL`, so a remap matches what the printer reports.
- **Spool labels lead with the spool number** (#1491) - on every label layout the spool number
  is the first and largest line, and weight moves to its own line so a narrow tape truncates
  that rather than the number.
- **The filament system screen fits small and portrait screens** - the loaded spool is a
  compact strip: a colour bar beside the slot and its remaining weight, the filament name on
  one line, and the clog and buffer meters underneath, which step aside during a load or
  unload so its progress has the room. The action buttons sit two to a row, so the side
  column no longer scrolls at 480x272, 480x320 or 800x480. Temperature and humidity stand in
  a pill at the end of each box, showing the dryer's time left while it runs. On small
  screens with five or more lanes, the slot labels alternate between two rows so each keeps
  to its own space, and the tool badges stay hidden while every tool feeds its own slot,
  appearing only when tools are remapped. In portrait the diagram runs the full width, with
  the loaded strip and status above it and the actions and load progress below.
- **Slot labels name the spool** (#1687) - each slot reads the spool's name above its
  material ("Bone White" over "PLA+"). A slot shows only the material when nothing names the
  spool, when it is empty, or when five or more slots share the row, and only the name when
  the name already says the material.
- **Creator 5 on Z-Mod: Load feeds the head** (#1779) - Load on a head feeds it through
  Z-Mod's afclite (`CHANGE_TOOL LANE=E<n>`), heating only that head. The Creator 5 has no
  automatic unload, so Unload shows a notice telling you to cut and pull the filament at the
  head, and sends nothing; to mount a head without feeding it, use Select slot in the slot
  menu. A fault from Z-Mod opens a dialog in the firmware's own words.
- **AD5X on Z-Mod's AFC layer: no tool remap error at print start** - Z-Mod maps tools in its
  own "Select print materials" prompt and refuses the screen's remap, so the screen no longer
  offers one there or sends it. That prompt's Start print button is also no longer read as the
  print having started.
- **Creator 5 on Z-Mod: a spool bound from outside shows up without a reboot** (#1833) - when
  another tool sets a head's spool (an NFC tag reader through `CHANGE_ZCOLOR`, or
  zmod_afclite's `SET_SPOOL_ID`), the screen reads the record it wrote and shows the new
  spool, name and colour within a couple of seconds. (thanks @DIEHARDave)
- **A new Edit Spool editor** - tap a slot and choose Edit Spool for one screen that shows
  the filament in words with its colour and remaining weight, the temperatures for what you
  picked, and Material, Variant, Brand and Product lists that narrow each other down, with
  the full filament catalog one tap away. Colour swatches are at least 7 mm across on every
  screen. The full colour view puts the presets and a custom colour picker side by side where
  both fit, and on small screens they take turns. On systems that read a spool's tag, a
  "from tag" pill marks each value the tag supplied, and Reset to tag puts them all back.
  Spoolman's spool list and QR scan are round buttons in the header, and a linked spool's
  price, location, lot and notes sit under More. A save still talking to Spoolman finishes
  even if you leave the screen, and a save that only partly succeeds says which part did
  not: with Spoolman offline, the slot saves and price, location, lot and notes wait in the
  editor. Which tool a slot feeds is picked from Assigned Tool in the slot's menu.
- **Creality CFS on the K2 Plus community firmware** - on the Kalico-based K2 Plus port and
  its own CFS module, loads use the module's slot selection, and each CFS unit shows its own
  temperature and humidity. With API revision 4 or later of the CFS module, the Filament
  Mapping you choose when starting a print is sent to the CFS just before the print and
  applies to that print only; a mapping that points at a slot the CFS cannot feed, or at a
  unit that has stopped answering, stops the print from starting and says why. With an
  older module HelixScreen sends no print map, and the print uses the module's own slots.
  A unit that stops answering is marked Disconnected until it comes back.

- **AFC: Calibrate Toolhead** - on an AFC version that has the guided toolhead calibration,
  the Toolhead section of the filament system's settings starts with a Calibrate Toolhead
  button. AFC then walks you through measuring the sensor-to-nozzle and unload distances and
  the cutter retract, asking each question on screen.
- **The spool editor is now Edit Spool** - a slot's Spool Info is renamed Edit Spool and
  opens straight into the editor. Material, variant, brand and product are dropdowns that
  narrow each other, with Browse catalog... at the end for searching or typing your own, and
  the colour grid sits beside or below them depending on the screen. Tap the weight to type
  the grams left. Price, location, lot and notes sit under More. Save is in the header, and
  Back with unsaved changes asks whether to save or discard them.

**Drying filament**

- **Dry filament on the heated bed (#1730)** - on an enclosed printer, Dry Filament on the
  bed card (or in Advanced) offers to unload the toolhead, homes, moves the plate as far from
  the nozzle as it goes, and asks you to lay the spools on the plate under a box. A banner
  shows the unload and the plate move as they run, and tapping it offers Stop, which ends
  the flow before the spools go on. The bed then holds a drying temperature for the
  material, capped at 90°C, for 12 hours, with a chamber dryer alongside when one is
  fitted. A printer with a plain chamber heater and no dryer gets **Heat the chamber too**
  instead: the chamber holds the material's drying air temperature, capped at the heater's
  limit, and is turned off when the run ends unless you changed it yourself. You are reminded to
  flip the spools halfway, and asked to take them off once the bed is below 40°C. Until you
  confirm the spools are off, HelixScreen will not home, move, restart Klipper or start a
  print, and a banner says so across restarts and power cuts. Printers you enclosed yourself
  can be marked enclosed in Settings > Printing.
- **Dry filament with a Panda Breath (#1299)** - a Panda Breath on stock firmware can run
  its filament-drying cycle from the chamber card: pick a material preset, and the card shows
  the chamber temperature against the drying target and the time left, with a Stop button. A
  chamber that levels off below the target is shown as a number rather than treated as a
  fault. An optional bed assist heats the bed to 70°C for the run and turns it back off
  however the run ends, and Klipper's idle timeout is held off for the run, which otherwise
  switches the heaters off five minutes in. DragonBreath firmware has no drying control in
  Klipper, so it is started from the unit there.

**Chamber heaters**

- **Add-on chamber heaters are detected and explained** (#1290) - DragonBreath and Panda
  Breath heaters are found at startup, each with its own safety ceiling. On the temperature
  screen, the chamber card shows any fault with a translated reason and a Reset button, the
  element temperature, and the filter fan with its own toggle, so the graph keeps the rest
  of the screen. A heater that has dropped off the network says Offline instead of offering
  a target it cannot reach. The Panda Breath's status format has not been verified on a
  stock unit.
- **Chamber temperature per material** (#1263) - Material Temperatures has a chamber column,
  editable on printers with a chamber heater.
- **A chamber target set to the printer's own resting temperature is no longer hidden** - on
  printers whose M141 splits the setpoint across a heater and a cooling fan (the K2), a
  chamber target of 35°C or below parks on the fan with the heater target reading 0. A target
  that equals the fan's configured resting value (35°C on a stock K2) is byte-identical to
  "off" on the wire, and the print screen used to show it as unset. That state now shows the
  number with a circled-A auto glyph beside it: the printer's standing behavior, not a
  deliberate set. Deliberate targets that differ from resting keep the checkmark, and the
  print screen's chamber status now follows the real setpoint while a print runs.

**Printing**

- **Pause a running print at a layer** (#1703) - tap the layer count on the print
  screen to pause at the next layer or one you pick, see the scheduled layer beside the count
  and as a tick on the progress bar, and cancel it. Uses the printer's own
  `SET_PAUSE_NEXT_LAYER` / `SET_PAUSE_AT_LAYER` macros (Mainsail and Fluidd client macros,
  Snapmaker U1 stock firmware) and needs a slicer that reports layers; hidden on printers
  without the macros.
- **Heater duty on every heater** (#1290) - the Controls and print status screens show how
  hard each heater is working as a percentage beside its status, so a nozzle running flat
  out and falling behind looks different from one holding temperature easily.
- **The print screen's speed chip shows live speed and flow** - instead of the speed and flow
  percentages, the chip beside the printer picture reads how fast the toolhead is moving right
  now in mm/s, with the volumetric flow in mm³/s under it. The percentages live in Tune; the
  chip turns amber while either is not 100%, and tapping it opens Tune.
- **Runout dialogs can hide manual actions mid-print** and carry an advisory rather than a
  warning header, so a dialog that is telling you something reads differently from one asking
  you to act.
- **A print can be queued while another one runs** (#1395) - a Files button on the print
  status screen opens the file list mid-print. While a job prints, or is still
  preparing before its first layer, the file view's Print button becomes Add to Queue with
  a clock icon, and a toast confirms the job's position. The pre-print options you set are
  saved with the queued job and come back when it starts; the filament mapping is not
  saved, it is worked out when the job starts. Needs Moonraker's `[job_queue]` component;
  with `automatic_transition: True` in `moonraker.conf` the options card is hidden while
  queueing, because Moonraker starts queued jobs itself.
- **The queue is visible while a print runs, and starting the next job is one tap** - an
  "Up next: name (+N)" line on the print status screen and the home print card names the
  first queued job; tapping it mid-print opens the Job Queue. The completion dialog gains
  a **Start next** button. Tapping the line, that button or a queue row while the printer
  is idle opens the file in the file view with its saved options already set - check the
  bed is clear, tap Print, and the job leaves the queue only once the print actually
  starts.
- **The Job Queue home widget shows what runs next** - the next job's name, how many wait
  behind it, and a **Bed clear · Start next** button while the printer is free. From a wider
  size it adds the job's picture and time estimate, and the largest size lists the jobs
  behind it. While a print runs it shows the queue count and the next job, and says "Starts
  automatically" only when Moonraker will really start it. Start next uses the pre-print
  options the job was queued with and runs the same checks as the file view, so a material
  or spool warning still asks first; a job the widget cannot check, or one that needs you to
  choose a filament remap, opens in the file view with a notice. The widget needs Moonraker's
  `[job_queue]` and can be shrunk to 1x0.5. In the file view, Print reads "Checking file..."
  while a check it needs is still running.
- **The Print Stats home widget is a week at a glance** - the hours printed this week, with a
  bar for each of the last seven days and weekday initials at the largest size, and a line of
  lifetime prints, hours and filament in meters where there is room. At its smallest it is one
  line, and it can be shrunk to 1x0.5.
- **Spaghetti detection, built in (#1378)** - installing HelixScreen on a K2 Plus stops the
  stock AI failure-detection loop along with the stock UI, so HelixScreen ships its own
  detector: during a print it runs Creality's own `/usr/bin/detection` on camera snapshots at
  the interval and threshold the printer's `ai_control` settings already hold, and never
  writes them. A confirmed detection pauses the print and opens a dialog offering Resume,
  Abort, Reduce Sensitivity or Turn off detection; the alert is translated and shows the
  detection's confidence. The whole pipeline is vendor-neutral: any
  detection source only reports, and two settings in Safety & Alerts decide what
  happens - **Spaghetti Detection** (watch at all) and **Pause on Detection** (pause, or only
  warn). On the first start both are seeded once from the printer's own stored choice
  (`switch` / `pausePrint`) and are HelixScreen's from then on. Printers whose firmware
  pauses by itself, like the U1, are never double-paused and always get the response dialog
  (their Pause on Detection row hides; the firmware's pause is not HelixScreen's to govern).
  The rows appear only on printers with detection hardware. Uninstalling restores the stock
  service.
- **K2 with the k2-improvements mod** - its print start sequence is recognised, so the
  preparation steps are named correctly, heat soak and the chamber wait show as soaking, and
  the bed is no longer meshed twice.
- **Camera button on remote screens** - running HelixScreen against a printer on another
  machine, the print screen gains a Camera button for the full-screen webcam view. It stays
  hidden on the printer's own screen.
- **Macros remember their parameters** - save default values for any macro that takes
  parameters, from edit mode in the Macros panel or a home Macro Button's options. With Ask
  for parameters on, the form opens already filled in; switch it off and the macro runs
  straight away with the saved values. Defaults are kept per printer, and a dangerous macro
  still asks for confirmation.
- **Pre-print options are tiles you tap** - each option is a tile in a two-column grid with
  its own icon: tap anywhere on the tile to switch it on or off. A tile that is on fills with
  the accent color and shows a check in place of its icon. Long option names wrap onto a
  second line inside their tile instead of running under a switch.
- **More room on small print file screens** - the sliced-colors switch is now a palette
  button on the 3D preview, the filament card is more compact, and on 480x320 and smaller
  screens the pre-print time estimate gives its line to the options, so four options fit
  there without scrolling in landscape. On small portrait screens the filament chips share
  the FILAMENTS title's line and the preview gives up just enough height for every option
  to show.
- **Filament chips you can read and change in place** - the FILAMENTS card on the print
  file screen shows one chip per tool the print uses: the file's material on its color at
  the top, and the slot that will feed it underneath, as a spool dot in that slot's color and
  its number ("3", or "2·1" for the first slot of the second unit). The outline says whether
  the pairing is a problem: grey when it matches, amber when only the color differs, red when
  the tool has no slot, its slot is empty or the material differs. A tool with no slot shows
  an empty dashed spool. The warning icons beside the card's title are gone. Tap a chip and
  a list opens under it with Auto and every slot, best match first, each with a reason
  ("exact match", "color differs", "material differs", "empty"), and "More options…" for
  the full Filament Mapping dialog. On a printer with no filament system the card now
  appears too, comparing the file with the loaded spool, with a swap icon on each later
  filament change.
- **Delete and Print stay on screen** - the filament mapping and the options scroll in one
  column that ends above the buttons, so starting the print never depends on finding
  the button at the bottom of a scroll. A fade with a small arrow sits on the list's bottom
  edge whenever more of it is below.
- **The print file screen has a portrait layout** - on tall panels the preview, options and
  buttons stack top to bottom, and the preview shrinks just enough to keep the first row of
  option tiles in view instead of leaving a sliver of the list.
- **"Printed N times" sits under the file name** - the print count now reads as part of the
  file's identity beside the metadata, instead of being a line in the scrolling column
  below.
- **The 2D preview shades creases** (#1314) - outer walls darken where the surface folds,
  so overhang ledges, rims and steps read as edges while straight and evenly sloped walls
  keep their colour. Sequential prints, objects at different layer heights and spiral vases
  are left alone.

**Calibration and tuning**

- **Pressure advance, measured by the printer** (#1452) - on printers that can measure it
  themselves, a Pressure Adv. button on the Controls panel runs the measurement for the tool
  you pick and shows the result, flagged if it's outside the usual range; a Pressure Advance
  row under Advanced > Calibration opens it too. The Snapmaker U1 applies and keeps the
  value; on the FlashForge Creator 5 Pro you copy it into the slicer. Takes a few minutes and
  heats and purges. (thanks @Monstrofil)
- **Belt Tension check** (#1303, #1721) - on a CoreXY printer with an accelerometer,
  Advanced > Belt Tension drives each belt path with the motors and compares the two
  responses on one chart: a similarity score, the peaks both paths share and the ones only
  one path has. Re-test A or B sweeps one path again and keeps the other, with the old curve
  ghosted so you can see what your adjustment moved. Needs HelixScreen on the printer's own
  computer, and warns first on boards with little memory.
- **Guided belt tuning** (#1736) - after a Belt Tension check, Tune Belts walks you through
  closing the gap one belt at a time. It tells you when the belts already match, finds the
  zone to tune with one calibration turn instead of trusting the tallest peak, runs short
  settling laps before each sweep, and re-measures only the stretch of the spectrum that moved,
  always inside your printer's sweep range. It checks that readings repeat before it advises
  anything, and flags a path that resonates far harder than the other as a mechanical problem
  rather than tension. On Voron 2.4 and Trident it suggests how far to turn, in quarter turns;
  other printers get direction-only advice. Tune Belts stays greyed out until the printer has
  reported its sweep range, so it never sends a sweep your machine cannot take. The screen fits
  small, portrait and very wide displays, with the chart beside the controls on a wide one, and
  the chart's axis labels no longer clip. The thresholds are provisional until it has run on
  more hardware.
- **Which belt is which?** - Belt Tension can buzz each path's motor so you can see which
  belt it drives, then name the paths left and right or upper and lower. Every instruction
  then uses those names ("Tighten the left belt one full turn"), saved per printer.
- **Live sweep graphs** (#1812) - the Input Shaper screen, the setup wizard's Input Shaper
  step and Belt Tension draw the response live while the printer sweeps, instead of a bare
  progress bar for minutes. Klipper's own analysis replaces the live curve when the sweep
  ends, and every recommendation still comes from that analysis. Needs HelixScreen on the
  printer's own computer; the smallest screens keep the progress bar alone.

- **Switch a Kalico heater between PID and MPC** (#1237) - the method selector on Kalico
  printers is out of beta. It migrates your config in both directions, and cancelling a
  running MPC calibration asks first, because cancelling means an emergency stop and a
  firmware restart.
- **Input shaper saves the shaper you picked** - the chips under the results graph are now
  one choice per axis, and Save writes that choice to your config. Save used to write
  Klipper's own recommendation regardless of what you selected.
- **Baby-step size is remembered across launches** - the Z-offset step you last chose is the
  one you get next time.
- **Save Z-Offset can be turned off per printer** (#1766) - **Save Z-Offset Buttons** in
  Settings > Motion. Off removes the save from the header, Controls, the Z-Offset widget and
  Z-offset calibration, and Print Tune calls an adjustment temporary; baby-stepping still
  works. It starts off on tool changers, where one machine-wide save would shift every
  tool's offset, and on everywhere else. Printers whose firmware keeps the offset itself
  do not show it.

**Printers**

- **VzBot 330 and 235** (#1689), **FLSUN S1 and S1 Pro**, and the **base Creality K2**
  (#1606), each with artwork, and the **FLSUN T1 Pro** (#1781), which comes set up with its
  fans, its enclosure light and a bed levelling button. The base K2 is now told apart from
  the K2 Plus and Pro, and its camera option no longer claims the Plus's AI model.
- **FlashForge Creator 5 and Creator 5 Pro are supported printers (#1714)** - both models of
  FlashForge's four-head tool changer are recognized and given their own settings, on either
  of the two firmwares that replace its stock software: Z-Mod and Reforge. Mounting and
  parking a head is driven from the screen, and the swap shows its steps (dock / pick up)
  as Z-Mod narrates them; Z-Mod's always-on endless-spool switching is reported. On Z-Mod
  1.7.4+ the four heads read their material and colour straight from the printer, which
  also names itself correctly in debug bundles instead of reading as an Adventurer 5X. The
  firmware's own filament-system faults surface with their own wording, a head without
  filament reads Empty, and a read-only tool-state query replaces the stale boot state on
  older Z-Mod builds. All of this is verified against a simulated printer and a field
  debug bundle; the swap-step wording on a live print is the one piece still pending
  hardware verification.
- **Snapmaker U1 firmware settings** - the pre-print toggles (bed mesh, input shaper,
  pressure advance, timelapse) read and set the firmware's stored values instead of always
  reading off. Tangle sensitivity, end-of-print unload per toolhead, and manual bed levelling
  with a PEI plate check are all on screen. Loading preheats to the firmware's own
  temperature for that filament. Firmware faults appear with translated wording.
- **Snapmaker U1: unload at end of print sticks** - turn it on per toolhead under the filament
  system's Print Behaviour settings and it now applies to every print, including Print Again
  and a restart from the beginning; before, the printer forgot it after the next print, and
  switching several toolheads on quickly could leave only the last one set. A toolhead loaded
  with soft filament such as TPU is never unloaded, as on the Snapmaker screen. HelixScreen
  now sends the choice with every print it starts, so a different choice made from another
  app between prints is replaced by HelixScreen's.
- **Snapmaker U1 with the multiACE mod** (#1426) - keeps its filament screen instead of being
  mistaken for a plain ACE setup.
- **Power-loss recovery works on Qidi printers (#1716)** - on a Q2, Q1 Pro or Plus 4 running
  the stock firmware, the resume dialog now appears after a print was cut short by a power
  loss, offering the printer's own `RESUME_INTERRUPTED` flow or a clean discard.

**Display, sound and touch**

- **Glass Effects** - an appearance option under Settings > Appearance that gives buttons a
  lit, glassy finish and every card and panel a soft light from its top-left corner. It is
  drawn from your theme's own colors, so it works with every theme in dark and light mode,
  and it leaves filament color swatches, camera views and previews untouched. It only
  appears on hardware fast enough to draw it: HelixScreen measures this once on your screen
  and hides the option on slower boards and 16-bit panels, which keep the flat look.
- **Bouncing Printer screensaver** (#1680) - the screensaver everyone already knows. Your
  printer drifts across a black screen and reflects off the edges, picking up a new tint on
  every wall. Land a true corner and it celebrates. The sprite is whatever printer this
  screen is attached to, auto-detected or picked by hand, so a Trident bounces a Trident. It
  costs a fraction of what Flying Toasters does; on AD5M-class hardware the corner
  celebration is a backdrop flash rather than confetti, to stay clear of the print loop.
  Select it under Settings > Display > Screensaver. (thanks @Tintef)
- **Bouncing Logo screensaver** - the HelixScreen logo bouncing around a black screen in its
  own colors, for anyone whose printer image is not much to look at. Select it under Settings >
  Display > Screensaver.
- **Flip Clock screensaver** - big light digits on dark split-flap cards, the time of day in
  your 12- or 24-hour format. While a print runs the cards count down the time left, with
  "done at 9:42 PM · 63%" underneath; a pause freezes the count and a finished print shows
  DONE for a minute. Every digit has its own card, and only the digits that change fold
  their top half down; slower boards swap the digits instead. Select it under Settings > Display > Screensaver.
- **Fireworks screensaver** - shells rise from behind a line of hills under a starry night
  sky, trail sparks, and burst as peonies, chrysanthemums, willows or rings, each spark
  fading from white through its shell's colour to an ember. It adjusts how many sparks and
  bursts it draws to what the board can afford. Select it under Settings > Display >
  Screensaver.
- **The Flip Clock is the new default screensaver** - fresh installs show the split-flap clock, which costs almost nothing and counts down a running print. A screensaver you already chose stays as it is.
- **3D Maze screensaver** - the old Windows walk through a brick maze, with wood floors, a
  tiled ceiling and the original's wide view. The maze has a few loops and small rooms, and
  the halls fade into the dark. Spinning HelixScreen logos float in the halls, gray rocks flip
  the view upside down, a tiny toolhead scurries past, and the walls are hung with framed
  thumbnails of your recent prints and other files, and your printer. The pictures come from
  thumbnails the screen already has, so the saver never downloads anything. Select it under
  Settings > Display > Screensaver.
- **3D Pipes looks like the Windows original** - fat, shiny pipes in the sixteen classic
  materials (emerald, ruby, brass, silver, plastics and rubbers) grow through the
  screen with elbows, ball joints and the occasional teapot. Near pipes now always pass in front
  of far ones, the screen fills up, then dissolves away in blocks and starts over from a
  slightly turned view.
- **Hyperspace screensaver** - a flight through glowing rainbow space: star flares stream
  past, a shimmering wormhole sweeps in and out, and now and then a shell of flares bursts
  ahead. Select it under Settings > Display > Screensaver.
- **Fireflies screensaver** - coloured swarms of glowing fireflies chase each other through
  the dark, trailing long tails that drift in the wind. Now and then a swarm stops, circles,
  speeds up, splits in two or joins another. Select it under Settings > Display > Screensaver.
- **Aqua Surface and High Fly screensavers** - two clock savers after the old Flash
  originals. Aqua Surface is dark, glassy water sloshing in slow motion behind a tank wall,
  with a three-ring clock in the water and the date under it; a droplet flung from a crest
  always falls back into the water. High Fly skims sunlit cloud tops under a deep blue sky,
  passing tall clouds and now and then flying through one, with a large thin clock at the
  cloud line. While a print runs, both show when it ends and how long is left. Select them
  under Settings > Display > Screensaver.
- **A screensaver your printer can't run is left out** - each saver measures what it costs
  on your board, and one that would still slow the printer at its lightest setting drops out
  of the list. Another saver plays in its place instead of a black screen, and your choice
  stays saved for a faster printer. 3D Maze and Hyperspace are too heavy for the AD5M.
- **Telemetry counts screensavers** - with telemetry on, the settings report names the
  screensaver you picked, and a report every 4 hours says how long each one showed, how often
  it started, whether a touch or something else ended it, and how light a setting it had to
  drop to on your board. The Test button's previews are not counted.
- **All five screensavers on every board** - Flying Toasters, Starfield, 3D Pipes, Bouncing
  Printer and Fireworks now run on the AD5M and Centauri Carbon too, which had none, and on
  every other board with a 16-bit display. Each saver draws less on a slower board rather
  than stalling it, and the starfield redraws only what moved, so it costs a small fraction
  of what it did.
- **Screensaver after, on screens without a dimmable backlight** - a phone or tablet running
  the Android app, and any screen whose brightness HelixScreen can't change, has nothing to
  dim, so the dim timer is what starts the screensaver. Settings > Display now offers it there
  as Screensaver after, with the same choices as Screen Dim, instead of hiding it and always
  waiting 10 minutes. The row is hidden while the screensaver is Off.
- **The AD5X buzzer plays chords and music** - UI sounds play as chords, tracker music plays
  in four voices, and `M300` beeps from your macros take the buzzer when Klipper holds it.
- **On-screen keyboard keys look like keys** - each key has a raised edge in its own colour
  instead of a flat black shadow.
- **Hardware rotation with touch that follows** (#1275) - a panel turned 180 degrees can
  rotate on the display hardware where it supports it, and touch, mouse and touch
  calibration all follow the turned picture. Quarter turns stay in software, which is the
  only way touch can follow them today.
- **Pinch to zoom the 3D views** - on a capacitive screen, pinching zooms the 3D G-code preview
  and the 3D bed mesh in on the spot between your fingers, and a two-finger drag moves the zoomed
  view. Rotate, tap and the exclude-object long-press stay off until every finger has lifted.
  Resistive screens detect one finger and keep one-finger rotate only.

**Setup and settings**

- **Share the screen with Mainsail and Fluidd** (#1825) - Settings > Connection > Screen
  Sharing > Share Screen adds a **HelixScreen Screen** webcam that shows the printer's screen, and Allow
  Remote Control lets you tap and drag on it from the browser. On MainsailOS and KIAUH
  installs it follows Moonraker's own access rules; on the K1, K2, Adventurer 5M and
  Centauri Carbon a browser pairs once with a six-digit code shown on the screen. A small icon on the
  printer shows while someone is driving it remotely. Off by default, and it costs nothing
  while nobody watches. Printers whose firmware already shares the screen keep theirs.
- **Screen Sharing is per printer** (#1825) - each printer you control keeps its own Share
  Screen, Allow Remote Control and the new Add to Mainsail/Fluidd Webcams setting, so sharing
  one printer's screen never lists it among another printer's webcams. With Add to
  Mainsail/Fluidd Webcams off the screen stays shared at its browser address but no webcam
  entry is added. A beta that had sharing on keeps it on for the printer that was selected.
- **Find your printer by searching** (#1028) - first-run printer selection replaces the
  scroll through about 105 machines with a search box, a grid of vendors and a list per
  vendor.
- **Install HelixScreen's helper macros from Settings** (#1271) - a row under Advanced
  installs or updates the macro pack without running the installer. It backs up
  `printer.cfg` first, restarts Klipper right away when idle, offers the restart for after
  the print when one is running, and refuses to touch anything mid-print.
- **Hardware Health shows its status** - the Settings row carries the live result with a
  severity-coloured icon, and critical problems get their own alert icon.
- **Settings > Printer opens the Printer Manager** - the printer's name, picture, model,
  software versions and detected hardware are one tap away in Settings, so they are reachable
  without the home screen's printer picture.

**Plugins**

- **Lua plugins** - drop a plugin into `helixscreen/plugins` in your printer's config folder
  and HelixScreen picks it up, on every printer you connect to. A plugin can add home screen
  tiles, its own screens and a settings page, and draw charts. Each one runs in a sandbox with
  its own memory and time limits, and asks before it gets access to G-code, Moonraker or the
  network; its Moonraker calls get the same refusals as the app's own, so a plugin cannot
  start a print or send a restart the screen itself would refuse. Settings > Plugins lists
  them, and nothing runs until you enable it there. A plugin that changes on the printer
  reloads in place.
- **Six example plugins and a build tutorial** - a temperature sparkline, an LED effects
  tile, Maintenance Meter (consumable hour meters on a home tile, with confirm-guarded
  resets), Print Watcher (print start, finish and failure notifications to ntfy, Discord,
  Telegram or a webhook), Ambient Tile (room conditions from Home Assistant or any JSON
  endpoint, with a sparkline) and Calibration Bridge, a proof of concept for a slicer
  calibration companion that asks you to pick a result on screen. A step-by-step tutorial
  walks through building the first of the new four. Plugins can be shared in the Discord
  #plugin-showcase channel (#1771).
- **Plugin dialogs** - a plugin can ask a question in a dialog that looks and closes like
  the app's own, sized for your screen. Plugin dialogs and screens never cover the app's: one
  that arrives while an error, a warning, the print-complete dialog or the lock screen is
  showing waits its turn and appears once you have dealt with it, waking the screen. Calibration
  Bridge now asks which result was best this way when its print completes.
- **Plugin translations** - a plugin can ship an `i18n/` folder with translation packs, and
  its manifest text, screens and code follow the app language. A pack only supplies words the
  app does not already translate, so a plugin cannot change the app's own labels; English
  packs are refused, a pack over 256 KiB is skipped, and an edited pack needs an app restart.
- **Plugins can drive their own settings** - a plugin's own screen can act as the control
  surface for the settings it declares (`helix.settings.set`). The generated settings page
  stays in step, and setting a value it already holds saves nothing.
- **Plugins can add actions to the app's menus** - an enabled plugin can put its own items on
  the print details screen's menu (the dots button), in a column of their own in a filament
  slot's menu, in the Spoolman spool menu, and behind the gear on its own home tile in edit
  mode. Each item gets the file, slot or spool the menu was opened on, and can hide itself or
  show greyed out with a reason when it does not apply. A plugin can only add rows: the app's
  own items stay as they are, and a menu never grows long enough to scroll. Settings >
  Plugins lists which menus a plugin adds to, before you enable it and on its row. A plugin
  can also build up to 32 widgets.

**Files, USB and installing**

- **USB sticks mount on boards that do not mount them** - where nothing else mounts a USB
  stick, HelixScreen mounts it read-only itself and unmounts it on shutdown. Sticks plugged
  in while the app runs appear within about a second, and short DOS-style names such as
  `3DBENC~1.GCO` are recognised as G-code.
- **One package for the K1 series and the AD5X** - both boards run the same build, which is
  smaller than the K1C's was. It is still published under each board's name, so existing
  updaters keep finding it.
- **Smaller downloads** - packages carry only the splash screens and printer images the
  panel's resolution can show (K2 assets 42MB to 9MB, Centauri Carbon to 2.8MB), and
  embedded packages drop music the machine cannot play.
- **Logs and caches survive an update** - on the K1, Centauri Carbon, U1 and AD5X, a web
  update used to delete the logs you needed and throw away the thumbnail cache. Both now
  live beside the install instead of inside it.
- **Run HelixScreen in a window on a Linux desktop** - a new package for x86_64 Linux PCs
  (Debian 11, Ubuntu 20.04, Fedora 34 or newer, on X11 or Wayland) runs HelixScreen as an
  ordinary app window that connects to your printer over the network. Install it with
  `install.sh --desktop`: no `sudo`, it goes into your home folder, and HelixScreen appears in
  your applications menu. Without the flag, on a PC with a desktop the installer asks
  whether you want a window or a full-screen kiosk, and suggests the window. Install Update
  opens the release page in your browser instead of updating in place. The 3D G-code preview
  uses your graphics driver's OpenGL ES when it has it, and draws in 2D otherwise.

### Changed

- **The print status picture holds still** (#1879) - the chips around the printer picture no
  longer rearrange it when a fan spins up, the speed is tuned or the light turns on. The
  layout is decided once for preparing and once for printing; a reading that becomes unusual
  mid-print (a spinning aux fan) slots into free space instead. The picture is also drawn
  from a copy sized to fit, as on the home screen.
- **One name per fan and per pre-print step** (#1879) - the hotend fan reads "HE" in fan
  settings, as on its chip, and the fan stack's third row reads "Aux", the same fan the print
  status chip calls Aux. The pre-print option toggles use the step names the progress
  checklist shows, so the purge toggle reads "Purge".
- **Chamber reading on the home picture without a heater** (#1879) - a printer with only a
  chamber thermometer now gets the chamber chip on the home printer picture, as Print Status
  already showed it.
- **Stop buttons say "Stop"** - the red button that ends a running Input Shaper test, a heater
  (PID) calibration, a tool offset calibration, the setup wizard's Input Shaper step and a belt
  tension check now all read "Stop" with the same octagon icon, instead of "Abort", "Cancel" or
  a plain cross. They still do what they did: an emergency stop followed by a firmware restart,
  because none of these can be paused partway through.
- **Spools look like spools** (#1817) - every drawn spool has a vented flange: six slots, and
  vent dots on larger spools, that show the filament wound inside, so you can read how full a
  spool is through them. The flat AMS spool style now draws the spool face-on through a smoked
  flange instead of plain rings. The Filament tab and filament icons are a spool instead of the
  nozzle, which stays on the buttons that are about the nozzle, such as Purge.

**Screens and settings**

- **Beta features gates two things** - the Dev update channel and the prompt offering to install
  the HelixPrint plugin. The Z Calibration button on Controls, the Macro Browser and
  multi-printer management work without it, and so does everything new in 1.1: Belt Tension,
  both pressure advance tools, Tool Offsets, pausing at a layer and choosing lanes to dry.

- **Settings is one list of twelve pages in three groups** (#1023) - Screen (Display, Appearance,
  Touch & Input, Sound), Printer (Printing, Devices, Safety & Alerts, Connection) and HelixScreen
  (Language & Time, System, Updates, Help & About). Every page is one tap from the Settings screen,
  and Display, Appearance, Sound, Devices, Connection, Language & Time and Updates show a short
  status line, such as your brightness and sleep time or whether an update is waiting. Nothing is
  reset: every setting keeps its value. Where things went:

  | Was | Now |
  |-----|-----|
  | Display & Sound | Display, Appearance, Sound, Language & Time |
  | Printing > Toolhead Style, G-code Preview, Z Movement | Appearance > Printer Visuals |
  | Hardware & Devices | Devices |
  | Hardware & Devices > Printers | Connection > Printers |
  | Safety & Notifications | Safety & Alerts |
  | Safety & Notifications > Allow cold load/unload, Cool nozzle after filament ops | Printing |
  | System > Network Settings, Host | Connection |
  | System > Touch & Input | Touch & Input, on the Settings screen itself |
  | Display & Sound > Scroll Buttons, System Keyboard, Keep Navigation Bar | Touch & Input |
  | Help & About > About > Update Channel, Check for Updates | Updates |

  The upgrade banner's Update button now opens Settings > Updates. The empty Plugins screen is
  gone from System (#1235).
- **The Controls panel fits small screens** - the Calibration & Tools card holds only
  calibration (Bed Mesh, Z Calibration, Pressure Adv., Bed Screws, and QGL, Z-Tilt or Tool
  Offsets where the printer has them) in a grid that keeps every label on one line. Motors
  Off moves beside Motion on the Position card, sized to tap. The Light becomes a Quick
  Actions choice: pick **Light** for any Quick Button in Settings > Printing > Macro Buttons,
  and while an LED is controllable and you have left a slot unassigned, the first such slot
  shows it. A slot you cleared stays empty.
- **The print screen's buttons are named on the smallest panels** - on 480x320 and
  480x272 screens the action buttons (Light, Pause, Tune, Cancel) stack their icon over
  their label and fill the column, where they used to leave a dead band under short
  buttons; at 480x272 the Speed/Flow readout gives way so every button keeps its label.
- **The temperature screen's control strip fills its column on small panels** - on
  480x320 and 480x272 screens the strip of heater controls is wider, the tool buttons
  get real gaps between them, and the preset buttons fill the strip instead of leaving
  empty space under the tool picker.
- **Advanced hides empty sections** - a section whose rows all hide on your printer takes its
  heading with it.
- **Filament operations on an unhomed printer home without asking** - the "Home printer
  first?" dialog is gone: asking for a load or unload is taken as asking for the home it
  needs. On the Snapmaker U1, which homes and heats for its own loads, HelixScreen no longer
  sends a home or a preheat first, and the load step bar follows the printer's own order.
- **Loading filament does not preheat twice** (#1494, #1495) - when the filament system, your
  macro or the "allow cold load" setting already handles heat, the screen no longer preheats
  first. On QIDI printers this stops a preheat that the stock macro then overrode.
- **Temperature keypads stop at your printer's maximum** (#1615, #1619) - and say what the
  limit is when you hit it. Material temperature presets are not limited, so an ABS preset
  above a 260 degree hotend's limit saves fine and is applied at the limit.
- **Tools and lanes are numbered from 1** (#957) - everywhere the screen names a tool, a
  nozzle or a lane for you to read, it counts the way people do: "Tool 1" and "Nozzle 1" where
  it used to say T0, including the pre-print filament check, the tool switcher and the
  tool-change and preheat toasts ("Switched to Toolhead 4" on a Snapmaker U1, not "Switched to
  T3"). G-code, macros and the console keep the T0 spelling the printer expects.
- **Clear Spool forgets everything** (#1661) - it erases everything HelixScreen and the
  printer's firmware remember about the slot, leaving only what the hardware can physically
  read. That includes the printer's own record: the whole gate map entry on Happy Hare, and
  the slot record on AFC, AD5X, QIDI Box and Kalico-based CFS. ACE, the Snapmaker U1 and stock
  CFS keep their own record, because their firmware offers no way to erase it or re-reads the
  spool's tag. Clearing a single field brings back what the machine reports for it. Clear
  Spool is refused on the lane feeding a print that is running, paused or preparing, and the
  menu says why. On Happy Hare in Spoolman pull mode, clears and edits are refused with
  Spoolman named as the reason, instead of appearing to work.
- **Tool changers without ASSIGN_TOOL remap without beta features** - the job file is
  rewritten before printing whenever remapping needs it, and the plugin's Install row is back
  in Settings > Advanced, no longer beta-gated. A dialog at startup warns when Moonraker is
  too old for remapping, and can be dismissed per printer.
- **The log records more by default** - production builds log at Info and write each line to
  the log file as it happens, so a log taken right after a problem already holds what
  happened. A Log Level you set in the app still wins.

- **Material types and your Material Temperatures changes live in one editable file** - the
  built-in material table (PLA, PETG, ABS and the rest) now ships in the filament catalog, and your
  per-material temperatures and preheat macros move out of `settings.json` into
  user_filaments.json, next to any brands and products you added. You can edit it from Mainsail or
  Fluidd, change a built-in material, or add a material type of your own. Existing changes move over
  automatically on the first start. **Downgrading to an earlier version loses your Material
  Temperatures changes**, because the older version only reads them from `settings.json`; note them
  down first if you might go back.
- **The filament panel fits small and portrait screens** - the Load/Unload/Purge and
  Extrude/Retract buttons grow into the space the right column used to leave empty, and a divider
  separates the material presets from the operations. Cool Down is now a fixed cell in the preset
  grid, dimmed while nothing is heating, so no button moves when a heater starts or stops; the
  cold-extrusion warning takes the Operations heading's line for the same reason. The tool
  picker's caption uses the printer's own word ("Toolhead" on the U1) and the closed dropdown shows
  just the number, or the tool's own name if it has one. In portrait the panel is a single column:
  temperatures, presets, the temperature graph when there is room for it, operations, then the
  lane strip and tool picker.

- **Emptying a page in edit mode removes it from your saved layout** (#1638) - moving a page's
  last widget to another page, or removing it, deletes the empty page. The main page stays, and
  so does a page still holding widgets that are greyed out because their hardware is not detected.
- **Adding a page: a visible + past your last page, or a drag past either edge** (#1638) - the
  page past your last one carries a + you can tap to add a page, editing or not, and it stays
  reachable even when you have a single page. To grow a page out of a widget instead, long-press
  the home screen, pick up a widget and drag it past your last page: an empty page slides in, and
  dropping the widget there creates the page with the widget on it, in place, without the home
  screen sliding back to the page before it first. At your first page's left edge nothing slides
  in: let go with most of the widget past that edge and a new page is created in front, with the
  widget at its left side. At the 8-page limit there is no + tile and neither edge creates a
  page.
- **Cancelling a print from a runout dialog asks first** - the guidance dialog cancelled on
  the first tap, while the print-status Stop button has always confirmed. One printer had two
  cancel affordances and only one of them asked, and the unconfirmed one sat in a dialog whose
  every other button is harmless. Both dialogs now raise the same confirmation with the same
  wording.
- **The clog meter's text slots each say one thing** - three of the six repeated each other:
  AFC wrote its buffer state into both top slots, buffer mode printed the distance twice, and
  the encoder scale was labelled by headroom running opposite the fill it annotates. Each slot
  now names the source, the reading, or an axis end, and severity is a check/alert/nozzle
  glyph rather than a repeated status word. The end labels render only in FlowGuard, which
  takes the linear track from 149px to 220px.
- **Portrait ships the print-status widget in its Detailed layout** - the Library layout clips
  its last action row at every measured portrait geometry. Detailed gained the Job Queue
  button, so the queue stays reachable from the home screen.
- **Icon sizes step down at the top three tiers** - the ladder jumped a glyph from 48 to 64px
  crossing into Large with no more cell to grow into, taking 53% of the box where every other
  rung sits at 40-50%. Ten widget and screen-size combinations were clipping because of it.
- **Clog-meter wording is translated** - Clog, Auto, Manual, buffer, TANGLE and CLOG are
  words and are translated; FlowGuard and AFC are product names and stay put. English filled,
  the other eight locales carry placeholders pending the next sweep.
- **Print preparation progress no longer needs PRINT_START instrumentation (#1234)** - the
  phase-tracking toggle in settings, the installer's `--with-phase-tracking` flag and the
  plugin's macro-rewriting service are removed. The screen infers the current phase from
  toolhead movement and temperature cues on the status stream, which works on any printer
  without editing its config. A PRINT_START that carries instrumentation keeps emitting
  `HELIX:PHASE` markers - harmless, and still parsed. Uninstalling the plugin (the app's
  Advanced row, or `install.sh --uninstall`/`--uninstall-auto`) now removes those marker
  blocks from PRINT_START itself, backing up every file it edits as
  `<file>.bak.<YYYYMMDD_HHMMSS>` first; the edit takes effect at the next Klipper restart.
  A printer instrumented by v0.99.111 or earlier may still carry a duplicated PRINT_START
  tail from that older writer; restoring its own `<stem>.bak.<epoch>` backup undoes that
  part, but discards every config change made after that backup was written. The app's
  Uninstall row now tells the three outcomes apart instead of always reporting success: a
  clean removal, a removal that left a config file needing a manual look, and an outright
  failure.

- **The 3D G-code preview works on slow boards** - on a Raspberry Pi 3-class screen a
  sharp 3D image of the whole model is ready in well under a second, and the preview
  stays responsive: while your finger is on it you see a lighter model that keeps the
  top, bottom and first layer and follows the drag smoothly, and the sharp image returns
  within about a second of letting go. During a print, each finished layer appears on its
  own instead of forcing a full redraw, and small files now get the 3D preview during the
  print on low-memory boards (where they previously always fell back to the 2D view).
- **The 3D preview sits on the same background as the 2D one** - the 3D view used to
  trade the shaded backdrop for a plain black one; both now share it, so flipping
  between them changes only the picture.

**Printer-specific**

- **K1: Creality Print keeps working** (#1468, #1637) - installing HelixScreen on a K1 no
  longer stops Creality's backend services, so Creality Print can still reach the printer.
  Only the stock screen is replaced.
- **Cool Down works on the base K2** - it no longer errors on a K2 whose chamber has a fan
  but no heater. Your own customised cooldown macro is never rewritten.
- **The chamber heater owns the chamber reading** (#1465) - a probe named "chamber" no longer
  hides the U1's cavity sensor. A sensor you assign by hand still wins.
- **Snapmaker U1: a print with an unset spool is stopped before it starts** - a tagless spool
  with no material set is caught up front, instead of the printer heating and homing and
  then reporting a runout minutes later.
- **AD5M: no startup music** - playing it ties up the AD5M's single core, and it has killed
  prints. The AD5X keeps its music.
- **Forge-X: installs stay put** - HelixScreen installs outside the folder Forge-X cleans
  (either of its clean actions deleted the install), and an install that replaces an older
  one removes it, which could hold 81MB and put two screens on the display at boot. A
  Forge-X install refuses `--auto-update`, which would have wiped your preserved settings.
- **QIDI: the stock screen stays off** (#1533) - it is disabled before it is stopped and kept
  off at every boot, so it no longer comes back after a restart. Installing sets aside only
  the screen software that is actually present, and restores only that.

**Installing and diagnostics**

- **Uninstalling HelixScreen also removes its Moonraker plugin**, so Moonraker stops logging
  an error for it on every boot.
- **Self-built and forked builds do not send diagnostics** (#1410) - debug bundles and crash
  reports upload only from official release builds by default. `HELIX_DIAGNOSTIC_UPLOADS=1`
  turns them on.

### Fixed

- **Printers that require a Moonraker login connect** - with `force_logins` enabled,
  Moonraker refused every request from the screen. HelixScreen now sends the printer's
  `moonraker_api_key` setting to that printer's Moonraker, over the WebSocket and every HTTP
  request, and to nothing else.
- **Print Status shows the chamber temperature with the chamber heater off** - the chamber
  chip appeared only while the chamber heated or was above 50°C, so a QIDI Q2 printing PLA,
  or a printer with only a chamber sensor, never showed it. It now shows whenever the
  printer has a chamber.
- **The layer count no longer jumps on every travel move** - on printers whose slicer does
  not report layers, the current layer is worked out from the nozzle height, and each Z-hop
  bumped it up a few layers and back. A new layer now shows once the nozzle has stayed at
  that height for a moment, so the count steps forward only when the print really moves up.
- **K2 pre-print steps** - the Creality K2's preparing checklist no longer marks Purge done
  while the nozzle is being cleaned. The nozzle clean parks over the purge chute and pushes a
  little filament to check flow, which read as the purge; the checklist now keeps it under
  Cleaning nozzle and shows Purge once, after the bed mesh, when the real purge runs.
- **Closing the printer type warning keeps your type** (#1881) - tapping X on the "printer
  type mismatch" warning now counts as keeping the saved type, like Review > Keep current,
  so it stops coming back on every restart.
- **The QIDI Box shows its own logo** (#1514) - instead of the generic filament system chip.
- **HelixPrint storage report loads again** (#1885) - the plugin's storage module failed to
  load on Moonraker because it was loaded before the photo module it reads from.
- **Storage warnings now translate** - when a storage cleanup folder was configured unsafely,
  the plugin overview showed the Moonraker plugin's raw English sentence. Those reasons are
  reason codes now, translated like every other plugin message (#1867).
- **Chart axis labels no longer clip** - on the Input Shaper results graph and the belt tension
  chart, the vertical percent labels and the "100 Hz" style labels at the chart edges were cut
  off on screens where the text is large. The space for the labels now follows the text size,
  and the labels stay inside the chart.

**Performance**

- **Faster drawing on every board** - the screen draws directly instead of through a separate
  render thread, which measured cheaper on every shipping board. The K1 family gains an image
  cache that cuts home screen drawing by about 14%, the home screen loads its printer picture
  at the exact size first instead of rescaling on every visit, and the theme skips rebuilding
  when nothing changed.
- **Raspberry Pi: GPU presentation where the Pi has a hardware renderer** (#1580) - the Pi
  package includes a build that hands finished frames to the GPU. The launcher uses it only
  when it finds a real hardware renderer, and falls back to the standard display path if it
  is declined or crashes.
- **A large G-code preview could freeze a Raspberry Pi 3 or older** - the 3D preview now caps
  its detail by what the GPU can draw. On a Pi 0 to 3 a file too big for that gets a 3D still
  drawn in layer bands, and shows as 2D only when the bands would be too coarse to read.
- **Tool remapping streams the file** instead of holding the whole G-code file in memory.
- **Turning animations off stops all of them**, including screen transitions and the heater
  icon pulse.

**Crashes and stability**

- **The screen crashed and restarted during print start on the K2** - matching the printer's
  start-sequence messages could exhaust the small stack the K2 gives each thread, and
  HelixScreen died with no crash report. The K1, AD5X and Creator 5 Pro builds share the same
  limit. The matching no longer recurses, every
  thread gets a larger stack on these printers, and a crash from the main thread running out
  of stack now leaves a report.
- **One Klipper fault, one dialog** - a shutdown that names its reason no longer stacks a
  Printer Error alert under the recovery dialog. While the recovery dialog is up, a further
  fault shows as a notification and in the notification history instead of another dialog.
- **Unplugging a touchscreen or mouse** could crash the app or leave input dead.
- **A bad byte in a Wi-Fi name, printer name or file name** (#1493) could stop the app or
  silently lose a whole settings save.
- **Small-memory boards** (#724) - a background thread that failed to start could take the
  whole app down.
- **AD5M Pro crash loop** - the watchdog could retry a crashing app forever with the screen
  dark. It now gives up and hands over to the system.
- A whole class of crash when leaving a screen, where a screen reacted to a value that had
  already been freed, is closed off across the app.
- **Opening a file's details could crash the app on some boards** (#1742) - on a screen
  drawn through the GPU (seen on an H616 board with Mesa 25), the 3D G-code preview loaded
  the graphics driver a second time and crashed. It now uses the display's own graphics
  connection, and a driver crash while the preview starts is remembered, so the next start
  shows the 2D preview instead of crashing on every open.
- **A read-only install could abort at exit, or at startup after a crash** - failing to
  remove the app's crash-restart counter is now logged and passed over instead of stopping
  the app.

**Network**

- **A stuck WiFi service froze the screen** - when the printer's WiFi service stopped
  answering, every scan, status check or tap on a network held the whole screen for up to
  15 seconds. WiFi is now handled in the background, so the screen stays responsive. After a
  few slow answers HelixScreen shows "WiFi isn't responding", keeps the last network list and
  status on screen, stops piling on requests, and checks again after about 15 seconds, then
  less often up to every 2 minutes; the message clears on its own when the service answers.
  Connecting, disconnecting, forgetting a network and the WiFi switch each show that message
  once instead of a failure of their own.
- **A printer whose address changed was retried at the old one forever** - the Android app
  hanging mid-print until a restart. The printer's name is looked up again before every
  reconnect.
- **Joining Wi-Fi while Ethernet is connected** (#1542) says so immediately, instead of timing
  out after 45 seconds.

- **A Klipper that failed to start looked shut down forever** - Moonraker never says when
  Klipper's startup fails, so after a restart the screen kept showing the old shutdown. It now
  asks until Klipper is ready or reports its startup error, and the recovery dialog shows
  Klipper's own message. A Firmware Restart from the recovery dialog is checked the same way,
  and where HelixScreen installed a recovery script for the platform (such as the Creality K2
  and the Snapmaker U1) it runs once if the restart does not bring Klipper back, with a time
  limit so it can never hang.

**Android app**

- **Settings were never backed up** - the app had nowhere to keep its rolling settings
  backups, and Android's own backup tried to save the whole app, far over its limit, so it
  saved nothing. The app now keeps its backups in its own storage, and Android's backup takes
  just your settings, so a phone restored from a backup gets them back.
- **An update could overwrite your own files** - installing a new APK rewrote files you had
  changed, such as a printer database override. An update now only adds files that are
  missing and leaves yours alone.
- **Telemetry never sent from Android** - with telemetry turned on, nothing left the device.
  It now sends, the same as on every other platform. Telemetry is still off unless you turn
  it on.
- **The app took over 200 MB more storage than it needed** - test prints for the simulated
  printer and font source files came along with every install. They are left out of the APK,
  so the download is smaller, and an update removes the copies an older version unpacked.

**Filament systems**

- **Filament stuck in the toolhead with no slot claiming it could not be unloaded** (#1324) - on
  the AD5X and the other systems that can tell, the filament sidebar's and Filament panel's
  Unload now pulls it out of the active head, and a slot's own Unload greys
  out in that state, since the slot it names may not be the one holding the filament. On an
  AD5X with the standalone IFS module, which has to be told a lane, Unload uses the lane it
  remembers loading when that lane still holds filament, and otherwise asks "Which lane is in
  the toolhead?" before heating.
- **AD5X IFS: a failed load or unload left the screen busy for 90 seconds** - when the IFS
  macro fails (a filament stalled in the IFS, say), the operation now stops at once with the
  firmware's own message.
- **Loading showed the nozzle temperature twice** - during the filament screen's own
  preheat, the status line names the action ("Heating nozzle") and the step list carries the
  reading.
- **The Spoolman QR scanner found no camera on most printer builds** (#1788) - on the K1
  series, Ender 3 V3 KE, AD5M, AD5X, Centauri Carbon, K2 and Snapmaker U1 it always said "No
  camera", even with a webcam set up in Moonraker. It now uses the printer's webcam, and on a
  stock K1 it also finds the camera the printer serves without telling Moonraker.
- **A Happy Hare fault often showed no recovery popup (#1323)** - a fault during a print, or a
  load or home that failed outside one, now always opens the popup with Happy Hare's own reason,
  and Happy Hare's own error notice closes behind it. Outside a print it offers no Resume and
  leads with Recover, which now lets Happy Hare detect the filament position instead of sending a
  state it did not recognise. The slot error marks follow Happy Hare's real pause, so resuming or
  cancelling clears them.
- **Turning Happy Hare's MMU motors on homed the MMU** - the Motors toggle now only powers the
  motors.
- **Filament details you entered on a Happy Hare lane were gone at the next launch** - brand,
  spool name, weights, colour name and the catalog product you picked were kept on screen for
  the session and never written down, on every Happy Hare printer. Clearing a lane had the
  mirror-image problem: the lane emptied on screen and the old details came back on the next
  start. Both stick now.
- **The old spool's brand stayed on a lane** (#1672) - when Spoolman dropped a spool, or
  another tool (Mainsail, a macro, the MMU's own screen) swapped one, the outgoing spool's
  details lingered. Seen on AD5X IFS and Happy Hare.
- **Snapmaker U1: Unload could be unavailable, or the filament system stuck on Unloading** - a
  head fed to the nozzle by purging can now be unloaded, picking a tool no longer reads as
  loaded, an unload or preload that settles straight into its resting state ends properly, and
  a leftover unload state after a restart no longer opens a runout grace window or ends a load
  running on another lane.
- **Creality CFS** (#1623, #1625, #1512) - restoring the slot mapping at startup refused every
  slot on a box whose size was not yet known, and threw away the record a reattached box
  needed. Clearing a slot left a dangling Spoolman link, and bypass after an unload could stay
  armed forever.
- **Creality CFS: a tag's raw record showed as the brand** - a tagged spool whose material
  code HelixScreen doesn't know showed a 40-character string as its brand. Creality spools now
  read as Creality, and other tags show no brand.
- **AD5X IFS** (#1631, #1654, #1626) - your own colour edit was recorded as if the printer had
  reported it, colours set from the stock screen were misread, and the runout warning never
  lit: the filament system's snapshot dropped the runout flag the backend raised, along with
  any other field the backend did not copy by name.
- **No ACE Pro slot ever showed as loaded on the Kobra S1 fork (#1069)** - that driver
  ("ACEPRO") states the loaded tool only through its manager object's `current_index`, a field
  the backend never read, so slots, colours and materials displayed while the loaded one never
  did. The index is read on both status paths now, with -1 meaning nothing loaded; the fork's
  `material` and `dryer_status` key spellings are accepted too, and a status frame carrying
  both the manager and a unit no longer drops the unit's update. Confirmed from a user's live
  captures and pinned by tests built from them; not yet verified on the hardware itself.
- **An ACE load the driver declined** (#1676) still marked the slot as loaded.
- **ACE stuck on Unloading** (#1720) - an unload that never reached the printer left the ACE
  marked busy until restart.
- **AFC in toolchanger mode** draws its real toolheads (a Box Turtle as four heads, not one
  nozzle behind a hub).
- **Reprint from Print History could crash** (#1815) - pressing Reprint on a job's details
  read the job's filename after the history list had already let go of it, which crashed
  some printers and silently picked the wrong text on others. It now keeps its own copy.
- **Selecting a parked hub-fed head swapped its filament** (#1811) - where a hub feeds
  several lanes into one toolhead, tapping that head in the Tool Switcher, or preheating it,
  picked the head's lowest lane instead of the one loaded in it. It now keeps the loaded lane.
- **AFC re-announced a resolved error at every start** (#1589) - a message AFC had already
  cleared popped up as new each time HelixScreen connected.
- **Tool changers: a tool change is no longer reported as a dock fault** - while a tool
  travels between dock and head, its sensors read the same as a fault, and the screen raised
  an error on the first moment of an ordinary swap. Real dock faults still show, with one
  translated wording.
- **A tool changer with nothing mounted showed T0 as active** - when the printer reported that
  no head was on the carriage, the active-tool highlight fell back to the first head anyway,
  so the screen claimed head 1 was selected while the carriage was empty. No head is shown as
  active until one is actually mounted.
- **A second filament system's slots showed the first system's materials** - on a printer with
  two filament systems, each slot now shows its own system's material and tool badge.
- **The heat-first warning judged the wrong slot** - Load and Unload now check the material of
  the slot you selected, and Purge, Extrude and Retract the loaded slot's, instead of the active
  slot's. On systems that load several heads at once, the sidebar buttons read **Load...** and
  **Unload...** because they open a picker.
- **The MMU's selector, buffer and bypass boxes ignored taps right after the screen opened** -
  a tap on the filament path now lands on the box you see as soon as the panel has slid in.
- **Device operation messages said "AFC" on every filament system** - Home, Recover and Abort
  toasted "Homing AFC system..." and the like whatever the system; they now read "Homing...",
  "Recovering..." and "Aborting...".
- **The Change Filament spool picker could not tell two spools of one vendor apart** - two
  PETG spools from one brand read identically; each row now carries Spoolman's filament name.
- **The clog threshold became unreadable exactly when it mattered** - the danger shading and a
  warning fill are both drawn in the danger colour, so a reading past the threshold merged the
  two into one red block. The threshold is its own rule now, drawn after the fill.
- **The AMS loaded card clipped the filament name** - "Polymaker" became "Polymak" because the
  content-sized meter column grew to fit longer source strings. The reading moved out of the
  36px arc that was drawing it clipped, and the material name wraps instead of marquee-ing
  forever.

- **AFC toolhead sensors showed up as runout switches** - the toolhead sensors of each AFC
  extruder were listed in sensor settings and the setup wizard as standalone runout sensors.
  On AFC builds where the toolhead sensor is a virtual switch, turning it off there would
  have told AFC the tool was unloaded. They are now left to AFC, like its other sensors.
- **Filament systems with more than 16 lanes greyed out the extra lanes** - a large AFC or
  Happy Hare setup now shows every lane up to 64, and a lane past that still counts for the
  loaded spool and tool tracking.
- **The load step list skipped its first steps after an outside operation** - when a tool
  change started from a macro or the console ended, the next load could start with its
  heating and early steps already passed, or keep showing the last operation's "Load
  complete". A tool change started from a macro also no longer jumps back to its first step
  when the step list appears.
- **Long printer prompts pushed their buttons off the screen** - a prompt with many buttons,
  such as AFC's calibration asking which of six units to calibrate, now scrolls its text and
  buttons while the bottom row (Cancel, Back) stays on screen. Five or more buttons line up
  in a grid of two or three columns instead of one long wrapped list. The AFC fault diagram
  in a prompt no longer showed every stop point at once after switching printers, and names
  the fault in one line (Stopped, or Jammed at the toolhead).

- **AD5M: the Spoolman spool editor showed a Print Label button that did nothing** - the
  AD5M build has no label printing, and the button is hidden there now.

**Printing and preview**

- **The file's material named a tool the print never uses** - the material in a file's
  details came from the first filament the slicer listed, even when the print only uses a
  later one, so a file printing only ABS-GF read "ABS". It now lists the materials of the
  tools the print actually uses, such as "PLA, PETG".
- **The prep time estimate on print details** - the "~6 min prep time" line counts heating
  from the current temperatures up to the file's own first-layer temperatures, and matches
  the countdown shown once the print starts. It learns from how long your printer actually
  took, with separate timings for a cold bed and a warm one. Turning a pre-print option off
  takes its time off the estimate; turning one on adds time only when your recent prints
  skipped that step. The line waits for the file's details to load, and is not shown for
  files on a USB stick.
- **A heater wait names the heater it waits on** - during print preparation the label reads
  the waiting command (M109, M190, M191 or TEMPERATURE_WAIT) instead of guessing from which
  heater is short, so a nozzle settling at its target, a cool-down before probing, or the
  tail of a bed wait no longer shows the wrong heater.
- **Creality error codes are explained on every printer** (#1513) - pre-heat, MCU link and
  out-of-range errors showed as raw firmware codes on printers without a CFS.
- **Print preparation shows what the printer is waiting for** - during a long heat-up after
  the firmware's last announced step (such as the Snapmaker U1's plate check), the screen
  names the heater still short of its target, the bed, the nozzle or the chamber, and the
  nozzle keeps its label until it is within 2 degrees. If Klipper shuts down during
  preparation, the progress display stops instead of counting on for up to half an hour.
- **FlashForge AD5M on Forge-X 1.4: print preparation finished minutes early** - the
  screen said the print was starting as soon as the nozzle clean ended, skipping the whole
  bed mesh, the reheat and the priming line, and the time estimate was far off. Preparation
  now follows the mod's own step reporting through to the first layer: heating, the nozzle
  clean and its cool-down for probing, the bed mesh with its point count, the reheat and the
  priming line.
- **Print preparation time estimates no longer balloon** - on every printer, the estimate
  could jump to two or three times the real preparation once the bed reached its last slow
  degrees. It now stays close to how long preparation really takes.
- **A timed-out print request could start a second print.**
- **A start macro that heats before replying** (#1451) no longer shows a timeout dialog over a
  print that started fine.
- **Starting a job from the Job Queue skipped the normal print checks** - a tap removed the
  job from the queue and started the file directly, bypassing the pre-print options, the
  filament mapping and every start gate, including the refusal of a file containing a
  command the printer would turn into an emergency stop, and it did nothing without a notice
  whenever a print was still preparing or running. A queued job now goes through the same
  start pipeline as any other print, and a busy printer says so.
- **The current layer could drop back at print start** - a status report lagging behind the
  printer's own layer count lowered the layer shown; during a print it now only climbs.
- **Preparing a large print file no longer dims its preview** - the loading note sat
  centered over a dimmed picture, dark on a dark model and under whatever toast
  appeared; it is now a small pill in the preview's corner that keeps the percentage
  and leaves the picture alone.
- **G-code with signed coordinates** like `G1 X+10.5` (#1658) previews completely.
- **A preview from the previous print** could replace the running print's preview, layer
  count and pause ticks.
- **A file moved out of the G-code folder** (#1575) stayed listed until you left the screen.
- **The budget preview on small boards** (#1555) keeps its tool colours and framing.
- **A layer of a large file that failed to read** drew as a permanent gap in the 2D preview.
  It is retried, and skipped with a log warning only if it keeps failing.
- **.gcode.3mf files lost their thumbnail and layer height in the file details** (#1713) - and
  print history showed untranslated job states and missing thumbnails for files in subfolders.
- **Deleting a print history record asks first** (#1373), and **View Timelapse** opens the
  timelapse browser and plays videos stored on the printer.
- **The unread history badge** (#1525) was cleared by a history screen you were not looking at.
- **K2 Plus: saving config flashed "Printer Shutdown"** during the normal restart.
- **Two speed and flow code paths were unreachable**, and the overrides now go through one
  clamp rather than three copies of it.
- **Sliced for a Different Setup named a toolhead the print never uses** - a multi-tool
  file lists a nozzle for every toolhead, so a different nozzle on an unused one raised the
  warning. The fit check now judges only the toolheads the file prints with.

**Calibration**

- **Input shaper progress** assumed a fixed 5-100 Hz sweep, so on printers configured to sweep
  higher the bar filled with a quarter of the test left and "Analyzing" sat for the last half
  minute. It follows the real range now.
- **Dismissing the input shaper memory warning** made the next single-axis run carry on into
  an unrequested Y sweep.
- **MPC calibration outliving its timeout** (#1544) was reported as failed with Retry offered
  over the still-running calibration.
- **Centauri Carbon** (#1529) - screws tilt, probe accuracy and Z-offset no longer probe on an
  untared load cell, and the tare uses the command COSMOS actually defines.
- **Bed screw adjustment said Tighten where it meant Loosen** - the word followed Klipper's
  clockwise/counterclockwise, which on a spring-mounted bed is the opposite. It now comes
  from each corner's probed height. The AD5M and AD5X no longer reverse the arrows, which
  now always match Klipper's console (#1860).
- **Bed screw re-probes measured against a stale home** - turning a screw moves the bed under
  a Z that still reads homed, so screws tilt now homes before every probe.
- **Z-offset kept showing a pending delta after a successful save**, could not tell a
  clamped-to-limit adjustment from a failed one, and would clear a valid stored step size on
  an out-of-range write.

**Home screen**

- **Tapping the lights quickly said the printer was busy** - each command keeps the printer
  briefly busy, so a few quick taps on an idle printer brought up "Printer is busy - your
  change will run when it's ready". The message now appears only when something other than
  your own commands is keeping the printer busy, such as a bed mesh started from elsewhere.
- **The printer picture** showed a generic silhouette until restart when detection finished
  late (#1552), and on the K1 family drew blank for up to a second on first show.
- **Widgets did not adapt to the panel they were on** - the size bands were flat pixel values
  calibrated on a small screen, so a one-cell widget on a 1080p panel held 32px type in a
  182px box and clipped: fan names cut off, one glyph per line in the active spool tile. The
  temperature graph measured nothing at all, wrapping "300" onto two lines and overlapping
  its time labels. Verified by rendering nine geometries from 480x272 to 1920x1080.
- **The bypass tile works** - it could be added but never appeared, and it showed as engaged
  whenever the printer did not report bypass at all.
- **The Detailed print-status card blanked for the rest of the session** if you removed the last
  print-status widget and added one back.
- **Narrow widgets could only be resized, never dragged** - the resize hit band was a flat 18px
  per edge, so on a widget under 36px wide the two bands overlapped and every pixel reported
  an edge.
- **Dropping a widget on an occupied spot moved the selection** (#1638) - the widget under your
  finger was selected instead of the one you dragged, which now stays selected.
- **Tapping Done on a later home page slid back to the first page** (#1638) - leaving edit mode
  with a second or later page showing scrolled the home screen back to its first page. The page
  you were on stays on screen, and pages change only when you swipe or tap a page arrow.
- **A one-page home kept swiping after leaving edit mode** (#1638) - after tapping Done on a home
  with a single page, a sideways swipe slid onto an empty page with a "+" on it. A single page
  does not swipe now, in edit mode or out of it.
- **Fan and light controls could show frozen dials** - after the fan or light controls had been
  opened from two different places (the home widget and the Controls panel, say), going back to
  the first could bring up a stale copy whose dials no longer moved. Every entry point now opens
  the same live screen.
- **The widget catalog leaked its whole tree on every open**, and a malformed layout file left
  edit mode believing the catalog was still open forever, with a stranded backdrop over the
  panel.
- **Long widget names ran straight through the size badge** on a 480px panel.
- **The notification count is readable on small tiles** - the count sat in a fixed dot
  that a single digit overflowed, reading as an exclamation mark; it now scales with the
  bell it sits on.
- **Power and macro tile names stay on one line** - a long name wrapped to two lines
  instead of shortening with dots, and a power tile the printer has never reported drew
  a crossed-out icon over the word Configure; it keeps its own icon until it is set up.
- **A cold nozzle read as one word** - the gap between the temperature and its "off"
  target was tighter than the space inside a word, so the row showed "47.0 off".
- **The Detailed idle actions squeezed to 77px on a portrait card** with their captions
  overrunning the button border. The action row now spans the card and stacks when the buttons
  do not fit side by side.
- **The firmware restart badge overran its glyph box** on six screen sizes, and the job queue
  widget could be shrunk below the size its own summary line fits.
- **Long names show both ends when they are not scrolling** (#1441) - with animations off, a
  file name too long for its space shows its start and end with an ellipsis between them,
  instead of being cut off at the edge, so two jobs that differ only in their suffix no longer
  look identical.
- **A toast closing interrupted whatever your finger was doing** - when a notification timed out
  or was dismissed, a drag, a slider or a scroll anywhere else on screen was cancelled mid-way.
  Only a press on the toast itself is cancelled now.

**Display and touch**

- **Picking a theme froze the screen for about two seconds** - every pick in the theme
  explorer, and Apply, drew the theme twice. Each now draws it once.
- **Backing out of the theme explorer kept the previewed light or dark mode** - flipping Dark
  Mode while browsing and then leaving without Apply kept the flipped mode. Leaving now puts
  back the theme and mode you had.
- **The Nord theme looked out of date and lost your edits on update** - every install carried
  an old copy of Nord that hid the current one and was rewritten on each update. That copy is
  removed on the next start; a Nord you edited yourself is kept.
- **The touch that wakes the screen pressed whatever was under it** - from a dimmed screen
  the waking tap went straight through (a tap meant to wake it could press Back), and from
  sleep or a screensaver a slightly long touch could too. The waking touch now does nothing
  until the finger lifts, in every mode.
- **Rotated panels** (#1580) - touch no longer drifts from the picture on displays using
  hardware planes.
- **Scroll Guard had no effect** - with the post-scroll click guard turned on, a tap right after
  a scroll still went through. It works again on the printer's touchscreen, and needs a restart
  after changing it.
- **The number keypad was cramped on 480x320 and 480x272 screens** - it is wider there, its keys
  fill the height, heater keypads are titled with the short heater name, and header titles
  shorten with dots instead of wrapping.
- **The number pad puts 1 2 3 on top, like a phone** - backspace moves to the bottom left, and
  on whole-number fields such as temperatures a confirm key sits bottom right, where the
  decimal point would be. The unit (°C, mm) now sits inside the value field, which spans the
  keys. Prefer 7 8 9 on top? Settings > Touch & Input > Number Pad Layout > Calculator.

- **Colour swatches were small targets** - every colour you can pick (the filament colour
  grids, the colour picker, the LED colour rows, the theme editor) now has a full fingertip-sized
  touch area, and neighbouring swatches no longer steal each other's taps. The LED preset row
  wraps to a second line in portrait instead of running off the edge.

**Printer identification**

- **Fans in the Printer Manager** - the Fans capability chip never appeared on any printer.
  It now shows when the printer has controllable fans besides part cooling, with their count.
- **Extra heaters counted as tools** - a heater named like `extruder_mixing` could turn a
  three-tool machine into a four-tool AD5X.
- **Creality CFS machines** (#1498) - plain and CFS K1 variants, and the K2 Plus and Pro, are
  told apart reliably.
- **Delta printers** (#1607) no longer save the wrong vendor on a shared hostname, and the
  Kobra 2 and Qidi Max 4 identify exactly.

**Snapmaker U1**

- **Faults reach the screen** - standing faults such as power loss were not shown at all, and
  coded faults showed raw console text. A spaghetti pause is no longer called a dirty bed.
- **Per-filament temperatures and per-head end unload** did not work against the real
  firmware.
- **A refused resume explains itself** - when a head has no filament type set, Resume shows
  "Filament not set" with what to do, instead of a raw "Failed to resume" toast.
- **Print Tune no longer says the Z-offset is auto-saved** - on the U1 a baby step lasts
  until the next tool change or the end of the print, and Print Tune now says so.
- **Pre-print steps read right** - the Z touch after each tool pick shows as Probing Z
  instead of Bed mesh, and the firmware's bed soak and mesh steps get their own labels.
- **A material set on the screen reaches the printer** - a head's material set in HelixScreen
  is written to the U1's own filament record, and before resuming a pause (other than a
  runout) a used head with a blank record is filled in from the material HelixScreen knows,
  so Resume is no longer refused with "not edit filament".
- **The fit check knows which nozzle each head really has** - it read the 0.4 mm the
  printer's config lists for every head, so a correct job for a 0.6 mm head was warned about
  and a 0.4 mm job sent to a 0.2 mm head passed. It now reads the size and type the U1
  reports for each head. A file sliced for the other nozzle type (High Flow or standard)
  gets a warning with **Start Anyway**.

**Setup and installing**

- **The step counter jumped mid-wizard** (#1550), from "Step 2 of 6" to "Step 3 of 3".
- **The setup wizard's connection step came up with blank address fields** - the IP and port
  boxes on the Moonraker connection step opened empty instead of carrying the default
  (127.0.0.1 and port 7125), so the whole address had to be typed by hand. Both fields are
  seeded again.
- **Uninstall** now removes HelixScreen's leftover folder instead of only emptying it.
- **Mainsail kept showing the old version after an update** (#1727) - the first start after an
  update asks Moonraker to refresh it.
- **K1 series and AD5X: switching back to Stable could not install 1.0** (#1789) - the
  update offered "Switch to v1.0.4" and then failed with "No update available", because 1.0
  releases name their download after the printer rather than the shared build. Both names
  are now looked for.
- **Installing the HelixPrint plugin from the screen failed on the K1 series** (#1790) - the
  install could not find Moonraker where Creality puts it. It now finds the running Moonraker
  wherever it lives, and the reason for a failed install is written to the log.
- **Creator 5 and Creator 5 Pro: a channel with no build for your printer says so** - switching
  to Stable, whose newest release has no build for these printers, used to offer the AD5X
  build. The update row now reads "v1.0.5 has no build for the FlashForge Creator 5 Pro, so
  it can't be installed here. Pick a channel with a newer release." and never offers Install.
  On every printer the same message replaces the old "No update available" failure for a
  release that has no download for it.

- **`HELIX_SCREEN_SIZE` left no trace in the log** - the line saying which size was applied,
  and the warning for a value it could not read, were written before logging started, so the
  `journalctl` check in Troubleshooting found nothing. Both are logged now.

**Translations**

- **Filament system errors are translated** for the first time, across AFC, Happy Hare, AD5X
  IFS, CFS, ACE and QIDI, and the CFS load failure no longer overflows or cuts a character in
  half.
- **Gate and lane** (#957) - 26 phrases used the word for "slot" where English says gate or
  lane.
- **Japanese and Chinese** (#1620) - missing characters showed as boxes; the material
  temperature table (#1263) fits small screens and is translated; the motion, batch filament
  and U1 fault text is translated in every language.

### Internal

- The mock Voron personas model a real centre-origin bed (-175..175 on the 2.4, -125..125 on
  the Trident) through every path the mock serves, so the demo G-code files sliced for
  centre-origin printers pass the print-start fit check: 14 of 18 fit the default persona
  instead of 3 (#1868).
- The pluck-based belt tuner libraries (#1303, #1231: pluck detection, harmonic pitch
  estimation, the five-pluck median) stay in the tree with no UI. They have never measured a
  real belt and their thresholds come from one Voron's captures; `BELT_TUNER.md` § Validation
  status lists the hardware runs needed before any screen uses them.
- helix-xml warns on a style attribute written with `:` for its state selector
  (`style_bg_opa:checked`, which applies nothing) and on a negated const reference (`-#space_md`,
  which it drops), and the unknown-attribute check now covers misspelled `style_*` names on
  built-in widgets. `make lint-xml` fails on both spellings. The input shaper's recommended row
  gets the highlight its markup always asked for; the dead chip, preset and Save attributes are
  gone, and the home AMS label and notification badge get the offsets they were written with.

- K1, AD5X, Creator 5 Pro and K2 builds (musl) link with `-Wl,-z,stack-size=1048576`, because
  musl gives each thread 128 KiB unless the binary asks for more. The crash handler runs on a
  64 KiB alternate stack for the installing thread; other threads have none, so an overflow
  there still leaves no crash file. A lint gate keeps `std::regex`, whose matcher recurses per
  input character, out of app code (`// STD_REGEX_OK: <reason>` opts out).
- A cached `lv_obj_t*` member is a `helix::ui::WidgetRef`, which clears itself on the widget's
  delete, instead of a per-site `LV_EVENT_DELETE` hook (#1298). A ratchet counts the raw
  members that remain.
- The four `SlotRegistry` backends build their snapshot from their own system info and
  overwrite only the slot-owned fields, instead of copying the rest by name (#1626).
- `helix-tests` links through a response file: the inline object list passed Linux's 128 KiB
  per-argument limit and the ASAN link failed with "Argument list too long".
- Debug bundles, crash reports and telemetry name the printer board (for example
  `creator5-pro`) next to the platform key, so one `mips` build is no longer one lump in the
  numbers. The K1's panel size now comes from its own table row, so its startup log names the
  known panel instead of the 160 DPI fallback; sizing does not change. One board table
  (`include/board_identity.h`) answers which printer the build is on, and a lint ratchet
  refuses new vendor branches in generic code (`scripts/check_vendor_branches.py`).

- The pre-v22 grid is reconstructed rather than recorded. `legacy_grid_cols()` is a frozen
  copy of the old column table, and the old row count is read back off the saved layout, which
  is what makes converting a layout possible at all without having stored a resolution.
  Boundaries are mapped once per axis and every widget rebuilt from them, so neighbours that
  were flush stay flush; mapping each widget's own edges opens sliver gaps between them.
- Three cross-test state leaks, found because four tests failed only in a full run and passed
  alone. Two set the process-global `ui_breakpoint` subject without restoring it, so every
  later test built a Micro-cell grid. Fixing that exposed the third and worse one: a test had
  registered a **stack** `lv_subject_t` under a name four production components bind, and
  LVGL's XML subject scope has no unregister, so the first test to build one of them
  afterwards dereferenced a dead stack frame. The suite went from four failures to none.
- The temperature graph allocated a timer per drawn frame and the filament path one per setter
  in a state update, both on the belief that `lv_async_call` deduplicates by callback. It does
  not. Both now coalesce onto one timer whose destructor cancels its own pending work.
- `StaticSubjectRegistry` appended a shutdown callback per registration, so a subject source
  torn down and re-created under the same name left its superseded callback to run at exit
  alongside the live one.
- The print-status thumbnail wrote synchronously inside an update batch, cascading a layout
  pass into a grid that might be mid-rebuild. The idle path already escaped; the active one
  had no guard.
- Filament load, unload, purge and cancel had grown to three copies of the same dispatch
  ladder across two owners; they now share one executor. Bed dimensions resolve through one
  fallback chain and one mm-to-pixel mapper instead of per-view copies. The clog meter's arc
  and bar now subscribe once and share one safe-state predicate, and dropping the fill-mode
  presentation removed about 280 lines reachable only through a flag nothing set any more.
- A worktree whose `libhv.a` predated another tree's patch application kept that archive
  forever, and the two objects disagreed about class layout, which read as a deadlock in test
  teardown. The build now depends on the patched sources rather than a per-tree stamp.
- The mock could not reach the clog meter at all: one healthy reading seeded in the
  constructor and never moved, FlowGuard unreachable entirely. Nine scenarios now walk
  healthy, warning, blocked, the three FlowGuard positions, buffer armed and counting down,
  and no-hardware. `SAVE_CONFIG` also returned failure from every gcode script call, which
  meant no test could drive a probe calibration through to a save.
- BTT K-Touch firmware: the theme, tool switcher, filament sidebar and slot menu values and
  the settings store moved from the ESP32's scarce internal RAM to PSRAM, keeping internal RAM
  under the ceiling the WiFi driver's receive buffers need.
- 306 test files were missing their copyright header.
- A content-fit sweep now measures every widget against its authored minimum on all eight
  shipping geometries, and the user guide, configuration reference and FAQ were corrected:
  the home layout upgrade promise, widget config units, a probe widget that does not exist,
  23 of 37 widget IDs and 9 of 11 config keys that were undocumented, and three pages routing
  users to a settings overlay that is gone.
- The mock printer can be a named printer type (`HELIX_MOCK_PRINTER=snapmaker_u1`), so a
  `--test` run renders that printer's real pre-print options and hardware instead of the
  generic persona. The Snapmaker U1 persona has four extruders and auto-detects from the
  U1's own objects, with no saved printer type.
- `helix-screen ctl click` on a checkable widget now flips its checked state and sends
  VALUE_CHANGED then CLICKED, the way a finger tap does, so it toggles an option tile.
