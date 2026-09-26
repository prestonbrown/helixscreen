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

**Keeping it current:** the trunk moves. To see what has landed since this file
was last touched:

```bash
git log --no-merges --oneline <last-reviewed-sha>..main --not release/1.0
```

---

## [1.1.0] - UNRELEASED

**Upgrading?** The home screen moves to a square-cell grid, and your layout comes
with it. Positions are converted onto the new grid in proportion: a widget that
filled the left third of the screen still fills the left third, and two widgets
that were touching stay touching. Sizes can shift slightly, because the new grid
does not divide the screen the same way and each widget lands on the nearest size
it is allowed to hold. A widget that genuinely will not fit is re-placed
automatically, and only that widget. Which widgets you have, their per-widget
settings, and your extra pages all survive. The conversion happens the first time
the home screen is drawn after the update, so what you see on that first boot is
what gets saved.

### Added

- **Belt tension is measured by plucking, not by a driven sweep (#1303, #1231)** - park the
  gantry, pluck each belt by hand, and the tool listens on Klipper's live accelerometer
  stream and reports the belt's fundamental. It reads the fundamental off the whole
  harmonic series rather than the tallest peak, which is what the old sweep got wrong: a
  belt whose 2nd harmonic dominates reported exactly one octave sharp. The number you act
  on is the median of five accepted plucks. The `TEST_RESONANCES` sweep, the strobe path
  and the never-measurable Z-belt path are deleted.

  > **⚠ RELEASE BLOCKER - do not ship 1.1 with this marked done.** This is green in CI and
  > has **never measured a real belt**. Its gate thresholds were measured against captures
  > from one Voron 2.4 on one evening, and the algorithm was then tuned against that same
  > set - circular, and not yet broken. It stays beta-gated and must not be promoted until
  > the hardware matrix in `BELT_TUNER.md` § Validation status has actually been run.
  > Delete this box only when that is done, not when the code looks finished.

- **The home screen is one square-cell grid on every panel (#1126)** - both axes now divide the
  panel by the same per-breakpoint track size, so a cell is square everywhere and a rotated
  panel transposes its grid exactly. The per-layout-type branches, the fixed count table and
  the separate width and height targets are gone. Tracks are half cells, which is three times
  the placement resolution the old grid had, so the compromises it forced are gone too: tips
  is no longer suppressed in portrait, and bed temperature ships enabled everywhere instead
  of losing a coin toss against AMS.
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
  the widget still under your finger. Drag it past your last page and an empty page slides in:
  dropping the widget there creates the page and shows it at once. Pressing a widget that is not
  selected only selects it, so a swipe that starts on a widget still flips the page: press the
  selected widget again, or hold a widget, to pick it up. The long press that enters edit mode
  only selects.
- **The widget catalog opens by category (#1016)** - thirty-seven widgets in one flat scroll had
  stopped being a list you read and become one you hunt through, worst on the 480px panels.
  It now opens on five category rows and dives into one, with the same back button and slide
  the settings sub-pages use. Widget names and descriptions are properly translated for the
  first time: 18 of 37 names and every description were invisible to the string extractor and
  only appeared in English. 74 new keys across all nine languages.
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
- **AFC buffers with a pressure sensor drive sync feedback** - an `AFC_buffer` of type
  `FPS_PSF` measures what Happy Hare's sync feedback measures, so it is published the same
  way and the buffer meter, path tint and filament page all work on AFC unchanged. Not yet
  verified on hardware: the only AFC rig on hand is a switched TurtleNeck, which sends none
  of these keys and is unaffected.
- **Baby-step size is remembered across launches** - the Z-offset step you last chose is the
  one you get next time.
- **1080p and wider panels get their own grid tier** - XXLarge was pinned onto XLarge, so a
  1080p panel drew the same physical widget size as a 720p one while fonts and icons scaled
  up 1.6x around it. Hence 128px glyphs and text running out of its box. Navigation width
  gained matching rungs.
- **Runout dialogs can hide manual actions mid-print** and carry an advisory rather than a
  warning header, so a dialog that is telling you something reads differently from one asking
  you to act.
- **Scheduled pauses are marked on the progress indicator (#1509)** - filament changes (`M600`),
  explicit `PAUSE`/`M601`, and slicer pause-at-layer stops each draw a tick where they sit in
  the print, on both the home print-status card's bar and arc and the print panel's bar. The
  tick's position follows the same axis the bar fills on: a file sliced with `M73` progress
  lines places ticks at the slicer's own percentages, anything else at file position. A print
  whose gcode was never fetched (an external start during the setup wizard, or a file too
  large to preview) simply shows no ticks.
- **Spaghetti detection, built in (#1378)** - installing HelixScreen on a K2 Plus stops the
  stock AI failure-detection loop along with the stock UI, so HelixScreen ships its own
  detector: during a print it runs Creality's own `/usr/bin/detection` on camera snapshots at
  the interval and threshold the printer's `ai_control` settings already hold, and never
  writes them. A confirmed detection pauses the print and opens a dialog offering Resume,
  Abort, Reduce Sensitivity or Turn off detection. The whole pipeline is vendor-neutral: any
  detection source only reports, and two settings in Safety & Notifications decide what
  happens - **Spaghetti Detection** (watch at all) and **Pause on Detection** (pause, or only
  warn). On the first start both are seeded once from the printer's own stored choice
  (`switch` / `pausePrint`) and are HelixScreen's from then on. Printers whose firmware
  pauses by itself, like the U1, are never double-paused and always get the response dialog
  (their Pause on Detection row hides; the firmware's pause is not HelixScreen's to govern).
  The rows appear only on printers with detection hardware.
- **Power-loss recovery works on Qidi printers (#1716)** - on a Q2, Q1 Pro or Plus 4 running
  the stock firmware, the resume dialog now appears after a print was cut short by a power
  loss, offering the printer's own `RESUME_INTERRUPTED` flow or a clean discard.
- **FlashForge Creator 5 and Creator 5 Pro are supported printers (#1714)** - both models of
  FlashForge's four-head tool changer are recognized and given their own settings, on either
  of the two firmwares that replace its stock software: Z-Mod and Reforge. Mounting and
  parking a head is driven from the screen. Reading each head's material and colour from the
  printer, and showing the mounted head at boot, both need Z-Mod's pending status update:
  until it ships, a Z-Mod machine reports a dock sensor error at boot until its first tool
  change, and colour or material edits stay on the screen instead of being saved to the
  printer. All of this is verified against a simulated printer, not yet on the hardware.
- **A print can be queued while another one runs** - while a job prints, or is still
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

### Changed

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
  widget at its left side.
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
- **Half-cell-capable widgets have a real minimum size** - shutdown, lock, firmware restart,
  LED controls and the clock could be shrunk to a 31-40px half track, where the icon, its
  caption and the clock face all clip. The floor is now one whole cell; edit mode still offers
  odd-track sizing above it.
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

### Fixed

- **No ACE Pro slot ever showed as loaded on the Kobra S1 fork (#1069)** - that driver
  ("ACEPRO") states the loaded tool only through its manager object's `current_index`, a field
  the backend never read, so slots, colours and materials displayed while the loaded one never
  did. The index is read on both status paths now, with -1 meaning nothing loaded; the fork's
  `material` and `dryer_status` key spellings are accepted too, and a status frame carrying
  both the manager and a unit no longer drops the unit's update. Confirmed from a user's live
  captures and pinned by tests built from them; not yet verified on the hardware itself.
- **Filament details you entered on a Happy Hare lane were gone at the next launch** - brand,
  spool name, weights, colour name and the catalog product you picked were kept on screen for
  the session and never written down, on every Happy Hare printer. Clearing a lane had the
  mirror-image problem: the lane emptied on screen and the old details came back on the next
  start. Both stick now.
- **Pure black was dropped as a filament colour on AFC and Happy Hare lanes** - picking black
  recorded no colour at all, so the lane kept whatever colour was on it before, and the
  recommended nozzle and bed temperatures for the filament were not saved with it. Black is now
  a colour like any other on HelixScreen's own screens; on these two systems it still does not
  reach the printer's own lane record or its LEDs (#1597).
- **Resume and Cancel Print did nothing on the home tile's paused modal** - a runout that paused
  a print gave you a Resume button that closed the dialog and left the print paused.
- **Declining a cancel confirmation dropped you on a bare screen** - the guidance dialog was
  already exiting when the confirmation appeared, and on the runout path it did not come back:
  reconsidering a cancel cost you Load, Unload, Purge and Resume for the rest of that pause.
- **One tap on the filament tile disarmed the runout warning for the whole session** - the
  advisory flag was shared and never reset, so it swapped the alert icon for a neutral one on
  every later runout dialog, including the one that pops when a print actually pauses.
- **The clog threshold became unreadable exactly when it mattered** - the danger shading and a
  warning fill are both drawn in the danger colour, so a reading past the threshold merged the
  two into one red block. The threshold is its own rule now, drawn after the fill.
- **The AMS loaded card clipped the filament name** - "Polymaker" became "Polymak" because the
  content-sized meter column grew to fit longer source strings. The reading moved out of the
  36px arc that was drawing it clipped, and the material name wraps instead of marquee-ing
  forever.
- **Half-cell widgets rendered with no background at all** - the merged card fill was computed
  in cell coordinates, so any widget on an odd track or with an odd span dropped out of the
  merge and sat on the bare panel. A widget could also be backed by a card laid half a cell
  away from it.
- **Widgets did not adapt to the panel they were on** - the size bands were flat pixel values
  calibrated on a small screen, so a one-cell widget on a 1080p panel held 32px type in a
  182px box and clipped: fan names cut off, one glyph per line in the active spool tile. The
  temperature graph measured nothing at all, wrapping "300" onto two lines and overlapping
  its time labels. Verified by rendering nine geometries from 480x272 to 1920x1080.
- **Auto-placement could seat a widget straddling two cells** - and a saved odd origin survived
  restarts, so the grid handed out positions edit mode would then refuse to give back. Adding
  a widget from the catalog had the same problem, and the growth half of the fix was inert.
- **The home grid was built 8-23% too narrow** - the track size divided the panel resolution
  rather than the container's content box, and panel chrome takes a different bite out of each
  axis and each orientation. Rows were also sized to the widgets in use rather than the grid,
  which stretched a 1x1 widget into a tall rectangle on a half-full page.
- **A layout that omitted a widget's span rendered it at a quarter of its area** with nothing
  logged.
- **The Detailed print-status card blanked for the rest of the session** if you removed the last
  print-status widget and added one back.
- **Narrow widgets could only be resized, never dragged** - the resize hit band was a flat 18px
  per edge, so on a widget under 36px wide the two bands overlapped and every pixel reported
  an edge.
- **The widget catalog leaked its whole tree on every open**, and a malformed layout file left
  edit mode believing the catalog was still open forever, with a stranded backdrop over the
  panel.
- **Long widget names ran straight through the size badge** on a 480px panel, and **every widget
  size in the catalog badge read double** - a one-cell Power widget badged 2x2.
- **The Detailed idle actions squeezed to 77px on a portrait card** with their captions
  overrunning the button border. The action row now spans the card and stacks when the buttons
  do not fit side by side.
- **The firmware restart badge overran its glyph box** on six screen sizes, and the job queue
  widget could be shrunk below the size its own summary line fits.
- **Z-offset kept showing a pending delta after a successful save**, could not tell a
  clamped-to-limit adjustment from a failed one, and would clear a valid stored step size on
  an out-of-range write.
- **Two speed and flow code paths were unreachable**, and the overrides now go through one
  clamp rather than three copies of it.
- **A toast closing interrupted whatever your finger was doing** - when a notification timed out
  or was dismissed, a drag, a slider or a scroll anywhere else on screen was cancelled mid-way.
  Only a press on the toast itself is cancelled now.
- **Dropping a widget on an occupied spot moved the selection** (#1638) - the widget under your
  finger was selected instead of the one you dragged, which now stays selected.
- **A one-page home kept swiping after leaving edit mode** (#1638) - after tapping Done on a home
  with a single page, a sideways swipe slid onto an empty page with a "+" on it. A single page
  does not swipe now, in edit mode or out of it.
- **Tapping Done on a later home page slid back to the first page** (#1638) - leaving edit mode
  with a second or later page showing scrolled the home screen back to its first page. The page
  you were on stays on screen, and pages change only when you swipe or tap a page arrow.
- **A tool changer with nothing mounted showed T0 as active** - when the printer reported that
  no head was on the carriage, the active-tool highlight fell back to the first head anyway,
  so the screen claimed head 1 was selected while the carriage was empty. No head is shown as
  active until one is actually mounted.
- **The setup wizard's connection step came up with blank address fields** - the IP and port
  boxes on the Moonraker connection step opened empty instead of carrying the default
  (127.0.0.1 and port 7125), so the whole address had to be typed by hand. Both fields are
  seeded again.
- **Starting a job from the Job Queue skipped the normal print checks** - a tap removed the
  job from the queue and started the file directly, bypassing the pre-print options, the
  filament mapping and every start gate, and it did nothing without a notice whenever a
  print was still preparing or running. A queued job now opens in the file view with its
  saved options and goes through the same start pipeline as any other print; it leaves the
  queue only once the print has actually started, and a busy printer says so.

### Internal

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
