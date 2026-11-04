# Screensavers

A screensaver runs when the display dims (`DisplayManager::check_display_sleep`), covers the
panel with a full-screen overlay, and measures its own CPU cost so it never slows the printer.
This doc is the map: which file owns what, how the gate decides, and how to add a saver.

## Key files

| File | Owns |
|---|---|
| `include/screensaver_registry.h` | `ScreensaverType`, the `kScreensavers` rows (type, stable name, label key, color depths), clamp, the stable names and the frozen legacy type numbers, the fresh-install default, the too-heavy fallback order and the settings dropdown's choices. Always compiled |
| `include/screensaver_setting.h` | Reading the persisted choice: a name in any case, or a legacy number |
| `include/screensaver_base.h` | `SaverBase`: start, frame and stop sequence, levels, level requests; `ladder_period()` for a ladder table |
| `include/screensaver_sim_contract.h`, `include/screensaver_sim_saver.h` | The calling shape of a sim (`init_sim`, `step_sim`, with or without an rng) and `SimSaver<Sim>`, which implements every `SaverBase` hook from it |
| `include/screensaver_clock_saver.h` | `ClockSaver<Sim, State>`: a SimSaver whose sim draws a clock, with the input source (`ClockInputSink`, reached through `Screensaver::clock_input_sink()`), the clock state, the text cache and the redraw-over-what-changed step |
| `include/screensaver_image.h` | Decoding a sprite or picture once into an owned draw buffer, reading an image header, and the printer picture a saver shows |
| `include/screensaver_overlay.h` | `SaverOverlay`: the black, clickable overlay on `lv_layer_top()`, so nothing beneath it is ever the press target |
| `include/screensaver_canvas.h` | `SaverCanvas`: stride-sized buffer, the two canvas formats (`kSaverBuildCanvasFormat`, `kLvDrawCanvasFormat`), hide-before-free, dirty areas reduced to `kSaverMaxDirtyAreas` or the whole canvas (`reduce_dirty`), the dirty-box layer session |
| `include/screensaver_frame_timer.h` | `SaverFrameTimer`: frame timer with a `MotionClock`, period changes without a motion jump |
| `include/refresh_period_hold.h` | The display refresh while a saver runs, at the saver's current period; `period()` holds `HELIX_SCREENSAVER_REFR_PERIOD_MS` |
| `include/screensaver_frame.h`, `include/screensaver_pixel_writer.h` | `DirtyRect` (`whole`, `clipped`), `reduce_dirty`, `FrameTarget`, `frame_row`, `with_pixel_type`, `scaled_extent`, `round_px`; `PixelWriter` (RGB565 and XRGB8888, ordered dither, max blend, `blend_over`, `fill_rect`, lines) and the colour rules every saver shares (`mix_rgb`, `blend_cov`, `scale_rgb`, `to_u8`, `unit_to_u8`, `edge_coverage`). No LVGL |
| `include/screensaver_motion.h`, `include/screensaver_vec.h`, `include/screensaver_buffer.h` | Frame-time motion and the portable rng draws (`random_int`, `random_between`, `random_normal`, `shuffle_in_place`), `wrap_index`/`wrap_period`; `SaverVec3`; `buffer_bytes`/`release_buffers` for the buffers a saver holds only while it runs |
| `include/screensaver_gate.h`, `include/screensaver_cpu_clock.h` | Budget, decision, idle baseline, windows, environment switches, the CPU clock seam |
| `include/screensaver_level_store.h` | The stored level per saver |
| `include/board_fingerprint.h` | `BoardFacts`, `board_fingerprint()` and `current_board_facts()`: the board identity the level store keys on, shared with the glass gate (`include/glass_gate.h`) so both measure again on the same boards |
| `include/screensaver.h`, `src/ui/screensaver/screensaver_manager.cpp` | `ScreensaverManager`: factories, holds, the gate, the too-heavy fallback and the last-resort black screen |
| `include/screensaver_usage.h` | `SaverUsage`: seconds shown, starts, run endings and settled levels per saver, which the manager writes and telemetry reads (`screensaver_usage` in `docs/devel/TELEMETRY_ADMIN.md`). Always compiled |
| `include/ui_screensaver.h`, `include/screensaver_starfield.h`, `include/screensaver_pipes.h`, `include/screensaver_bounce.h`, `include/screensaver_fireworks.h`, `include/screensaver_hyperspace.h`, `include/screensaver_flip_clock.h`, `include/screensaver_fireflies.h`, `include/screensaver_aqua.h`, `include/screensaver_highfly.h` | The savers |
| `include/screensaver_pipes_sim.h` | 3D Pipes: the node grid and growth rules, one screen of pipes, the meshes, the z-buffered rasterizer and the dissolve. No LVGL |
| `include/screensaver_maze_sim.h`, `include/screensaver_maze.h` | The 3D maze: maze generation and opening up, the wall-following walk, the raycaster and its fog (the sim, no LVGL), and picking wall art from the thumbnail cache (the saver) |
| `include/screensaver_fireflies_sim.h` | Fireflies: the swarms, their modes and wind, and the additive drawing (`FirefliesSim`). No LVGL |
| `include/screensaver_flip_clock_sim.h` | Flip clock: the cards a reading shows (`flip_face`, `FlipClockState`), the card layout, and the drawing (`FlipClockSim`). No LVGL |
| `include/screensaver_clock_reading.h` | What every clock saver reads: `ClockState` (the countdown, pause and done-hold machine), `ClockReading`, `clock_time`, `clock_drift_at`. No LVGL |
| `include/screensaver_clock.h` | The app side of the clock savers: `saver_clock_now`, `clock_print_phase`, the print, date and AM/PM text, `clock_font_fitting`, and `SaverLabel`, a text line kept over a scene that repaints beneath it |
| `include/screensaver_stroke_digits.h` | Digits as strokes along lines and arcs, rasterised at any size and stroke weight: the flip clock's bold cards, Aqua's ring digits and High Fly's thin numerals |
| `include/screensaver_heightfield.h` | The voxel-space column renderer for a surface seen at a grazing angle (`HeightfieldRenderer`), and `upscale_frame`. No LVGL |
| `include/screensaver_fast_math.h` | Table sine and e^-s, built at compile time into .rodata |
| `include/screensaver_aqua_sim.h`, `include/screensaver_aqua.h` | Aqua Surface: the water (`AquaWater`), droplets (`AquaSplashes`), the ring clock and the scene (`AquaSim`) |
| `include/screensaver_highfly_sim.h`, `include/screensaver_highfly.h` | High Fly: the cloud field, textures, the thin clock and the scene (`HighFlySim`) |
| `scripts/screensaver-perf/` | Measuring a saver on a real board |

