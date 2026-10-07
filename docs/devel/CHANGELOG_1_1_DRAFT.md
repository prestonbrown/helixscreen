# CHANGELOG draft - 1.1

Working draft of the release notes for 1.1. Lives here rather than in
`CHANGELOG.md` so the release tooling owns that file uncontested; at release,
this becomes the `## [1.1.0]` entry more or less verbatim. The heading has to be
the exact string in `VERSION.txt` at that point, because
`scripts/generate-whatsnew.sh` looks the section up by it and hard-fails when it
is absent.

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

**Home screen**

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

**Home screen widgets**

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
  on a circle instead. Moves are XY only, homing first when needed, and the whole tab is
  disabled while a print runs or is paused, or while the printer is not ready. The
  positions and Park grey out while the toolhead is moving, so a second tap cannot land
  mid-move. Park and Motors Off sit under the grid.
- **Park parks the toolhead** - it runs the printer's own parking macro when one is
  detected (`PARK`, `PARK_TOOLHEAD` or `TOOLHEAD_PARK`), otherwise lifts the nozzle 10mm
  and parks over the rear of the plate, never past it; unhomed axes are homed first. Point it at a different macro in
  Settings > Printing > Macro Buttons.
- **Tap a coordinate in the header to move there** - a number pad opens for that axis, and
  a value outside the printer's range is refused with the allowed range while nothing
  moves; an unhomed axis is homed first, then moved. A Target / Actual chip beside the
  coordinates names which position is shown and toggles it, lights up for Actual, and is
  remembered per printer.
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

**Tool changers and multi-toolhead printers**

