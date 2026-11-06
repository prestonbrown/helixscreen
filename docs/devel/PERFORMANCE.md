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
| `config/perf_budgets.json` | Per-board budgets for that gate, with the calibration each budget came from |
| `scripts/screensaver-perf/` | Screensaver and Pi 3B measurement harness (see `SCREENSAVERS.md`) |
| `scripts/telemetry-pull.sh`, `scripts/telemetry-analyze.py` | Field memory and frame-time data from opted-in installs |
| `include/refresh_timing.h` | Main-loop sleep caps and the timer periods that change while the display sleeps |

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