## The foundation

Every saver derives from `SaverBase` and implements `canvas_format()`, `on_start()`,
`on_frame()`, `on_stop()` and its ladder (`ladder_size()`, `ladder_period_ms()`). The base
creates the overlay, the canvas when the saver has one, seeds the saver's own random sequence,
starts the frame timer at the level's period, and invalidates the dirty areas each frame
returns. A saver that writes pixels itself returns boxes; a saver drawing with `lv_draw_*`
opens `SaverCanvas::begin_layer()`, marks what it draws with `mark_dirty()` and finishes with
`finish_layer()`. Toasters use no canvas: their sprites are LVGL images.

A saver whose scene is a simulation derives from `SimSaver<Sim>` instead, and writes only what is
its own: the type and the canvas format. The sim follows the contract in
`screensaver_sim_contract.h` (`init`, `step`, `request_level`, `clear`, `kLevels`), and SimSaver
clears it before every start, so a sim never depends on how the last run ended. A clock saver
derives from `ClockSaver<Sim, State>`, which adds the time-and-print input and the text drawn over
the scene (`draw_clock_text()`, `reset_clock_text()`).

Every ladder starts at 16 ms (`kSaverFastPeriod`, one refresh of a 60 Hz panel), or at
`HELIX_SCREENSAVER_REFR_PERIOD_MS` when that is set for manual testing
(`SaverBase::level_period_ms`); levels 1 and up keep their declared periods either way. Starfield
then runs at 33 ms; pipes per `kPipesLevels`: 16 ms, 33 ms, then 100 ms at half resolution (the
scale changes at the next screen); toasters at 33 ms with every sprite, then 33 ms with ten; the
bouncing printer and bouncing logo (one class; `BouncingLogoScreensaver` swaps the sprite and drops the tint and corner celebration) at 33 ms, then 33 ms with the corner flash but no confetti; fireworks per
`kFireworksLevels`; the 3D maze per `kMazeLevels`: full resolution at 16 ms, then half resolution at
33 ms, the same with flat floor and ceiling, and quarter resolution at 50 ms and at 66 ms; hyperspace per `kHyperspaceLevels`, which keeps 33 ms past level 0
and then draws its tunnel and goo in 2x2 blocks, drops the goo, draws the tunnel in 4x4 blocks, and
last drops the tunnel and flies 96 of its 160 stars; the flip clock per `kFlipClockLevels`: the folding
flip at 33 ms, then an instant digit swap checked every 250 ms; fireflies per `kFirefliesLevels`:
33 ms past level 0, then tails one pixel wide with no glow halo, then 120 flies with 1.5 s tails
at 50 ms, and last 48 flies with 0.75 s tails at 100 ms; Aqua Surface per `kAquaLevels`: the
water at half resolution at 33 ms, then quarter resolution with fewer slices at 50 ms and
100 ms (no new splashes at 100 ms), and last a still frame that redraws only the clock every
250 ms; High Fly per `kHighflyLevels`: half resolution at 33 ms, the same without the
translucent layers, quarter resolution with fewer slices and no towers at 50 ms and at 125 ms,
and last a still frame that redraws only the clock. While a saver runs, `helix::RefreshPeriodHold` refreshes the display at
the saver's current period (`follow`), so the display and the saver stay equal at every level. Device measurements assume the pacing that keeps 16 ms even: EGL vsync and a 1 ms
main-loop floor, which every device run sets explicitly (`scripts/screensaver-perf/arm_measure.sh`).

