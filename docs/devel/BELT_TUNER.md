# Belt Tuner (pluck-based)

Developer guide for the live belt-tension tuner: the user plucks a belt by hand, the tool
listens on Klipper's live accelerometer stream, and reports the belt's fundamental
frequency. Replaces the deleted `TEST_RESONANCES` sweep and the strobe path.

**Panel**: Belt Tension (`panel_belt_tension`) - Advanced panel row, beta-gated
**User guide**: `../user/guide/calibration.md` § Belt Tension

---

## ⚠ Validation status: NOT verified on hardware

**Read this before changing a threshold, before promoting the feature out of beta, and
before believing a number it prints.**

It is green in CI - 96/96 shards - and it has **never measured a real belt under its own
UI**. Everything below is open.

### The thresholds are circular

Every gate constant in `pluck_detector.h`, `pluck_aggregator.h` and `pitch_estimator.h`
was measured against captures from **one Voron 2.4, on one evening (2026-08-09)** - and
the algorithm was then tuned against that same set. Nothing has broken the circle yet.

Two capture counts appear in the tree and they are not the same thing. Conflating them
overstates the evidence:

| Number | What it actually is |
|---|---|
| **60 captures** (cited in `pluck_detector.h`) | Individual plucks from the 2026-08-09 probing session. Scratchpad only, **not committed**. |
| **8 fixtures** (`tests/fixtures/belt_plucks/`) | The committed corpus the unit tests run against. Two belts, one machine, one evening. See that directory's `README.md`. |

Both come from the same session on the same printer. "60" is not 60 independent
conditions; it is 60 plucks of two belts.

### What a hardware session must produce

The deliverable is **the capture corpus, not a pass/fail**. Set `HELIX_BELT_CAPTURE_DIR`
for every run (see `ENVIRONMENT_VARIABLES.md`); every resolved event, accepted or
rejected, is written in the fixture format and drops straight into
`tests/fixtures/belt_plucks/` with no conversion.

Minimum matrix, agreed with the maintainer:

1. **Same belt, same pluck, with a known vibration source on and off** - e.g. a BoxTurtle
   fan running vs unplugged. This is the tone-rejection gate's only real test.
2. **Deliberate non-plucks** - bump the frame, close the door, let the toolhead settle
   after a park. Each must be *rejected*. The shape gate has never seen a real thump.
3. **A deliberately loose belt** - the accepted range has only ever seen belts at 86 Hz
   and 82 Hz. Nothing pins behaviour far from the reference machine's tension.
4. A second machine, whenever one is available. Every claim in this file is
   single-machine until then.

### Known traps when you first run it

- **A soft first pluck producing no response is a threshold, not a hang.**
  `MIN_DETECTABLE_RATIO` moved 3.0 -> 5.0, so strikes between 3x and 5x the noise floor
  are now dropped silently rather than surfaced as "pluck harder". That was deliberate -
  the evidence says nothing below 5x was a pluck at all - but it reads as a dead UI.
- **Verify the deployed artifact two independent ways.** They fail independently:
  read make's own exit code from a file (never a trailing `echo`, never a harness
  notification), **and**
  `strings build/bin/helix-screen | grep -c HELIX_BELT_CAPTURE_DIR`. That string exists
  only on this feature, so zero hits means a stale binary whatever the exit code claimed.
- **Replaying the same capture path twice is a no-op** - LVGL does not notify a string
  subject's observers when the written value equals the current one. Set it to `""`
  between replays.
- The absolute frequency target is **span-dependent** (`110 * 150 / L` on a Voron). A/B
  *matching* is span-independent and is the robust reading; treat the absolute number as
  the fragile one.

### Open questions this feature cannot answer about itself

- Does the shape gate reject a real frame bump, or only synthetic ones?
- Does tone rejection survive a fan whose blade-pass frequency lands near f0?
- Is the 5-pluck median enough when failures are **correlated** (the same belt reading
  the same wrong octave five times)? The independent-Bernoulli estimate that once
  justified this was removed as unsound; correlated failure is the mode this feature
  actually hits.
