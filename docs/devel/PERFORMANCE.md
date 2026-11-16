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

## Where idle CPU goes

The main loop sleeps until the next LVGL timer is due (`main_loop_sleep_ms`), so idle cost
is set by how many timers come due and how often. On the Creator 5 Pro before the fixes,
four timers near 30 ms, out of phase with each other, woke it about 115 times a second
whether the screen was on or asleep, at about 0.25-0.4 ms per wake:

| Timer | Period | Now |
|---|---|---|
| Input read (`lv_indev_read_timer_cb`) | 33 ms | unchanged: a tap shorter than the read period is lost, and wake needs the press |
| UI update queue drain | 33 ms | 200 ms while the display sleeps (`update_queue_period_ms`) |
| Debug-touch ripple | 30 ms, always | 500 ms while the setting is off (`debug_touch_period_ms`) |
| LVGL animation timer | 33 ms | stops when no spinner is visible: a hidden `<spinner>` parks its animations and restarts on its next draw |

## Open costs

- **About 3% of a core while asleep is real work, not wakes.** Status updates still flow
  through subjects into widgets nobody can see. Gating that work while the display sleeps
  is the next lever.
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