## Waking

The touch that wakes the display never presses anything, whether the display is asleep,
dimmed or under a screensaver. On every embedded backend the sleep-aware read wrapper
(`src/application/display_manager.cpp#sleep_aware_read_cb`) asks
`DisplaySleepController::note_press()` whether a press is waking the display; when it is,
`helix::WakeTouchLatch` (`include/wake_touch_latch.h`) reports every read of that touch as
released, at the pointer's resting point, until the finger lifts. A lift within
`WakeTouchLatch::kRearmMs` (200 ms) of the waking press keeps the latch armed, so contact
chatter as the finger lands, or a stray contact from a panel just powered on, is swallowed
too; a tap after the window is an ordinary one. LVGL sees no press, drag or
long press, so neither the overlay nor anything under it can act. The wake itself runs on the
press: the next idle tick calls `DisplaySleepController::wake()`, which stops the saver and
restores brightness while the finger is still down.

`wake()` also runs `DisplayManager::cancel_pointer_presses()` on every pointer. A pointer the
wrapper does not cover has already handed the press to LVGL: SDL's (which carries Android's
touch), an evdev USB mouse beside the touchscreen on the DRM and fbdev backends, and the
`ctl` pointer; the cancel resets it and makes it wait for release, so it cannot click, long-press or
scroll, though the widget under it still sees PRESSED and PRESSING. Every wake counts as
activity, so the swallowed touch does not leave the display to dim again on the next tick.

## The 3D maze

A port of the Windows 95 OpenGL "3D Maze", drawn Wolfenstein-style: one ray per column through
a tile grid, no polygons. A maze of 12 x 12 cells is a 25 x 25 tile map whose odd tiles are the
cells; corridors are a tile wide and walls a tile tall, with the eye at mid-height.

- **The view** sees 90 degrees vertically, as the original did, and at most 120 degrees across:
  `maze_focal()` takes the larger of `h / 2` and `w / 2 / tan 60`, so 800x480 sees 90 x 118
  degrees and a frame wider than 120 degrees narrows its vertical view instead. Walls, floor
  rows and sprites share that one focal length and camera plane (`MazeRenderer::set_level`).
  `kMazeHfovMaxHalfTan` is the knob if the edges bend too far.
- **The maze** starts as a perfect maze from a recursive backtracker. The walk starts in one of
  its dead ends facing out past the start sign, and the exit, a translucent smiley, is the cell
  furthest from the start in that tree. `MazeMap::open_up` then opens it: a wall out of half the
  other dead ends (toward a neighbouring dead end where there is one), six more walls at random,
  and 2 or 3 rooms of 2x2, 2x3 or 3x2 cells with every tile inside open, pillars included. Start
  and exit keep their walls, and any opening that would cut one of their walls off from the
  outer ring is undone, so both stay dead ends on the wall a hand can follow to them.
- **Boundary walls**: `MazeMap::boundary_wall` marks the walls joined to the outer ring
  (4-connected, a flood fill run whenever the tiles change). A wall that is not is an island,
  which a hand on the wall would circle for ever.
- **The walk** (`MazeWalker`) keeps a hand on the wall and never stops in a cell: doorway to
  doorway at a constant speed, straight through or round a quarter arc about the inner corner,
  so the wall ahead stays at least about three quarters of a tile away. Only a dead end (or a
  turn back) walks in to the middle, spins round and walks out, the one place the view faces a
  wall. While a stop cell is set it remembers each (cell, heading) it entered since its hand
  last changed; entering one again means it is circling an island, so it walks the shortest
  way (a breadth-first search over those states) to a cell whose hand-side wall is a boundary
  wall, and follows that. It eases to a stop in the exit's doorway and holds there with the
  smiley a tile ahead, then a new maze starts. `MazeSim::step_bound()` allows each hand (the
  first and one per rock) an island lap, a way off and a boundary lap; a walk past it starts a
  new maze instead (`abandoned()` counts those, and the tests require none).
- **Items**: spinning Helix icons where the original had OpenGL logos, a start sign carrying the
  icon, gray icosahedron rocks that flip the view upside down (a squash through zero) and swap
  the walk to the other hand, and a toolhead that wanders on its own walk, which has no stop
  cell and so never tracks circling.
- **Fog**: every surface darkens with distance toward a dark tint, keeping
  `k / 32` of its colour with `k = round(32 * (0.1 + 0.9 * exp(-(d - 0.5) / 5)))`
  (`maze_fog_k`, `maze_fog`): a third darker 3 tiles off, about 60% darker at 6. The renderer reads `k` from a 128-slot table, four slots a tile, and
  blends in the format's own lanes (RGB565 spread as `0x07E0F81F`), so nothing divides per
  pixel. A wall slice taller than the texture shades its 64 texels once and copies them; a
  shorter one shades per pixel. The textured floor and ceiling read 16 pre-fogged copies of
  their textures, one per row's band, built when a textured level is applied and freed at a
  flat one; flat levels bake the fog into each row's colour. Sprites take the fog of their
  depth. East-west faces keep 21/32 of that on top, as Wolfenstein drew them.
