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

**Keeping it current:** 1.1.0 ships from `release/1.1`, so that branch is what this file
tracks. Every entry of `CHANGELOG.md` from `1.1.0-beta.1` through `1.1.0-rc.4` is folded in,
checked against `release/1.1` at `7915d7fa5b`. An entry that also shipped in a 1.0.x release
is left out, since it is not new against 1.0; `git show origin/release/1.0:CHANGELOG.md`
lists those. The next fold starts at what lands on the release branch after rc.4:

```bash
git log --no-merges --oneline 7915d7fa5b..origin/release/1.1 --not origin/release/1.0
```

---

## [1.1.0] - UNRELEASED

**Upgrading from 1.0?**

- **Your print signal macros keep their names.** If your printer already has HelixScreen's
  helper macros, `HELIX_READY` and the `HELIX_PHASE_*` macros stay as they are, including when
  you update the macro pack from Settings, so your `PRINT_START` needs no changes. They also
  stay in your Fluidd or Mainsail macro list for now; you can hide them in Fluidd's macro
  settings. A later release will move existing printers to the hidden names.
- **Your home screen is converted, not reset.** The home screen moves to a square-cell
  grid, and your layout comes with it. Positions are converted onto the new grid in
  proportion: a widget that filled the left third of the screen still fills the left third,
  and two widgets that were touching stay touching. Sizes can shift slightly, because the
  new grid does not divide the screen the same way and each widget lands on the nearest size
  it is allowed to hold. A widget that genuinely will not fit is re-placed automatically,
  and only that widget. Which widgets you have, their per-widget settings, and your extra
  pages all survive. The conversion happens the first time the home screen is drawn after
  the update, so what you see on that first boot is what gets saved.
- **Going back to 1.0 costs you the home layout and your Material Temperatures changes.** A
  1.0 build saves home positions in its own grid's units, so when you return to 1.1 the home
  screen comes back rearranged, or, from some 1.0 builds, reset to its defaults. Arrange it
  again in edit mode. 1.1 keeps your Material Temperatures changes in user_filaments.json,
  and 1.0 only reads them from `settings.json`, so a 1.0 build does not see them; note them
  down first if you might go back. Everything else survives the round trip.
- **Lane details come from the printer.** HelixScreen keeps no lane details of its own any
  more: everything a lane shows is read from the printer, where 1.0 already wrote it. A detail
  that only ever lived on the screen, never sent to the printer, does not carry over; set it
  again. On AFC, AFC's own remember-spool setting decides what a lane keeps when its spool comes
  out.
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

- **Notification history without the bell** - with the Notifications widget off the home
  screen, notification history and Printer Events stay reachable: **Advanced > Notification
  History** appears whenever no bell is on any home page.
