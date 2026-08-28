# Belt pluck fixtures

Raw accelerometer captures the belt-tuner unit tests run against.

## ⚠ Provenance - read before trusting a threshold these pin

**All eight files come from one printer, on one evening.** A 300mm Voron 2.4 (CB1,
192.168.1.112), 2026-08-09, two belts, ADXL345 on the toolhead. That is the entire
independent-condition count: **one machine, one session, two belts.**

Every gate constant in `include/pluck_detector.h`, `include/pluck_aggregator.h` and
`include/pitch_estimator.h` was measured against this material, and the algorithm was
then tuned against it. **That is circular**, and it has not been broken yet. A green test
run here means "still consistent with 2026-08-09", not "correct".

The `60 real captures` cited in `pluck_detector.h` is the *same session's* full pluck
count, which lives in that session's scratchpad and was never committed. It is not 60
independent conditions.

| File | What it is |
|---|---|
| `a_belt_86hz_{1,2,3}.csv` | Path A at 86 Hz. Reference machine's actual tension - loose against the 110 Hz target |
| `b_belt_82hz_{1,2,3}.csv` | Path B at 82 Hz |
| `b_belt_82hz_hard_case.csv` | Path B pluck whose 2nd harmonic dominates - the octave-error case peak-picking got wrong |
| `weak_pluck_reject.csv` | Below the strength gate; must be rejected, not measured |

## Adding to this corpus

Do not hand-author CSVs. Run the app with `HELIX_BELT_CAPTURE_DIR` set (see
`docs/devel/ENVIRONMENT_VARIABLES.md`) - every resolved event, accepted or rejected, is
written in exactly this format and drops in with no conversion step.

What this corpus is missing, in priority order: a real frame bump or door close (a
non-pluck the shape gate has never seen), a belt under a running fan (tone rejection has
no real negative), a belt tensioned far from 82-86 Hz, and any second machine.

See `docs/devel/BELT_TUNER.md` § Validation status.