- **Wall art**: 16 picture walls per maze, each with its own index, show up to 12 pictures
  (index modulo the count; brick when there are none). `MazeScreensaver::on_start` gathers them
  from three pools and interleaves them: cached thumbnails of recent prints (print history, 5),
  of other files in the print-select list (6, shuffled; none before print select has been
  opened), and the printer picture. It only reads the local cache through
  `ThumbnailCache::peek` (the pre-scaled card `.bin`, then the full PNG), never
  `get_if_cached` (which deletes a stale entry and ages it) or `fetch`, so a saver start causes
  no network traffic. Cache ids carry the connected printer, so after a printer switch the
  previous printer's thumbnails never match. Any picture over 512 px a side is skipped, the
  printer picture included (its prerendered variants are 150 or 300 px); each decode is reduced
  to 64 px (`reduce_maze_image`) before the next starts, and no decode starts after 120 ms, so
  start-up is bounded by 120 ms plus one decode of at most 512 x 512. The pool-picking logic
  is pure (`history_art_candidates`, `cached_art_paths`, `mix_maze_art`) and tested without the
  app.
- **Cost**: it repaints every pixel every frame, so it is the costliest saver per frame and the
  one the gate is likeliest to mark too heavy on a small board. `kMazeLevels` pulls the
  internal resolution (each pixel replicated `scale x scale`), swaps textured floor and ceiling
  for flat fogged rows, and slows to 66 ms at the bottom. The wide view puts more of each frame
  on the dearer floor and ceiling, which the 66 ms rung pays for. It renders into its own
  column-major frame, then copies that into the canvas row by row. Frame, textures and sprites
  are packed at the display's own pixel width (2 bytes on RGB565), so the inner loops copy
  pixels and never convert colour. Resident art is `(3 + pictures) x 64 x 64` texture pixels
  plus the fog bands (`2 x 16 x 64 x 64` pixels) on textured levels.

Motion is a function of elapsed time (`include/screensaver_motion.h`), so a level that changes
the frame period changes nothing on screen but the frame rate, and at the toasters' lowest rung
the sprite count.

## 3D Pipes

A port of the Windows NT OpenGL "3D Pipes", with the original's numbers. The grid has 15
nodes along the long screen axis, the short axis by aspect, and 15 deep (15 x 9 x 15 at
800x480), nodes 7 apart, pipes of radius 1, seen from 75 back with a 90 degree vertical field
of view. One pipe grows at a time, the original's default single-pipe mode, and a screen
draws 5; a pipe that gets stuck ends with a ball cap and the next starts at a random empty node
until the 5 are used, or the nodes run out. Each step goes straight with the pipe's own weight (1-4, now and then 25-100) against one
per empty turn; a turn takes the direction with the longest empty run, looking two nodes
ahead. A turn's joint is a ball a third of the time, otherwise an elbow, and a teapot one
time in a thousand.

