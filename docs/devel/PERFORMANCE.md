# UI Cost on Real Boards

What HelixScreen costs the printer it runs on, measured: CPU at idle, while printing and with
the display asleep, plus memory, per board. On most supported printers the UI shares two
slow cores with Klipper, so every point here is a point Klipper does not get. This doc
holds the numbers, how each was taken so the next run is comparable, and the known costs
still open.

GPU and rendering-path measurements (EGL against software, Pi 3B screensaver pacing) live
in [`GPU_ACCELERATION.md`](GPU_ACCELERATION.md); the screensaver CPU gate and stored levels
in [`SCREENSAVERS.md`](SCREENSAVERS.md). They are linked here, not copied.

## Key files

| File | Role |
|------|------|
| `scripts/perf-farm-check.sh` | Pre-release CPU gate across the physical farm: idle, mock print with animations on, mock print with animations off |
| `scripts/perf-farm-scheduled.sh` | The same gate run unattended: a lock, a kept log per run, one JSON record per device per run in `~/.local/state/helix-perf-farm/results.jsonl`, and `--trend [device] [N]` |
| `scripts/perf-farm/` | A systemd user timer for the scheduled run, not installed anywhere |
| `config/perf_budgets.json` | Per-board budgets for that gate, with the calibration each budget came from |
| `scripts/screensaver-perf/` | Screensaver and Pi 3B measurement harness (see `SCREENSAVERS.md`) |
| `scripts/telemetry-pull.sh`, `scripts/telemetry-analyze.py` | Field memory and frame-time data from opted-in installs |
| `include/refresh_timing.h` | Main-loop sleep caps and the timer periods that change while the display sleeps |

## Reading the UI's own counters

`helix-screen ctl perf` and the debug bundle's `ui_perf` section report frames drawn,
their mean render time and slow-frame count, peak RSS, and each overlay's open time
(request to first drawn frame) and build count, all since start. During a print the
`[PrintHealth]` line gives the same frame figures per minute. Render time runs from
`LV_EVENT_RENDER_START` to `LV_EVENT_RENDER_READY`, so it tells more drawing (frames and
flushed screens up) from slower drawing (render ms up). `src/application/ui_perf_report.cpp#ui_perf_to_json`.

Flushed screens per second is the refreshed area: the pixels each flush hands the driver,
summed and divided by the screen size. In partial and direct render modes those are the
invalidated areas after LVGL joins overlapping ones, so two invalidations of the same
region count once. In full render mode (the DRM backend when it rotates in software)
LVGL redraws the whole screen for any change, so the figure equals frames per second.

## Keeping perf farm results

`scripts/perf-farm-scheduled.sh` runs `perf-farm-check.sh` from the host that reaches the
farm and keeps each device's samples, status, budgets and reported version, so a cost
that creeps up across builds shows up in `perf-farm-scheduled.sh --trend`. It takes the
same environment as the check. The timer in `scripts/perf-farm/` is written but not
installed: each run stops a printer's screen service for a few minutes, so which printers
run unattended, and at what hour, waits on prestonbrown/helixscreen#1910.

## Units and method

**Every CPU figure is percent of ONE core**, from `/proc/<pid>/stat` utime+stime over a
window: `(delta_jiffies / HZ) / window_seconds * 100`. On a 2-core board, 4% of one core
is 2% of the machine. Quote which one you mean when comparing with another screen's claim.

- **perf-farm phases** run a fresh `--test` instance on the device with the service
  stopped: `idle` (screen awake, no print), `print` (mock print, long overflowing filename,
  animations on), `print-off` (same, animations off). Mock prints carry no real status
  stream or Klipper load, so they are a floor.
- **Asleep** is not a perf-farm phase. Measure it by running the binary from a config copy
  with `"sleep_sec": 60` (only 0/60/300/600/1200/1800/3600 are accepted; anything else
  falls back to 1200) and `"dim_sec": 0`, waiting about 110 s, confirming the log says
  `Display sleeping`, then sampling 60 s. Run it without `--test` against the board's own
  Moonraker for the real number: the mock adds its own simulation work.
- **Wakes per second**: `voluntary_ctxt_switches` from `/proc/<pid>/status` over the same
  window. That file is the main thread, which is where LVGL runs. Per-thread CPU comes from
  `/proc/<pid>/task/*/stat`.