- **The home screen is one square-cell grid on every panel** (#1126, #1559) - both axes
  divide the panel by the same track size, so a cell is square everywhere and a rotated
  panel turns its grid exactly. Tracks are half cells, three times the placement resolution
  of 1.0's grid, so the tips widget shows in portrait and bed temperature is on the default
  layout everywhere.
- **Widgets can be sized and dropped at half-cell resolution** - drag and resize round to a
  per-widget step: one track for a widget whose content is continuous along that axis
  (charts, aspect-fit frames, wrapping text, scrolling strips, stacked readouts), a whole
  cell for the centered icon-and-label tiles, where an in-between size buys only whitespace
  and costs a fussier drag. Every shipped layout was re-authored against the new grid:
  default, portrait, landscape, the three printer preset seeds, plus a new ultrawide variant
  for the 1480x320 and 1920x440 panels that used to draw into the left fifth of the screen.
- **Edit mode shows the lattice a widget can snap to** - whole-cell boundaries are always
  drawn; the half-cell boundaries between them appear smaller and fainter only while a widget
  that supports them is selected, and only on the axes it supports. A visible dot is a legal
  drop target.
- **The default home layout fits your printer and screen** - an untouched default layout is
  built for your screen size from the widgets your printer can actually use, leaves clean
  gaps instead of filler, and keeps notifications in a corner. Add hardware later, such as a
  filament system or a chamber heater, and the default rearranges itself to include it. On
  tool changers such as the Creator 5 and Snapmaker U1, the default has the Tool Switcher,
  leaves out the nozzle and bed temperature tile that the switcher and the printer picture
  already cover, and keeps an open space beside the printer picture for widgets you add.
  Once you edit the home screen, the layout is yours and is left alone. The 1920x1080 and
  1080x2400 screens and the Creality Hi's 320x240 and 320x1480 screens each have their own.
- **A tour of what's new** - the first start after updating from 1.0 shows a short tour of
  the rebuilt home screen, the new Print Status screen and the Filament and filament system
  screens, with your own printer's picture. A new install gets the full tour, and Help &
  About replays it.
- **Drop a widget on another to swap them** (#1503) - in edit mode, dropping a widget on top
  of another trades their places, and the edit grid previews where the dragged widget will
  land.
- **Undo for a removed widget** - removing a widget in edit mode shows "Widget removed ·
  Undo" for a few seconds, and Undo puts it back where it was.
- **Edit mode swipes between pages, and widgets move across them** (#1638) - swiping flips
  pages in edit mode as it does outside it; it pauses while your finger is on the selected
  widget, while you drag or resize one, and while the widget catalog is open. Drag a widget over
  the page border, or hold it at the edge of the widget area, and the next page slides in with
  the widget still under your finger. Pressing a widget that is not
  selected only selects it, so a swipe that starts on a widget still flips the page: press the
  selected widget again, or hold a widget, to pick it up. The long press that enters edit mode
  only selects.
- **The widget catalog opens by category, with search** (#1016) - a long flat list had become
  one you hunt through, worst on 480px panels. It opens on five category rows and dives into
  one, with the same back button and slide the settings sub-pages use. A search box matches
  names and descriptions, and widgets this printer cannot use are listed in their own
  section with the reason. Widget names and descriptions are translated in every language. A
  widget with no free space on the page says how much room it needs instead of failing when
  you tap it.
- **UI Scale setting, with a layout per scale** (#1484) - Settings > Display gains a UI Scale
  choice: Automatic, which shows the scale it picked, or a fixed 100 to 200%. Each scale keeps
  its own home arrangement, so trying a different scale and coming back restores the layout
  you had, which 1.0 could not do. On very high-density displays such as phones, Automatic
  scales by the screen's real pixel density instead of drawing everything a third of its
  intended size.
- **1080p and wider panels get their own size** - a 1080p panel draws widgets sized to match
  its fonts and icons, instead of 720p-sized widgets with oversized icons and text running
  out of the box. The navigation bar widens to match.
- **Navigation buttons read by shape, not just color** - inactive buttons use outline icons
  and the active one is larger and filled, which survives washed-out panels and color-blind
  eyes. (thanks @just-trey)
- **Printer Manager lists everything HelixScreen detected** - the Capabilities section grows
  from 18 chips to over 40, grouped as Motion & Leveling, Thermal & Air, Filament & Toolhead,
  Printing, Electronics & Accessories and Add-ons. Chips carry details so you can confirm
  detection at a glance (Probe · Beacon, CoreXY, 350×350×340, Tool Changer · 4 Tools, 2 MCUs),
  capabilities forced on in settings say "set manually", and every chip is tappable: it opens
  the screen that uses it, or a short note on what it is. A Kalico printer's software row
  says Kalico.
- **A new screen starts with the printer's own home layout** - when a printer runs
  HelixScreen on its own display, other screens that connect to it copy that home layout the
  first time (your macro, LED, fan, sensor and camera picks included), and the Widget Catalog
  has a "Copy layout from printer" button to do it again.

**Home screen widgets**

- **The home screen's printer picture shows what the printer is doing** (#1397) - while the
  nozzle, bed or chamber heats, and until a heater that was turned off drops below 50°C, a
  temperature chip sits on the picture; the part fan shows its speed and the light shows
  when it is on. Tap a chip for that heater's graph, the fan controls or the light controls.
  On most printers with their own picture, each chip points at its part: beside the picture
  with a line to it on a wide tile, on top of it on a small one. A printer without positions
  shows the chips along the picture's edge until you tag it: Tag parts in the printer image
  picker asks for six taps on the picture (nozzle tip, part fan, both front corners of the
  bed, a spot inside the enclosure, the light), shows the chips where they will sit, and
  saves. That works for your own photo too, and re-tags a shipped picture you disagree with;
  Reset tags puts it back.
- **The printer picture shows an attached CFS** - a Creality K2 with a CFS connected shows
  the K2 with the CFS on top, switching on its own when the CFS reports in or drops out. A
  picture you picked yourself stays in place. Each K2 model, Pro or Plus, shows its own
  picture with the CFS.
- **Icon tiles resize** - the single-icon tiles (power, lock, shutdown, firmware restart,
  LED, network and the rest) scale with their tile instead of staying one fixed size, and
  can be resized for the first time. On most panels they take half-cell steps in width; on
  the smallest panels, and for the Power tile, they keep whole cells. None goes below one
  whole cell, where the icon, its caption and the clock face would clip. A widget refuses a
  size it cannot draw, and edit mode shows that size in red.
- **The filament sensor tile on the home screen is tappable** - a tap opens the tile's
  dialog, and Load, Unload, Purge, Resume and Cancel Print work from it as they do in the
  runout guidance dialog. Which sensor the tile watches is picked in edit mode.
- **The clog meter is a horizontal scale, two cells wide** (#1017) - one cell was too narrow
  for the arc, its value and its mode text. It is drawn as a scale with a label at each end:
  TANGLE/CLOG for FlowGuard's symmetrical range, SAFE/FAULT or a detection length for the
  linear ones, so a reading says which fault it is heading toward, not just how far. The arc
  stays where it fits, in the AMS sidebar and on the loaded-spool card. With nothing printing,
  its caption reads "Clog detection" rather than an alarm-like "Disarmed".
- **Tapping the FlowGuard tile shows the reading, not just the mode** - the dialog carries the
  same bar as the tile, with the value, the threshold and the peak, beside Automatic, Manual
  or Off.
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
  following the percent or mm/s choice the print screen uses. At its largest size it gains -
  and + buttons while printing, 5% for speed and 1% for flow, and a long press on a value
  puts it back to 100%. Tap anywhere else on the tile to open Tune.
- **The home print card matches Print Status** - the detailed print card shows the toolhead
  speed in its own row and the volumetric flow beside the filament used, with Print Status's
  icons. When a row is tight it drops the least useful piece, and the nozzle icon carries
  its tool number.
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
  a heated box's humidity, while drying the time left inside a progress ring, and on a 2x2
  tile one row per dryer. A tap opens that dryer's own screen. It never starts or stops a cycle.
- **A humidity droplet replaces the water icon** - the home Humidity tile, the AMS overview card,
  the environment pill, the environment screen and each lane's readout draw a drop filled to the
  humidity reading. The water line sits at the reading's share of the drop's height, with two
  waves of different size in two tones taken from the theme's info color. The surface drifts
  gently when animations are on and holds still when they are off. A sensor with no reading shows
  an empty outline.
  The Filament Dryer widget's idle humidity carries the same drop, filled to the wettest box.
- **A Host Health widget** - CPU load and temperature, memory and the busiest controller board,
  with a trace of recent load. It turns amber or red at 70% and 90% load, at 70 and 80 C, on CPU
  throttling, on rising MCU retransmits and on under 500 MB free for G-code files. A tap opens
  Settings > System > Performance. Only offered once Moonraker reports performance data.
- **A Toolhead widget you can move the toolhead from** - X, Y and Z, shown as `--` until each
  axis is homed, with a plate view and a crosshair for the nozzle from 1.5x1 up. Once the
  printer is homed and idle, tap the plate to pick a spot and confirm with Move here, or drag
  to move the toolhead live, just like the Motion screen's Bed tab; while it can move, a hold
  or drag on the plate never enters edit mode or swipes the page. At 2x2 a Home All button
  appears while an axis is not homed, and during a print the active tool and live speed take
  its place while the plate shows the head over the print. A 2x3 tile adds three preset buttons
  (Front, Center and Park by default; any Move-tab position, Park, Z +10, Z -10 or a macro, set
  from the gear in edit mode). A tap anywhere else opens Motion. Not available on the K-Touch.
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
- **Scheduled pauses are marked on the progress indicator** (#1509) - filament changes (`M600`),
  explicit `PAUSE`/`M601`, and slicer pause-at-layer stops each draw a tick where they sit in
  the print, on both the home print-status card's bar and arc and the print panel's bar. The
  tick's position follows the same axis the bar fills on: a file sliced with `M73` progress
  lines places ticks at the slicer's own percentages, anything else at file position. A print
  whose gcode was never fetched (an external start during the setup wizard, or a file too
  large to preview) simply shows no ticks.
- **Every light gets the same controls, and each light button picks its own light** (#1130) -
  the LEDs screen has a tab per light with a dot showing whether it is on and in what color.
  Each tab shows only what that light can do: power and brightness, white tones and colors,
  effects or presets, or On and Off for macro lights. Home light buttons each control one
  light or All lights, chosen from the gear in edit mode; a wide button has a › that opens
  that light's tab. Automatic LED Control has its own "Applies to" list. If you had picked
  some but not all of your lights for the light button, that choice carries over to
  Automatic LED Control and your light buttons start out on the chamber light; set them from
  the gear.
- **A Maintenance widget** (#1807) - HEPA filter, nozzle and linear rail hour meters with a
  due notice, and a detail screen to reset each meter or change its interval.
- **Set a heater target from the thermistor widget** - a thermistor tile showing a
  `heater_generic` or `temperature_fan` displays its current and target temperature, and a
  tap opens the temperature keypad, limited to that heater's own `max_temp`. A sensor the
  printer does not report reads --°C instead of 0°C.
- **Filament dryers and other add-on heaters on the temperature graph** - every
  `heater_generic` besides the chamber is graphed with its history, on the graph screen and
  in the home graph widget, and is named as a heater (Chamber Heater, Filament Dryer
  Heater). The graph's legend has its own row, so a trace near the top of the scale does not
  run under it.
- **Power tile color** - the power tile takes the same color choices as the macro tile.

**Motion**

- **The Motion screen is organized into Jog and Move tabs** (#865) - a labeled rail on the
  left in landscape, icon pills in the header in portrait, and it opens on Jog every time.
  The position readout that used to sit in a card beside the jog pad moved into the header,
  so it stays visible on both tabs and the jog pad takes the width the card used (the
  separate "Act:" line is gone; see the swap icon below). An axis that is not homed shows its
  position grayed out. The Z keys show what they move: a drawing of your printer's toolhead,
  or of the bed on printers where the bed moves, in place of the words Print Head or Bed.
- **The Move tab sends the toolhead to named bed positions** - a 3x3 grid laid out like the
  bed seen from above (Rear at the top, Front at the bottom), placed on the print plate
  from the `[bed_mesh]` probing area rather than the full axis travel, so no preset aims
  at a purge area or tool dock past the plate; outer positions sit 10% in from the plate
  edges, and on a delta the eight outer positions sit on a circle instead. A move lifts the
  nozzle to the Bed Map Clearance first when it is lower, and above your last print while
  that may still be on the bed. The positions are disabled until every axis is homed, and
  the whole tab while a print runs or is paused, or while the printer is not ready. The
  positions and Park gray out while the toolhead is moving, so a second tap cannot land
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
  what the limit is; a partial move happens silently. The Z buttons gray out at their limit
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
- **Pick a Jog-tab layout** (#865) - Settings > Printing > Motion > Layout offers the jog
  pad, Arrows (an XY cross, a Z pair, home buttons and one step selector, the Mainsail
  arrangement), Arrows + Home (a home key in the center of each cross, the Fluidd
  arrangement) and Axis Rows (a row of minus and plus keys per axis at the jog mode's two
  distances). The arrow layouts' step selector offers every distance you set, so changing
  Turbo to 25 and 100 puts 25 and 100 on it. Home keys stay solid until their axis is homed.
- **Home bar on the Motion screen** - the Jog tab has a home bar under the jog pad. Where five
  buttons fit at a comfortable size it shows All, X, Y, XY and Z; on a narrower bar, such as
  800x480, it shows X, Y, XY and Z, because the jog pad's center button homes all axes.

**Tool changers and multi-toolhead printers**

- **IDEX and dual-extruder printers get the multi-tool screens** (#1350) - a printer is
  recognized as multi-tool by counting its hot ends, so IDEX machines, dual extruders and
  tool changers that do not run klipper-toolchanger get the tool selector and filament
  screens instead of the single-extruder ones, and `T0`/`T1` tool switching works on them.
  Built from test configurations; not yet verified on these machines.
- **Tool Offsets screen with automatic calibration** (#1480) - per-tool X, Y and Z offsets in
  one place, and a one-tap calibration that runs your printer's own routine, follows it
  live, and saves every axis with a single restart. Where that routine calibrates every
  tool, the manual paper test is hidden. (thanks @Monstrofil)
- **Per-tool Z offset from Print Tune** - Print Tune has a Global / T0..Tn selector, so each
  tool keeps its own first-layer height, and the save button appears whenever any tool's
  offset is unsaved and writes it permanently, so it survives the next Klipper restart.
- **Snapmaker U1: load or unload several toolheads at once** - a picker lists every head
  with what it holds, pre-ticks loaded heads for an unload, and says which heads cannot take
  the operation and why. The printer's own batch command does the work, preheating the next
  head while the current one finishes, and the header names the head being worked on. An All
  row ticks every head, and is hidden when the filament system cannot run that many at once.
  A row says a head is ready to load only when its sensor sees filament, and empty only when
  the sensor sees none; a head with no sensor shows just its material. The filament
  sidebar's buttons read **Load...** and **Unload...** where they open this picker.
- **Snapmaker U1 toolhead offset calibration** - the U1's own multi-head offset calibration
  runs as a guided flow under Controls > Calibration > Tool Offsets, in the stock screen's
  order with its brush, plate and probing prompts. It refuses to start during a print, and a
  calibration left behind by a closed session or another screen is cleaned up or asked about
  rather than left holding the heaters.
- **Snapmaker U1: Auto loading per toolhead** (#1982) - each toolhead's feeder has its own
  Auto loading switch under Print Behavior in the filament system's settings, so a head fed
  by a BMCU or by hand can have it off. A head with Auto loading off is left out of the
  printer's Auto-Replenish, and Load on it opens the hand-load dialog.
- **One target tool on multi-head printers** - the Controls temperature card, the Filament
  panel and the temperature graph aim at the same tool, chosen from the Home tool switcher's
  cells, and picking one never changes tools. A tool change moves them to the mounted tool,
  and tapping a Nozzle Temps row on the home screen opens the graph on that row's tool. The
  Filament panel's temperatures, material presets, Cool Down and the preheat before Load,
  Unload, Extrude and Purge all follow the chosen tool, and a Pick up or Park button beside
  the heads mounts or parks it. Extrude, Retract and Purge pick up the chosen tool first when
  it is not on the carriage, with the usual tool change confirmation, and send nothing if the
  pickup fails. On Controls, the nozzle row heats whichever tool it shows. Every chooser
  numbers tools the way the rest of the screen does, so a cell and "Nozzle 2" always agree.
  A printer with one hotend fed by several lanes, such as AFC, Happy Hare or a CFS, gets no
  tool chooser, since those lanes all feed one nozzle.
- **Bondtech INDX** (#1333) - early support for INDX tool changers, detected from the
  `[indx]` module. Tool changes, parking, and loads by material preset or temperature work
  from the screen, with batch Load, speed mode, guided tool cycle and heater calibrations, an
  offer to update the heater model after a material change, and a confirmed recovery when a
  tool is not seated. Tool Offsets can seat a tool by hand. Docked tools show their material
  without a temperature, and the nozzle refuses to heat with no tool on the head. Please
  report how it behaves on your machine in #1333.
- **MedusaHC: purge length, tool offsets, nozzle cleaning and clearer failures** - a Purge
  Length setting (0-200 mm, default 50) under Purge & Wipe in Device Operations sets how far
  a load purges past the nozzle, per printer, and when loading several tools each next tool
  preheats to its full load temperature. Tool offsets edit X, Y and Z, and a change to the
  mounted tool applies right away. A **Clean nozzle** action under Maintenance runs the
  printer's `CLEAN` macro. A failed tool change quotes the controller's own message (for
  example "sensors did not confirm T1") and names the tool it was going to, and Resume after
  a failed swap waits for that tool to heat above 190°C.

**Filament systems**

- **A lane shows what the printer holds** (#1649, #1653) - HelixScreen keeps no copy of its
  own of your lanes. Each lane shows what your filament system holds first (AFC, Happy Hare, the
  CFS, the AD5X's color menu and so on), then the lane record stored on the printer, then
  Spoolman's details for the spool the lane is linked to, and nothing else. A second screen,
  Mainsail, Fluidd and OrcaSlicer all see the same lane, and a change made on any of them shows
  here within about ten seconds.
- **Edits are written where the printer keeps them** - on AFC an edit goes to AFC itself
  (spool, color, material, weight and tool commands), on Happy Hare to its gate map,
  including the spool's name and temperature, and on the CFS, AD5X, QIDI Box, Snapmaker U1,
  ACE and tool changers to the printer's own commands plus the shared lane record. HelixScreen
  never overwrites a lane record another program writes. On an OpenAMS with openams_spoolman,
  edits go through openams_spoolman, each bay shows what it reports, and Clear Spool is grayed
  out with the reason. On a Happy Hare that fills its gates from Spoolman, a change Spoolman owns
  is refused and the editor says to make it in Spoolman.
- **Saving says whether it worked** - the spool editor shows Saving... until the printer has
  the change, then closes. If the printer refuses it, or Spoolman is offline, the editor stays
  open with a line under Save saying why and keeps what you typed, rather than showing a
  pop-up message.
- **Editing waits for the printer** - while HelixScreen is connecting or reading the lanes for
  the first time, Edit Spool is unavailable and the editor shows Loading lane details. While the
  printer is disconnected, edits are refused instead of being kept to send later.
- **Happy Hare tuning lives on the printer** - gear and extruder speeds, toolhead distances and
  the sync setting set from HelixScreen are stored on the printer, so every screen shows the
  same values, and clog detection shows what the MMU reports.
- **A new spool is judged by what the hardware read** (#1710) - when a spool goes into a
  slot, a different tag, or a different material or color read off the spool, clears the
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
- **OpenAMS support** (#1150, #1786) - a printer running OpenAMS through its `oams_manager`
  gets the filament system screens: lanes, loads and unloads, spool edits and the filament
  path, with each unit's FPS shown as a live-tinted box on the path. Each unit shows the
  temperature and humidity of the sensor its config names, and its dryer is started and
  stopped per unit from the environment screen. A latched stop or pause fault on a unit
  marks that unit's slots and opens the recovery dialog in your language, and Reset clears
  it on each unit that offers it. OpenAMS units carry their own full-color logo. OpenAMS
  run through AFC stays an AFC setup.
- **Several ACE units on the ACEPRO driver** - each daisy-chained ACE shows as its own unit,
  named after its model (ACE Pro or ACE Pro 2), with its own dryer, temperature and
  humidity, firmware version and connection state. Commands and status updates reach only
  the unit they are for. Slots show the driver's tool numbers where they differ from the
  slot, and a load from the screen while another tool is in the head runs as the driver's
  tool change, showing the old filament retracting first.
- **Snapmaker U1 with multiACE** (#1660) - the ACE units behind each head load, swap, unload
  and dry from the filament screen, and bay spools can be edited. With multiACE active, a
  runout never sends the stock refeed.
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
  it is part of a cycle: drying, waiting for a heater, finished or stopped. The dryer
  controls take an optional humidity target; leave it blank for the printer's default
  (#1755).
- **Favorite filaments** (#1100) - star any row in the filament catalog. Favorites lead the
  vendor list and float to the top of their vendor's view, on every printer.
- **Load and Unload for the external spool** (#1486) - the bypass spool's menu on the
  filament system screen has its own Load and Unload, engaging bypass first when needed, so
  you do not have to go to the Filament screen for it.
- **Happy Hare: Preload, extruder-only loads and Refresh Spoolman** - the slot menu gains
  **Preload** (spool to gate, grayed out during a print or while filament is loaded), the
  Maintenance section gains **Load Extruder** and **Unload Extruder** for filament already at
  the toolhead (refused during a print), and Accessories gains **Refresh Spoolman**. Recover
  State also offers Bypass, or lets Happy Hare detect what is loaded.
- **ACE shows where the filament actually is** (#1677, #1678) - the path reads the ACE's hub
  and toolhead sensors, so a strand parked short of the hub is drawn there rather than back
  at the spool, and on a Kobra S1 an idle slot the driver reports as parked is drawn staged
  at the hub. An ACE Pro with a fifth spool on its bypass switch gets a working bypass
  control. During a tool change the path draws the outgoing lane retracting until the hub
  clears, then the lane being loaded.
- **A new Filament Buffer widget shows where your filament buffer sits** (#1724) - an upright
  slider with loose filament up and tight down, beside a big reading that is gray on target,
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
  compact strip: a color bar beside the slot and its remaining weight, the filament name on
  one line, and the clog and buffer meters underneath, which step aside during a load or
  unload so its progress has the room. The action buttons sit two to a row, so the side
  column does not scroll at 480x272, 480x320 or 800x480. Temperature and humidity stand in
  a pill at the end of each box, showing the dryer's time left while it runs. On small
  screens with five or more lanes, the slot labels alternate between two rows so each keeps
  to its own space, and the tool badges stay hidden while every tool feeds its own slot,
  appearing only when tools are remapped. In portrait the diagram runs the full width, with
  the loaded strip and status above it and the actions and load progress below.
- **Slot labels name the spool** (#1687) - each slot reads the spool's name above its
  material ("Bone White" over "PLA+"). A slot shows only the material when nothing names the
  spool, when it is empty, or when five or more slots share the row, and only the name when
  the name already says the material. The line names the spool or product, never a made-up
  color name.
- **Creator 5 on Z-Mod: Load feeds the head** (#1779) - Load on a head feeds it through
  Z-Mod's afclite (`CHANGE_TOOL LANE=E<n>`), heating only that head. The Creator 5 has no
  automatic unload, so Unload shows a notice telling you to cut and pull the filament at the
  head, and sends nothing; to mount a head without feeding it, use Select slot in the slot
  menu. A fault from Z-Mod opens a dialog in the firmware's own words.
- **Your tool mapping reaches Z-Mod** (#2033, #2038) - on an AD5X or Creator 5 Pro running
  Z-Mod, the tool mapping you choose before a print is handed to Z-Mod when the print
  starts, whether or not Z-Mod's built-in AFC is on, so the print uses the slots you picked
  with no second color prompt from Z-Mod. This includes prints the HelixPrint plugin starts.
  A remap made for one print is put back when that print ends. Older Z-Mod versions that do
  not advertise it work as before.
- **Creator 5 on Z-Mod: a spool bound from outside shows up without a reboot** (#1833) - when
  another tool sets a head's spool (an NFC tag reader through `CHANGE_ZCOLOR`, or
  zmod_afclite's `SET_SPOOL_ID`), the screen reads the record it wrote and shows the new
  spool, name and color within a couple of seconds. (thanks @DIEHARDave)
- **A new Edit Spool editor** - a slot's Spool Info is renamed Edit Spool and opens straight
  into one screen that shows the filament in words with its color and remaining weight, and
  the temperatures for what you picked. Material, Variant, Brand and Product are dropdowns
  that narrow each other down, with Browse catalog... at the end for searching the full
  filament catalog or typing your own. Tap the weight to type the grams left. Color swatches
  are at least 7 mm across on every screen; the full color view puts the presets and a
  custom color picker side by side where both fit, and on small screens they take turns. On
  systems that read a spool's tag, a "from tag" pill marks each value the tag supplied, and
  Reset to tag puts them all back. On AFC, a lane with a TD-1 scan shows the scanned color
  and TD, with Use Scan to take them (#1151). Spoolman's spool list and QR scan are round
  buttons in the header, and a linked spool's price, location, lot and notes sit under More.
  Which tool a slot feeds (Assigned Tool) and its Endless Spool backup are set in Edit Spool
  and save with its other changes; a slot's menu holds only actions. With AFC's multiple
  tool mapping turned on, Assigned Tool is disabled and says that tools are assigned in AFC.
  Save sits in the header and stays disabled until you change something and the printer's
  records have loaded, and Back with unsaved changes asks whether to save or discard them. A
  save still talking to Spoolman finishes even if you leave the screen, and a save that only
  partly succeeds says which part did not: with Spoolman offline, the slot saves and price,
  location, lot and notes wait in the editor.
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
- **Material presets heat the bed and chamber** - a material preset sets the bed, and the
  chamber on a printer with a chamber heater, along with the nozzle, on the Filament panel,
  the Preheat widget and the temperature graph's nozzle presets. Turn off Settings >
  Printing > Material presets heat the bed and chamber to heat the nozzle only.
- **Load filament by hand in one dialog** (#2000) - on a printer with no filament system and
  no load macro, and on a Snapmaker U1 toolhead with no feeder attached, with Auto loading
  off, or with filament pushed in past its feeder, Load opens one Load Filament dialog. It
  heats the nozzle, waits while you push the filament into the extruder (a filament sensor
  starts the extruder for you, or tap Extrude), extrudes a short length and asks whether new
  filament is coming out. Tap Extrude more until it does, then Done. Cancel, or closing the
  dialog any other way, turns the heater off at once. The dialog does not open during a
  print; during a print, paused or not, a U1 head that loads by hand says why, and you push
  the filament in yourself.
- **The filament path is drawn from what each unit really has** - every unit type draws one
  route from spool to toolhead, with sensor marks only where the unit reports a sensor, a
  hub sized to its tubes, and the hub and buffer stacked above the toolhead. The route in
  use glows in the accent color. A jammed lane, a buffer fault or a lane off the active route
  shows its error where it happens, filament stuck at the hub after a failed unload is drawn
  there, and Happy Hare's bowden progress fills the tube during a load or unload. Dry-box
  units are drawn as a box with the spools on its floor under a glass lid, and the loaded
  spool glows around its outline, pulsing during a load.
- **Page through your units** - on the filament system overview, tapping a unit card zooms
  into that unit, with arrows, swipe and page dots to move between units, and the overview
  draws every unit, hub, buffer and toolhead for up to 16 units.
- **More lane details** - a lane that measures its remaining filament length shows it under
  the spool and on the loaded-spool card, and a lane that reports its own temperature shows
  it above its humidity. A unit the printer reports offline reads Disconnected and fades,
  but stays tappable. Device operations lists each unit's firmware and serial, the
  toolchange purge, the Spoolman mode and a pending spool.
- **Open the slot editor when you load a new spool** (#1335) - optional, under Settings >
  Printing > Edit filament on insert, and offered in the setup wizard. Filament inserted into
  an empty lane, or into a printer with no filament system or with bypass engaged, opens the
  slot or external-spool editor. It stays quiet while printing, during a filament operation,
  and right after startup or a reconnect. On the Snapmaker U1 a tagged spool does not open
  it; an untagged one still does.
- **OrcaSlicer picks the exact filament preset** (#2002, #1923) - lane records carry the
  filament's OrcaSlicer id when exactly one catalog product has it, or when the lane's
  linked Spoolman filament has an `orca_filament_id` extra, so OrcaSlicer 2.5 selects that
  exact filament preset instead of a generic one. The id clears when the spool or the extra
  is removed, and only a single token with a letter in it is used, so a record number or
  preset name typed into the field does not replace a good catalog id. Earlier OrcaSlicer
  versions match by material type, as before. (thanks Roman Lisunov)
- **Search SpoolmanDB from the spool wizard** - type in the vendor step and matching spools
  appear as you type. Picking one fills in the vendor, material, color, temperatures and
  weights, and reuses a vendor and filament you already have when they match exactly. The
  search is hidden on Spoolman servers older than 0.26.
- **Swatches show the filament** - multi-color spools draw split into their colors, and
  silk, matte, marble and sparkle filaments draw with their finish, on spools, slots, the
  tool switcher, the filament mapping chip and the temperature graph.
- **Catalog additions** - Eryone filaments, and recycled grades filed under their polymer as
  the Recycled variant.
- **Clog Detection from a plain motion sensor** (#1823) - a Klipper `filament_motion_sensor`
  you enabled in Settings feeds the Clog Detection tile when nothing else reports clogs,
  reading OK, Jammed (only during a print) or Off when Klipper has disarmed it.
- **Happy Hare 4: why the clog bar tripped** (#1755) - the line under the clog bar gives
  FlowGuard's trip reason or the tangle prevention state, in your language, and clears once
  the reason does. Happy Hare 4's gate vendor shows as the lane's brand.
- **Endless Spool switch for Happy Hare** - single-unit Happy Hare setups can turn Endless
  Spool on and off from the screen. A switch the printer rejects flips back.
- **ACE Endless Spool** (#1679) - the ACE shows the driver's Endless Spool groups. Only ready
  slots count as backups, and materials match regardless of case or stray spaces.
- **QIDI M604 loads finish on the screen** - on the Q2 and Max 4 (firmware 01.01.02.x), a
  load from HelixScreen runs the whole stock wizard: push the filament in, Extrude more or
  Done, then the nozzle cools and the filament sensor is turned back on. With a QIDI Box,
  box slots load and unload the way the stock screen does them.
- **Creality CFS: a spool running dry mid-print** - when a CFS box runs a spool out during a
  print while the toolhead is still fed from the tube, a warning names the slot before the
  toolhead runs dry.
- **K1 with a CFS: cutter and purge chute calibration** (#1282) - on a K1, K1C or K1 Max with
  Creality's CFS upgrade, the CFS device menu has Calibrate Cutter and Calibrate Purge Chute.
  Cutter calibration homes and sweeps the cutter to find its position again, about a
  minute, and reports the result on its row. Chute calibration moves the toolhead to the
  chute, lets you jog Y until the nozzle is centered over the opening, and saves it, then
  shows the saved position. During a print these send nothing, Save refuses on an unhomed
  printer, and a sleeping screen keeps your session.

**Drying filament**

- **Dry filament on the heated bed** (#1730) - on an enclosed printer, Dry Filament on the
  bed card (or in Advanced) offers to unload the toolhead, homes, moves the plate as far from
  the nozzle as it goes, and asks you to lay the spools on the plate under a box. A banner
  shows the unload and the plate move as they run, and tapping it offers Stop, which ends
  the flow before the spools go on. The bed then holds a drying temperature for the
  material, capped at 90°C, for 12 hours, with a chamber dryer alongside when one is
  fitted. A printer with a plain chamber heater and no dryer gets **Heat the chamber too**
  instead: the chamber holds the material's drying air temperature, capped at the heater's
  limit, and is turned off when the run ends unless you changed it yourself. You are
  reminded to flip the spools halfway, and asked to take them off once the bed is below 40°C.
  Until you confirm the spools are off, HelixScreen will not home, move, restart Klipper or
  start a print, and a banner says so across restarts and power cuts. A print that starts
  while the spools are on the bed wakes the screen and raises an alarm (#1745); starting one
  from the screen opens the reminder instead of failing. Printers you enclosed yourself can be
  marked enclosed in Settings > Printing.
- **Dry filament with a Panda Breath** (#1299) - a Panda Breath on stock firmware can run
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
- **A chamber target at the printer's own resting temperature shows** - on printers whose
  M141 splits the setpoint across a heater and a cooling fan (the K2), a chamber target of
  35°C or below parks on the fan with the heater target reading 0, and a target equal to the
  fan's resting value (35°C on a stock K2) looks exactly like "off" to the screen. The print
  screen shows that number with a circled A beside it: the printer's standing behavior, not a
  target you set. Targets you set keep the checkmark, and the print screen's chamber status
  follows the real setpoint while a print runs.

**Printing**

- **A new Print Status screen** - the screen is built around a picture of your printer. Each
  heater, fan, the light and speed/flow get a chip; on printers whose picture has been mapped
  each chip sits beside its part with a dotted line to it. A heater chip is red with a flame
  while it heats, green with a check at its target and blue with a snowflake while it cools,
  turns amber when it is off target during a print, and carries a progress bar while it
  climbs; the picture glows at a heater only while it climbs. Tap any chip for its detail;
  the light can be toggled from its chip. Progress, time remaining, finish time, elapsed
  time, the filament in use and the next queued job sit in one strip with Pause, Tune, More
  and Cancel, and Camera, Timelapse and Light live under More. The layout is chosen from the
  space the screen actually has, portrait or landscape, down to 480x320: when chips do not
  fit, the picture shrinks a little first, then chips showing an ordinary value (a fan at 0%
  or on auto, the light off) step aside, and only then do chips move onto the parts
  themselves. A heater off target or a tuned speed is never left out. On a tool changer the
  nozzle chip shows the active tool's temperature (#1818).
- **A pre-print checklist** - while a print prepares, the preview is replaced by the start
  sequence: finished steps with how long they took, the current step in amber with a
  countdown, and an estimate for each step still to come. Small screens show the current
  step and a dot per step. A step the printer has not announced is never listed ahead of
  time. A soft highlight sweeps the unfilled part of the progress bar on Print Status and
  the home print card, in place of a spinner, so a long heat soak never looks frozen.
- **Why a print stopped** - a canceled or failed print shows the reason, such as a quad
  gantry level that never converged, in the completion dialog, the print screen's cancel and
  error messages and the cancel toast. A paused print says why it paused too. The reason is
  Klipper's own message when it gives one, otherwise the last error the printer reported.
- **Sliced .3mf files print where the printer cannot read them** (#2063) - the file list
  shows a .3mf's plate picture and details from inside the archive, and a plate stepper
  shows the name you gave each plate in the slicer, with its position beside it. The plate
  printed is always the plate shown, and Print sends that plate as G-code. With the
  HelixPrint plugin, uploaded .gcode.3mf plates are converted on the printer. On printers
  that print .3mf files themselves, such as Z-Mod 1.7.5 and later, Print Files offers the
  plates the printer lists, and the chosen plate starts straight from the .3mf with its
  filament mapping. Reprint starts the same plate, and Print Status shows that plate's
  picture, time and materials.
- **Skip bed mesh, QGL and Z tilt for one print** - the print file screen has an option tile
  for each of these steps your printer's start macro runs, beside the other pre-print
  options, even when `PRINT_START` runs the step unconditionally. A skip applies to that one
  print. It needs the small `helix_skips.cfg` that installing or updating HelixScreen's
  helper macros adds, with one include line at the top of `printer.cfg` (backed up first);
  your own `PRINT_START` is never edited. The HelixPrint plugin is not needed.
- **Pre-print options from your PRINT_START macro** - when the printer database has no
  options for your printer, the print file screen builds them from what your PRINT_START
  actually does, following wrapper macros into the macro that does the work. Reading the
  printer's configuration for this follows its include files the way Klipper does, and a
  configuration that cannot be read in full is reported instead of being judged from part
  of it.
- **Pick objects to skip before the print starts** - the print file screen has the same
  skip button as Print Status. Pick objects from the list, the map or the 3D view, and they
  are excluded as soon as Moonraker confirms the start. Picking every object refuses the
  start, and queuing the file instead of printing drops the picks and says so. Needs
  `[exclude_object]` on the printer.
- **Excluded objects show in the G-code preview** - skipped objects are grayed out with a red
  hatch in both the 2D and 3D preview, and numbered badges on the render match the object
  list beside it. In portrait the list shows each object's status beside its name.
- **Door-open warning** - on a printer with a door or lid switch, Start Print warns with
  **Start Anyway** when a door is open and the print uses a material that needs a warm
  enclosure (ABS, ASA, PC, nylon), and a door opened mid-print on such a print shows a
  notice once. PLA, PETG and unknown materials never warn. An Inputs section in Settings >
  Devices > Sensors shows every switch input, marks which are doors, and turns the check off.
- **Mixed-temperature warning** - a file that pairs a material needing a heated enclosure
  with one that cannot share its nozzle temperature, such as PLA with ABS, gets a
  **Mixed-Temperature Materials** warning before it starts, saying whether the two share one
  nozzle or one chamber and bed.
- **Chamber not heating prompt** - when a print waits on a chamber that is not heating, or on
  a chamber target the printer refused, a prompt offers Try Again, Keep waiting or Cancel
  print.
- **Snapmaker U1 bed check** - the U1's dirty-bed pause gets its own prompt (Restart print,
  Cancel print, Turn off bed check), and Settings > Safety & Alerts has a Bed Clear Check
  switch.
- **Print history reaches further back** - the history list loads older jobs as you scroll,
  and the dashboard says when its numbers cover only the newest prints.
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
- **Spaghetti detection, built in** (#1378) - installing HelixScreen on a K2 Plus stops the
  stock AI failure-detection loop along with the stock UI, so HelixScreen ships its own
  detector: during a print it runs Creality's own `/usr/bin/detection` on camera snapshots at
  the interval and threshold the printer's `ai_control` settings already hold, and never writes
  them. A confirmed detection pauses the print and opens a dialog offering Resume, Abort,
  Reduce Sensitivity or Turn off detection; the alert is translated and shows the detection's
  confidence and the camera frame it was judged on, with the camera widget's rotation and flips
  (#1506). Any detection source only reports, and two settings in Safety & Alerts decide what
  happens: **Spaghetti Detection** (watch at all) and **Pause on Detection** (pause, or only
  warn). On the first start both are seeded once from the printer's own stored choice (`switch`
  / `pausePrint`) and are HelixScreen's from then on. Printers whose firmware pauses by itself,
  like the U1, are never double-paused and always get the response dialog (their Pause on
  Detection row hides; the firmware's pause is not HelixScreen's to govern). On the U1, the
  Spaghetti Detection switch and the alert's Turn off detection button turn the printer's own
  detection off and on; the first connect keeps what the printer is set to, and after that
  HelixScreen sends the setting only when it differs from the printer's. The rows appear only
  on printers with detection hardware. Uninstalling restores the stock service.
- **K2 with the k2-improvements mod** - its print start sequence is recognized, so the
  preparation steps are named correctly, and heat soak and the chamber wait show as
  soaking.
- **Camera button on remote screens** - running HelixScreen against a printer on another
  machine, the print screen gains a Camera button for the full-screen webcam view. It stays
  hidden on the printer's own screen. A camera button in the corner of Print
  Status's preview card swaps the thumbnail or G-code render for the live webcam feed; tap
  it again to go back, and the choice carries over to the next print.
- **Macros remember their parameters** - save default values for any macro that takes
  parameters, from edit mode in the Macros panel or a home Macro Button's options. With Ask
  for parameters on, the form opens already filled in; switch it off and the macro runs
  straight away with the saved values. Defaults are kept per printer, and a dangerous macro
  still asks for confirmation.
- **Pre-print options are tiles you tap** - each option is a tile in a two-column grid with
  its own icon: tap anywhere on the tile to switch it on or off. A tile that is on fills with
  the accent color and shows a check in place of its icon. Long option names wrap onto a
  second line inside their tile instead of running under a switch.
- **More room on small print file screens** - the sliced-colors switch is a palette
  button on the 3D preview, the filament card is more compact, and on 480x320 and smaller
  screens the pre-print time estimate gives its line to the options, so four options fit
  there without scrolling in landscape. On small portrait screens the filament chips share
  the FILAMENTS title's line and the preview gives up just enough height for every option
  to show.
- **Filament chips you can read and change in place** - the FILAMENTS card on the print
  file screen shows one chip per tool the print uses: the file's material on its color at
  the top, and the slot that will feed it underneath, as a spool dot in that slot's color and
  its number ("3", or "2·1" for the first slot of the second unit). The outline says whether
  the pairing is a problem: gray when it matches, amber when only the color differs, red when
  the tool has no slot, its slot is empty or the material differs. A tool with no slot shows
  an empty dashed spool. The warning icons beside the card's title are gone. Tap a chip and
  a list opens under it with Auto and every slot, best match first, each with a reason
  ("exact match", "color differs", "material differs", "empty"), and "More options…" for
  the full Filament Mapping dialog. On a printer with no filament system the card
  appears too, comparing the file with the loaded spool, with a swap icon on each later
  filament change.
- **Delete and Print stay on screen** - the filament mapping and the options scroll in one
  column that ends above the buttons, so starting the print never depends on finding
  the button at the bottom of a scroll. A fade with a small arrow sits on the list's bottom
  edge whenever more of it is below.
- **The print file screen has a portrait layout** - on tall panels the preview, options and
  buttons stack top to bottom, and the preview shrinks just enough to keep the first row of
  option tiles in view instead of leaving a sliver of the list.
- **"Printed N times" sits under the file name** - the print count reads as part of the
  file's identity beside the metadata, instead of being a line in the scrolling column
  below.
- **The 2D preview shades creases** (#1314) - outer walls darken where the surface folds,
  so overhang ledges, rims and steps read as edges while straight and evenly sloped walls
  keep their color. Sequential prints, objects at different layer heights and spiral vases
  are left alone.

**Calibration and tuning**

- **Pressure advance, measured by the printer** (#1452) - on printers that can measure it
  themselves, a Pressure Adv. button on the Controls panel runs the measurement for the tool
  you pick and shows the result, flagged if it's outside the usual range; a Pressure Advance
  row under Advanced > Calibration opens it too. The Snapmaker U1 applies and keeps the
  value; on the FlashForge Creator 5 Pro you copy it into the slicer. Takes a few minutes and
  heats and purges. (thanks @Monstrofil)
- **Pressure advance tuning on a live test print** (#982) - Advanced > Calibration > Pressure
  Advance Tuning prints a small test tower and raises pressure advance with height; tap - or +
  at any moment to take over, then pick the height band that looked best from what the
  printer reported. The result is applied, can be saved to the spool's Spoolman filament
  (applied again whenever that filament becomes active, never mid-print), and is kept in
  Moonraker's database for slicer plugins. The print Tune overlay also gains a live
  pressure advance row.
- **Bed screw adjustment shows the tilt** - the bed diagram is shaded by height in the
  bed mesh colors, with a line giving how far apart the highest and lowest corners are, and
  the screw furthest off is highlighted.
- **Belt Tension check** (#1303, #1721) - on a CoreXY printer with an accelerometer,
  Advanced > Belt Tension drives each belt path with the motors and compares the two
  responses on one chart: a similarity score, the peaks both paths share and the ones only
  one path has. Re-test A or B sweeps one path again and keeps the other, with the old curve
  ghosted so you can see what your adjustment moved. Needs HelixScreen on the printer's own
  computer. It warns first on a board with little memory, and refuses on one too short of
  memory.
- **Guided belt tuning** (#1736) - after a Belt Tension check, Tune Belts walks you through
  closing the gap one belt at a time. It tells you when the belts already match, finds the
  zone to tune with one calibration turn instead of trusting the tallest peak, runs short
  settling laps before each sweep, and re-measures only the stretch of the spectrum that moved,
  always inside your printer's sweep range. It checks that readings repeat before it advises
  anything, and flags a path that resonates far harder than the other as a mechanical problem
  rather than tension. On Voron 2.4 and Trident it suggests how far to turn, in quarter turns;
  other printers get direction-only advice. Tune Belts stays grayed out until the printer has
  reported its sweep range, so it never sends a sweep your machine cannot take. The screen fits
  small, portrait and very wide displays, with the chart beside the controls on a wide one.
  The thresholds are provisional until it has run on
  more hardware.
- **Which belt is which?** - Belt Tension can buzz each path's motor so you can see which
  belt it drives, then name the paths left and right or upper and lower. Every instruction
  then uses those names ("Tighten the left belt one full turn"), saved per printer.
- **Live sweep graphs** (#1812) - the Input Shaper screen, the setup wizard's Input Shaper
  step and Belt Tension draw the response live while the printer sweeps, instead of a bare
  progress bar for minutes. Klipper's own analysis replaces the live curve when the sweep
  ends, and every recommendation still comes from that analysis. Needs HelixScreen on the
  printer's own computer; the smallest screens keep the progress bar alone.
- **Heater calibration on tool changers** (#1870) - PID and MPC calibration offer a tool row
  on printers with several heated heads, defaulting to the active tool, instead of always
  tuning the first extruder.
- **Eddy, Beacon and Cartographer calibration from the probe screen** (#1994) - the probe
  screen's calibrate buttons open the Z-offset calibration screen, so you can jog, accept or
  abort on the printer. A BTT Eddy runs its own calibration and saves it, and on printers
  with Creality Helper-Script or Z-Mod an Eddy probe runs its full calibration sweep instead
  of a plain Z-offset adjustment.
- **Z-Tilt and QGL use your Bed Level macro** (#2083) - if you assigned a macro to Bed Level
  in Standard Macros, the Z-Tilt and Quad Gantry Level buttons run it too.
- **Saved Z-offset in Print Tune** (#1998) - the Z-offset adjustment shows the saved value
  beside it. Printers that keep a Z-offset per head do not show this line.
- **Switch a Kalico heater between PID and MPC** (#1237) - the method selector on Kalico
  printers is out of beta. It migrates your config in both directions, and canceling a
  running MPC calibration asks first, because canceling means an emergency stop and a
  firmware restart.
- **Input shaper saves the shaper you picked** - the chips under the results graph are one
  choice per axis, and Save writes that choice to your config rather than Klipper's own
  recommendation.
- **Baby-step size is remembered across launches** - the Z-offset step you last chose is the
  one you get next time.
- **Save Z-Offset can be turned off per printer** (#1766) - **Save Z-Offset Buttons** in
  Settings > Printing > Motion. Off removes the save from the header, Controls, the Z-Offset
  widget and Z-offset calibration, and Print Tune calls an adjustment temporary; baby-stepping
  still works. It starts off on tool changers, where one machine-wide save would shift every
  tool's offset, and on everywhere else. Printers whose firmware keeps the offset itself do not
  show it.

**Printers**

- **VzBot 330 and 235** (#1689), **FLSUN S1 and S1 Pro**, the **base Creality K2** (#1606)
  and the **Anycubic Kobra X** (#2074), each with artwork. The Kobra X was detected as a
  Neptune 4 Pro before. The **FLSUN T1 Pro** (#1781) gets its own picture, and the T1 Pro
  and S1 are treated as enclosed. The base K2 is told apart from the K2 Plus and Pro, and
  its camera option does not claim the Plus's AI model.
- **QIDI Plus 5** (#1791) - detected and supported, with the QIDI Box and HelixScreen running
  on the printer's own display. (thanks @Gromatou)
- **Jacobean's K2 Plus firmware is recognized** (#2087) - a K2 Plus running Jacobean's
  Kalico-based replacement firmware is detected as itself instead of as k2-improvements, with
  print-start progress that follows its own START_PRINT. A printer type mismatch warning
  also leaves the screen as soon as the type is changed in Settings. Its CFS works with the
  commands that firmware has: the spool editor opens on the product the box reports and
  saves its name, Recover and Clean Nozzle send that firmware's commands, Reset only re-reads
  the box, and Refresh RFID is not offered. When the firmware is set not to unload after a
  print, the warning about an empty toolhead before a print stays.
- **FlashForge Creator 5 and Creator 5 Pro are supported printers** (#1714) - both models of
  FlashForge's four-head tool changer are recognized and given their own settings, on the
  firmwares that replace its stock software: Z-Mod, Reforge and OpenCreator. Mounting and
  parking a head is driven from the screen, and the swap shows its steps (dock / pick up)
  as Z-Mod narrates them; Z-Mod's always-on endless-spool switching is reported. On Z-Mod
  1.7.4+ the four heads read their material and color straight from the printer, which
  also names itself correctly in debug bundles instead of reading as an Adventurer 5X. The
  firmware's own filament-system faults surface with their own wording, a head without
  filament reads Empty, and a read-only tool-state query replaces the stale boot state on
  older Z-Mod builds. All of this is verified against a simulated printer and a field
  debug bundle; the swap-step wording on a live print is the one piece still pending
  hardware verification. On Reforge without AFC, picking up a head sends the tool number in
  the form Reforge accepts (#2027), so the Filament panel's Pick up, Load and Unload work.
  On Z-Mod, a print shows Preparing as soon as it starts and follows the Creator 5's own
  start sequence: tool pickup and docking, nozzle settling, mesh progress, the bed wait, and
  a purge only when the real line purge runs; Stop works during that start, and no other
  print can start over it (#2054).
- **Creator 5: tool offsets, motor tuning and chamber air** - on Z-Mod, Tool Offsets runs
  Z-Mod's own offset calibration prompt, a **Vertical Artifact Compensation** row under
  Advanced tunes the X and Y motors with a head you pick (also on FlashForge's stock motor
  resonance tester), and Fan Control has a **Chamber air** row for Recirculate, Exhaust and
  Off. On OpenCreator firmware, Vertical Artifact Compensation and the tool offset wizard run
  through OpenCreator's macros, Park Tool docks the mounted head, and the chamber heat fan
  shows as a heater fan.
- **Snapmaker U1 firmware settings** - the pre-print toggles (bed mesh, input shaper,
  pressure advance, timelapse) read and set the firmware's stored values instead of always
  reading off. Tangle sensitivity, end-of-print unload per toolhead, and manual bed leveling
  with a PEI plate check are all on screen. Loading preheats to the firmware's own
  temperature for that filament. Firmware faults appear with translated wording.
- **Snapmaker U1: unload at end of print sticks** - turn it on per toolhead under the filament
  system's Print Behavior settings and it applies to every print, including Print Again and
  a restart from the beginning, and switching several toolheads on quickly sets each one. A
  toolhead loaded with soft filament such as TPU is never unloaded, as on the Snapmaker
  screen. HelixScreen sends the choice with every print it starts, so a different choice made
  from another app between prints is replaced by HelixScreen's.
- **Snapmaker U1: set each toolhead's nozzle** (#1927) - after a nozzle swap, pick its size and
  Standard or High Flow in the filament system's Toolhead settings, and the printer stores
  it. Locked while a print runs or is paused. Choosing 0.2 mm asks first, because the
  printer then clears the head's filament info if the loaded filament can't go through a
  0.2 mm nozzle. High Flow is available at 0.4 mm only. A change the printer refuses is
  reported and the setting goes back. (thanks Roman Lisunov)
- **Snapmaker U1 Top Cover** (#2015) - the U1's picture shows the top module when it is
  fitted (or a chamber heater is set up) and switches live when you lift it off. The Fans
  screen has the cover's purifier modes and fans, and Fan settings has its mode targets. The
  first time the cover is fitted, a notice opens the Fans screen and offers to recalibrate
  input shaping, and you get a warning if it is removed. The Filter meter follows the filter
  fan's own run hours, and Reset clears the printer's counter. While a chamber air mode
  holds the chamber under a ceiling and the sensor reads above the firmware's alarm
  temperature, Resume warns you before continuing.
- **Power-loss recovery on QIDI printers** (#1716) - on a Q2, Q1 Pro or Plus 4 running the
  stock firmware, the resume dialog appears after a print was cut short by a power loss,
  offering the printer's own `RESUME_INTERRUPTED` flow or a clean discard. With the
  HelixPrint plugin on a Q2, the plugin's guided resume takes its place (see
  **HelixPrint plugin**).

**Display, sound and touch**

- **Glass Effects** - an appearance option under Settings > Appearance that gives buttons a
  lit, glassy finish and every card and panel a soft light from its top-left corner.
  Switches, sliders, input fields, keyboard keys and the navigation bar get it too. It is
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
  own colors, for anyone whose printer image is not much to look at. Select it under
  Settings > Display > Screensaver.
- **Flip Clock screensaver** - big light digits on dark split-flap cards, the time of day in
  your 12- or 24-hour format. While a print runs the cards count down the time left, with
  "done at 9:42 PM · 63%" underneath; a pause freezes the count and a finished print shows
  DONE for a minute. Every digit has its own card, and only the digits that change fold
  their top half down; slower boards swap the digits instead. Select it under Settings >
  Display > Screensaver.
- **Fireworks screensaver** - shells rise from behind a line of hills under a starry night sky,
  trail sparks, and burst as peonies, chrysanthemums, willows, rings, palms, crossettes or
  strobes, each spark fading from white through its shell's color to an ember. It adjusts how
  many sparks and bursts it draws to what the board can afford. Select it under Settings >
  Display > Screensaver.
- **The Flip Clock is the new default screensaver** - fresh installs show the split-flap
  clock, which costs almost nothing and counts down a running print. A screensaver you
  already chose stays as it is.
- **3D Maze screensaver** - the old Windows walk through a brick maze, with wood floors, a
  tiled ceiling and the original's wide view. The maze has a few loops and small rooms, and
  the halls fade into the dark. Spinning HelixScreen logos float in the halls, gray rocks flip
  the view upside down, a tiny toolhead scurries past, and the walls are hung with framed
  thumbnails of your recent prints and other files, and your printer. The pictures come from
  thumbnails the screen already has, so the saver never downloads anything. Select it under
  Settings > Display > Screensaver.
- **3D Pipes looks like the Windows original** - fat, shiny pipes in the sixteen classic
  materials (emerald, ruby, brass, silver, plastics and rubbers) grow through the screen with
  elbows, ball joints and the occasional teapot. Near pipes always pass in front of far ones,
  the screen fills up, then dissolves away in blocks and starts over from a
  slightly turned view. It draws one pipe at a time, five to a screen.
- **Hyperspace screensaver** - a flight through glowing rainbow space: star flares stream
  past, a shimmering wormhole sweeps in and out, and now and then a shell of flares bursts
  ahead. Select it under Settings > Display > Screensaver.
- **Fireflies screensaver** - colored swarms of glowing fireflies chase each other through
  the dark, trailing long tails that drift in the wind. Now and then a swarm stops, circles,
  speeds up, splits in two or joins another. Select it under Settings > Display >
  Screensaver.
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
- **Screensavers on every board** - Flying Toasters, Starfield, 3D Pipes, Bouncing Printer
  and Fireworks run on the AD5M and Centauri Carbon, which had none in 1.0, and on every
  other board with a 16-bit display. Each saver draws less on a slower board rather
  than stalling it, and the starfield redraws only what moved, so it costs a small fraction
  of what it did.
- **Screensaver after, on screens without a dimmable backlight** - a phone or tablet running
  the Android app, and any screen whose brightness HelixScreen can't change, has nothing to
  dim, so the dim timer is what starts the screensaver. Settings > Display offers it there
  as Screensaver after, with the same choices as Screen Dim, instead of hiding it and always
  waiting 10 minutes. The row is hidden while the screensaver is Off. On a screen that can
  dim, choosing a screensaver turns Screen Dim into Screensaver after too, and a timer left at
  Never becomes 10 minutes, so the screensaver you picked actually starts.
- **The AD5X buzzer plays chords and music** - UI sounds play as chords, tracker music plays
  in four voices, and `M300` beeps from your macros take the buzzer when Klipper holds it.
- **On-screen keyboard keys look like keys** - each key has a raised edge in its own color
  instead of a flat black shadow.
- **Hardware rotation with touch that follows** (#1275) - a panel turned 180 degrees can
  rotate on the display hardware where it supports it, and touch, mouse and touch
  calibration all follow the turned picture. On a screen drawn through the GPU (DRM with
  EGL), every angle, quarter turns included, rotates on the GPU with touch following, even
  where the panel has no rotation-capable plane; elsewhere quarter turns stay in software.
- **Pinch to zoom the 3D views** - on a capacitive screen, pinching zooms the 3D G-code preview
  and the 3D bed mesh in on the spot between your fingers, and a two-finger drag moves the zoomed
  view. Rotate, tap and the exclude-object long-press stay off until every finger has lifted.
  Resistive screens detect one finger and keep one-finger rotate only.
- **Readable panels on ultrawide screens** - on a screen much wider than it is tall, such as
  1480x320 or 1920x440, panels and overlays stop at 2.4 times the screen height and stay next
  to the navigation bar, so a settings list or the jog pad is not stretched across the whole
  display. Overlay headers still span the screen, with their buttons at the right edge. The
  home screen, print file list, Print Status, bed mesh and the filament system screens use
  the full width.
- **Print Status in columns on ultrawide screens** - at 1480x320 and 1920x440 the model
  preview is a square at full height, progress, times, layer, filament and the next job sit
  in a column in larger type, the printer picture keeps its shape with its callouts, a card
  lists the nozzle, bed and chamber with their targets beside speed, flow and the fans, and
  Pause, Tune, More and Cancel stack on the right.
- **Random screensaver** - picks a different screensaver your screen can run each time it
  goes idle, never the same one twice in a row. Select it under Settings > Display >
  Screensaver.
- **1 hour Display Sleep** - a new choice beside the shorter sleep times.
- **Delta printers get a round bed** - the bed mesh (3D surface, zero plane, wireframe and
  2D heatmap), the Motion screen's Bed tab and the exclude-object map draw a round plate on
  delta printers, with a 50 mm reference grid.
- **Portrait splash screens** (#2073, #2072) - Pi and desktop installs with a portrait
  display get a full-screen splash image drawn for that shape, instead of a landscape image
  scaled down or drawn past the sides of the screen.
- **Hide the on-screen keyboard while a USB keyboard is attached** (#1572) - the Hardware
  Keyboard setting in Settings > Touch & Input.
- **A PWM buzzer on any Linux board** - a passive buzzer on a hardware PWM pin works on a
  Raspberry Pi or any other board: set `sound.pwm_channel` in `settings.json`, or override it
  with `HELIX_PWM_SOUND`. The buzzer is never left sounding after the app exits or crashes,
  and quiet volume settings do not mute every tone.

**Setup and settings**

- **Share the screen with Mainsail and Fluidd** (#1825) - Settings > Connection > Screen
  Sharing > Share Screen adds a **HelixScreen Screen** webcam that shows the printer's screen,
  and Allow Remote Control lets you tap and drag on it from the browser. On MainsailOS and
  KIAUH installs it follows Moonraker's own access rules; on the K1, K2, Adventurer 5M and
  Centauri Carbon a browser pairs once with a six-digit code shown on the screen. A small icon
  on the printer shows while someone is driving it remotely. Off by default, and it costs
  nothing while nobody watches. Printers whose firmware already shares the screen keep theirs.
  The webcam takes the screen's own shape, such as 5:3 on 800x480. Taps work behind a reverse
  proxy that sends `X-Forwarded-Host` or `Forwarded`, and the shared picture shows a
  placeholder while the pairing code is on the screen. Opening the screen in a browser wakes a
  sleeping display; while the page is open the screen dims and runs its screensaver but never
  goes black, and the tap that wakes it never presses what is under it. Webcam integrations
  such as Obico and Home Assistant do not keep it awake.
- **Screen Sharing is per printer** (#1825) - each printer you control keeps its own Share
  Screen, Allow Remote Control and the new Add to Mainsail/Fluidd Webcams setting, so sharing
  one printer's screen never lists it among another printer's webcams. With Add to
  Mainsail/Fluidd Webcams off the screen stays shared at its browser address but no webcam
  entry is added.
- **WiFi country** - Network Settings shows and sets the radio's country under the WiFi
  switch, so 5 GHz networks show up in scans before the first connection. The United States,
  United Kingdom, Germany, Canada, Australia and France are listed first. A country the
  printer's own system set is remembered, so when a firmware update resets the radio to the
  restricted worldwide mode, HelixScreen sets it again at the next start and 5 GHz networks
  reconnect on their own.
- **Find your printer by searching** (#1028) - first-run printer selection replaces the
  scroll through about 105 machines with a search box, a grid of vendors and a list per
  vendor.
- **Install HelixScreen's helper macros from Settings** (#1271) - a row under Advanced
  installs or updates the macro pack without running the installer. It backs up
  `printer.cfg` first, restarts Klipper right away when idle, offers the restart for after
  the print when one is running, and refuses to touch anything mid-print.
- **Hardware Health shows its status** - the Settings row carries the live result with a
  severity-colored icon, and critical problems get their own alert icon.
- **Find printers on your local network** - the setup wizard's connection step and Change
  Host / Add Printer list every Moonraker on your local network, not only those that announce
  themselves (which needs `[zeroconf]` in Moonraker). Picking one fills in the address and
  port. A Moonraker behind a login is listed too, and a printer running HelixScreen finds its
  own Moonraker. When the port field still holds the default 7125 and nothing answers there,
  the connection screens try port 80 (Moonraker behind a web server) and save the port that
  answered. Test Connection in Change Host / Add Printer checks an address without switching
  the screen to it, and names the port that answered and Klipper's state, or what failed;
  only Save switches.
- **A moved printer is offered its new address** (#1217) - when a connection fails, one
  quick network scan runs, and if a single Moonraker answers with the saved printer's
  hostname, you are offered Use New Address.
- **Add Printer can save a printer that is off** - if Test has not passed, Save asks "Can't
  reach this printer. Save anyway?" instead of staying disabled.
- **Safe mode after a crash loop** (#1315) - three restarts inside two minutes start the app
  with plugins off and the default home layout, and say so on screen. Layout edits are
  refused while in safe mode, so your saved layout is not overwritten; a restart returns to
  normal.
- **A preferences step in the setup wizard** (#1336) - just before the summary, toggles for
  matters of taste (UI sounds, and the filament system options when your printer has one),
  so you do not have to hunt for them in Settings later.
- **Join a hidden WiFi network during setup** (#1975) - the setup wizard's WiFi step has Add
  Hidden Network, as Network settings does.
- **Timelapse setup installs the plugin for you** - when HelixScreen runs on the same machine
  as Moonraker, Setup Timelapse downloads and installs the timelapse plugin itself instead of
  asking you to do it over SSH. If ffmpeg, which timelapse needs to make videos, is missing,
  the last screen says so and shows the command that installs it. The Check Again button on
  the install step is always on screen.
- **Restart Moonraker from Advanced** - the action bar at the bottom of Advanced has a
  Restart Moonraker button beside Restart Klipper, Firmware Restart and E-Stop. It asks
  before restarting, and is unavailable while offline, during a print or while Klipper is
  restarting.
- **Fan RPM on standard Klipper** (#2071, #2082) - fans with a tachometer show their speed in
  RPM on the Fan control screen, on the part cooling fan's dial as well as on the automatic
  fan cards. Before, only Creality's own fan feedback showed RPM.
- **LED settings warn about macros that set LEDs** - if `PRINT_START` or `PRINT_END` reaches
  an LED command, directly or through other macros, LED settings says that it and Automatic
  LED Control may override each other.
- **Help opens this release's guides** - the docs QR codes open the pages for the version you
  run, and Help has Getting Started and User Guide rows.

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
  walks through building Maintenance Meter. Plugins can be shared in the Discord
  #plugin-showcase channel (#1771).
- **Plugin dialogs** - a plugin can ask a question in a dialog that looks and closes like
  the app's own, sized for your screen. Plugin dialogs and screens never cover the app's: one
  that arrives while an error, a warning, the print-complete dialog or the lock screen is
  showing waits its turn and appears once you have dealt with it, waking the screen.
  Calibration Bridge asks which result was best this way when its print completes.
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
  show grayed out with a reason when it does not apply. A plugin can only add rows: the app's
  own items stay as they are, and a menu never grows long enough to scroll. Settings >
  Plugins lists which menus a plugin adds to, before you enable it and on its row. A plugin
  can also build up to 32 widgets.
- **Plugins can play sounds, show tables and use the keypad and keyboard** (#1826, #1827,
  #1828, #1829) - for plugin authors: `helix.sound.play` plays a cue from the user's sound
  theme, so their sound settings apply (the everyday UI and event cues, rate-limited per
  plugin). `helix.ui.table` builds a table with your own columns and widths whose rows carry
  a key, so tapping a row runs your handler with it and a refresh keeps the selection.
  `helix.ui.edit_number` opens the numeric keypad with a minimum, maximum, decimals and unit,
  and `helix.ui.edit_text` the on-screen keyboard with a length limit or password mode; a
  validator can reject a value with a message while the editor stays open. A plugin can ship
  its own PNG icons for its Home tile and layouts, drawn in the theme's colors. `subscribe`
  delivers a full snapshot on its first call and again after a reconnect or Klipper
  restart, and `helix.printer` reports Klipper's state.

**HelixPrint plugin**

HelixPrint, HelixScreen's Moonraker plugin, gains a set of features with 1.1. Each entry
says what works without it; the HelixPrint Plugin page of the user guide covers each one on
attached and remote screens.

- **Guided power-loss resume** (#1716, #1863, #1864) - the plugin saves the print's position
  about once a second, whichever app started the print. After a power cut, Resume opens a
  guided resume that checks the part is still on the bed, heats bed and nozzle together with
  the nozzle left in place, has you confirm the nozzle height, then purges away from the
  part and continues. Nothing moves until you tap a button, and a big red Cancel stops
  everything. The QIDI Q2 resumes from the height it saved. A printer that is not in
  HelixScreen's database gets assisted recovery, where you guide the nozzle back onto the
  part before the print resumes; printers that cannot resume safely are left out, and the
  HelixPrint Plugin page says why: a Voron 2.4 style gantry that drops when the power is
  cut, a delta, or firmware that has its own recovery. Saving is on by default on eMMC
  storage and off on an SD card, under **Settings > Printing > Power-loss recovery**. A
  resume reloads the printer's default bed mesh; **Resume reloads the saved bed mesh** in
  the same settings adds HelixScreen's include line to `printer.cfg` so a resume reloads the
  mesh the print started with, keeps a backup, and offers the Klipper restart that applies
  it. Tapping it again removes the line, and uninstalling the plugin removes it too.
- **Does the file fit this printer?** - before a print starts, the file is compared with the
  printer's travel, heater limits and tools. A file that would move past the travel,
  overheat or use a missing tool is stopped with **File Does Not Fit This Printer**, naming
  each problem; a different nozzle size gets a warning with **Start Anyway**; a different
  printer model is only noted. The findings show in a card in the file's details. This works
  without the plugin too, from the file's preview; with it, every move is checked, purge
  line and wipe tower included. **Hold slicer prints that do not fit** (Settings > Safety &
  Alerts, off) lets the plugin stop a slicer's upload-and-print that would fail the check.
- **Print photos** - two camera pictures of every print, one after layer 2 and one at the
  end, shown with the slicer preview in the print's history details. On by default in
  **Settings > Devices > Print photos** when a webcam is set up. With the plugin the printer
  takes them for every print; without it, the screen takes them while it is connected.
- **Storage** - **Settings > System > Storage** shows what fills the printer's storage and
  cleans up print copies, file analysis indexes, photos, timelapse videos and old logs, never
  your own files. The plugin adds an optional daily automatic cleanup. HelixScreen also
  refuses an upload, print copy or USB copy that would leave less than 100 MB free.
- **Printer Events** - tap the Notifications widget, then **Printer Events**, for a log of
  what happened to the printer: shutdowns and their reasons, lost MCUs, firmware restarts,
  pauses, runouts, errors and how each print ended. With the plugin it includes what
  happened while no screen was connected. Debug bundles include it.
- **Switch spools at each tool change** - in **Settings > Devices > Spoolman**, the active
  Spoolman spool follows the tool or lane that is printing, so each spool is charged for its
  own filament. With the plugin it covers every print and the print's history lists the
  spools it used; without it, the screen does it while connected.
- **Start options for prints from other apps** - **Settings > Printing > Apply start options
  to prints from other apps** sends your standing print options, such as which U1 toolheads
  unload at the end, to prints started from Mainsail, Fluidd, a slicer or the job queue.
- **Thumbnails and faster previews** - the plugin reads every uploaded file once: files saved
  without a picture get a thumbnail (in Mainsail and Fluidd too), large files preview
  sooner, and per-tool filament is ready for the pre-print checks.
- **Faster starts for edited prints** - when a print start changes the file, the plugin makes
  the copy on the printer, so a large file does not travel to the screen and back.
- **Safer config saves** - Input Shaper Save, the PID/MPC switch and probe settings are
  written by the plugin in one step, refused if someone else changed the same setting
  meanwhile. The last five copies of each file are kept in the hidden `.helix_backups`
  folder in your config folder.
- **AD5X and ACE update instantly** - with the plugin, lane changes are pushed to the screen
  instead of polled.
- **Plugin overview** - **Advanced > HelixPrint Plugin** lists every feature in short rows
  grouped as Running, Off and Not available here, so the whole list fits on the screen. A
  feature that cannot run says why in one plain line, and tapping a feature opens the page
  where its switch lives.

**Files, USB and installing**

- **USB sticks mount on boards that do not mount them** - where nothing else mounts a USB
  stick, HelixScreen mounts it read-only itself and unmounts it on shutdown.
- **One package for the K1 series and the AD5X** - both boards run the same build, which is
  smaller than the K1C's was. It is still published under each board's name, so existing
  updaters keep finding it.
- **Smaller downloads** - packages carry only the splash screens and printer images the
  panel's resolution can show (K2 assets 42MB to 9MB, Centauri Carbon to 2.8MB), and
  embedded packages drop music the machine cannot play. The K1 and AD5X package also leaves
  out pictures for other printers and unused translation files, about half its previous
  size.
- **Logs and caches survive an update** - on the K1, Centauri Carbon, U1 and AD5X, the logs
  and the thumbnail cache live beside the install instead of inside it, so a web update keeps
  them. User config and uninstall records survive a
  Moonraker web update on SysV and procd platforms too.
- **Run HelixScreen in a window on a Linux desktop** - a new package for x86_64 Linux PCs
  (Debian 11, Ubuntu 20.04, Fedora 34 or newer, on X11 or Wayland) runs HelixScreen as an
  ordinary app window that connects to your printer over the network. Install it with
  `install.sh --desktop`: no `sudo`, it goes into your home folder, and HelixScreen appears in
  your applications menu. Without the flag, on a PC with a desktop the installer asks
  whether you want a window or a full-screen kiosk, and suggests the window. Install Update
  opens the release page in your browser instead of updating in place. The 3D G-code preview
  uses your graphics driver's OpenGL ES when it has it, and draws in 2D otherwise.
- **A 32-bit Android app** - devices that refused the app with "no matching ABIs" can install
  `helixscreen-android-armv7`: the Echo Show 5 (2nd gen) on LineageOS, Fire tablets and
  older phones. Low-density screens like the Echo Show's are drawn at full size instead of
  at half.
- **The installer shows the HelixScreen logo** - as a real image in kitty, iTerm2 and sixel
  terminals, and as block art everywhere else.

**BTT K-Touch (beta)**

- **HelixScreen firmware for the BigTreeTech K-Touch** - each release carries
  `helixscreen-esp32-ktouch` firmware for this standalone ESP32 touchscreen. It joins your
  Wi-Fi and runs as a remote screen for your printer's Moonraker, with print history,
  Reprint Last and the idle print card. Add several printers and switch between them live.
  It holds on through weak Wi-Fi, moving to another access point when one keeps stalling,
  and after repeated boot crashes it waits for you to pick a printer instead of reconnecting
  to the one that crashed it (#1750). Its time zone is set from your phone during setup. It
  is a beta and the newest port, with rough edges, and some screens, such as the Toolhead
  widget, are not available on it. The install guide (`docs/user/guide/install-esp32.md`)
  covers flashing from the browser or with esptool, first-boot setup and recovery.

### Changed

**Across the app**

- **The E-stop sits on the navigation bar during a print** - while a job holds the machine,
  the E-stop is at the end of the navigation bar on every screen, and stays bright and
  tappable over dialogs and number pads. The separate E-stop buttons on Home, Controls and
  Print Status are gone. The lock screen and the full-screen camera keep their own.
- **Pressed feedback everywhere** (#1297) - cards, rows, chips, value fields, camera source
  rows and filament temperature rows respond when pressed. On slower boards a pressed card
  shows a ring in the primary color instead of shrinking, which costs less to draw.
- **HelixScreen's print signals stay out of your macro list** - a new install of the helper
  macros names its signals with a leading underscore (`_HELIX_READY`, `_HELIX_ENDED`,
  `_HELIX_RESET` and `_HELIX_PHASE_*`), so Fluidd and Mainsail do not list macros nobody
  runs by hand. On a new install, add `_HELIX_READY` to the end of `PRINT_START`. A guide that
  still says `HELIX_READY` gets you an `Unknown command` message and no ready signal, and so
  does a printer that removed the macros, kept those calls and then installs them again:
  rename the calls. `HELIX_START_PRINT`, `HELIX_CLEAN_NOZZLE`, `HELIX_BED_MESH_IF_NEEDED` and
  `HELIX_UNLOAD_FILAMENT` keep their names.
- **The print status picture holds still** (#1879) - the chips around the printer picture
  stay put when a fan spins up, the speed is tuned or the light turns on. The
  layout is decided once for preparing and once for printing; a reading that becomes unusual
  mid-print (a spinning aux fan) slots into free space instead. The picture is also drawn
  from a copy sized to fit, as on the home screen.
- **One name per fan and per pre-print step** (#1879) - the hotend fan reads "HE" in fan
  settings, as on its chip, and the fan stack's third row reads "Aux", the same fan the print
  status chip calls Aux. The pre-print option toggles use the step names the progress
  checklist shows, so the purge toggle reads "Purge".
- **Chamber reading on the home picture without a heater** (#1879) - a printer with only a
  chamber thermometer gets the chamber chip on the home printer picture, as Print Status
  already showed it.
- **Stop buttons say "Stop"** - the red button that ends a running Input Shaper test, a heater
  (PID) calibration, a tool offset calibration, the setup wizard's Input Shaper step and a belt
  tension check all read "Stop" with the same octagon icon, instead of "Abort", "Cancel" or
  a plain cross. They still do what they did: an emergency stop followed by a firmware restart,
  because none of these can be paused partway through.
- **Spools look like spools** (#1817) - every drawn spool has a vented flange: six slots, and
  vent dots on larger spools, that show the filament wound inside, so you can read how full a
  spool is through them. The flat AMS spool style draws the spool face-on through a smoked
  flange instead of plain rings. The Filament tab and filament icons are a spool instead of the
  nozzle, which stays on the buttons that are about the nozzle, such as Purge.

**Screens and settings**

- **The Beta features setting gates two things** - the Dev update channel and the prompt
  offering to install the HelixPrint plugin. The Z Calibration button on Controls, the Macro
  Browser and multi-printer management work without it, and so does everything new in 1.1: Belt
  Tension, both pressure advance tools, Tool Offsets, pausing at a layer and choosing lanes to
  dry. Configure PRINT_START, the beta macro rewrite under Advanced, is removed; the pre-print
  skip options on the print file screen work without it.
- **Settings is one list of twelve pages in three groups** (#1023) - Screen (Display,
  Appearance, Touch & Input, Sound), Printer (Printing, Devices, Safety & Alerts, Connection)
  and HelixScreen (Language & Time, System, Updates, Help & About). Every page is one tap from
  the Settings screen, and Display, Appearance, Sound, Devices, Connection, Language & Time and
  Updates show a short status line, such as your brightness and sleep time or whether an update
  is waiting. Each page groups its rows in captioned cards, and the PID screen is titled Heater
  Calibration. Nothing is reset: every setting keeps its value. Where things went:

  | Was | Now |
  |-----|-----|
  | Display & Sound | Display, Appearance, Sound, Language & Time |
  | Printing > Toolhead Style, G-code Preview, Z Movement | Appearance > Printer Visuals |
  | Hardware & Devices | Devices |
  | Hardware & Devices > Printers | Connection > Printers |
  | Hardware & Devices > Multi-Filament System Management | Printing |
  | Safety & Notifications | Safety & Alerts |
  | Safety & Notifications > Allow cold load/unload, Cool nozzle after filament ops | Printing |
  | System > Network Settings, Host | Connection |
  | System > Touch & Input | Touch & Input, on the Settings screen itself |
  | Display & Sound > Scroll Buttons, System Keyboard, Keep Navigation Bar | Touch & Input |
  | Help & About > About > Update Channel, Check for Updates | Updates |

  The upgrade banner's Update button opens Settings > Updates. Plugins appears in Settings only
  once a plugin is installed (#1235).
- **The Controls panel fits small screens** - the Calibration & Tools card holds only
  calibration (Bed Mesh, Z Calibration, Pressure Adv., Bed Screws, and QGL, Z-Tilt or Tool
  Offsets where the printer has them) in a grid that keeps every label on one line. Motors
  Off moves beside Motion on the Position card, sized to tap. The Light becomes a Quick
  Actions choice: pick **Light** for any Quick Button in Settings > Printing > Macro Buttons,
  and while an LED is controllable and you have left a slot unassigned, the first such slot
  shows it. A slot you cleared stays empty.
- **The print screen's buttons are named on the smallest panels** - on 480x320 and 480x272
  screens the action buttons (Light, Pause, Tune, Cancel) stack their icon over their label and
  fill the column, with no dead band under short buttons; at 480x272 the Speed/Flow readout
  gives way so every button keeps its label.
- **The temperature screen's control strip fills its column on small panels** - on
  480x320 and 480x272 screens the strip of heater controls is wider, the tool buttons
  get real gaps between them, and the preset buttons fill the strip instead of leaving
  empty space under the row of tools.
- **Advanced hides empty sections** - a section whose rows all hide on your printer takes its
  heading with it.
- **Loading filament does not preheat twice** (#1494, #1495) - when the filament system, your
  macro or the "allow cold load" setting already handles heat, the screen does not preheat
  first. On QIDI printers this stops a preheat that the stock macro then overrode.
- **Temperature keypads stop at your printer's maximum** (#1615, #1619) - and say what the
  limit is when you hit it. Material temperature presets are not limited, so an ABS preset
  above a 260 degree hotend's limit saves fine and is applied at the limit.
- **Tools and lanes are numbered from 1** (#957) - everywhere the screen names a tool, a
  nozzle or a lane for you to read, it counts the way people do: "Tool 1" and "Nozzle 1" rather
  than T0, including the pre-print filament check, the tool switcher and the
  tool-change and preheat toasts ("Switched to Toolhead 4" on a Snapmaker U1, not "Switched to
  T3"). G-code, macros and the console keep the T0 spelling the printer expects. A per-printer
  setting, **Number slots from 0** in Settings > Printing, counts slots, lanes, gates, tools
  and nozzles from 0 instead, matching T0 in G-code, everywhere including the spool strip
  badges, the tool changer's load prompt and multi-ACE error messages (#2079). The G-code
  HelixScreen sends does not change.
- **Clear Spool forgets everything** (#1661) - it erases everything HelixScreen and the
  printer's firmware remember about the slot, leaving only what the hardware can physically
  read. That includes the printer's own record: the whole gate map entry on Happy Hare, and
  the slot record on AFC, AD5X, QIDI Box and Kalico-based CFS. ACE, the Snapmaker U1 and stock
  CFS keep their own record, because their firmware offers no way to erase it or re-reads the
  spool's tag. Clearing a single field brings back what the machine reports for it. Clear
  Spool is refused on the lane feeding a print that is running, paused or preparing, and the
  menu says why. On Happy Hare in Spoolman pull mode, clears and edits are refused with
  Spoolman named as the reason, instead of appearing to work. Printers without a filament
  system get Clear Spool in the slot editor too, and it works while Spoolman is offline.
- **Tool changers without ASSIGN_TOOL remap without beta features** - the job file is
  rewritten before printing whenever remapping needs it, and the plugin's Install row sits in
  Settings > Advanced without beta features. A dialog at startup warns when Moonraker is
  too old for remapping, and can be dismissed per printer. A rewritten file also has its
  `TOOL=` and `INITIAL_TOOL=` values remapped, and while your picks move a tool the mapping
  dialog warns about start-macro values it leaves alone.
- **Material types and your Material Temperatures changes live in one editable file** - the
  built-in material table (PLA, PETG, ABS and the rest) ships in the filament catalog, and
  your per-material temperatures and preheat macros move out of `settings.json` into
  user_filaments.json, next to any brands and products you added. You can edit it from Mainsail
  or Fluidd, change a built-in material, or add a material type of your own. Existing changes
  move over automatically on the first start.
- **The Filament panel is laid out in cards** - like the Controls panel. The Temperature card
  at the top left shows the same heater rows as Controls, with the nozzle row for the tool the
  panel acts on, above the temperature graph and the lane bars; tapping the row of lane bars
  opens the filament system screen. On the right, the Material card holds the presets, and
  the Operations card holds compact Load, Unload, Purge, Extrude and Retract buttons, with
  the row of heads and Pick up or Park on a multi-head printer. Cool Down is a fixed cell,
  dimmed while nothing is heating, so no button moves when a heater starts or stops; the
  cold-extrusion warning takes the Operations heading's line for the same reason. In portrait
  the cards stack in one column, the presets share one row with Cool Down in the Material
  card's header, and on a multi-head printer the lane bars move into the Operations header.
- **Heater rows read the target after the reading** - on the Controls temperature card and
  the Filament panel, each heater row reads like "Nozzle 205°C / 210°", the reading colored
  by its heating state and the target in smaller text, or "/ Off" when no target is set.
  Tap a row to open the temperature graph on that heater and set its target there. Where the
  card has room, the line under each row says Heating, At target or Cooling.
- **Filament system settings in the header** - the Settings button on the filament system
  screen is a gear in its header, so the action buttons below fill their rows evenly. On
  multi-head printers the Currently Loaded card also shows which tool is mounted.
- **Emptying a page in edit mode removes it from your saved layout** (#1638) - moving a page's
  last widget to another page, or removing it, deletes the empty page. The main page stays, and
  so does a page still holding widgets that are grayed out because their hardware is not
  detected.
- **Adding a page: a visible + past your last page, or a drag past either edge** (#1638) - the
  page past your last one carries a + you can tap to add a page, editing or not, and it stays
  reachable even when you have a single page. To grow a page out of a widget instead, long-press
  the home screen, pick up a widget and drag it past your last page: an empty page slides in, and
  dropping the widget there creates the page with the widget on it, in place, without the home
  screen sliding back to the page before it first. At your first page's left edge nothing slides
  in: let go with most of the widget past that edge and a new page is created in front, with the
  widget at its left side. At the 8-page limit there is no + tile and neither edge creates a
  page.
- **Canceling a print from a runout dialog asks first** - the runout guidance dialog asks
  for the same confirmation, in the same words, as the print-status Stop button.
- **The clog meter's labels each say one thing** - each label names the source, the reading
  or an end of the scale without repeating another, and severity shows as an icon rather
  than a repeated status word. The end labels show only for FlowGuard, which gives its bar
  more room.
- **Portrait ships the print-status widget in its Detailed layout** - the Library layout cuts
  off its last row of buttons on portrait screens. Detailed has the Job Queue button, so the
  queue stays reachable from the home screen.
- **Icons fit their tiles on large screens** - on the largest screen sizes the icons in
  widgets step down a size, so they sit inside their tiles.
- **Clog-meter wording is translated** - Clog, Auto, Manual, buffer, TANGLE and CLOG are
  words and are translated; FlowGuard and AFC are product names and stay put.
- **Print preparation progress needs nothing in PRINT_START** (#1234) - the phase-tracking
  toggle in settings, the installer's `--with-phase-tracking` flag and the plugin's
  macro-rewriting service are removed. The screen works out the current phase from toolhead
  movement and temperatures, which works on any printer without editing its config. A
  PRINT_START that still carries the old `HELIX:PHASE` markers keeps working. Uninstalling
  the plugin (the app's Advanced row, or `install.sh --uninstall`/`--uninstall-auto`) removes
  those markers from PRINT_START, backing up every file it edits first; the edit takes
  effect at the next Klipper restart. The app's Uninstall row tells a clean removal, a
  removal that left a config file needing a manual look, and an outright failure apart.
- **The 3D G-code preview works on slow boards** - on a Raspberry Pi 3-class screen a
  sharp 3D image of the whole model is ready in well under a second, and the preview
  stays responsive: while your finger is on it you see a lighter model that keeps the
  top, bottom and first layer and follows the drag smoothly, and the sharp image returns
  within about a second of letting go. During a print, each finished layer appears on its
  own instead of forcing a full redraw, and small files get the 3D preview during a print on
  low-memory boards too. In
  Auto, a file whose 3D preview would cost too much CPU shows its thumbnail instead, judged
  from how fast this board read earlier files, with a lower limit while a print runs, and
  preview work runs at the lowest priority so it yields to the screen and to Klipper. The
  preview draws on its own thread, so rotating it never freezes the rest of the screen, and a
  board that still cannot rotate it at about 7 frames a second switches to the 2D view for the
  rest of the session instead of stalling.
- **Rotate icon resets the 3D preview** - the rotate icon in the corner of the 3D preview, on
  the print file screen and on Print Status, is a button: tapping it puts the model back at
  its starting angle and zoom.
- **The 3D preview sits on the same background as the 2D one** - both use the shaded
  backdrop, so flipping between them changes only the picture.
- **Controls fits portrait screens** - on tall screens the Position card opens Motion when
  tapped and keeps Motors Off as an icon in its header, and the Cooling card shows Part Fan
  with its speed beside the other fans.
- **Tool lists say each number once** - counted from zero a tool reads "Tool 0", not
  "Tool 0 (T0)"; counted from one it still reads "Tool 1 (T0)".
- **Pick up and Park everywhere** - putting a tool on the carriage is called Pick up on every
  screen: the slot menu, the filament system's sidebar, the load picker and its steps.
- **The tool switcher is rebuilt for tool changers** (#1784, #1769) - a segmented strip that
  keeps every head a finger-sized target. The Home tile fits up to six heads, Park and the
  feeder in one row, shows a detail block when it has room, and can Park the mounted head;
  the picker is a grid of the same segments. Each head shows its filament color and
  material, a head you named in your config shows that name beside its G-code name ("Left
  (T0)"), and a tool changer fault marks every head but the mounted one. A paused print
  still offers a head change, behind a confirmation, and parking refuses a paused job.
- **Tool changes say "Switching to"** - the toast appears when the change is accepted, so a
  swap that then fails does not report success first.
- **Screens slide in from the right in every orientation** - in portrait too, with the back
  arrow on the left, the same as landscape.
- **Bigger buttons on 480x320 screens** - buttons are 44 px tall for easier taps, with
  dialogs, prompts and the filament sidebar fitted to match.
- **Outlined text fields** - text inputs, dropdowns and number spinners are drawn with an
  outline that keeps at least 3:1 contrast in every theme, dropdown lists stay opaque and
  center their rows, and an off switch's knob is muted (outlined white in light mode).
- **Progress bars have an outline**, so the empty part of the track stays visible on any
  card.
- **New icons** - flow shows a nozzle instead of a water drop, filament sensors show a sensor
  icon instead of a spool, and Spoolman shows the real Spoolman logo everywhere it appears.
- **Cleaner printer pictures** - four printer images lost their white backgrounds, the
  artwork is cropped to the printer, and printers without their own picture get new generic
  CoreXY art.
- **Tune steps flow finely** - the Tune flow buttons step by 1% and 5%; speed keeps 5% and
  10%.
- **Filament system settings** - every system's settings screens use the same switches,
  sliders and lists as the rest of Settings, with icons, a typed value for sliders, and the
  reason a setting is unavailable shown under it.
- **The color palette leads with filament colors** - the color picker opens with the colors
  people actually load: black, white, gray, red, orange, yellow, green, teal, blue, navy,
  purple, pink, brown, silver and gold, with fewer near-identical browns and yellows.
- **Filament catalog order** - both pickers list a family's base material first, then its
  "+" and filled grades, then the rest.
- **CFS lanes are named the way Creality names them** - the tool dropdown and the slot menu
  call CFS lanes "1A" to "4D".
- **Settings > Sensors** - every kind of sensor uses the same row, with one switch size.
  Chamber and dryer humidity chips and board thermistors, such as an extruder board's, are
  listed, and a probe you set to no role keeps it.
- **Delete Page is in the nav bar** (#2023) - in home screen edit mode the nav bar shows Add,
  Delete Page and Done.
- **View Full History is always on screen** - the button that opens the full print history
  sits in the Print History header, so it is reachable without scrolling on smaller screens.
- **Run Macro dialog** - fields sit in columns on wider screens, and the dialog sizes to its
  content.
- **A notification with no title leads with its message** in the notification history.
- **Translations fit and count right** (#1307, #1309, #1775, #1808) - rows of buttons share
  room so a long translated label takes space from short neighbors instead of clipping, and
  counts use each language's own plural forms, including Russian's. Print Tune's speed and
  flow, fan names, history cards, the filament status line and the PID fan hint stay within
  their space, and the theme preview, LED swatches and About paths fit a 480x320 screen.
  Dotted labels stay on one line, long macro descriptions wrap, and multi-word button labels
  use Title Case.
- **US English** - on-screen text uses US spelling (canceled, color, behavior) in English.

**Printer-specific**

- **K1: Creality Print keeps working** (#1468, #1637) - installing HelixScreen on a K1 leaves
  Creality's backend services running, so Creality Print can still reach the printer.
  Only the stock screen is replaced.
- **Cool Down works on the base K2** - including a K2 whose chamber has a fan but no
  heater. Your own customized cooldown macro is never rewritten.
- **The chamber heater owns the chamber reading** (#1465) - the U1's cavity sensor is the
  chamber reading even when a probe is named "chamber". A sensor you assign by hand still wins.
- **Snapmaker U1: a print with an unset spool is stopped before it starts** - a tagless spool
  with no material set is caught up front, instead of the printer heating and homing and
  then reporting a runout minutes later.
- **AD5M: no startup music** - playing it ties up the AD5M's single core, and it has killed
  prints. The AD5X keeps its music.
- **Forge-X: installs stay put** - HelixScreen installs outside the folder Forge-X cleans
  (either of its clean actions deleted the install), and an install that replaces an older
  one removes it, which could hold 81MB and put two screens on the display at boot. A
  Forge-X install refuses `--auto-update`, which would have wiped your preserved settings.
  An update with the installer restarts HelixScreen the way the printer does at boot,
  instead of leaving the screen dark until a reboot. The installer takes over the display on
  a fresh Forge-X too, and when a display change is still waiting for a reboot it says to
  reboot first.
- **QIDI: the stock screen stays off** (#1533, #1854) - it is disabled before it is stopped
  and kept off at every boot, however its service starts it, so it stays off after a
  restart. Installing sets aside only the screen software that is actually present,
  and restores only that.
- **QIDI Q2 presets** - the Q2 shares the Plus 5 and Max 4 chamber heater, sensor and aux fan
  presets.
- **Snapmaker U1 picture** - a new top-down view.
- **AD5M Pro fan names and sound** - aux is the side fan and chamber is circulation, existing
  Forge-X and Z-Mod settings are corrected, and the KlipperMod AD5M Pro ships with sound
  off.
- **AD5X loads during a pause** - a paused, homed AD5X can load, so the runout dialog offers
  Load first. A load refuses while filament sits at the toolhead, the refusal names homing
  only when homing is the reason, and the runout Load sends the right channel.

**Installing and diagnostics**

- **Uninstalling HelixScreen also removes its Moonraker plugin**, so Moonraker stops logging
  an error for it on every boot.
- **Self-built and forked builds do not send diagnostics** (#1410) - debug bundles and crash
  reports upload only from official release builds by default. `HELIX_DIAGNOSTIC_UPLOADS=1`
  turns them on.
- **Debug bundles explain a Klipper that stopped** - a bundle includes the kernel's
  out-of-memory and crash records and the launcher log with each exit code, says when the
  Klipper log predates the running Klipper and reads the live one, names any log read from
  the printer's disk with its age, and reports a Klipper that never reported a state as
  such rather than as a shutdown. It also describes the host: the Linux distribution, failed
  services, and other programs that might be fighting over the display (#1692).
- **Debug bundles carry a Wi-Fi link log** (#1190) - the app records signal strength and link
  changes as they happen, and a debug bundle includes a network section, for chasing
  dropouts.
- **The installer shows what it is doing** - it detects first, prints a plan (such as
  `Update <installed> -> <target>`, naming only the stock screens that are actually
  running), asks once before changing anything, and then shows each step as it runs.
  `--clean` lists what it will delete in that plan. The log survives failures, package and
  service output goes to the log instead of the screen, and `--dry-run` and `--verbose` are
  supported. `--uninstall` lists what it removed and what it kept.
- **Network library messages reach HelixScreen's log** (#1317) - tagged `[libhv]`, so
  connection problems leave a trace beside the rest of the log.

### Fixed

**Connection, settings and dialogs**

- **The sample Bed Level button runs a real macro** - the settings template and the
  configuration guide named a macro the helper pack does not define, so a button copied from
  them got `Unknown command`. They now use `HELIX_BED_MESH_IF_NEEDED`.
- **Printers that require a Moonraker login connect** - with `force_logins` enabled,
  Moonraker refused every request from the screen. HelixScreen now sends the printer's
  `moonraker_api_key` setting to that printer's Moonraker, over the WebSocket and every HTTP
  request, and to nothing else.
- **Enter the Moonraker API key on the screen** (#1929, #1930) - Settings > Connection has an
  API Key row, and the Change Host dialog a masked key field beside the host and port; saving
  reconnects with the key at once. The setup wizard shows the field when Test Connection finds
  a printer that requires a login. A printer that refuses the login now says so once, with a
  button to the key, instead of a toast for every request it refused. Add Printer has the same
  key field (#1970, #1971), and fixing the login on the Moonraker side clears the warning.
- **Editing a setting that appears twice in printer.cfg** changes the copy Klipper actually
  reads, the last one.
- **Stacked confirmation dialogs keep their own buttons and text** instead of taking the top
  one's.
- **The recovery dialog has a close X** in its header, and never shows a message from before
  the shutdown under its shutdown title.
- **Bluetooth, barcode scanners and label printers** - starting a new Bluetooth scan no
  longer crashes the settings screens or waits out the old scan, a scan that could not run
  says so instead of "0 found", and pairing works on systems whose adapter is not hci0.
  Barcode scanners with a generic name are listed, opening the scanner screen no longer
  freezes it for up to 5 seconds, and a USB label printer with an unreadable ID no longer
  crashes the app.
- **Small screen and dialog glitches** - a scroll that starts on a settings row leaves the
  row unpressed, a text field bound to a setting keeps its whole value instead of one
  character, a second notification panel no longer stacks on one sliding out, a screen is
  no longer closed twice by a canceled duplicate open, dialog buttons fill their rounded
  corners, the runout dialog's Resume button shows its accent and disabled states, and a
  broken printer database shows one notice instead of one per lookup. "Connection restored"
  is a success notice and no longer adds a second row to the notification history.

**Performance**

- **Faster drawing on every board** - the screen draws directly instead of through a separate
  render thread, which measured cheaper on every shipping board. The K1 family gains an image
  cache that cuts home screen drawing by about 14%, the home screen loads its printer picture
  at the exact size first instead of rescaling on every visit, and the theme skips rebuilding
  when nothing changed. The toolhead, spools, filament path and darkened backdrop draw with
  less work, and the screen redraws only what changed.
- **Faster startup and screen changes** - screens are built the first time you open them
  instead of all at startup (#1756); the home screen builds its printing views only during a
  print and keeps its tiles when you switch between printers with the same widgets; the
  print file list builds cards while the listing arrives and repaints only the rows that
  changed; the Macros screen reuses its rows, so a long macro list opens and scrolls quickly
  (#1748); and the filament system screens stay built between opens. Scrolling redraws only
  what moved.
- **Screens open with one redraw** - a settings or overlay screen redrew the whole display
  once before it appeared and again as it slid in. It is built off screen now, which saves a
  full-screen draw on every open on slower printers.
- **Screens give their memory back** - Motion, the temperature graph, the theme editor,
  calibration, PID, Z offset, touch calibration, the Printer Manager, LED and filament system
  screens free their memory when you close them. The print file list loads the first time
  you open it instead of at startup, and the bed mesh holds 0.75 to 1.5 MB less.
- **Less CPU while idle and asleep** - the screen no longer wakes the board about 115 times a
  second when nothing is happening, and hidden loading spinners stop animating. On a Creator
  5 Pro, sleeping use dropped from 3.5% to 3.0% of a core and idle use from 4.5% to 3.6%. A
  finished sound also no longer keeps the audio device busy every millisecond until the
  next one.
- **Home screen edit mode is smoother** - dragging, resizing and leaving edit mode no longer
  rebuild the page, and nothing is saved until your edits settle. The widget catalog opens
  faster, and the temperature graph stops sampling while its screen is not showing.
- **Raspberry Pi: GPU presentation where the Pi has a hardware renderer** (#1580) - the Pi
  package includes a build that hands finished frames to the GPU. The launcher uses it only
  when it finds a real hardware renderer, and falls back to the standard display path if it
  is declined or crashes. The Pi and x86 app starts whether or not the GPU libraries are
  installed, and uses them when they are.
- **A large G-code preview could freeze a Raspberry Pi 3 or older** - the 3D preview now caps
  its detail by what the GPU can draw. On a Pi 0 to 3 a file too big for that gets a 3D still
  drawn in layer bands, and shows as 2D only when the bands would be too coarse to read.
- **Tool remapping streams the file** instead of holding the whole G-code file in memory.
- **Turning animations off stops all of them**, including screen transitions and the heater
  icon pulse.

**Crashes and stability**

- **A thread crash always leaves a crash report** - a crash on any thread, including one that
  ran out of stack, leaves a report, where a background thread's crash used to leave none.
  On the K1, AD5X, Creator 5 Pro and K2, whose builds give each thread a small stack, every
  thread gets a larger one.
- **One Klipper fault, one dialog** - a shutdown that names its reason no longer stacks a
  Printer Error alert under the recovery dialog. While the recovery dialog is up, a further
  fault shows as a notification and in the notification history instead of another dialog.
- **Unplugging a touchscreen or mouse** could crash the app or leave input dead.
- **Small-memory boards** (#724) - a background thread that failed to start could take the
  whole app down.
- **AD5M Pro crash loop** - the watchdog could retry a crashing app forever with the screen
  dark. It now gives up and hands over to the system.
- **Crashes when leaving a screen** - a screen could react to a value that had already been
  freed; that whole class of crash is closed off across the app.
- **Background blur could crash at startup on some Allwinner boards** (#1742) - the blur
  behind dialogs opened a second connection to the graphics driver. It now uses the
  display's own.
- **A read-only install could abort at exit, or at startup after a crash** - failing to
  remove the app's crash-restart counter is now logged and passed over instead of stopping
  the app.
- **Recovery when Klipper never starts** - when HelixScreen connects while Klipper is not
  running at all, the recovery dialog opens instead of leaving the screen half loaded, and
  its Restart Klipper and Firmware Restart buttons restart the Klipper service. Restart
  Klipper on the Advanced screen does the same.
- **Clear message when Klipper cannot be restarted** - where Moonraker cannot restart
  services and HelixScreen has no recovery script, such as a Snapmaker U1 on the extended
  firmware, the restart buttons say to turn the printer off and on again instead of showing
  Moonraker's "not allowed" error.
- **A Klipper that failed to start looked shut down forever** - Moonraker never says when
  Klipper's startup fails, so after a restart the screen kept showing the old shutdown. It
  now asks until Klipper is ready or reports its startup error, and the recovery dialog
  shows Klipper's own message. When Klipper is already in error or shut down as HelixScreen
  starts, including a config Klipper cannot load, the recovery dialog opens with Klipper's
  message, also right after the setup wizard. A Firmware Restart from the recovery dialog is
  checked the same way, and where HelixScreen installed a recovery script for the platform
  (such as the Creality K2 and the Snapmaker U1) it runs once if the restart does not bring
  Klipper back, with a time limit so it can never hang. A mainboard switched off through a
  Moonraker power device stays quiet.
- **Rare crashes** when switching printers, closing screens with animations off, closing the
  G-code viewer while it loads, starting a sound at startup, and when a Moonraker, history or
  Spoolman reply arrives after its screen is gone.
- **Rarer crashes at shutdown and in drawing** - a display shutting down could leave its
  brightness timer firing on freed memory, a background canvas that failed to resize kept
  drawing from a freed image, and the sound player could be freed in the middle of a note.
- **Switching printers could crash** - switching from Home now waits for the screen to
  finish what it was doing, and the favorite macro color dialog no longer crashes after a
  live switch.
- **Display resize and shutdown crashes** (#1809, #1810) - the resize timer and teardown no
  longer touch freed screens.

**Network**

- **WiFi recovers from a slow or busy wpa_supplicant** (#2066) - one reply that came back
  late no longer makes every later WiFi command read the wrong answer, which emptied the
  network list and showed "scan failed" or "Internal WiFi error" on a working radio. Turning
  WiFi on also starts the WiFi system when it did not start at boot, instead of answering
  "WiFi system not ready" until a restart.
- **Switching printers** - a "Switching to" card covers the screen until the new printer
  connects, and leaving a printer that is still printing asks first. The previous printer's
  job, message, temperatures and history are cleared, a file or picture still downloading
  from it never lands on the new one, and a same-named file on each printer gets its own
  picture. The connection-failed prompt waits while the printer list is open, a printer
  name that arrives late goes to the right printer and never recreates a deleted one, a lost
  connection no longer saves the hostname as the name, and only the connected printer shows
  a connection dot. In Change Host, a blank address is refused and adding a printer you
  already have switches to it instead of duplicating it.
- **A printer whose address changed was retried at the old one forever** - the Android app
  hanging mid-print until a restart. The printer's name is looked up again before every
  reconnect.
- **Joining Wi-Fi while Ethernet is connected** (#1542) says so immediately, instead of timing
  out after 45 seconds.
- **Forget and the Wi-Fi switch are hidden where they cannot work** (#1429) - on systems
  whose network service cannot forget a network or turn the radio off, the buttons are no
  longer offered, in Network settings and the setup wizard.
- **Joining a hidden Wi-Fi network shows Connecting** while it connects.
- **The Ethernet row keeps refreshing** (#1976) - closing Network settings while it was
  checking the cable left the Ethernet row frozen the next time you opened it.
- **Sheet labels over IPP** (#1851) printed blank pages; they print again.

**Android app**

- **Android's backup saved nothing** - it tried to save the whole app, far over its limit.
  It takes just your settings now, so a phone restored from a backup gets them back.
- **Printers on Wi-Fi with mobile data on** - connections to printers on your network go over
  Wi-Fi, while updates and everything else still use whichever network reaches the
  internet.
- **Motion screen crash after a rotation** - resizing or rotating the window could leave the
  jog pad pointing at freed memory.
- **Telemetry never sent from Android** - with telemetry turned on, nothing left the device.
  It now sends, the same as on every other platform. Telemetry is still off unless you turn
  it on.
- **The app took over 200 MB more storage than it needed** - test prints for the simulated
  printer and font source files came along with every install. They are left out of the APK,
  so the download is smaller, and an update removes the copies an older version unpacked.

**Filament systems**

- **Filament stuck in the toolhead with no slot claiming it could not be unloaded** (#1324) - on
  the AD5X and the other systems that can tell, the filament sidebar's and Filament panel's
  Unload now pulls it out of the active head, and a slot's own Unload grays
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
- **A Happy Hare fault often showed no recovery popup** (#1323) - a fault during a print, or a
  load or home that failed outside one, now always opens the popup with Happy Hare's own reason,
  and Happy Hare's own error notice closes behind it. Outside a print it offers no Resume and
  leads with Recover. The slot error marks follow Happy Hare's real pause, so resuming or
  canceling clears them.
- **Turning Happy Hare's MMU motors on homed the MMU** - the Motors toggle now only powers the
  motors.
- **Filament details you entered on a Happy Hare lane were gone at the next launch** - brand,
  spool name, weights, color name and the catalog product you picked were kept on screen for
  the session and never written down, on every Happy Hare printer. Clearing a lane had the
  mirror-image problem: the lane emptied on screen and the old details came back on the next
  start. Both stick now.
- **The old spool's brand stayed on a lane** (#1672) - when Spoolman dropped a spool, or
  another tool (Mainsail, a macro, the MMU's own screen) swapped one, the outgoing spool's
  details lingered. Seen on AD5X IFS and Happy Hare.
- **Snapmaker U1: Unload could be unavailable, or the filament system stuck on Unloading** - a
  head fed to the nozzle by purging can now be unloaded, picking a tool no longer reads as
  loaded, and a leftover unload state after a restart no longer opens a runout grace window
  or ends a load running on another lane.
- **Creality CFS** (#1623, #1625) - restoring the slot mapping at startup refused every slot
  on a box whose size was not yet known, and threw away the record a reattached box needed.
  Clearing a slot left a dangling Spoolman link.
- **Creality CFS: a tag's raw record showed as the brand** - a tagged spool whose material
  code HelixScreen doesn't know showed a 40-character string as its brand. Creality spools now
  read as Creality, and other tags show no brand. A box dropping off the bus is not treated
  as a new spool.
- **AD5X IFS** (#1631, #1654, #1626) - your own color edit was recorded as if the printer had
  reported it, colors set from the stock screen were misread, and the runout warning never
  lit.
- **No ACE Pro slot ever showed as loaded on the Kobra S1 fork** (#1069) - with the ACEPRO
  driver, slots, colors and materials showed but the loaded slot never did. It shows now,
  along with the fork's material and dryer readings. Confirmed from a user's live captures;
  not yet verified on the hardware itself.
- **An ACE load the driver declined** (#1676) still marked the slot as loaded.
- **ACE stuck on Unloading** (#1720) - an unload that never reached the printer left the ACE
  marked busy until restart.
- **AFC in toolchanger mode** draws its real toolheads (a Box Turtle as four heads, not one
  nozzle behind a hub).
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
- **The MMU's selector, buffer and bypass boxes ignored taps right after the screen opened** -
  a tap on the filament path now lands on the box you see as soon as the panel has slid in.
- **The clog threshold became unreadable exactly when it mattered** - the danger shading and a
  warning fill are both drawn in the danger color, so a reading past the threshold merged the
  two into one red block. The threshold is drawn on top of the fill now.
- **AFC toolhead sensors showed up as runout switches** - the toolhead sensors of each AFC
  extruder were listed in sensor settings and the setup wizard as standalone runout sensors.
  On AFC builds where the toolhead sensor is a virtual switch, turning it off there would
  have told AFC the tool was unloaded. They are now left to AFC, like its other sensors.
- **The load step list skipped its first steps after an outside operation** - when a tool
  change started from a macro or the console ended, the next load could start with its
  heating and early steps already passed, or keep showing the last operation's "Load
  complete". A tool change started from a macro also no longer jumps back to its first step
  when the step list appears.
- **The AFC fault diagram in a prompt** showed every stop point at once after switching
  printers; it names the fault in one line now (Stopped, or Jammed at the toolhead).
- **Unit cards on the Multi-Filament overview show every lane** - the filament color bars span
  the whole card instead of sharing it with the temperature and humidity box, which cut them
  down to one or two bars and a "..." in place of the lane count. When the card has room, the
  temperature and humidity sit on one line under the bars; when it does not, the unit's own
  screen still shows them. A unit with one lane reads "1 lane".
- **Hubs on the overview only where lines meet** - the Multi-Filament overview draws a hub
  only where two or more units feed the same toolhead, as one box where their lines join, and
  a hub with a buffer as one Hub+BUF box. A unit that feeds a toolhead on its own goes
  straight to it with its hub sensor on that line, as does an AFC unit whose lane runs
  directly to its own toolhead, such as a toolchanger's one-lane Tools unit. Each line runs
  at its own height, and lines cross or touch only where units feed toolheads in the opposite
  order. Hub boxes fit their labels, every bend is rounded, and the bypass spool is drawn the
  same size as the spools in each unit. A unit whose lanes feed no tool ends at its hub, in
  the unit view as in the overview.
- **The AFC overview follows routing changes** - when an AFC lane moved between a hub and a
  direct route, or the printer reported its extruder count late, the overview kept drawing the
  old route. A lane that runs direct on a single-extruder printer is not treated as its own
  toolhead, so loading it unloads the current lane first.
- **Parked heads on an AFC toolchanger** - with every head parked, the screen showed the first
  tool, or the tool whose lane AFC last loaded, as mounted; it shows no tool on the carriage.
  A lane jam or timeout no longer marks every parked head as faulted in the tool switcher.
  AFC printers with a single toolhead are unchanged.
- **AFC lanes keep their temperatures when you unlink a spool** - unlinking a Spoolman spool
  made AFC forget the lane's nozzle and bed temperatures, so the next load heated to AFC's
  default for the material. On AFC v1.2.0 and newer, HelixScreen sends the lane's
  temperatures back to AFC after the unlink.
- **One dialog on a Happy Hare 4.2 pause** - HelixScreen closes Happy Hare's own Pause Notice
  when its recovery dialog opens, as it does with the Error Notice of earlier versions, so the
  two dialogs no longer stack, and the recovery popup carries Happy Hare's hint about what to
  do next under the reason.
- **Clear Spool resets the gate temperature on Happy Hare** - a cleared gate goes back to the
  printer's default extruder temperature on every Happy Hare version. Newer Happy Hare builds
  keep a gate's temperature unless it is named, so a cleared gate kept the old spool's.
- **Creality CFS loads and run-out spools** - a CFS load cannot be stopped once it starts, so
  no Cancel button is offered during one. When the CFS has reported a spool ran out and an
  unload leaves filament in the toolhead, the error says the CFS cannot pull the leftover
  back and asks you to remove it by hand, instead of suggesting the cut or retract may not
  have finished.
- **Picking up a tool onto an empty carriage shows the pickup steps** - on a Medusa HC,
  picking up a tool with no tool mounted listed the steps of a filament load, including Heat
  nozzle and Purge. It shows the steps of the tool change. Only the list was wrong: the
  pickup never heated or purged.
- **AD5M: the Spoolman spool editor showed a Print Label button that did nothing** - the
  AD5M build has no label printing, and the button is hidden there now.
- **K2 with and without a CFS** (#1904, #1933) - on a K2 with no CFS, the toolhead filament
  sensor shows up in Settings > Sensors and can watch for runout. A K2 that has a CFS no
  longer looks single-spool while its CFS is still answering at boot.
- **A load macro HelixScreen does not recognize runs as it is** (#1819) - Load on an unhomed
  printer no longer homes first before a `LOAD_FILAMENT` macro HelixScreen does not know, so
  a Voron whose macro only extrudes is not homed.
- **After a restart, a print's tool remap is still put back** (#1684, #2032) - the tool map you
  changed for a print is put back when that print ends, even if HelixScreen restarted during
  the print or was updated in between, and never in the middle of the print. If the filament
  system refuses the restore, it is retried once it will accept it, and a tool that was
  unmapped before the print stays unmapped.
- **Filament Mapping keeps what you picked** (#1859, #1862) - rows you left alone follow the
  printer's tool map even when its tool table arrives or changes after you opened the file,
  and a guessed free lane is never sent as a remap. A pick survives the file's colors
  arriving late, OK in the remap dialog keeps only the rows you changed, flipping auto color
  mapping takes effect on the open file, and a hidden mapping card sends nothing. On a
  multi-lane printer the mapping card appears even when the filament system reports in after
  the file opened.
- **Mapping checks judge the lane that prints** - OpenAMS and ACE are judged on the lane each
  tool really loads from, an Auto pick is judged on its routed lane instead of reading as
  unmapped, and printers whose firmware picks lanes at print time (Z-Mod) do not claim one.
  A tool with no lane assigned says so in the pre-print check, instead of asking you to load
  filament.
- **Filament diameter comes from your printer** (#1504) instead of assuming 1.75 mm,
  including for filaments HelixScreen creates in Spoolman.
- **Your Standard Macros choice for Load and Unload is used** (#2067) - a Load or Unload macro
  you pick in Standard Macros runs on filament systems that otherwise load or unload by
  hand, instead of only showing the manual message.
- **A stray template tag in a macro no longer fails a load or unload** - text Klipper
  reports as an unknown command shows a warning, and the operation carries on.
- **A runout with filament still at the feeder** says the motion sensor is stuck, instead of
  calling it a runout.
- **Filament fault dialogs** (#1838) - faults use the standard dialog with its usual buttons,
  ESC and the backdrop close without acting, and a fault with no recovery actions offers OK.
- **Unavailable buttons stay unavailable** - a calibration or device button the filament
  system cannot run right now is grayed out with its reason instead of sending its command,
  and machine-moving actions wait for the print to end.
- **Cancel only where it works** - QIDI Box, Snapmaker U1 and Multi-ACE no longer offer a
  Cancel or Abort that does nothing. Elsewhere the status line has a Cancel while an
  operation runs.
- **Happy Hare loads and unloads flashed Idle** (#1057) before the printer reported them. A
  preheat from the filament sidebar also cancels the previous operation's cooldown, and says
  so when a load is dropped.
- **AFC tool numbers past the lane count** - a lane running as T7 on a four-lane unit can be
  remapped, restored and changed to, remapping two lanes swaps their tools as AFC does, and
  lane settings show the tool a multi-tool lane is running.
- **AFC tool changers** (#1824, #1850) - tapping a parked head only mounts it instead of
  swapping filament, a Load that needs a lane swap really loads, and a head missing from the
  toolchanger is warned about instead of leaving the screen on Selecting for five minutes.
- **AFC tool changes from Orca** (#1848) - files whose tool changes carry extra parameters,
  as Orca's AFC Change Filament G-code writes them, show every tool in the preview and the
  filament mapping.
- **AFC faults during a paused print** - an AFC load that failed mid-print paused it without
  a dialog. It now opens the loading error dialog, and recovery ejects the lane of the
  newest fault. An old AFC error no longer stays in the status line through a print that
  runs fine, and an AFC error that arrives with its prompt no longer pops a toast on top of
  it.
- **A recovered AFC lane loses its error marking** (#2049, #2050) - a successful lane reset
  clears AFC's leftover error, a lane that still shows one has Clear Error in its menu, and a
  faulted spool glows red.
- **MedusaHC loads and swaps** - a Load inserts the filament at the docked hot end first,
  heats the right tool, and trusts a swap only once the dock sensors confirm it; a failed or
  canceled run turns the heaters off and never moves a carriage the sensors cannot vouch
  for. The dock sensors rather than the tool number say which tool is on the carriage, swaps
  are refused while a dock sensor fault stands, and a tool docked by hand after a failed
  change does not leave a sensor error on screen once the docks agree. A load waiting for
  you to act waits 3 minutes instead of about 19 seconds, a run ends cleanly when Klipper
  restarts or the connection drops, and a print cannot start while one runs. Unload is
  offered for docked heads and ends by saying which tools to pull the strand from, the
  status line does not read idle between swaps, and one failure is reported once.
- **ACE Feed Assist** (#1853) - the switch shows what the driver is actually doing, flips
  back if the driver does not take the change, and is grayed out with no tool loaded.
- **ACE load status** - a unit's own busy and ready reports no longer end a screen load early
  or wipe an error the unit reported during it.
- **ACE read the wrong slots** (#1678).
- **Creality CFS** (#1820) - a load or unload fault clears once the toolhead sensor shows the
  problem was fixed by hand, and on the community box firmware a reprint sends its filament
  mapping too and a start with nothing to map clears a stale one.
- **Creality CFS with a box switched off** (#1464) - a box that is off shows as Not
  connected: its card is dimmed, its bays are left out of counts and lane choices, and tool
  changes and Clear Spool refuse them. Bays are numbered the way the firmware numbers them,
  so a Klipper restart with one box off no longer shifts spools and colors onto the wrong
  bays, and a box behind a switched-off one still works.
- **Purge uses the waste chute on Creality printers with a CFS** (#2065) - without a purge
  macro, Purge on the Filament panel moves the nozzle over the CFS waste chute before
  extruding and parks it afterwards, the way loading does, so filament no longer lands on
  the bed or a paused print. Printers without a configured chute position, or not yet homed,
  purge in place as before.
- **No false runout after a print on a K2 with a CFS** - the CFS pulls filament back after
  every print, which raised "Filament removed". A runout during a print still alerts.
- **RFID-tagged third-party spools in a CFS keep the details you give them** (#2048) - a
  brand you set is no longer replaced by "Creality" or by a raw tag id the box could not
  decode, and only a tag written in Creality's own layout links the lane to a Spoolman spool,
  so a third-party tag no longer links it to an unrelated one.
- **QIDI Box ignored live updates** - after startup, changes on the box never reached the
  screen.
- **QIDI Box with newer Box firmware** - slots set on the Box's own screen (Box firmware
  02.03.01.21 and later) showed no color or material; they show both, and the same spool is
  no longer mistaken for a new one before the Box has described it.
- **Z-Mod AD5X after a failed filament change** (#2086) - Unload and Cancel come back as soon
  as the printer goes idle instead of after three minutes, a new lane no longer shows "?" as
  its material, and one failure shows one message.
- **Spoolman gets the slicer's material** - saving a lane filed a precise type such as ABS+
  as Spoolman's material with no temperatures, so OrcaSlicer fell back to Generic PLA.
  Spoolman gets the base material (ABS) with nozzle and bed temperatures, and a linked AFC
  lane sends the same base.
- **No silent reuse of the wrong Spoolman filament** - an existing filament is reused only
  when its material, color and name match exactly; a close match asks "Use existing
  filament?".
- **Spoolman active spool** - linking a spool to the loaded lane makes it active at once, the
  external spool only becomes active while it feeds, and unlinking clears only its own
  spool.
- **The spool picker marks only the linked spool** - an unlinked slot showed the most recent
  spool as its current one. Unlinking a slot as a different filament also no longer keeps
  the old spool's weight.
- **Picking a spool on a five-slot or larger system selects that spool** - a tap could select
  the neighboring spool, and the slot numbers were hidden.
- **More spools in the picker on small screens** - on 480x320 and similar panels the spool
  picker's rows are shorter, so about five spools fit on screen instead of three.
- **Slot picker width** - the picker sizes to its rows instead of a fixed 480px, so short
  choices no longer leave it half empty and long lane names are no longer cut off.
- **A lane with no color stays uncolored** - clearing a lane on a CFS, AD5X, Snapmaker U1 or
  tool changer no longer records it as gray for Mainsail and other screens.
- **A loaded black or gray spool shows on the nozzle** instead of the bare metal tip.
- **A gray picked in a Spoolman color picker is saved** instead of reading as no color, and
  the picker's black and other near-blacks are named Black (the darkest Deep Black) instead
  of Dark Gray.
- **The QIDI Box shows its own logo** (#1514) - instead of the generic filament system chip.
- **The home spool row** stacks the spool's name over the spool and fits itself again when
  the tile is resized.
- **Spoolman QR codes** (#1744) starting `web+spoolman:` in any mix of capitals are
  recognized.
- **The default ABS preset reads ABS/ASA**, since the two print in the same range.

**Printing and preview**

- **The file's material named a tool the print never uses** - the material in a file's
  details came from the first filament the slicer listed, even when the print only uses a
  later one, so a file printing only ABS-GF read "ABS". It now lists the materials of the
  tools the print actually uses, such as "PLA, PETG". Print history's job details and rows
  name each material once in the same way, and a long list wraps instead of being cut off.
- **The prep time estimate on print details** - the "~6 min prep time" line counts heating
  from the current temperatures up to the file's own first-layer temperatures, and matches
  the countdown shown once the print starts. It learns from how long your printer actually
  took, with separate timings for a cold bed and a warm one. Turning a pre-print option off
  takes its time off the estimate; turning one on adds time only when your recent prints
  skipped that step. The line waits for the file's details to load, shows the first time you
  open a file's details rather than only after you change an option, and is not shown for
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
- **Print preparation time estimates no longer balloon** - on every printer, the estimate
  could jump to two or three times the real preparation once the bed reached its last slow
  degrees. It now stays close to how long preparation really takes. During the bed mesh the
  time left no longer sits at a few seconds for minutes on end, and recording a new start
  keeps the earlier timing history instead of losing or doubling it.
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
- **The budget preview on small boards** (#1555) keeps its tool colors and framing.
- **A layer of a large file that failed to read** drew as a permanent gap in the 2D preview.
  It is retried, and skipped with a log warning only if it keeps failing.
- **Deleting a print history record asks first** (#1373), and **View Timelapse** opens the
  timelapse browser and plays videos stored on the printer.
- **The unread history badge** (#1525) was cleared by a history screen you were not looking at.
- **K2 Plus: saving config flashed "Printer Shutdown"** during the normal restart.
- **Sliced for a Different Setup named a toolhead the print never uses** - a multi-tool
  file lists a nozzle for every toolhead, so a different nozzle on an unused one raised the
  warning. The fit check now judges only the toolheads the file prints with.
- **Reprint on Print Status runs the pre-print checks** - Reprint checks the file it
  reprints the way Print does from the file list: whether it fits the printer, enough
  filament, an empty slot, a material mismatch, an unassigned tool and an open door. A check
  that warns asks first, and Cancel starts nothing. A file the printer cannot read in time
  ends with a Failed to reprint message instead of a button that never comes back.
- **No second heater shutdown after a print error on Jacobean's K2 Plus firmware** - that
  firmware turns the heaters off itself on an error, so HelixScreen leaves it to the
  firmware there. (thanks @Jacob10383)
- **Bed mesh shows in the pre-print checklist during an adaptive mesh** - on printers
  without a dedicated start profile, the checklist marks Bed mesh active when Klipper starts
  an adaptive mesh inside the start macro.
- **Part Fan slider sets the part fan** - on printers whose part cooling fan is a named fan
  rather than the plain fan, the Controls slider changes that fan instead of a different one.
  On small screens its knob is no longer cut off at the ends or along its top and bottom.
- **Timelapse works after setup** - when Klipper has not loaded the timelapse macros,
  HelixScreen adds them to its own helix_macros.cfg and offers Restart Klipper, on the setup
  screen and on Timelapse Settings, or shows the line to add to printer.cfg when it can't.
  Before, a new install captured no frames until printer.cfg was edited by hand. Timelapse
  Settings shows the plugin's real values instead of the defaults, Timelapse Videos shows
  the preview image saved with each video, and when HelixScreen runs on the printer it puts
  an older moonraker-timelapse checkout back on its main branch when that is safe, so
  Moonraker can update it again. The Print Status timelapse button shows the plugin's real
  setting instead of reading off until tapped, and turning timelapse on in Settings is no
  longer turned back off at the next print start.
- **Fan readouts follow the active tool on tool changers** - where each tool has its own part
  and hotend fan, the Part and HE readouts on Print Status, the fan on the home printer
  picture, the Fan Speeds widget and the Part Fan slider follow the tool that is printing,
  and the Aux readout never shows a tool's own fans.
- **Resume while the printer reheats** - Resume no longer reports a failure or offers the
  button again while the printer is still reheating to resume, which on Z-Mod printers can
  take several minutes after a filament error. A warning the printer prints during that
  reheat no longer re-enables Resume either, so a second tap cannot start the resume
  sequence again in the middle of the print.
- **Creality K2: the bed mesh ran twice** - on stock firmware and with the k2-improvements
  mod, the pre-print mesh is skipped when the file's start G-code already calls the same
  command, and only then.
- **Print Status shows the chamber temperature with the chamber heater off** - the chamber
  chip appeared only while the chamber heated or was above 50°C, so a QIDI Q2 printing PLA,
  or a printer with only a chamber sensor, never showed it. It now shows whenever the
  printer has a chamber.
- **K2 pre-print steps** - the Creality K2's preparing checklist no longer marks Purge done
  while the nozzle is being cleaned. The nozzle clean parks over the purge chute and pushes a
  little filament to check flow, which read as the purge; the checklist now keeps it under
  Cleaning nozzle and shows Purge once, after the bed mesh, when the real purge runs.
- **Temperature changes during an AD5X print start** - a temperature, fan or light change
  while the start macro runs its bed mesh is sent once the macro finishes, instead of timing
  out with "Failed to set temperature", and a jog in that window is refused instead of
  queued for minutes.
- **Jogging beside other apps** - jogs are no longer let through a bed mesh or other long
  command started from Mainsail or Fluidd, and still work during the app's own homing.
- **Chamber warm-up says so** - while a print waits for the chamber, the pre-print screen
  says "Heating chamber" instead of "Heat soak", which is kept for an actual soak.
- **Two start warnings at once** - the second dialog opened behind the first; it now waits
  for the first to close.
- **CFS on the K1 series** (#968) - the fit check before a print allows for the 5 mm of Y
  travel the CFS upgrade takes on a K1, K1C or K1 Max.
- **Action prompts behave like Mainsail's** - closing a printer prompt on screen ends it on
  the printer, each button group gets its own row, and secondary buttons use the secondary
  color.
- **The Light button on Print Status turns on your light** - it drove the first addressable
  LED on the printer, so a Voron with a Box Turtle toggled its corner indicators instead.
- **"Print starting..." stayed on the home print card after a cancel.**
- **The objects button on Print Status** appears and hides as the printer's
  `[exclude_object]` support is found or changes.
- **The layer line after a cancel shows the last layer printed** (#2055) - after a canceled
  print, Print Status shows the height of the last layer printed instead of the height the
  head parked at.
- **Reprint after a print from a rewritten file** - a print HelixScreen started from a
  modified copy reprints the original file after a restart too, shows in history as a print
  of the original, and a print another client started the same way is followed.
- **Pre-print options stay with their printer** - stored option settings reset when the
  printer's hardware changes, and an option the firmware already handles no longer causes
  the file to be rewritten.
- **Timelapse frame counts** start over with each new print, and survive joining a print
  that is already running.
- **G-code previews belong to the right file** - two printers with a same-named file no
  longer share a preview, a stored preview whose size no longer matches the file on the
  printer is fetched again, and closing and reopening Print Status mid-download joins the
  running download instead of starting a second one.
- **The 3D preview** uses the theme color for a file with no color information, matching
  2D, instead of a rainbow height gradient, and filament colors survive switching to 3D
  while printing. The Building label shows real progress and is translated, and the spinner
  shows while a switch to 3D builds.
- **Files on a USB stick show their details** - the preview, pre-print checks and thumbnail
  read the file on the stick instead of waiting out a timeout, Cura's JPEG thumbnails show,
  and listing the stick no longer freezes the screen.
- **Print file details** - an open file's details update when a new analysis of the file
  lands, and details lost with a dropped connection are asked for again instead of being
  worked out from the G-code.
- **Thumbnails are sturdier** - a cached thumbnail that is not a real image is deleted and
  made again, downloads land whole or not at all, a refresh keeps a local file's thumbnail,
  leftovers from a crash are cleaned out of the cache, and free space that cannot be
  measured is not treated as a full disk.
- **Print file cards fit** under the Recently Printed banner instead of clipping the bottom
  row.
- **Print history dates** (#1865) - Started and Ended show the date over the time instead of
  clipping.
- **Print history after viewing a job** - going back from a job's details to the full
  history list shows the list where you left it, instead of a blank screen.

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
- **Bed screw arrows on the AD5M and AD5X** (#1860) - they were reversed there; they match
  Klipper's console now, as on every other printer.
- **Z-offset kept showing a pending delta after a successful save**, could not tell a
  clamped-to-limit adjustment from a failed one, and would clear a valid stored step size on
  an out-of-range write.
- **Z offset saves on printers that home Z on a switch** - on a printer whose Z homes on an
  endstop switch, such as a stock Voron 2.4 or an Ender with a BLTouch used only for the bed
  mesh, Z offset calibration and Save set the Z endstop position, so the change survives a
  restart. A delta without a probe adjusts the offset for the session only.
- **MPC calibration names a tool's part fan on tool changers** - switching a tool to MPC
  writes its part fan as the cooling fan when the tool names that fan without the
  `fan_generic` prefix.
- **The screensaver no longer stops a calibration** - heater (PID and MPC), pressure advance,
  Z-offset, bed mesh, input shaper and screws tilt calibrations keep running behind it and
  still save their results, and waking the screen shows their progress instead of a frozen
  screen. A calibration that finishes behind the screensaver saves on its own, which
  restarts Klipper, just as it does with the screen on.
- **Input shaper refuses to start with too little free memory** (#1738) - a resonance sweep on
  a board short of memory could wedge Klipper, as seen on the Centauri Carbon. When Klipper
  stalls anyway, the message says to power-cycle the printer.
- **Input Shaper results** - the comparison table fits its rows, the legend no longer hides
  behind Save, and the filled curve draws without stripes.
- **A failed Z-offset run closes its probe session** (#1994) - Mainsail and Fluidd no longer
  keep a manual probe dialog open after the failure, and the error stays on screen.
- **Z-offset tools respect per-head firmware** - on the Creator 5 and Snapmaker U1,
  HelixScreen does not offer the single-offset calibration screens, and the Tune nudge says
  it is saved for the mounted head only.
- **Calibration during a pause turns its fan off** - a calibration started while a print was
  paused left its fan running afterward.
- **Beacon accelerometers** found only through the resonance tester no longer drop out on
  every second hardware check.
- **Probe settings left unset show Klipper's default** and no longer save the word
  "default".
- **Chart axis labels no longer clip** - on the Input Shaper results graph and the belt tension
  chart, the vertical percent labels and the "100 Hz" style labels at the chart edges were cut
  off on screens where the text is large. The space for the labels now follows the text size,
  and the labels stay inside the chart.

**Home screen**

- **Tapping the lights quickly said the printer was busy** - each command keeps the printer
  briefly busy, so a few quick taps on an idle printer brought up "Printer is busy - your
  change will run when it's ready". The message now appears only when something other than
  your own commands is keeping the printer busy, such as a bed mesh started from elsewhere.
- **The printer picture** showed a generic silhouette until restart when detection finished
  late (#1552), on the K1 family drew blank for up to a second on first show, and could stay
  blank until restart if the screen changed while the picture was still being prepared.
- **The trash button no longer deletes by accident** - in edit mode, a drag that started on
  it, a long press, or the second tap of a double tap deleted the widget. The trash,
  configure and delete-page buttons act only on a plain tap that ends on them, and the trash
  and configure buttons ignore taps for a moment after they appear.
- **Power tiles show the right device** - a power tile switched to another device no longer
  shows the old device's ON or OFF under the new name, and a tile that lost its device's
  status gets the device's name back when the status returns.
- **Widgets did not adapt to the panel they were on** - on a large panel a one-cell widget
  held text too big for it and clipped: fan names were cut off, and the active spool tile
  showed one letter per line. The temperature graph wrapped "300" onto two lines and ran
  over its time labels.
- **The bypass tile works** - it could be added but never appeared, and it showed as engaged
  whenever the printer did not report bypass at all.
- **The Detailed print-status card blanked for the rest of the session** if you removed the last
  print-status widget and added one back.
- **Narrow widgets could only be resized, never dragged** - on a narrow widget every touch
  landed on a resize edge.
- **Tapping Done on a later home page slid back to the first page** (#1638) - leaving edit mode
  with a second or later page showing scrolled the home screen back to its first page. The page
  you were on stays on screen, and pages change only when you swipe or tap a page arrow.
- **A one-page home slid onto an empty page** (#1638) - after tapping Done on a home with a
  single page, a sideways swipe slid onto an empty page with a "+" on it. It no longer
  does.
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
  or was dismissed, a drag, a slider or a scroll anywhere else on screen was canceled mid-way.
  Only a press on the toast itself is canceled now.

**Display and touch**

- **Picking a theme froze the screen for about two seconds** - every pick in the theme
  explorer, and Apply, drew the theme twice. Each now draws it once.
- **Backing out of the theme explorer kept the previewed light or dark mode** - flipping Dark
  Mode while browsing and then leaving without Apply kept the flipped mode. Leaving now puts
  back the theme and mode you had.
- **The Nord theme looked out of date and lost your edits on update** - every install carried
  an old copy of Nord that hid the current one and was rewritten on each update. That copy is
  removed on the next start; a Nord you edited yourself is kept.
- **The number keypad was cramped on 480x320 and 480x272 screens** - it is wider there, its keys
  fill the height, heater keypads are titled with the short heater name, and header titles
  shorten with dots instead of wrapping.
- **The number pad puts 1 2 3 on top, like a phone** - backspace moves to the bottom left, and
  on whole-number fields such as temperatures a confirm key sits bottom right, where the
  decimal point would be. The unit (°C, mm) now sits inside the value field, which spans the
  keys. Prefer 7 8 9 on top? Settings > Touch & Input > Number Pad Layout > Calculator.
  Fields that take negative values get a +/- key, the field's name shows above the value,
  and the keys keep the same shape on every screen size.
- **Color swatches were small targets** - every color you can pick (the filament color
  grids, the color picker, the LED color rows, the theme editor) now has a full fingertip-sized
  touch area, and neighboring swatches no longer steal each other's taps. The LED preset row
  wraps to a second line in portrait instead of running off the edge.
- **Touch stops working on some Pi 5 screens** (#2069) - when a USB touch controller resets,
  as some HDMI touchscreens do when the picture changes, HelixScreen reopens it instead of
  losing touch until a restart.
- **Touch calibration on a rotated screen** (#1714) - canceling or retrying no longer brings
  back an old touch range, and the preview uses the range it just worked out.
- **Screens rotated by the kernel get touch right on first boot** (#1428).
- **The rotation probe asks again if nobody answers** - with no tap during the first-boot
  probe, the screen was saved as unrotated and never asked again. It asks again at the next
  boot, keeps the backlight on while it asks, and picking a rotation in Settings ends it.
- **Taps** - slow taps on folders and Home tiles register, and releasing a long press on a
  macro, file card or speed tile no longer also taps it.
- **Slow screens scrolled short** - a flick glides by the time that passes rather than by
  the frames drawn, so a screen that draws slowly scrolls as far as a fast one, and the
  scroll settings reach every touch input as soon as they change.
- **Waking the screen keeps what was open** - the filament runout guidance, a pending print,
  QR scans, the spool wizard, touch calibration, unsaved macro visibility edits and the
  timelapse install wizard are still there after the screen wakes.
- **Camera views** - closing a camera view no longer freezes the screen for up to five
  seconds on a camera that stopped answering, and two views of the same camera take turns,
  so a camera that serves one viewer at a time keeps streaming.
- **The WiFi switch** takes the first tap, stays put while the radio changes, and finishes
  the change even if you leave Network Settings.
- **Colors set in a screen's layout survive a theme or dark/light switch** (#1735), and
  light mode text is readable on Motion's selected tab, the jog pad and the bed mesh labels.
- **Large Chinese and Japanese fonts load quickly** - the font is read in large blocks
  instead of millions of tiny reads.
- **Long values in info rows** wrap inside the row instead of clipping or breaking the label.
- **Small and portrait screens** - the Controls screen, Print Tune at 272x480, the filament
  dryer controls and the printer picker fit their space.
- **Dialog buttons fit on 480x272 screens** (#2024), and every row of a stacked menu responds
  across its full width on portrait and taller screens.
- **No page swipe with the paging arrows** (#1978), and **the keyboard has an underscore key**
  (#1977).
- **Screensaver settings** - builds without screensavers hide the screensaver rows and Test
  button, and the dim and sleep rows follow one rule.
- **`M300` stays silent during a print** - on a printer whose only speaker is its `M300`
  beeper, such as the KlipperMod AD5M Pro, a tune played mid-print stalled the toolhead. A
  paused or idle printer still beeps, and a preset's sound default applies at once.
- **The first-run tour waits** while a dialog, such as a power-loss resume, covers Home.
- **The time zone follows the system until you pick one** - a screen whose time zone was
  never set showed UTC.

**Printer identification**

- **Fans in the Printer Manager** - the Fans capability chip never appeared on any printer.
  It now shows when the printer has controllable fans besides part cooling, with their count.
- **Extra heaters counted as tools** - a heater named like `extruder_mixing` could turn a
  three-tool machine into a four-tool AD5X.
- **Creality CFS machines** (#1498) - plain and CFS K1 variants, and the K2 Plus and Pro, are
  told apart reliably.
- **Delta printers** (#1607) no longer save the wrong vendor on a shared hostname, and the
  Kobra 2 and Qidi Max 4 identify exactly.
- **Klipper version on printers without a git install** - such as FlashForge printers on
  Z-Mod: the Printer Manager and About show the version Moonraker's update manager knows
  instead of Unknown. Moonraker's version shows the same way on installs whose server
  reports none.
- **Better printer matches after an update** (#2075) - when a newer release recognizes your
  printer better, HelixScreen offers the better match once, even if you answered an earlier
  type question. If you declined the question before, you are asked once more.
- **Firmware-variant installs keep their own hardware** (#2064) - a Z-Mod or other variant
  install no longer gains the stock printer's fans and runout switch in its expected
  hardware on every boot.
- **Forge-X on the AD5X** (#2092) - an AD5X running Forge-X is no longer mistaken for Z-Mod.
- **No phantom light on a stock AD5X** - Z-Mod declares a chamber light on every AD5X even
  though a stock printer has none, so HelixScreen offered light controls that did nothing.
  New AD5X setups leave the light controls off, and an existing setup is unchanged. If you
  have fitted a light, turn on **Chamber light fitted** in Settings > Devices.
- **Fan RPM on Creality printers** (#2071) - on the K1 and K2 series, a fan's RPM shows on
  that fan's own dial or card. Before, one fan's RPM could show under another, such as the
  part fan's under an auxiliary fan, and a dial set to Off could show a leftover RPM over its
  zero.
- **Cartographer and Beacon probes are found as one probe**, and the probe's state shows.
  K1, K2 and QIDI printers keep their probe Z offset live.
- **A missing extruder can be ignored** - Hardware Health offers Ignore on critical rows, and
  a printer with `kinematics: none` is not asked for an extruder.
- **Creality Nebula Pad** - a retrofitted Ender is offered as an Ender instead of a Creality
  Hi.
- **A K2 Pro with the k2-improvements mod is a K2 Pro** - it was named a K2 Plus, because
  the mod had a detection entry only for the Plus. The K2 Pro has its own now, and the bed
  size picks between them. On both, a K2 that kept the stock PRTouch probe no longer shows
  it as a Cartographer, so its probe tools work; a fitted Cartographer still shows as one.
  When detection cannot tell a K2 Plus from a K2 Pro, HelixScreen asks instead of keeping a
  stock K2 profile that loads the wrong print-start steps.
- **Closing the printer type warning keeps your type** (#1881) - tapping X on the "printer
  type mismatch" warning now counts as keeping the saved type, like Review > Keep current,
  so it stops coming back on every restart.

**Snapmaker U1**

- **Faults reach the screen** - standing faults such as power loss were not shown at all, and
  coded faults showed raw console text. A spaghetti pause is no longer called a dirty bed.
- **Per-filament temperatures and per-head end unload** did not work against the real
  firmware.
- **Resume after a failed load** - a head whose load failed or never reached the toolhead
  holds Resume until a retried load is confirmed, a finished hand feed counts, and Resume is
  refused while a feed is still moving.
- **Prints started from a slicer** - a print started from Orca or Fluidd gets one clear
  warning when it uses a toolhead past T0, since the firmware then skips per-head feeding
  and the end-of-print unload.
- **Reprints keep their routing** - a reprint reads the file's toolheads, so per-head feeding
  and unload at end still apply.
- **Spool records** - when the U1 resets a head's record after filament leaves it, the spool
  you set is written back while it stays in the feeder, and no write lands during a tag read
  or a calibration.
- **The mounted head no longer resets on a temperature update**, and a malformed slot color
  leaves the slot's color alone.
- **Z offset on the probe screen** (#2089) - the probe screen shows the printer's real Z
  offset instead of 0.000, and hides the value on printers that do not report one.
- **Timelapse controls** - the Timelapse Settings screen talks to an interface the U1's
  firmware ignores, so its settings came back as off. That screen and the Print Status
  timelapse button are hidden on the U1; use Record Timelapse when you start a print.
- **SnapSpeed filament** - changing only a spool's color no longer turns a SnapSpeed filament
  into Basic. A new material or brand still starts at Basic.
- **Filament use on a Snapmaker U1 with multiACE comes off the right spool** - what a
  nozzle fed from an ACE bay extruded was charged to the head's lane instead of the bay's
  spool, so the bay's remaining weight never went down.
- **No more console flood on connect** - the filament temperature table is read from the
  printer's files instead of printing tens of kilobytes into every console.
- **The touchscreen is found properly** - on the U1, and other boards whose build has no
  libinput, touch worked through a fallback and the check for whether the panel needs touch
  calibration never ran.
- **The fit check knows which nozzle each head really has** - it read the 0.4 mm the
  printer's config lists for every head, so a correct job for a 0.6 mm head was warned about
  and a 0.4 mm job sent to a 0.2 mm head passed. It now reads the size and type the U1
  reports for each head. A file sliced for the other nozzle type (High Flow or standard)
  gets a warning with **Start Anyway**.
- **Filament pushed straight into a toolhead** (#2000) - a head loaded past its feeder, such
  as one fed by a BMCU, showed as empty, so the pre-print check called it an empty slot and
  Load tried to feed from the empty feeder. Such a head counts as holding filament, and Load
  opens the hand-load dialog.

**Setup and installing**

- **The step counter jumped mid-wizard** (#1550), from "Step 2 of 6" to "Step 3 of 3".
- **The setup wizard's connection step came up with blank address fields** - the IP and port
  boxes on the Moonraker connection step opened empty instead of carrying the default
  (127.0.0.1 and port 7125), so the whole address had to be typed by hand. Both fields are
  seeded again.
- **Uninstall** removes HelixScreen's leftover folder instead of only emptying it, removes an
  install in your home folder (#1600), and refuses an install folder it cannot safely delete.
- **K1 series and AD5X: switching back to Stable could not install 1.0** (#1789) - the
  update offered "Switch to v1.0.4" and then failed with "No update available", because 1.0
  releases name their download after the printer rather than the shared build. Both names
  are now looked for.
- **Creator 5 and Creator 5 Pro: a channel with no build for your printer says so** - switching
  to Stable, whose newest release has no build for these printers, used to offer the AD5X
  build. The update row now reads "v1.0.5 has no build for the FlashForge Creator 5 Pro, so
  it can't be installed here. Pick a channel with a newer release." and never offers Install.
  On every printer the same message replaces the old "No update available" failure for a
  release that has no download for it.
- **`HELIX_SCREEN_SIZE` left no trace in the log** - the line saying which size was applied,
  and the warning for a value it could not read, were written before logging started, so the
  `journalctl` check in Troubleshooting found nothing. Both are logged now.
- **Restart HelixScreen from Mainsail and Fluidd after installing** - when the installer
  adds HelixScreen to Moonraker's list of services it may manage, it restarts Moonraker so
  the list takes effect.
- **The installer no longer edits Z-Mod's own moonraker.conf on the AD5X** - it writes
  HelixScreen's update entry to the user moonraker.conf Z-Mod's file includes, so Moonraker
  keeps updating Z-Mod. When an earlier install already edited Z-Mod's file, the installer
  says which updates are blocked and prints the command that restores it. (thanks @catduckgnaf)
- **No HelixPrint plugin downgrade at startup** - when Moonraker already runs a newer
  HelixPrint plugin than this HelixScreen ships, HelixScreen leaves it in place instead of
  restarting Moonraker onto its own older plugin. Versions compare by number, so 1.10.0 is
  newer than 1.9.0.
- **HelixPrint plugin updates that need a reboot say so** (#2086, #2090) - when a restart for
  the HelixPrint plugin does not happen, as on Z-Mod or other printers that cannot restart
  Moonraker, HelixScreen says to reboot the printer, keeps the message in the notification
  history, and asks again on the next start instead of giving up silently.
- **Installing or removing the HelixPrint plugin restarts Moonraker properly** - the screen
  no longer marks a plugin installed while Moonraker never loaded it.
- **Keeping your files across web updates on Z-Mod** (#2085) - updating HelixScreen from
  Fluidd or Mainsail on a Z-Mod printer no longer deletes tool spools, saved filaments or
  crash history. The update that installs this fix still loses them once, because the old
  version is what gets replaced. The telemetry device id also survives web updates on every
  printer, though it changes once on the first update.
- **Update channel edits in Mainsail or Fluidd stick** - changing HelixScreen's channel in
  `moonraker.conf` by hand switches the screen's channel too, instead of being written back,
  and two screens sharing one Moonraker leave each other's entry alone.
- **Update failures show the real cause** on the touchscreen, the installer's own error
  rather than an earlier harmless one or the name of the failed step.
- **Re-running the installer keeps your settings** - detected rotation and touch settings are
  only seeded on a fresh install.
- **The installer on Creality K2 and multi-user setups** - it finds the `moonraker.conf`
  Moonraker actually loads, leaves a k2-improvements WebRTC camera alone, puts the KIAUH
  extension into the right user's home, and lets a Moonraker running as another user load
  the plugin. Uninstall removes the updater entry from included config files too.
- **Installer** (#1534, #1693) - a stock screen or KIAUH service that turns itself back on
  stays off, and installing from a desktop session no longer takes down your desktop.
- **Free-space checks work on BusyBox systems**, and a printer that ignores a custom install
  folder still migrates an existing install (#1674).
- **Installs on ZFS** - a freshly downloaded update is no longer rejected as "too small" on a
  ZFS filesystem.
- **The install command older guides and forum posts give** (the main/scripts/install.sh
  address) works again.
- **A printer preset applied at first boot sets the filament sensor roles at once**, instead
  of after a restart, and applying any preset reloads the filament, probe and width sensor
  settings.
- **K1 series first boot** - the splash and screen come up the right way round on the first
  start instead of running the rotation check.
- **Black screen at startup** on boards with a display card but no libdrm, and on EGL
  displays, shows the splash.
- **Z-Mod restarts** - the screen no longer disappears when Z-Mod restarts it on a Klipper
  start.
- **Shutdown and Reboot from the screen** work when HelixScreen runs as a regular user.
- **Connection tests say why they failed** - the setup wizard and Change Host tell a timeout
  from a refused port.
- **K2 debug bundles** keep much more of Klipper's log.

**Translations**

- **Filament system errors are translated** for the first time, across AFC, Happy Hare, AD5X
  IFS, CFS, ACE and QIDI, and the CFS load failure no longer overflows or cuts a character in
  half.
- **Gate and lane** (#957) - 26 phrases used the word for "slot" where English says gate or
  lane.
- **Japanese and Chinese** (#1620) - missing characters showed as boxes; the material
  temperature table (#1263) fits small screens and is translated; the motion, batch filament
  and U1 fault text is translated in every language.
- **A language change reaches the whole screen** - tool, filament system, temperature graph
  and preheat text, the Check for Updates status and the timelapse button change right away
  instead of after a restart. Durations use the current language, and capitalized labels
  work for Cyrillic, Greek and accented Latin text.
- **Printer connection errors and notices are translated**, including Wi-Fi failures, and
  French "Currently Loaded" no longer shows garbled characters (#1678).

### Internal

- Two speed and flow code paths were unreachable, and the overrides go through one clamp
  rather than three copies of it.
- The mock Voron personas model a real center-origin bed (-175..175 on the 2.4, -125..125 on
  the Trident) through every path the mock serves, so the demo G-code files sliced for
  center-origin printers pass the print-start fit check: 14 of 18 fit the default persona
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
  musl gives each thread 128 KiB unless the binary asks for more. Every app thread, libhv's
  loop and the thumbnail workers included, installs its own signal stack
  (`helix::install_thread_altstack`), so an overflow on any of them leaves a crash file. A
  lint gate keeps `std::regex`, whose matcher recurses per
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
  Boundaries are mapped once per axis and every widget rebuilt from them, so neighbors that
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
- BTT K-Touch (beta) firmware: the theme, tool switcher, filament sidebar and slot menu
  values and the settings store moved from the ESP32's scarce internal RAM to PSRAM, keeping
  internal RAM under the ceiling the WiFi driver's receive buffers need.
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