- Does any of it hold on a printer that is not a 300mm Voron 2.4?

---

## Architecture

```
BeltTensionPanel (UI, state machine, XML-bound subjects)
  |  Gates -> park gantry -> LISTEN per belt -> COMPARE
  |
  +-> evaluate_belt_gate()          belt_gating.h
  |     One ordered predicate chain -> one user-facing reason string.
  |     Also owns park_y_for_span() and park_x_center().
  |
  +-> BeltStreamClient              belt_stream_client.h
  |     Klipper's UDS webhooks endpoint (dump_adxl345), \x03-framed JSON,
  |     on its own libhv event loop. Klipper serves a second client
  |     alongside Moonraker, so this does not disturb the printer.
  |
  +-> BeltListenSession             belt_listen_session.h
  |     Owns the live detection window. Applies MIN_DETECTABLE_RATIO,
  |     hands accepted strikes to the detector, drives the capture writer.
  |
  +-> PluckDetector                 pluck_detector.h
  |     Onset detection, strength gate, temporal-shape gate. Extracts the
  |     ring-down segment (SKIP_MS past the transient, ANALYZE_MS long).
  |
  +-> pitch_estimator               pitch_estimator.h
  |     Harmonic product spectrum over the ring-down. Search window derived
  |     from span length, NOT a fixed band.
  |
  +-> PluckAggregator               pluck_aggregator.h
        Running median across plucks. Aggregation is mandatory, not a
        refinement - a single pluck is ~95% right, a median is the number
        the user acts on.
```

Supporting units: `BeltCaptureWriter` (`belt_capture.h`, gated on
`HELIX_BELT_CAPTURE_DIR`), `belt_dsp_probe` (the hardware-speed gate behind
`HARDWARE_TOO_SLOW`), `KlippyFrameDecoder` (`klippy_frame_decoder.h`),
`screen_locality` (co-location telemetry), `ui_belt_trace` / `ui_pluck_animation`
(procedural drawing widgets).

## The detection window is not the ring-down

The single easiest mistake in this code. `rms_ratio()`, `passes_gate()` and
`has_sharp_onset()` operate on the **live detection window**, which includes the onset.
`extract_ringdown()` returns a *different* buffer that has already decayed for `SKIP_MS`
or more, and reads several times lower. `PluckWindow` deliberately has no strength field
so the two cannot be silently swapped - `extract_ringdown()` is static and has no access
to the window it came from.

## Why peak-picking was wrong

`find_peak_frequency()` took the largest PSD bin in 20-200 Hz. On one real belt, six
plucks reported 70, 70, 164, 82, 164, 164 Hz - exactly one octave sharp whenever the 2nd
harmonic dominated, which on that belt it always did. Harmonic product spectrum plus a
median across plucks fixed it. HPS is search-range sensitive: drop the floor below f0/2
and it locks onto the subharmonic, which is why the window comes from span length.

## Things established as *not* problems

Measured on the reference machine, so these do not need re-testing first:

- **Pluck position does not matter.** Midpoint vs 1/3-along: 86.0 Hz every time.
- **Stepper energization does not matter.** Motors on vs off: identical. No homed machine
  required.
- **Z belts are not measurable** from a toolhead accelerometer - four plucks of one Z belt
  gave 228, 164, 92, 58 Hz, against 5/5 identical on an A/B belt. The dead Z path was
  deleted rather than wired to a UI; do not restore it.

## Where it is tracked

- GitHub #1303 (accelerometer-based CoreXY belt tension) and #1231 (Beta: Belt Tension -
  finish or drop).
- `CHANGELOG_1_1_DRAFT.md` carries the release-note entry, flagged unvalidated.
- `../user/guide/beta-features.md` carries the user-facing status.

**Do not promote this out of beta, and do not mark it verified in the 1.1 changelog,
until the matrix above has actually been run.**
