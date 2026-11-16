# Vertical Artifact Check (motor ripple speed sweep)

Fine, evenly spaced vertical lines on printed walls (VFA) come from stepper torque ripple.
Each motor's torque wobbles once, twice and four times per electrical cycle (the 1x, 2x and
4x ripple), so each ripple's frequency is `k * speed / L`, where `L` is the belt travel per
electrical cycle (`rotation_distance / gear_ratio * 4 / full_steps_per_rotation`; 0.8 mm on
a 40 mm, 200-step drive). At the print speed where one of those frequencies lands on a
frame or toolhead resonance, the toolhead shakes and the walls show it.

The check drives each XY motor alone through about 37 speeds from 20 to 300 mm/s, streams
the accelerometer, measures each ripple's amplitude at each speed, and reports the bands of
speed to avoid. On printers with FlashForge's MCLib motor compensation it runs the sweep
twice, with the compensation off and then on, and shows how much it removes. A print-start
check warns when a file's outer walls land in a saved band.

User guide: `../user/guide/calibration.md` § "Vertical Artifact Check".

## Key files

| File | What |
|------|------|
| `include/motor_ripple_types.h` | Constants (speeds, cruise, band rule, ceiling) and the shared types |
| `include/motor_ripple_plan.h`, `src/calibration/motor_ripple_plan.cpp` | Pure motion plan: one G-code script per motor and speed, the begin/restore scripts |
| `include/motor_ripple_analysis.h`, `src/calibration/motor_ripple_analysis.cpp` | Pure analysis (Goertzel per Hann frame, median over moving frames) and the band rule |
| `include/motor_ripple_runner.h`, `src/calibration/motor_ripple_runner.cpp` | The run: scripts in order, the stream cut into captures, retry, cleanup |
| `include/motor_ripple_store.h`, `src/calibration/motor_ripple_store.cpp` | The last completed run in Moonraker's database (`helix-screen` / `speed_sweep`) and the cache the print-start check reads |
| `include/motor_ripple_view.h`, `src/calibration/motor_ripple_view.cpp` | Pure screen text, the capability rule, resampling onto the chart's even grid |
| `include/ui_panel_motor_ripple.h`, `src/ui/calibration/ui_panel_motor_ripple.cpp`, `ui_xml/motor_ripple_panel.xml` | The overlay: Start, Running, Results, Error |
| `src/printer/print_start_checks.cpp#gate_outer_wall_speed` | The print-start warning |
| `src/rendering/gcode_footer_summary.cpp#parse_gcode_footer_summary` | The outer wall speed from the file's footer |
| `src/printer/stepper_resonance_calibration.cpp#compensation_toggle` | Which printers can switch motor compensation on and off |
| `include/ripple_batch_source_mock.h`, `src/mock/ripple_batch_source_mock.cpp` | The `--test` stream |
| `tests/fixtures/motor_ripple/` | Real sweeps from a Voron 2.4 and a QIDI Q2 that pin the band rule |

## Motion

- **CoreXY**: a diagonal move turns one motor. `stepper_x` is A = x + y and `stepper_y` is
  B = x - y, so passes along (1, 1) turn only A and passes along (1, -1) only B. For belt
  speed s the toolhead feedrate is s / sqrt(2).
- **Cartesian**: X passes turn the X motor. The Y motor is measured only when
  `[resonance_tester] accel_chip_y` names an accelerometer for Y; otherwise it is skipped
  and the screen says why.
- **Speed axis**: motor belt speed everywhere, the same number as an axis-aligned print
  speed and so the number a slicer setting uses.
- Each speed is one script: an approach, an even number of passes giving about 2 s of
  cruise (pass length `clamp(v * 1 s, 30 mm, 0.85 * usable diagonal)`), `M400`, a 250 ms
  dwell. The run starts with `SAVE_GCODE_STATE`, Z to 20 mm (or the current Z if higher,
  capped at max Z) and `SET_VELOCITY_LIMIT ACCEL=min(10000, max_accel)
  MINIMUM_CRUISE_RATIO=0`, and ends with the limits it read at the start and
  `RESTORE_GCODE_STATE`.
