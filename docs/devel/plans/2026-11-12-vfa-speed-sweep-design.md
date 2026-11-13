# Motor ripple speed sweep ("Vertical Artifact Check") - design

Status: design, 2026-11-12. Target 1.2. Deleted when the work ships.

## Why

Fine, evenly spaced vertical lines on walls (VFA) come from motor torque ripple: each
stepper's torque wobbles once, twice and four times per electrical cycle (1x, 2x, 4x). At
the print speed where one of those ripple frequencies lands on a frame or toolhead
resonance, the toolhead shakes and the walls show it.

Cart's OpenCreator firmware for the FlashForge Creator 5 measures this with the toolhead
accelerometer, tunes FlashForge's MCLib motor compensation, and draws a before/after PNG
per motor (`VFA_COMPENSATION_GRAPH`). Preston wants that in HelixScreen 1.2, with three
changes:

- drawn natively in the style of belt tuning and input shaper, not a PNG;
- **useful on Voron, Creality, Anycubic and QIDI printers**, not just FlashForge;
- results that fill in live while the test runs.

Success: a user with VFA lines runs one test from the screen, sees which print speeds shake
their machine, gets a concrete slicer change ("set Orca's Outer wall speed to 120, not
160"), and is warned before printing a file whose outer walls land in a bad band. On a
FlashForge MCLib printer they also see how much the compensation helps.

## Evidence this rests on (spike, 2026-11-12)

Scripts and data were in the session scratchpad; the results are recorded here.

- **The method works on ordinary printers.** A Voron 2.4 (ADXL345, TMC2240, Kalico) showed
  one resonance near 216 Hz, hit by the 4x ripple at 43 mm/s, the 2x at 86 mm/s (about
  3000 mm/s^2, 2.7x the broadband baseline) and the 1x at 172 mm/s. A QIDI Q2 showed a
  resonance near 131 Hz hit by all three ripples at their own speeds. The three harmonics
  agree on each machine, which is the self-check that the numbers are real.
- **Each test speed needs about 2 s of cruise.** 37 speeds per motor from 20 to 300 mm/s
  took 2 minutes per motor on the Voron and 3 on the Q2.
- **Measurements repeat to within 1-3%** between runs.
- **Accelerometer rate varies by firmware and must be measured, not assumed.** The Voron
  delivers 3105 Hz. QIDI's `lis2dw` driver polls about 385 Hz (the chip runs at 1600),
  so the Q2 sees nothing above about 170 Hz, which hides most wall-speed ripple.
- **Driver microstep-table tuning is not worth shipping.** On TMC2240s, reshaping the
  table (`driver_mslut*`, `offset_sin90`, `start_sin90`) changed the 4x ripple repeatably,
  but at a real resonance the best table only cut it about 10% (Voron Y, 43 mm/s), and
  the 1x and 2x hardly at all. FlashForge's MCLib gets 64-97% because it adds a
  correction at any phase inside its own current loop; a TMC table offers one fixed
  phase. The Q2 showed 52% only at a 10 mm/s point that does not matter for printing.

## Goals

1. **Speed sweep on any supported printer with an accelerometer**: per motor, ripple
   amplitude (1x, 2x, 4x) and overall shaking against print speed, drawn live as each
   speed finishes.
2. **Speeds to avoid**: bands of print speed that shake, each with the ripple and frequency
   that causes it, and slicer advice naming the setting to change.
3. **Pre-print warning**: when a file's outer-wall speed falls in a saved band for this
   printer, warn before the print starts (Warn, never Block).
4. **FlashForge MCLib printers** (stock, Z-Mod, OpenCreator): the same sweep run with the
   compensation off and then on, shown as before/after per ripple, the way Cart's chart
   does.
5. **Honest limits**: the screen says how high this printer's accelerometer can see, and
   which ripples at which speeds are hidden by it.

## Non-goals

- Tuning compensation on non-FlashForge printers, TMC microstep tables included (see the
  evidence above).
- Writing or editing slicer profiles.
- Delta and other kinematics beyond CoreXY and Cartesian.
- Running when HelixScreen is not on the printer's own computer (the stream needs
  Klipper's local socket, like Belt Tension).
- Replacing the existing Vertical Artifact Compensation row. It keeps running the
  firmware's own calibration.

## Anti-goals: a failure even if it technically works

- Moving the machine during a print, without the user confirming a clear bed, or leaving
  changed velocity limits, a changed gcode state or a changed compensation state behind
  (after success, error or cancel alike).
- False "avoid" bands from noise. Users slowing prints for nothing is worse than no
  advice. Bands need a clear margin over the sweep's own typical level.
- Presenting numbers above the accelerometer's measurable ceiling as if they were
  measured.
- Calling the FlashForge "after" curve "calibrated" on stock firmware, whose
  `STEPPER_RESONANCE_DAMP_ENABLE` applies fixed factory numbers.

## How it works

### Which motor each move turns

- **CoreXY** (`kinematics: corexy`): a diagonal move turns one motor. stepper_x is
  A = x + y, so moves along (1, 1) turn only A; stepper_y is B = x - y, so moves along
  (1, -1) turn only B. For belt speed s, the toolhead feedrate is s / sqrt(2).
- **Cartesian**: X moves turn the X motor. The Y motor is measured only when an
  accelerometer rides with the Y axis: `resonance_tester` `accel_chip_y`, or a single
  chip on a bed that moves in Y. With only a toolhead chip on a bed-slinger, Y is
  skipped and the screen says why.
- **Speed axis** on every chart is motor belt speed, which equals the print speed of an
  axis-aligned move. That is the number a slicer setting uses.

### Ripple frequencies

`f_k = k * s / L`, k in {1, 2, 4}, where L is the belt travel per electrical cycle:
`L = rotation_distance / gear_ratio_product * 4 / full_steps_per_rotation` (0.8 mm on the
Voron, 0.776 mm on the Q2). These are read from the configfile `stepper_x` / `stepper_y`
sections, which discovery does not read today.

### Capture

Stream samples with `BeltStreamClient` (`include/belt_stream_client.h`) from Klipper's
UDS endpoint `<chip>/dump_<chip>`, not `ACCELEROMETER_MEASURE` CSVs. It already handles
chip naming, rebased timestamps, overflow counts, the stall timeout and the measured sample
rate. The QIDI Q2's `lis2dw` exposes the same endpoint. The chip comes from
`resonance_tester.accel_chip` (`accel_chip_x` / `accel_chip_y` when set).

### Motion plan (pure, testable)

Per motor and speed, one gcode script:

1. Approach the start point at travel speed.
2. Do an even number of back-and-forth passes.
3. `M400`, then a short dwell so the next capture starts quiet.

Rules from the spike:

- Pass length is `clamp(v * 1 s, 30 mm, ~85% of the usable diagonal)`, centred on the
  `resonance_tester` probe point. Use the inputs `BeltSettleInputs` already gathers (axis
  limits, homing origin, tool count for docks).
- Enough passes for about 2 s of cruise in total.
- Speeds run geometrically from 20 mm/s, each about 8% above the last, up to
  `min(300, 0.9 * max_velocity)` (about 37 per motor).

Around the whole run:

- `SAVE_GCODE_STATE` and a snapshot of `max_accel` and `minimum_cruise_ratio`.
- During the run, `SET_VELOCITY_LIMIT ACCEL=min(10000, max_accel) MINIMUM_CRUISE_RATIO=0`.
- Afterwards, restore both, plus `RESTORE_GCODE_STATE`. This also runs on error, cancel
  and panel teardown.
- `src/calibration/flashforge_pa_sweep.cpp#Sweep::restore_script` is the template.
  Older firmware without `MINIMUM_CRUISE_RATIO` gets `ACCEL_TO_DECEL`, matching whatever
  the toolhead status publishes.

Preconditions:

- Refuse during a print (`machine_motion_blocked`).
- Home through `ensure_homed_then`.
- Raise Z to a clearance height (20 mm, or the current Z if higher, capped at max Z).
- The confirm screen asks the user to clear the bed.

### Analysis (pure, no LVGL)

For each speed's samples:

1. Split into Hann frames of `max(0.16 s, 6 / f_1)`, capped at 0.5 s, hopping by a
   quarter frame.
2. In each frame, take the amplitude at each `f_k` by single-bin DFT (Goertzel),
   combining the three axes in quadrature. Also take a broadband level from 15 Hz to the
   ceiling.
3. Keep only the frames with real motion (broadband above 30% of the 90th percentile).
   The median over those frames is the speed's value for each ripple and for broadband.
4. Mark a ripple unmeasurable at a speed when `f_k` is above the ceiling, about 0.45 x the
   measured sample rate.

Bands come from these values:

- **Rule:** a speed is "shaky" when any ripple there exceeds **3.0x that ripple's own
  median over the sweep and 500 mm/s^2**. Each shaky speed covers half the gap to its
  neighbouring speeds on either side, and overlapping or touching ranges merge into one
  band. The band is labelled with the ripple whose peak inside it is highest, that peak's
  speed and its frequency.
- **Why not broadband:** the spike tried broadband first, and it missed the Voron X
  motor's 1x peak at 172 mm/s. That ripple was 5x its own median, but broadband rose only
  1.2x. Broadband stays as a chart series only.
- **Fixture results:** checked on the spike fixtures (`tests/fixtures/motor_ripple/`), this
  rule gives:
  - Voron X: 77-97 (2x), 113-122 (2x) and 166-179 mm/s (1x).
  - Voron Y: 42-45 (4x), 77-97 (2x) and 154-179 mm/s (1x).
  - Q2: bands at about 39-45, 49-53 and 84-105 mm/s, among others.
- **Speed cost:** this runs per speed as each capture finishes, off the main thread, and
  must keep up on a MIPS K1C-class board (Goertzel, not a full FFT per frame, if the
  broadband step is too slow).

### FlashForge MCLib printers

FlashForge's compensation is enabled only during prints: print start runs
`STEPPER_RESONANCE_DAMP_ENABLE` and print end runs `..._DISABLE`. So:

- **Before:** sweep after `STEPPER_RESONANCE_DAMP_DISABLE`.
- **After:** sweep after `STEPPER_RESONANCE_DAMP_ENABLE`.
- **Afterwards:** `..._DISABLE` again, so the printer is back in its idle state.

This works on stock, Z-Mod and OpenCreator alike. It does not depend on Cart's `/tmp/vfa_*`
file formats. The capability is the presence of those two macros, which is a new row
beside `include/stepper_resonance_calibration.h`'s procedure table. Vendor names stay in
one module (`.claude/rules/vendor-abstraction.md`).

Results show each ripple's worst speed before and after, with the reduction, like Cart's
lower chart. On stock firmware the "after" is labelled "as your printer applies it", not
"calibrated" (anti-goal above).

After the existing Vertical Artifact Compensation row finishes and Klipper restarts, it
offers "Check the result", which opens this check in before/after mode.

### Slicer speed in the file and the pre-print warning

- **Reading it:** the gcode footer config block carries the outer-wall speed.
  - Orca, Bambu Studio: `; outer_wall_speed = N`.
  - PrusaSlicer, SuperSlicer: `; external_perimeter_speed = N`. This can be a percentage
    of `perimeter_speed`; resolve it, and treat it as unknown if that fails.
  - Cura: not in v1.
  - Add this to `src/rendering/gcode_footer_summary.cpp#GcodeFooterSummary`, which
    `print_start_inputs.cpp#fill_file_context` already scans.
- **The check:** a new pure `gate_outer_wall_speed` in `src/printer/print_start_checks.cpp`
  returns Warn when that speed falls inside a saved band.
- **The message** names the band and the cause, and the slicer setting by the name that
  slicer uses. It suggests the nearest clear speed below and above the band.
- **Wording:** the slicer can still slow walls for volumetric or layer-time limits, so the
  wording says "your file asks for", not "will print at".

### Persistence

The last result per printer goes in Moonraker's database, namespace `helix-screen`
(`moonraker_api.cpp#database_post_item`), key `speed_sweep`. It holds:
- the bands;
- per-motor, per-speed values (so a later run can overlay the previous one);
- the ceiling, sample rate and date;
- for MCLib, both curves.

The pre-print gate reads the bands from this key. A new run replaces it.

### Screen

- **Entry:** a new Advanced > Calibration row beside Belt Tension, gated on
  accelerometer + supported kinematics + co-location. It uses one `bind_flag_if` with a
  combined `cond` (two flag bindings on one widget fight).
- **Shape:** an overlay shaped like `BeltTensionPanel` (START / RUNNING / RESULTS /
  ERROR), with a stall timeout and a co-location probe on show.
  - **Start:** what the test does, how long it takes, that it is loud, a clear-the-bed
    confirm, and the measurable ceiling once known.
  - **Running:** the chart fills per speed, with the motor and speed in progress and a
    Cancel.
  - **Results:** a chart per motor (toggle or tabs), with ripple series coloured as in
    Cart's chart. Avoid-bands are shaded. Before (dashed or muted) and after (solid)
    appear on MCLib printers. A summary list of bands comes with the slicer advice, and a
    "Compare with last run" overlay when a saved run exists.
- **Chart:** extend `ui_frequency_response_chart` rather than add a new widget. It needs a
  unit-label setter (mm/s rather than Hz), shaded x-bands, and a dashed style or reuse of
  `set_series_muted` for "before". It already has series, markers and per-tier
  downsampling. `ShaperSweepChart`'s live hand-off is the pattern for the running state.
- **Layout:** the builder measures the panel with `ctl geom` at 800x480 and at the
  smallest breakpoint before choosing it.

### Mock

`--test` needs a synthetic source. Add a sweep-aware `SweepSynth` variant behind
`accel_batch_source_mock.cpp`'s SourceFactory that emits ripple at `f_k` with one or two
resonances, so the panel, bands and gate can be exercised and screenshot headless. The
MCLib mock gets the two damp macros and a lower "after" curve.

## Decisions

- **Preston's:** native charts instead of PNGs; non-FlashForge printers included; live
  results; speeds-to-avoid with slicer advice.
- **Mine, from the spike:**
  - No TMC table tuning.
  - Streaming instead of CSVs.
  - Our own before/after sweep on MCLib printers instead of reading Cart's files.
  - Motor belt speed as the speed axis.
  - Diagonal single-motor moves on CoreXY.
  - The band rule and its starting factor.
  - The persistence key.
  - Warn, never Block.
- **Mine, user-facing name:** "Vertical Artifact Check" sits next to the existing
  "Vertical Artifact Compensation". Preston may rename it.

### Settled after the builder check (my calls)

1. **Creator 5 tool changers.** FlashForge's compensation is per motor, not per tool, and
   the accelerometer rides on the carriage. The sweep runs with one tool picked up, as
   OpenCreator does (it docks everything, then picks up T0 so the moving mass matches
   printing). Default is T0, and the user may pick another as with Z-Mod's `per_tool` row.
   - Preconditions: the existing dock checks; park and return the tool after.
   - Bands are saved per printer, not per tool; the C5's heads are identical.
   - `STEPPER_RESONANCE_DAMP_ENABLE` / `_DISABLE` take no tool.
2. **The pre-print gate compares the slicer's speed against the bands as given.** An
   X- or Y-aligned wall turns both CoreXY motors at the print speed. Angled walls run one
   motor faster and one slower, and the file header cannot say which walls are angled.
   So the warning says "walls along X or Y at N mm/s". A band from either motor counts.
   Bands are stored per motor, and the gate tests the union.
3. **Stock firmware support is decided at runtime, not assumed.** The check probes
   co-location and the stream, as Belt Tension does. A printer whose Klipper refuses the
   `dump_<chip>` endpoint, or whose screen app cannot run on the printer, never shows the
   entry. The spike did not test stock C5 firmware. The builder verifies Z-Mod on the
   Creator 5 Pro at 192.168.30.130 and treats stock as best effort.
4. **Which accelerometer.**
   - CoreXY uses `resonance_tester.accel_chip`, the toolhead chip; diagonal moves need
     the chip that rides with both axes.
   - Cartesian X uses `accel_chip_x` when set, else `accel_chip`.
   - Cartesian Y is measured only when `accel_chip_y` is set. Discovery has no mounting
     field, and a single chip on a bed-slinger is nearly always on the toolhead.
   - Skipped motors are named on the screen with the reason.
5. **Only a completed run replaces the saved result.** Cancel or failure keeps the
   previous run, and the screen says the new one was not saved.
   - On MCLib printers both sweeps must finish.
   - The gate's bands there come from the **compensation-on** curve, because prints run
     with compensation on. The off curve is for the comparison chart only.

Also settled:
- **MCLib capability.** The damp-macro capability is a second query in the same module as
  the procedure table (`stepper_resonance_calibration`), not a row in that table, which
  holds calibration routines.
- **Gate data.** The gate's bands are cached when the printer connects and refreshed when
  a run saves. `PrintStartContext` gains one field.

## Left to the builder

- Exact frame and motion-threshold constants. The band rule's 3.0x and 500 mm/s^2 are
  fixed above, and the fixtures pin them.
- Module and class names.
- Whether the results chart is one widget with a motor toggle or two charts, chosen by the
  geometry measurements.
- How the per-speed segment boundaries are taken from the stream. The median over moving
  frames tolerates loose edges, so marking the stream position when each script is sent
  and when its RPC returns is enough.