- **Is the screen really off?** On boards with a sysfs backlight read
  `/sys/class/backlight/*/brightness`. Otherwise dump `/dev/fb0` (check
  `/sys/class/graphics/fb0/pan` for the visible buffer) and count non-zero RGB bytes.
- **Password-only boards** (dropbear that refuses our keys): perf-farm-check uses
  `BatchMode=yes` ssh, so run it with a `PATH` shim named `ssh` that wraps `sshpass` and
  rewrites `BatchMode=yes` to `no`.

### Finding what wakes the main loop

When a board burns CPU with nothing on screen changing, count which LVGL timers and
animations actually run. A temporary, env-gated block right after `lv_timer_handler()` in
`Application::main_loop()` does it: per pass, walk `lv_timer_get_next()` and count each
timer whose `last_run` changed; every 10 s log the counts with `period`, `paused` and the
`timer_cb` address, then walk `LV_GLOBAL_DEFAULT()->anim_state.anim_ll` and log each
animation's `exec_cb` and its `var`'s name chain (`lv_obj_get_name` up the parents, with
hidden flags). Symbolize the addresses with `nm -C build/mips/bin/helix-screen.debug`.
Never commit the block. On x86, gdb against the full debug binary takes many minutes to
load; `gdb --readnever` and raw struct reads are the fast path.

## CPU, measured

### Creator 5 Pro (Z-Mod, Ingenic X2600, 2 cores), 2026-11-06

| Phase | 1.1.0-beta.16 | with the display-sleep and spinner fixes (1.1.0-beta.17) |
|---|---:|---:|
| Real app, display asleep | 3.5 (91 wakes/s) | 3.0 (41 wakes/s) |
| Real install, awake idle | 5.0 (115 wakes/s) | not measured |
| perf-farm idle (mock) | 4.5 | 3.6 |
| perf-farm print, animations on | 22.5 | 20.4 |
| perf-farm print, animations off | 11.8 | 10.3 |
| Mock, display asleep | 3.1 (119 wakes/s) | 2.1 (39 wakes/s) |