- The panel homes (`ensure_homed_then`) and refuses while `machine_motion_blocked`; the
  runner assumes both.

## Capture and analysis

The stream is klippy's `<chip>/dump_<chip>` endpoint through `BeltStreamClient`, the
same path Belt Tension's live preview uses, so HelixScreen must run on the printer's own
computer. Each capture runs from the sample count when its script is sent to the count
when its reply arrives. A capture with dropped samples (any batch with errors or
overflows) or almost no samples is retried once; a second failure fails the run.

`analyze_capture` splits a capture into Hann frames of `clamp(6 / f1, 0.16 s, 0.5 s)`
hopping a quarter frame, takes each ripple's amplitude by Goertzel per axis summed in
quadrature, keeps only frames whose 15 Hz high-passed RMS is above 0.3 x the 90th
percentile, and reports the median over them. A ripple above **0.45 x the measured sample
rate** is reported as unmeasured, never as zero. QIDI's stock `lis2dw` driver delivers
about 385 samples a second, so on a Q2 nothing above about 170 Hz is measured.

## Bands

A speed is shaky when any ripple there exceeds **3.0 x that ripple's own median over the
sweep and 500 mm/s^2**. Each shaky speed covers half the gap to its neighbours, touching
ranges merge, and a band is labelled with its highest peak. Broadband shaking was tried
first and missed the Voron's 1x peak at 172 mm/s (5x its own median, 1.2x broadband).

On the fixtures this gives Voron X 77-97, 113-122 and 166-179 mm/s and Voron Y 42-45,
77-97 and 154-179 mm/s (`tests/unit/test_motor_ripple_bands.cpp`). Bands are stored per
motor; the screen and the print-start check use their union. On compare-mode printers the
saved bands come from the compensation-on sweep, because prints run with compensation on.

## Motor compensation

FlashForge's MCLib boards add a correcting current inside their own current loop, with a
free phase per ripple. Prints enable it at start (`STEPPER_RESONANCE_DAMP_ENABLE`) and
disable it at the end, so an idle sweep runs uncompensated. Compare mode sends
`_DISABLE`, sweeps, `_ENABLE`, sweeps, and `_DISABLE` again. On stock firmware
`_ENABLE` applies fixed factory numbers, and the screen says the "after" curve is
compensation as the printer applies it.

**Ordinary printers get no compensation.** On TMC2240 drivers (Voron and QIDI Q2, 2026-11)
reshaping the driver's microstep table (`driver_mslut*`, `offset_sin90`, `start_sin90`)
changed the 4x ripple repeatably, but at a real resonance the best table removed about
10%, and the 1x and 2x hardly at all. A table is quarter-wave symmetric and has one fixed
phase, where the ripple's phase moves with speed. TMC2209 drivers have no table at all.
The speeds-to-avoid advice is what those printers get.

## Print-start warning

`parse_gcode_footer_summary` reads Orca/Bambu `outer_wall_speed` and PrusaSlicer
`external_perimeter_speed` (a percentage of `perimeter_speed` is resolved). The footer is
read for it only while a saved sweep has bands (`FooterReadNeed::wall_speed`).
`gate_outer_wall_speed` warns, never blocks, when the speed is inside a band, and names
the nearest clear speeds below and above. The file header cannot say which walls are
angled, so the warning speaks of walls along X or Y.

## Mock

`HELIX_MOCK_ACCEL_RATE` sets the mock stream's rate (default 3200). Each speed script the
mock runs fills the stream with ripple through one 216 Hz resonance; the `creator5_zmod`
persona carries the damp macros, and enabling them cuts the ripple to a quarter.
`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` § `HELIX_MOCK_ACCEL_RATE`.