- **IDEX and dual-extruder printers get the multi-tool screens** (#1350) - a printer is now
  recognised as multi-tool by counting its hot ends, so IDEX machines, dual extruders and
  tool changers that do not run klipper-toolchanger get the tool selector and filament
  screens instead of the single-extruder ones, and `T0`/`T1` tool switching works on them.
  Built from test configurations; not yet verified on these machines.
- **Tool Offsets screen with automatic calibration** (#1480) - per-tool X, Y and Z offsets in
  one place, and a one-tap calibration that runs your printer's own routine, follows it
  live, and saves every axis with a single restart. Where that routine calibrates every
  tool, the manual paper test is hidden. Beta-gated for now. (thanks @Monstrofil)
- **Per-tool Z offset from Print Tune** - Print Tune has a Global / T0..Tn selector, so each
  tool keeps its own first-layer height, and the save button appears whenever any tool's
  offset is unsaved and writes it permanently. It used to vanish after the next Klipper
  restart.
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
  at the spool. An ACE Pro with a fifth spool on its bypass switch gets a working bypass
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

**Printing**

- **Heater duty on every heater** (#1290) - the Controls and print status screens show how
  hard each heater is working as a percentage beside its status, so a nozzle running flat
  out and falling behind looks different from one holding temperature easily.
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
  its own icon: tap anywhere on the tile to switch it on or off, and the check mark in its
  corner shows which are on. Long option names wrap inside their tile instead of running
  under a switch.
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

**Calibration and tuning**

- **Pressure advance, measured by the printer** (#1452) - on printers that can measure it
  themselves, a Pressure Adv. button on the Controls panel runs the measurement for the tool
  you pick and shows the result, flagged if it's outside the usual range; a Pressure Advance
  row under Advanced > Calibration opens it too. The Snapmaker U1 applies and keeps the
  value; on the FlashForge Creator 5 Pro you copy it into the slicer. Takes a few minutes and
  heats and purges. (thanks @Monstrofil)
- **Belt tension is measured by plucking, not by a driven sweep (#1303, #1231)** - park the
  gantry, pluck each belt by hand, and the tool listens on Klipper's live accelerometer
  stream and reports the belt's fundamental, with a live waveform and spectrum. It reads the
  fundamental off the whole harmonic series rather than the tallest peak, which is what the
  old sweep got wrong: a belt whose 2nd harmonic dominates reported exactly one octave sharp.
  The number you act on is the median of five accepted plucks. The `TEST_RESONANCES` sweep,
  the strobe path and the never-measurable Z-belt path are deleted.

  > **⚠ RELEASE BLOCKER - do not ship 1.1 with this marked done.** This is green in CI and
  > has **never measured a real belt**. Its gate thresholds were measured against captures
  > from one Voron 2.4 on one evening, and the algorithm was then tuned against that same
  > set - circular, and not yet broken. It stays beta-gated and must not be promoted until
  > the hardware matrix in `BELT_TUNER.md` § Validation status has actually been run.
  > Delete this box only when that is done, not when the code looks finished.

- **Switch a Kalico heater between PID and MPC** (#1237) - the method selector on Kalico
  printers is out of beta. It migrates your config in both directions, and cancelling a
  running MPC calibration asks first, because cancelling means an emergency stop and a
  firmware restart.
- **Input shaper saves the shaper you picked** - the chips under the results graph are now
  one choice per axis, and Save writes that choice to your config. Save used to write
  Klipper's own recommendation regardless of what you selected.
- **Baby-step size is remembered across launches** - the Z-offset step you last chose is the
  one you get next time.

**Printers**

- **VzBot 330 and 235** (#1689), **FLSUN S1 and S1 Pro**, and the **base Creality K2**
  (#1606), each with artwork. The base K2 is now told apart from the K2 Plus and Pro, and its
  camera option no longer claims the Plus's AI model.
- **FlashForge Creator 5 and Creator 5 Pro are supported printers (#1714)** - both models of
  FlashForge's four-head tool changer are recognized and given their own settings, on either
  of the two firmwares that replace its stock software: Z-Mod and Reforge. Mounting and
  parking a head is driven from the screen. Reading each head's material and colour from the
  printer, and showing the mounted head at boot, both need Z-Mod's pending status update:
  until it ships, a Z-Mod machine reports a dock sensor error at boot until its first tool
  change, and colour or material edits stay on the screen instead of being saved to the
  printer. All of this is verified against a simulated printer, not yet on the hardware.
- **Snapmaker U1 firmware settings** - the pre-print toggles (bed mesh, input shaper,
  pressure advance, timelapse) read and set the firmware's stored values instead of always
  reading off. Tangle sensitivity, end-of-print unload per toolhead, and manual bed levelling
  with a PEI plate check are all on screen. Loading preheats to the firmware's own
  temperature for that filament. Firmware faults appear with translated wording.
- **Snapmaker U1 with the multiACE mod** (#1426) - keeps its filament screen instead of being
  mistaken for a plain ACE setup.
- **Power-loss recovery works on Qidi printers (#1716)** - on a Q2, Q1 Pro or Plus 4 running
  the stock firmware, the resume dialog now appears after a print was cut short by a power
  loss, offering the printer's own `RESUME_INTERRUPTED` flow or a clean discard.

**Display, sound and touch**

- **Bouncing Printer screensaver** (#1680) - the screensaver everyone already knows. Your
  printer drifts across a black screen and reflects off the edges, picking up a new tint on
  every wall. Land a true corner and it celebrates. The sprite is whatever printer this
  screen is attached to, auto-detected or picked by hand, so a Trident bounces a Trident. It
  costs a fraction of what Flying Toasters does; on AD5M-class hardware the corner
  celebration is a backdrop flash rather than confetti, to stay clear of the print loop.
  Select it under Settings > Display > Screensaver. (thanks @Tintef)
- **Fireworks screensaver** - shells rise from behind a line of hills under a starry night
  sky, trail sparks, and burst as peonies, chrysanthemums, willows or rings, each spark
  fading from white through its shell's colour to an ember. It adjusts how many sparks and
  bursts it draws to what the board can afford. Select it under Settings > Display >
  Screensaver.
- **All five screensavers on every board** - Flying Toasters, Starfield, 3D Pipes, Bouncing
  Printer and Fireworks now run on the AD5M and Centauri Carbon too, which had none, and on
  every other board with a 16-bit display. Each saver draws less on a slower board rather
  than stalling it, and the starfield redraws only what moved, so it costs a small fraction
  of what it did.
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

- **Find your printer by searching** (#1028) - first-run printer selection replaces the
  scroll through about 105 machines with a search box, a grid of vendors and a list per
  vendor.
- **Install HelixScreen's helper macros from Settings** (#1271) - a row under Advanced
  installs or updates the macro pack without running the installer. It backs up
  `printer.cfg` first, restarts Klipper right away when idle, offers the restart for after
  the print when one is running, and refuses to touch anything mid-print.
- **Hardware Health shows its status** - the Settings row carries the live result with a
  severity-coloured icon, and critical problems get their own alert icon.

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

### Changed

**Screens and settings**

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

**Network**

- **A printer whose address changed was retried at the old one forever** - the Android app
  hanging mid-print until a restart. The printer's name is looked up again before every
  reconnect.
- **Joining Wi-Fi while Ethernet is connected** (#1542) says so immediately, instead of timing
  out after 45 seconds.

**Filament systems**

- **Filament stuck in the toolhead with no slot claiming it could not be unloaded** (#1324) - on
  the AD5X and the other systems that can tell, the filament sidebar's and Filament panel's
  Unload now pulls it out of the active head, and a slot's own Unload greys
  out in that state, since the slot it names may not be the one holding the filament.
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

**Printing and preview**

- **Creality error codes are explained on every printer** (#1513) - pre-heat, MCU link and
  out-of-range errors showed as raw firmware codes on printers without a CFS.
- **Print preparation shows what the printer is waiting for** - during a long heat-up after
  the firmware's last announced step (such as the Snapmaker U1's plate check), the screen
  names the heater still short of its target, the bed, the nozzle or the chamber, and the
  nozzle keeps its label until it is within 2 degrees. If Klipper shuts down during
  preparation, the progress display stops instead of counting on for up to half an hour.
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
- **.gcode.3mf files lost their thumbnail and layer height in the file details** (#1713) - and
  print history showed untranslated job states and missing thumbnails for files in subfolders.
- **Deleting a print history record asks first** (#1373), and **View Timelapse** opens the
  timelapse browser and plays videos stored on the printer.
- **The unread history badge** (#1525) was cleared by a history screen you were not looking at.
- **K2 Plus: saving config flashed "Printer Shutdown"** during the normal restart.
- **Two speed and flow code paths were unreachable**, and the overrides now go through one
  clamp rather than three copies of it.

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
- **Z-offset kept showing a pending delta after a successful save**, could not tell a
  clamped-to-limit adjustment from a failed one, and would clear a valid stored step size on
  an out-of-range write.

**Home screen**

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

**Printer identification**

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

**Setup and installing**

- **The step counter jumped mid-wizard** (#1550), from "Step 2 of 6" to "Step 3 of 3".
- **The setup wizard's connection step came up with blank address fields** - the IP and port
  boxes on the Moonraker connection step opened empty instead of carrying the default
  (127.0.0.1 and port 7125), so the whole address had to be typed by hand. Both fields are
  seeded again.
- **Uninstall** now removes HelixScreen's leftover folder instead of only emptying it.
- **Mainsail kept showing the old version after an update** (#1727) - the first start after an
  update asks Moonraker to refresh it.

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
- 306 test files were missing their copyright header.
- A content-fit sweep now measures every widget against its authored minimum on all eight
  shipping geometries, and the user guide, configuration reference and FAQ were corrected:
  the home layout upgrade promise, widget config units, a probe widget that does not exist,
  23 of 37 widget IDs and 9 of 11 config keys that were undocumented, and three pages routing
  users to a settings overlay that is gone.
- The mock printer can be a named printer type (`HELIX_MOCK_PRINTER=snapmaker_u1`), so a
  `--test` run renders that printer's real pre-print options and hardware instead of the
  generic persona.
- `helix-screen ctl click` on a checkable widget now flips its checked state and sends
  VALUE_CHANGED then CLICKED, the way a finger tap does, so it toggles an option tile.