The Creator 5 presets ship animations off, and 43 of the 56 Creator 5 installs reporting
settings keep them off, so the animations-off rows are what most owners see. On this board
Klipper runs at nice -20 and the UI at +10 (the launcher's co-host rule).

### K1C and AD5X, 2026-11-04 and 2026-11-05

Mock print at `--sim-speed 3` on the print status panel, a 1.2-dev build.

| Phase | K1C | AD5X |
|---|---:|---:|
| Print, animations on, long filename, `-Os` | 16.4 | 15.8 |
| Same, LVGL draw + stdlib at `-O2` | 13.8 | 13.1 |
| Print, animations off, `-Os` | 7.2 | 6.4 |
| Print, animations off, `-O2` | 7.1 | 6.2 |
| Idle, installed release | 5.1 | 3.5 |

`-O2` on the draw code adds 116 KB and only helps with animations on. A 1.40x
AD5X-against-K1C render factor measured earlier did not show up here.

### Whole-build `-O2` against `-Os`, Aqua screensaver, AD5M and AD5X, 2026-11-22

The same main source (591285c7c3, with prestonbrown/helixscreen#2156's per-pixel paths) built
both ways for each board, the whole build including LVGL and the other submodules. Each binary
ran as the board's app with `HELIX_SCREENSAVER_LEVEL` pinned (the CPU gate off), Aqua started
through `ctl screensaver aqua`, 15 s of settling, then 30 s sampled. Resident memory is
`VmRSS`/`VmHWM` after a level 3 run and a wake. The AD5M builds came from the
`helixscreen/toolchain-ad5m` image and both MIPS builds from zeus's `helixscreen/toolchain-mips`.

| | AD5M `-Os` | AD5M `-O2` | AD5X `-Os` | AD5X `-O2` |
|---|---:|---:|---:|---:|
| Level 3 (100 ms frames), runs | 63.0, 62.6 | 45.8, 45.4, 45.8 | 84.3, 83.3, 83.0 | 45.3, 45.1 |
| Level 2 (50 ms frames) | 97.6 | 90.6 | 97.1 | 88.5 |
| Stripped binary | 16.5 MB | 21.5 MB (+30.6%) | 30.9 MB | 37.1 MB (+20.3%) |
| `VmRSS` (`VmHWM`) | 23.3 (26.3) MB | 27.7 (30.9) MB | 36.9 (40.7) MB | 41.2 (45.0) MB |

At level 3 `-O2` cuts Aqua's CPU by 27% on the AD5M (Cortex-A7) and by 46% on the AD5X
(XBurst2); why the MIPS core gains so much more is not measured. Level 2 saturates the main thread on
both boards at `-Os` and nearly so at `-O2`. The cost is about 4.4 MB of resident memory on
both, mostly the larger text.

The MIPS target (K1, AD5X, Creator 5) builds at `-O2`: the AD5X has 474 MB, about 320 MB of it
available. The AD5M (108 MB, about 46 MB available) stays at `-Os`, since 4.4 MB more resident
is too much there.

`-O2` on `src/ui/screensaver/*.cpp` alone, over an `-Os` build of the same commit, gets the AD5M
nearly all of the speed for none of the memory, so every `-Os` build compiles the screensavers at
`-O2` (`mk/rules.mk`, keyed on `-Os` in `CXXFLAGS`):

| AD5M, Aqua level 3 | `-Os` | Screensavers `-O2` | Whole build `-O2` |
|---|---:|---:|---:|
| CPU, runs | 63.0, 62.6, 63.7 | 47.9, 48.0 | 45.8, 45.4, 45.8 |
| Stripped binary | 16.47 MB | 16.57 MB (+102 KB) | 21.5 MB |
| `VmRSS` (`VmHWM`) | 23.3-23.5 (26.3-26.7) MB | 23.4 (26.4) MB | 27.7 (30.9) MB |

### LVGL style cache, K1C and AD5M, 2026-11-16

`LV_OBJ_STYLE_CACHE` 1 against 0 on the same source, mock print at `--sim-speed 3`, three
interleaved rounds of three samples each (prestonbrown/helixscreen#2109). Medians:

| Phase | K1C off | K1C on | AD5M off | AD5M on |
|---|---:|---:|---:|---:|
| Idle | 3.1 | 3.0 | 2.5 | 2.5 |
| Print, animations on | 24.6 | 24.0 | 21.2 | 20.8 |
| Print, animations off | 9.4 | 9.2 | 8.2 | 8.1 |

A small, consistent win on both boards. On the AD5M two cache-on launches read far lower
than the rest (16.9 with animations on, 4.0 with them off); they look like the mock print
in a different state and are left out of the medians. The AD5M runs in Forge-X's chroot,
which `perf-farm-check.sh` reaches through the row's `chroot` field.

### Display asleep, K1C and AD5M, 2026-11-17

Idle printer, the board's own Moonraker, a config copy with `"sleep_sec": 60` and
`"dim_sec": 0`, three 60 s windows after a 110 s settle, the original service stopped. A
temporary probe could switch each change off by environment and counted which timers ran
(prestonbrown/helixscreen#2037). Medians, percent of one core (main-loop wakes a second).
The K1C sleeps with the software overlay, the AD5M with hardware blank.

| Build | K1C | AD5M |
|---|---:|---:|
| Status applied at full rate (no telemetry hold) | 2.46 (44) | 2.20 (43) |
| Telemetry held, as on main before this work | 1.65 (40) | 1.46 (40) |
| Plus `mcu.last_stats` on the telemetry list | 1.55 (40) | 1.38 (40) |
| Plus input read on arrival instead of every 33 ms | 1.26 (14) | 1.11 (13) |
| Plus the update queue, debug-touch and memory-stats timers idle | 1.18 (11) | 1.00 (10) |
| Plus the plugin-install prompt's wait for the screen stopped while asleep | 1.15 (11) | 0.96 (9) |
| Every status frame dropped while asleep (the ceiling, on the 1.65 build) | 1.18 (39) | 1.06 (39) |
| Awake idle | 3.65 (65) | 3.45 (65) |

The widget-binding hold (`lv_subject_hold_obj_observers()`) was measured on the earlier
build with the input read still polled: it took the K1C from 1.51 to 1.35 and the AD5M from
1.35 to 1.21, with no widget written and no refresh pass asleep, and the first frame after a
wake settling in 0.3-2.5 ms.

**With the hold, 2026-11-26** (main b7a2483fbd, same procedure: a side tree of the build
beside the installed one, its own config copy, three 60 s windows after a 110 s settle,
each window within 0.02 of the others):

| Build | K1C | AD5M |
|---|---:|---:|
| Main with every change above plus the binding hold | 0.87 (6.6) | 0.84 (5.7) |

Status arrives at about 4 frames a second on both boards (`toolhead.estimated_print_time`
changes in every one), and each MCU reports `last_stats` once a second, which released the
telemetry hold every second until it joined the list. What still wakes the loop asleep is
in "Open costs".

### For comparison

GrumpyScreen, as its users quote it: about 5% at idle and 0.7% with the screen off, 5 MB
of RAM. Whether that is one core or the whole machine is not stated.

## Does the UI cause "Timer too close"? (Creator 5 Pro, 2026-11-06)

Klipper on the Creator 5 is FlashForge's fork, which adds short-move limiters to its
`klippy/extras/gcode_move` module on the printer (moves of 0.75 mm or less take at least 2.5 ms; short extruding moves at
most 80 mm/s, from firmware 1.9.9) and a dynamic one to its `klippy/toolhead` module (scales in from 80 to
200 moves per second). Its own comment says they exist to avoid "Timer too close". Owners
who disable them for speed set `short_move_limit: False` and `short_extrude_move_limit:
False` under `[printer]`, which turns off both.

### XY-only moves

No heat, heads docked, moves sent through Moonraker's `gcode/script` at 30k accel, the UI
awake on the home screen. "Connected" means HelixScreen held a live Moonraker connection;
from the 240 mm/s row down, HelixScreen was on screen but disconnected (an oversized
console-history reply had dropped its websocket), so those rows compare the screen's own
CPU, not its load on Klipper.

| Load | No screen process | HelixScreen |
|---|---|---|
| Limiters on, 0.5-3 mm segments, 400-600 mm/s, 5 min | not run | connected: no TTC; klippy 20-39% of a core, HelixScreen 4.5-5.0% |
| Limiters off, 0.1-0.3 mm at 600 mm/s | TTC at 45, 35, 14 s | connected: TTC at 36, 11, 36 s |
| Limiters off, 0.2 mm at 240 mm/s | TTC at 54 s | not connected: TTC at 57 s |
| Limiters off, 0.2 mm at 200 mm/s, 3 min | 1 survived, 1 TTC at the same move block | not connected: 2 TTC at the same move block |
| That move block replayed alone | 4 of 4 survived | not connected: 4 of 4 survived |
| Limiters off, 0.2 mm at 170 mm/s, 3 min | survived | not connected: survived |

With the limiters off, klippy's single Python thread reaches 100% of one core near 750-900
moves per second, while the box still has a free core, and the MCU then shuts down with
"Timer too close" with or without a screen. Where both modes were connected-or-absent (the
600 mm/s row), the times to failure overlap. The seeded runs died on the same move block
in every mode, so the failure point follows the gcode.

### A 4-colour print with a tool change per quadrant

The reports that started this were 4-colour prints with tool changes and no prime tower, at
maximum speed. Test print: a 60 mm disc, 6 layers, each layer split into four quadrants
printed by the four heads (24 tool changes), perimeters as 0.4 mm arc segments, no prime
tower, Z-Mod's own `START_PRINT`/`END_PRINT`, FlashForge's PLA settings (0.42 mm lines,
0.25 mm first layer at 50 mm/s), speed set by the flow cap. HelixScreen connected and on the
print status panel unless stated. Percent of one core, 10 s windows while printing.

| Run | Limiters | Flow, speed | HelixScreen | TTC | klippy median / max | HelixScreen median / max | Box (2 cores) median / max |
|---|---|---|---|---|---:|---:|---:|
| 1 | on | 18 mm³/s, 214 mm/s | stopped | none | 20.5 / 28.1 | 0 | 14 / 22 |
| 2 | on | 18 mm³/s, 214 mm/s | connected | none | 22.4 / 32.0 | 10.3 / 51.2 | 27.5 / 51.0 |
| 3 | **off** | 24 mm³/s, 286 mm/s | connected | **none** | 24.0 / 36.3 | 9.8 / 40.5 | 29.9 / 51.9 |

Run 3 is the reported scenario with the limiters off, and it printed cleanly. HelixScreen
costs about 10% of one core while printing, about twice its idle cost, and peaks at 40-51%
for the first 10-30 s of printing while the print status panel loads the toolpath
preview. HelixScreen runs at nice +10 against klippy's -20 here. Untested: OpenCreator,
whose Klipper build and process priorities may differ.

### Repeating these tests

- The limiter override goes in `mod_data/user.cfg` (Z-Mod's empty last include) as a
  two-line `[printer]` section; restore the empty file afterwards.
- A Klipper restart on this firmware runs `/usr/prog/bin/wakeup_level` synchronously, and
  it can block on the level board for minutes; killing it lets klippy finish the restart.
- Long `gcode/script` bodies land verbatim in Moonraker's console history. Restart
  Moonraker between XY runs, or the history outgrows the client's 5 MB message cap.
- A print started through Moonraker waits at Z-Mod's "Select print materials" prompt; send
  the prompt's own `PRINT_ZCOLOR` line to start it. `LEVELING=1` there also persists Z-Mod's
  "mesh on every print" setting.
- A generated test file must re-send the layer Z after every `T<n>`: Z-Mod's tool change
  leaves the head wherever it docked.

## Memory, from telemetry

`memory_snapshot` events with more than 10 minutes of uptime, 2026-10-01 to 2026-11-06.

| Board | Total RAM | helix-screen RSS, median / p90 | Available, median / worst 10% |
|---|---:|---:|---:|
| ad5x | 473 MB | 31 / 46 MB | 366 / 301 MB |
| creator5-pro | 472 MB | 32 / 58 MB | 303 / 262 MB |
| creator5 | 472 MB | 32 / 39 MB | 398 / 383 MB |
| k2 | 491 MB | 28 / 44 MB | 337 / 282 MB |
| k1 | 209 MB | 17 / 30 MB | 114 / 96 MB |
| cc1 | 114 MB | 13 / 15 MB | 36 / 30 MB |
| ad5m | 107 MB | 15 / 18 MB | 37 / 34 MB |

The AD5M and CC1 are the boards under memory pressure. Across all telemetry, 59 of 61
Creator 5 devices report 472-473 MB (the other two are desktop installs). The fleet's
Creator 5 Pro test unit is the exception at 228 MB with no swap: its U-Boot (the same
2013.07 build the AD5X runs, which picks the kernel's `mem=` from the DRAM it detects)
boots it with `mem=236M@0x0` only, where a 512 MB board also gets `mem=256M@0x30000000`.
It is a 256 MB unit, or one whose second DRAM bank does not detect. Do not read its
memory headroom as typical.

### What the AMS views keep after closing, 2026-11-22

Desktop mock at 800x480, live heap from glibc `mallinfo2().uordblks` read on the main
thread 40 s after start (once the update check's TLS setup is done, which otherwise lands
about 0.6 MB in the window), then after the first open and close. Owners come from a
scratch `LD_PRELOAD` allocator shim that records a backtrace for every allocation made
after the first sample and dumps the ones still live, symbolized with `addr2line` against
the app binary. A second and third open and close move the heap by under 5 kB.

| Kept after the Happy Hare unit view closes | Bytes | Owner |
|---|---:|---|
| Spool render cache | 572 K | process-wide LRU in `src/ui/filament/ui_spool_canvas.cpp`: 24 new renders, 10 at 96 px and 12 at 64 px, the size a slot's spool has before layout sizes it |
| Widget tree, XML component registration, subjects, observers | about 410 K | the tree is kept between opens (below) |
| Spool canvas buffers | 175 K | each canvas's own ARGB pixels, in the kept tree |
| Current-spool glow | 99 K | the glow image, shared by its layer and the glow cache |
| Baked unit art | 70 K | `src/rendering/baked_art.cpp` |
| Total | 1.37 MB | |

The multi-unit overview (`HELIX_MOCK_AMS=multi`) keeps 0.85 MB the same way: two flattened
unit drawings of 122 K each, four lane fill masks of 30 K, about 56 K of spool canvases, 48
K of spool renders, and its tree. Both views free their largest buffers on close already:
the 1.5 MB backdrop snapshot and the unit view's 480 K path canvas.

The tree stays built on purpose: building it is about 690 of 1250 ms of a reopen on the
K-Touch, and critical memory pressure drops it (`src/ui/filament/ui_panel_ams.cpp#get_global_ams_panel`).
Its buffers go with it. The spool render cache was bounded at 128 entries, which at 96 px
is 4.7 MB. It is now bounded by bytes (`spool_render_cache_budget`): 2 MiB where spools
turn, which holds the 24 vent phases of a turning 96 px spool (885 K) beside a screen of
still ones, and 256 KiB where `reduced_effects()` holds (the AD5M, CC1 and K1 by memory,
the K2 by board), since spools never turn there and every canvas keeps its own pixels, so
an unchanged spool needs no cache entry on reopen. With reduced effects the unit view now
keeps 0.84 MB instead of 1.33 MB. An evicted state costs one render when a spool returns
to it: 216 us at 96 px and 61 us at 64 px on the desktop, not yet measured on a board.

## Where idle CPU goes

The main loop sleeps until the next LVGL timer is due (`main_loop_sleep_ms`), so idle cost
is set by how many timers come due and how often. On the Creator 5 Pro before the fixes,
four timers near 30 ms, out of phase with each other, woke it about 115 times a second
whether the screen was on or asleep, at about 0.25-0.4 ms per wake:

| Timer | Period | Now |
|---|---|---|
| Input read (`lv_indev_read_timer_cb`) | 33 ms | paused while the display sleeps for each device whose fd the backend knows (fbdev evdev): the main loop `poll()`s those fds and reads a device the moment it turns readable (`SleepInputWait`) |
| UI update queue drain | 33 ms | paused while the display sleeps: an enqueue from another thread wakes the loop through the main-loop wake pipe, and the loop drains the queue in every pass |
| Debug-touch ripple | 30 ms, always | 500 ms while the setting is off (`debug_touch_period_ms`), paused while the display sleeps |
| LVGL animation timer | 33 ms | stops when no spinner is visible: a hidden `<spinner>` parks its animations and restarts on its next draw |

### What sleep holds back

Status keeps arriving at full rate while the display sleeps, so a frame that has to wake
the screen (a print ending, an error, a prompt) is applied as soon as it arrives. Two holds
cut the work done for values nobody can see:

- **Telemetry status.** A status frame whose every attribute is on the telemetry list in
  `src/printer/status_coalescer.cpp` is merged into a held delta, latest value per
  attribute, and applied once a second (`kAsleepStatusIntervalMs`, which keeps the
  temperature graph's 1 Hz samples). Any other attribute applies the held delta first and
  then itself, so an attribute missing from the list is applied rather than held.
- **Widget bindings.** `lv_subject_hold_obj_observers()` (`patches/lvgl_observer_obj_hold.patch`)
  skips every observer bound to a widget, which every `lv_*_bind_*` and XML `bind_*` is,
  while observers that record data still see each value. With no label changing, no layout
  pass runs on the hidden screen.

Sleep turns both on (`src/application/asleep_status_wiring.cpp#wire_asleep_status`). Before
the first frame after a wake, the held status is applied, then the held widgets are brought
to the current values, then the update queue is drained, so that frame shows current state.

## Open costs

- **About 0.87% of a core asleep on the K1C, 0.84% on the AD5M** (the table above, measured
  2026-11-26 on main, with the 1 s cap): 6.6 and 5.7 wakes a second. On the 200 ms cap build
  the loop woke about 10 times a second: about 5 for status frames and the work other threads
  queue, about 4 for the cap on its wait (`main_loop_sleep_ms`) and about 1 for the display
  refresh when a label changes on the hidden screen. Awake, the
  plugin-install prompt still looks once a second while it waits for a free screen. Status processing and the widget
  updates behind it are most of the remaining CPU: dropping every frame read 1.18 and 1.06
  on the build before the input wait. What the 1 s cap delays is what the loop still polls
  for itself, a remote screen viewer's arrival; Moonraker request timeouts are checked every
  2 s by `Application::check_timeouts` whatever the cap. Boards whose input
  is not fbdev evdev (DRM with libinput, SDL) still poll input every 33 ms asleep. The
  Creator 5 Pro read 3.0% before this work and has not been measured since.
- **About 10% of one core while printing on the C5, 40-51% for the first 10-30 s** as the
  print status panel loads the toolpath preview. Twice the idle cost.
- **fbdev self-heal repaints the whole screen every 10 s** (`kFbSelfhealIntervalMs`): about
  29% of the pixels flushed on an idle AD5X status panel.
- **A K1C spent about 90% of a core for one 10 s window** as a mock print of a 24 MB file
  ended, attributed to `print_complete_overlay`. Cause open. Averages hide bursts like this
  on a 2-core board, so read per-window samples too.
- **The Creator 5 Pro's print phases run hotter than the K1C and AD5X** (20.4 against about
  16 with animations on). Its 90-degree software rotation is the suspect, unproven.
- **Spinners on `lv_layer_top`, and every spinner under hardware-blank or host sleep**, keep
  animating while the display is dark: only sleep paths that hide the active screen park
  them.