- **Drawing** (`PipesRenderer`): triangle meshes (16-sided cylinders, quarter-torus elbows,
  balls, freeglut's Newell teapot data) rasterized straight into the canvas through
  `PixelWriter` with a float inverse-depth z-buffer, so a near piece always covers a far one
  whatever order they were drawn in. Each pixel interpolates the eye-space normal and reads
  per-material lookup tables for one fixed directional light: GL fixed-function ambient,
  diffuse and specular with the 16 SGI materials of the original (emerald to white rubber),
  so the highlight sits up-right on every piece.
- **Pacing**: one growth step per 100 ms of frame time, at most three per frame; a frame with
  no step due draws nothing. A finished screen clears in a blocky dissolve (blocks of 1/32 of
  the short side, in random order) over 2 s of frame time, then the next screen starts with
  the view turned a further 9.73156 degrees about the vertical.
- **Cost**: only the pieces of each step and the dissolve's blocks are written and dirtied.
  Rasterizing is a small part of the cost, so the bottom rung saves mostly by its 100 ms
  period; it also rasterizes at half resolution, and the z-buffer is sized for it, so a level's
  scale waits for the next screen.

## The gate

`ScreensaverManager::on_idle_check_tick` runs on the display manager's idle-check tick and
samples `CLOCK_PROCESS_CPUTIME_ID` (every thread, LVGL's draw thread included) at most every
250 ms.

- While no saver runs, samples feed `IdleBaseline`: the CPU rate over the last idle stretch of
  up to 10 s, and 0 under 3 s of samples.
- A run ignores its first second, then closes a 5 s window after another for as long as it runs:
  `(cpu delta - baseline * wall) / wall` as a share of one core.
- The budget is `saver_budget_share`: 4 or more cores 50%, 3 cores 37%, 2 cores 25%, 1 core 10%,
  halved while `SaverHost::is_printing` says a print holds the machine.
- `decide_saver_level`: within budget the level stays (a run never steps up); over budget the
  saver steps down one level at its next natural break; over budget at the bottom level the
  saver is too heavy for the board, and its fallback takes over at once.

Each step down and each too-heavy mark is stored at `/display/screensaver_levels/<name>` as
`{"level", "too_heavy", "version", "board"}`, except one taken while printing: the halved
budget says nothing about the idle board, so that step down lasts for the run only, and a saver
too heavy at its bottom level while printing shows the black screen for the rest of the run,
unmarked. The next run of that saver starts there, unless
`helix_version_full()` or the board fingerprint (display path, cores, bogomips to the nearest
100, resolution, color depth) differs, which starts again at level 0.

**A too-heavy saver never shows a black screen while another saver fits.**
`ScreensaverManager::runnable_type` (`runnable_screensaver` in the registry) picks what runs in
its place: `kDefaultScreensaverType`, then the registry order, skipping every saver marked too
heavy for this board and version. `start()` resolves through it, so the idle timeout, the
settings Test button and `HELIX_SCREENSAVER_NOW` all run the same fallback, and the gate starts
it the moment a running saver is marked. A static black `SaverOverlay` with no frame timer is
the last resort, when every saver is too heavy. The stored `/display/screensaver_type` is never
rewritten, so a config moved to a faster board runs the saver it names.

The settings dropdown (`DisplaySettingsOverlay::init_screensaver_dropdown`) lists Off, the
savers `ScreensaverManager::settings_dropdown()` keeps, then Random, so its index is not the type: the overlay keeps
the stable name at each position and stores the picked one. A too-heavy stored choice is hidden
and the dropdown selects the saver that runs in its place. On a board where every saver is too
heavy, the stored one stays listed and selected, marked "(too slow)" in the list, so choosing
Off is a real change; the closed field shows its plain label. The XML `options` still carry the
full registry list as the translation source, and the row's `options_tag` puts that list back
on a language change, so the overlay rebuilds its own list after it.

Random (`ScreensaverType::RANDOM`, stored as `"random"`) is a choice, not a saver: it has no
`kScreensavers` row, and its value sits outside the row range so savers added later never
collide with it. Each `ScreensaverManager::start(RANDOM)` draws one saver with
`helix::ui::random_screensaver`: any registered saver the board can run, other than the one the
previous Random run drew while another is runnable. A start while a saver is up keeps it, and a
drawn saver that later proves too heavy falls back as any other would. Random stays listed while
any saver can run, and when every saver is too heavy only if it is the stored choice.

`HELIX_SCREENSAVER_BUDGET_PCT` replaces the budget and `HELIX_SCREENSAVER_LEVEL` forces a level
with the gate off (`docs/devel/ENVIRONMENT_VARIABLES.md`). A forced level runs a saver stored as
too heavy, with no fallback, and the dropdown offers it.

## Names, not numbers

Everything outside the app names a saver by its stable `kScreensavers` name (`"maze"`, `"off"`
for none): `/display/screensaver_type` in settings.json, `HELIX_SCREENSAVER_NOW`, `helix-screen
ctl screensaver <name>`, and the level store key. `ScreensaverType` is in-app identity only, so
reordering the registry, adding a saver or compiling one out never changes what a stored value
means. The setting is read leniently (`screensaver_from_setting`): a name in any case, or a
legacy integer through the frozen `kLegacyScreensaverTypeNames` table, which config migration
v28 also uses to rewrite old numbers as names. An unknown name, an out-of-range number or another
JSON type reads as the default. Writes always store the name.

## Color depth and builds

The registry row lists the depths a saver draws at. Every saver draws both RGB565 and
XRGB8888 through `PixelWriter`, with `kSaverBuildCanvasFormat` picking the display's depth
at compile time, so a 16 bpp build compiles all of them (Makefile `SCREENSAVER_16BPP_TARGETS`
is the depth-16 target list; `SCREENSAVER_32BPP_ONLY_SRCS` is the exclusion list a saver that
cannot draw 16 bpp would land on, currently empty). The two lists and the registry's depth
bits must agree.

The fresh-install default is depth-aware (`default_screensaver_type`): Flip Clock when the
build draws at its depth, otherwise the first registered saver that does, and Off when none do.

`HELIX_ENABLE_SCREENSAVER` decides whether a build has savers at all, and every guard tests
its value: `#if HELIX_ENABLE_SCREENSAVER` / `#if !HELIX_ENABLE_SCREENSAVER`, never `#ifdef`.
The Makefile (with `ENABLE_SCREENSAVER=yes`, the default) and the Android CMake build pass a
bare `-D`, which reads as 1; `ENABLE_SCREENSAVER=no` leaves it undefined, which reads as 0. The ESP32
firmware defines it as 0 (`firmware/helixscreen-esp32/components/helixapp/CMakeLists.txt`),
which an `#ifdef` would read as on. A build without savers still registers
`settings_screensaver_type` (always Off) and `settings_has_screensaver` (0), so the XML
bindings on them resolve: `row_screensaver` and `container_test_screensaver` start
`hidden="true"` in `ui_xml/settings_display_overlay.xml` and their bindings decide from
there, with no C++ hiding them.

## The idle timeout and the dim row

The saver starts at the dim timeout (`/display/dim_sec`). Display settings offers that
timeout in one of three forms, picked by `DisplaySettingsManager::dim_row_kind()` from whether
the display has a dimmable backlight and whether a saver is enabled (type not Off), and
published as `settings_dim_row_kind`:

| Dimmable backlight | Saver enabled | Row shown |
|---|---|---|
| yes | either | Screen Dim |
| no | yes | Screensaver after |
| no | no | none |

Both rows edit the same setting. The rule that sleep may not come before the dim stage
(`should_couple_sleep_to_dim()`) holds exactly when a row is shown, so the two cannot
disagree, and `set_screensaver_type()` republishes the row kind when the saver changes.

## Adding a saver

1. Add the type to `ScreensaverType` and a row to `kScreensavers` with a stable name and depths.
   The name is what configs store, so it never changes once shipped.
   `kLegacyScreensaverTypeNames` stays as it is: a new saver has no legacy number.
2. Add the label to `row_screensaver` in `ui_xml/settings_display_overlay.xml` (a test
   compares it with the registry; the overlay builds the shown options from the registry, and
   this list is their translation source), run `make translation-sync`, translate the new key in every
   `translations/*.yml`, then `make translations` (and `make regen-text-fonts` for new CJK text).
3. Derive from `SaverBase` in `include/screensaver_<name>.h` and `src/ui/screensaver/screensaver_<name>.cpp`;
   keep the scene in a pure simulation class when it writes pixels, so it is testable without a
   display, and derive from `SimSaver<Sim>` (or `ClockSaver`) to run it. Register the factory in
   `ScreensaverManager::ScreensaverManager`.
4. Add the new sources to `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`
   (the Makefile screensaver filter picks up `src/ui/screensaver/*.cpp` by itself).
5. Add the saver and its sim to the contract tests (step 8), test what is particular to it with
   the sim kit (`tests/test_helpers/screensaver_sim_kit.h`: `TestFrame`, `SimRun`, `diff_against`,
   `check_replays`), and record the saver's pixel fingerprint in
   `tests/unit/test_screensaver_fingerprint.cpp` (`HELIX_SCREENSAVER_FINGERPRINT_WRITE=1` records
   it). Then measure the saver on a board with `scripts/screensaver-perf/`.
6. A saver costs no RAM while it is not running. Nothing sizable goes in `.data` or `.bss`: no
   static or global textures, frames, sprite sheets or lookup tables (small `constexpr` tables in
   `.rodata` are fine). Everything is allocated in `on_start()` and released, capacity included,
   in `on_stop()`; `clear()` on a vector keeps its capacity, so swap it with an empty one. Check
   with `size build/obj/ui/screensaver/screensaver_<name>*.o`.
7. Size for the largest need of any rung from the start level down; never grow on a step down;
   free what the still rung doesn't need after its paint. A run never steps up (the gate only
   steps down), so init sizes for the start rung and every rung below it (a ladder whose still
   rung is finer than the rung above, as Aqua's and High Fly's are, counts that rung too), a step
   down allocates nothing, and a board that starts at a low rung never holds the top rung's
   memory. A buffer only some rungs use (maze's fogged floor and ceiling bands) is freed on a
   rung that drops it, and a still rung frees the scene behind its clock once it has painted it.
   A request for a finer level mid-run (a test, or a forced level) reallocates then; that is the
   rare path.
8. Two test files check every saver and sim, so a new one only adds itself to their type lists:
   `tests/unit/test_screensaver_contract.cpp` (level 0's period and refresh, registration, the
   canvas format, the sim's ladder, a level request reaching the frame period, a stopped saver
   holding no sim buffers, the stride-derived canvas extent, the X byte) and
   `tests/unit/test_screensaver_sim_contract.cpp` (dirty boxes cover every change, rows stay in
   bounds, XRGB8888 stays opaque, a seed replays and another differs, the heap never rises above
   its init value from any starting rung, a still rung frees its scene, `clear()` frees it all;
   and `[.][saver_bench]`, ms per frame at every rung). Every sim implements `heap_bytes()`.

## Fireflies

A clean reimplementation of Matt Perry's Fireflies from its described behaviour and published
numbers only: the original is GPL-2.0-only, so nothing of its source is used. On black, 2 to 5
invisible baits wander a box 80 units tall, aspect-scaled wide and 50 deep, seen through a fixed
perspective camera, and 100 to 175 flies chase them in coloured swarms. Seven modes break up the
wandering every 8 to 15 s: stop, loop, glow, hyperspeed, split, merge and a wind gust.

- **Fixed steps.** `FirefliesSim` advances the world in 10 ms steps and reads its random
  sequence only inside a step, so the motion is the same at every frame period and level. A
  level changes only what is drawn: the first `max_flies` of the whole population, tails cut at
  the level's `tail_ms`.
- **Tails.** Each fly records a point every 50 ms. A tail is drawn head to oldest as Bresenham
  lines widened along the minor axis, brightest in the middle (`fireflies_tail_brightness`), with
  each point blown downwind by its age.
- **Dirty tiles.** The loops of one swarm span most of the screen, so a box per swarm would
  redraw it all. The sim marks the 16 px tiles each frame draws in, blacks out the tiles the
  last frame drew, and reports both frames' tiles joined into at most
  `kFirefliesMaxDirtyAreas` boxes.
- **Cost.** The ladder test holds the bottom rung to a 1-core board's 10% with the K1C
  redraw model (`tests/test_helpers/screensaver_cost_model.h`), which does not count the sim's
  own drawing (about 1 ms a frame on a desktop at level 0). Only a device run of
  `scripts/screensaver-perf/` measures the whole cost.
- **RGB565.** Light adds through `add_light` with a `kBayer4x4` threshold, so a faint tail end
  shows as a sparse pattern instead of rounding to nothing.

## Flip clock

The clock savers (the flip clock, Aqua Surface and High Fly) are the ones that read app state.
`SaverHost::print_status` hands them the print lifecycle, the remaining time the print status panel shows (`print_time_left`, smoothed
by `PrintEtaEstimator`) and the displayed progress; `saver_clock_now()` adds the local time,
the date and the Time Format setting. `ClockState` (shared with Aqua Surface and High Fly)
decides what the clock reads, and `flip_face` turns that reading into cards:

- Idle, cancelled, failed, or preparing with no estimate yet: the time of day, HH:MM, with
  AM/PM on the first hour card in 12-hour mode, where that card is blank before 10.
- A print with an estimate: H:MM:SS (as many hour cards as the hours need, up to
  `kClockMaxCountdownS`) counting down at wall-clock rate, with "done at
  <time> · <n>%" under the cards. An estimate within `kClockCountdownResyncS` of the running
  count is ignored, so estimator jitter never flips the seconds backwards.
- Paused: the count freezes under PAUSED.
- A print the clock saw finish: 0:00:00 and DONE for `kClockDoneHoldMs`, then the clock.

Every digit has its own card, grouped by a `FlipShape` (hours, minutes, seconds).
`layout_flip_cards` puts every group in one row with a colon between groups, or, when that
gives larger cards (portrait), one group per row. `FlipClockSim` draws the digits from
strokes (lines and arcs, `build_stroke_digits` at a stroke of 0.2) into a coverage cache
sized to the layout, so they scale to any screen and draw at both depths. Only the cards whose digit changes flip, each with its own
fold and dirty box, all at once when several change (09:59 to 10:00 flips four, 10:59 to
11:00 three); nothing is written between changes. A new shape (the clock becoming a
countdown, the hours gaining a digit) and the drift every `kClockDriftStepMs` repaint the
layout whole. The caption and AM/PM marker are `lv_draw_label` text on the canvas
(`SaverLabel`), which is why its canvas uses `kLvDrawCanvasFormat`.


## Long runs

A saver can run for days. The sims keep time as a double and reduce every periodic phase from
it before it reaches float maths: High Fly's deck scroll to one field tile, the band drift to
one texture width and the wisp scroll to one table turn; Aqua's wave and mode phases to one
turn and the wrinkle scroll to one table turn. Tests start both sims thirty days in.

## The shared clock face

`ClockState::step` takes the time, the date and the print each frame and keeps one reading:
the time of day, or a countdown anchored to the print's remaining time (jitter under
`kClockCountdownResyncS` ignored), frozen while paused, and DONE for `kClockDoneHoldMs`
after a print it saw finish. The flip clock replaces the time with the countdown; Aqua
Surface and High Fly keep the time as the main readout and show the print as secondary text
from `clock_print_text`: "ETA 9:41 PM" and "1h 12m left". With no print they show
`clock_date_text`, the long date and weekday from `locale_formats`
(`format_localized_long_date`, `format_localized_weekday`). All three drift by
`clock_drift_at` every `kClockDriftStepMs` against burn-in.

Text on a canvas that repaints beneath it is a `SaverLabel`: it redraws its line when the text
or box changes, or when a box the frame repainted crosses it. The scene puts the pixels back
first; the label never erases.

## The heightfield renderer

Aqua Surface and High Fly are both a surface seen at a grazing angle from just above it, drawn
by `HeightfieldRenderer`, a voxel-space column renderer. Depth slices run front to back, each
`dz_grow` thicker than the last; for each slice the scene fills one row of heights and colours
(`HeightfieldScene::sample_slice`), the renderer projects each height to a screen row, and
where that row rises above the column's current top it fills the span and raises the top. The
cost is columns x slices samples plus the fills, whatever the scene shows; per-slice constants
are tables built once per view, so the inner loop has no divide.

- A span shades from this slice's colour at its top to the previous slice's at its bottom,
  only where the previous slice also drew the column. Elsewhere it is flat, or near colours
  would smear upward as icicles.
- `FEATHERED_SILHOUETTE` (water) draws hard spans, then gives the final silhouette pixel its
  fractional coverage and a [1 2 1] smoothing across columns.
- `SOFT_UNDER` (clouds) composites the top `feather_px` rows of each new span under what is
  already there and raises the column's top only to the span's top plus the feather, so
  farther clouds show through soft edges.
- `sink_depth` scales near heights down so near clouds pass under the camera instead of being
  cut into a wall by the near plane.

`render()` leaves premultiplied colour, coverage and the slice where coverage reached half
(for depth-testing billboards); `resolve()` composes that over a background into a row-major
frame; `upscale_frame()` writes rows and columns of it to the canvas at 2x (bilinear) or any
other scale (replicated), dithered at 16 bpp. It decides the output format once per call, sums
the three channels in 16-bit lanes of one word, and at 32 bpp copies a replicated row from the
row above. Both savers render at half the canvas and upscale.

## Aqua Surface

Calm, dark, glassy water sloshing in slow motion behind a tank wall (`AquaSim`). The height is
an analytic sum evaluated each frame into a grid indexed by screen column and depth: three
tank sloshing modes, four steep Gerstner waves with a one-pass inversion (crests stay peaked),
and impulse pulses that split and travel apart, all on sim time at a quarter of real time.
The shading is fresnel reflection (floored at `kAquaFresnelFloor`) of four soft "softbox"
bands, read from an environment table pre-blurred at six levels and picked per pixel by how
fast the reflection changes across neighbours, plus a sun glint, light through thin crests,
and one octave of wrinkle; then a near-neutral cool tint.

- The near edge always carries a lit meniscus (`kAquaRimFloor`); below it are near-black
  water, a faint downward smear of the edge's light, and rising bubbles.
- Droplets fly from fast-rising crests near the front on a 50 ms emission step. A droplet
  lives until it is below the surface at its own position, never on a timer, and lands with
  a brief glint. `kAquaMaxDroplets` caps what is emitted; nothing in flight is culled.
- The ring clock: the hour ring fills through the 12 or 24 hours, the minute ring through the
  hour, and the small top ring through the minute, or the print's progress while one runs.
  The date or print line under the rings is a `SaverLabel`.
- A frame repaints the water band (from the highest silhouette down, the clock included) and
  the boxes of droplets above it. The last rung keeps the scene still and redraws only the
  clock, from a copy of the pixels under it; a drift step puts back the old place and copies
  the new one, without rendering the water again.

## High Fly

A flight just above sunlit cloud tops (`HighFlySim`). Back to front: a static sky with a sun
glow and four soft shafts, built once; a far billow band, a tileable texture drifting sideways
so the skyline is never flat; the deck, the column renderer over a baked 256 x 256 tileable
cloud heightfield (puffs at three scales, noise, a talus pass and a blur; shaded by Lambert
light and two scales of ambient occlusion) scrolling toward the camera, with near clouds
pushed to white and far ones fogged toward the horizon blue; translucent full-width layers
and towering cumulus billboards, each depth-tested per pixel against the deck; and a static
white haze over the bottom rows.

- Towers: three shapes baked at start, lit from above with sunlit crowns; one enters every
  13 to 14 s on alternating sides, comes on at the deck's speed, keeps its world x so it
  slides out to the side as it nears (with a little wind drift), and leaves once off screen.
- A fly-through every one to two minutes: the cloud ahead grows from the centre, the screen
  eases toward a blue-grey interior capped at `kHighflyWhiteoutMax`, wisps stream outward
  from a polar lookup sampled bilinearly, and it clears. Over the clock the two together cover
  at most `kHighflyClockCoverMax` of the scene, so the numerals stay readable.
- The clock: thin stroke numerals at the cloud line with superscript seconds, AM/PM under
  them and a two-line date (or the print's ETA and time left) on the left. Every part has a
  soft deep-blue shadow so it reads over white cloud: under the numerals a halo baked once per
  glyph size (`build_stroke_halo`), under the text lines a `SaverLabelHalo`.
- The numerals and their shadows are baked once per digit change into a layer per group
  (hours and minutes, seconds): per pixel, the colour they leave over black and how much of the
  scene shows through. A frame composes those layers over whatever it repainted, so the
  stroke and halo blends run only when a digit changes, and the drift only moves the layers.
- A frame repaints the band of rows from the far billows or the deck down, plus boxes above it
  for the clock, towers and the approaching cloud; a fly-through repaints the whole screen. The
  last rung keeps the scene still and redraws only the clock.
- Its per-pixel work is fixed point: the deck is stored as 16-bit heights and byte shades (half
  the floats' cache footprint), billboards sample with two channels per multiply and depth-test
  through a per-slice visibility table, and the renderer's span gradient and feather step in
  16.16 and 1/255ths.
