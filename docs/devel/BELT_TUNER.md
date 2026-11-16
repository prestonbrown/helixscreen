# Belt Tuner (pluck-based)

Developer guide for the pluck-based belt-tension libraries: a belt plucked by hand is
heard on Klipper's live accelerometer stream, and the belt's fundamental frequency is read
from the ring-down.

**Panel**: none. The pluck libraries back no UI, and no user can reach them. The Belt
Tension screen users see is the driven-sweep comparison in `BELT_TENSION.md`, which shares
two pieces with this code: `BeltStreamClient` and `evaluate_belt_gate`.
**User guide**: none.

---

## ⚠ Validation status: NOT verified on hardware

**Read this before changing a threshold, before wiring any UI to these libraries, and
before believing a number they print.**

It is green in CI - 96/96 shards. Its one real-hardware test, on a Voron 2.4 350 (#1721),
failed: a clean audible pluck almost never cleared
`kMinRmsRatio`, a pluck hard enough to clear it rang as broadband noise, and one belt
read ~130, ~85 and ~120 Hz across repeats. Everything below is open.

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
  `kMinDetectableRatio` is 5.0, so strikes below 5x the noise floor are dropped
  silently rather than surfaced as "pluck harder". That is deliberate - the evidence
  says nothing below 5x was a pluck at all - but behind a UI it reads as a hang.
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
  the same wrong octave five times)? An independent-Bernoulli estimate cannot
  justify it, because correlated failure is the mode this feature actually hits.
- Does any of it hold on a printer that is not a 300mm Voron 2.4?

---

## Architecture

No panel drives this chain; the unit tests do. Two pieces have users outside it:

- `evaluate_belt_gate()` (`belt_gating.h`) is the Belt Tension panel's Start gate: one
  ordered predicate chain to one user-facing reason string.
- `BeltStreamClient` (`belt_stream_client.h`) reads Klipper's UDS webhooks endpoint
  (`dump_adxl345`), `\x03`-framed JSON, on its own libhv event loop. Klipper serves a second
  client alongside Moonraker, so this does not disturb the printer. It feeds the live sweep
  preview (`BELT_TENSION.md` § Live preview) through `IAccelBatchSource`.

```
BeltStreamClient (accelerometer batches)
  |
  +-> BeltListenSession             belt_listen_session.h
  |     Owns the live detection window. Applies kMinDetectableRatio,
  |     hands accepted strikes to the detector, drives the capture writer.
  |
  +-> PluckDetector                 pluck_detector.h
  |     Onset detection, strength gate, temporal-shape gate. Extracts the
  |     ring-down segment (kSkipMs past the transient, kAnalyzeMs long).
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
`HELIX_BELT_CAPTURE_DIR`), `belt_dsp_probe` (`belt_dsp_probe.h`, the time one windowed PSD
takes, for a hardware-speed gate), `belt_live_data.h` (the match and trace values a display
would draw), `KlippyFrameDecoder` (`klippy_frame_decoder.h`), and `screen_locality`
(co-location telemetry).

## The detection window is not the ring-down

The single easiest mistake in this code. `rms_ratio()`, `passes_gate()` and
`has_sharp_onset()` operate on the **live detection window**, which includes the onset.
`extract_ringdown()` returns a *different* buffer that has already decayed for `kSkipMs`
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
  gave 228, 164, 92, 58 Hz, against 5/5 identical on an A/B belt. There is no Z path,
  and none should be added.

## Where it is tracked

- GitHub #1303 (accelerometer-based CoreXY belt tension) and #1231 (Beta: Belt Tension -
  finish or drop).
- `CHANGELOG_1_1_DRAFT.md` § Internal lists the libraries as unvalidated, with no user entry.

**Do not wire a UI to these libraries, and do not call them verified in a changelog,
until the matrix above has actually been run.**
