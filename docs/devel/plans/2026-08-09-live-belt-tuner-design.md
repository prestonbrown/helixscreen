# Live Belt Tuner - Design

Status: approved, ready for implementation planning
Date: 2026-08-09
Hardware used for all measurements: Voron 2.4 (300mm), BTT CB1 (Allwinner H616, 3 cores,
986 MB), ADXL345 on the toolhead, Klipper + Moonraker, HelixScreen 0.99.107

## Problem

HelixScreen's Belt Tension panel is double-gated behind `<beta_feature>` and an
accelerometer check. Its measure-and-recommend path works, but its "Fine-Tune" strobe
sub-mode is a stub, its frequency-response chart is declared and destroyed but never
created, and its analysis reports an octave-wrong number under conditions that occur
routinely. Meanwhile belt tension is the mechanical prerequisite for input shaping, which
already shipped - the two features are in the wrong order relative to how a user should
use them.

The opportunity: Klipper can stream accelerometer data live, and the display hardware can
keep up. That turns belt tuning from a capture-analyze-report round trip into a live
instrument - the printer's own screen replacing the phone frequency-analyzer app the
Voron docs currently tell people to use.

## Evidence base

Every threshold in this document was measured, not assumed. Raw captures (`.npz`), probe
scripts, and analysis live in the session scratchpad.

**Transport.** Klipper's `adxl345/dump_adxl345` webhooks endpoint streams continuously to
any client that subscribes with a `response_template`. Measured 3053 Hz in ~100 ms
batches, 182 ms to first batch, attached as a *second* UDS client while Moonraker held
its own connection. `klippy`'s `ServerSocket` keeps a client dict and accepts many
connections.

**Compute.** `compute_psd` transcribed verbatim and benchmarked natively on the CB1 at
`-O2`:

| samples | seconds | shipping | phasor rewrite |
|---|---|---|---|
| 1024 | 0.32 | 11.9 ms | 2.4 ms |
| 2048 | 0.64 | 48.3 ms | 8.4 ms |
| 16000 | 5.00 | 3.0 s | 454 ms |
| 32000 | 10.0 | 12.2 s | 2.0 s |
| 64000 | 20.0 | **49.2 s** | 8.1 s |

The algorithm is O(n²) with `sin`/`cos` in the inner loop, run once per axis. A 2048
sliding window at 10 Hz costs ~48% of one core as-is, ~8% rewritten. Long captures are
the problem, not live.

**Pitch estimation.** Pooled 60 captures across three A/B runs:

| pluck strength | n | HPS correct | naive peak-pick correct |
|---|---|---|---|
| < 5x noise floor | 25 | 0% | 8% |
| 5-9x | 14 | 64% | 57% |
| > 9x | 21 | **95%** | **19%** |

Naive peak-pick gets *worse* as plucks get firmer, because a firmer strike puts more
energy into the 2nd harmonic. On the A belt the 2nd harmonic dominated on every single
pluck (172 Hz at +0 dB, 86 Hz fundamental at -2 dB), so peak-pick read exactly one octave
sharp, consistently and confidently.

Median accuracy, gated to firm plucks: 1 pluck 82%, 3 plucks 91%, **5 plucks 97%**,
7 plucks 98%. Ungated it never exceeds 48% at any count - noise triggers poison the
median. The gate matters more than the averaging.

**What does not matter.** Strike position (midpoint vs 1/3 along: 86.0 Hz on all six
plucks) and stepper energization (motors on A=86/B=82, motors off A=86/B=82). Neither
needs to be a precondition or a warning.

**What is not measurable.** Z belts, from a toolhead-mounted accelerometer. Four plucks
of one Z belt gave 228, 164, 92, 58 Hz against 5/5 identical on an A/B belt; signal was
2-5x the noise floor versus 12-14x. A knuckle-tap on the toolhead peaks at 38 Hz with no
86 Hz component, so this is not simple A/B leakage - there is no coherent Z tone reaching
the sensor. `TEST_RESONANCES` also cannot sweep Z: `resonance_tester.py:_parse_axis`
accepts only `x`, `y`, or a two-component XY vector.

**Span dependence.** Voron's target is 110 Hz at a 150mm span, idler centre to idler
centre; for any span the target is `110 * 150 / L`. During testing the maintainer, with
the documentation open, was unsure he had plucked the documented segment. Absolute
targets are therefore fragile; A-vs-B matching is span-independent and is the robust
measurement.

**Span is a function of gantry position, and we can compute it.** On the reference 300mm
2.4, span in mm ≈ `Y + 35`, measured against a tape at two positions (Y100 → 135mm,
Y115 → 151mm). So the panel can park the gantry at the Y that produces a nominal 150mm
span rather than asking the user to measure anything. The `+35` offset is geometry, so it
is a **per-printer-model constant**, and it has been validated on exactly one machine.

## Scope

**In for v1:** live pluck tuner for CoreXY A/B belts; co-located installs; idle printer;
hardware that passes a capability probe.

**Phase 2:** sweep-verify. The live tuner ships alone first; driving the belt paths with
`TEST_RESONANCES AXIS=1,1` / `1,-1` and comparing response curves is a separate piece of
work.

**Out for v1:** non-CoreXY kinematics. CoreXZ (Voron Switchwire) is the most likely next
candidate and would need its own isolation analysis - the `1,1`/`1,-1` trick is specific
to CoreXY's `ΔA = ΔX + ΔY` / `ΔB = ΔX - ΔY` relationship. Bed slingers have no equivalent
at all. Also out: remote installs (see Risks).

**Deleted:**

- Strobe in its entirety - `BeltTensionPanel::STROBE` state, `handle_lock_a_clicked` /
  `handle_lock_b_clicked`, `BeltTensionCalibrator::start_strobe` /
  `set_strobe_frequency`, `MoonrakerAdvancedAPI::set_strobe_frequency`, the `state_strobe`
  block in `panel_belt_tension.xml`, and `has_pwm_led` / `pwm_led_pin` if unused
  elsewhere. The live meter supersedes it, and the reference Voron has only neopixels,
  which cannot be driven at belt frequencies.
- `BeltTensionCalibrator::start_z_belt_listening()` and the `ZBeltCorner` /
  `ZBeltMeasurement` types and their tests. Unmeasurable; shipping it would present four
  corners of authoritative-looking noise.
- The `<beta_feature>` wrapper in `advanced_panel.xml`, once the rest lands.

**Fixed:** `compute_psd` gains a bandwidth parameter and a phasor inner loop. Both are
required in v1.

The bandwidth parameter is a correctness fix, not a tuning knob. `compute_psd` hard-caps
its output at 250 Hz, and a harmonic product spectrum over four harmonics needs bins out
to `4 * f0` - about 344 Hz for an 86 Hz belt. At the 250 Hz cap **no candidate has a
complete harmonic series and the estimator returns nothing at all**, verified against
every captured fixture. The pluck path must request roughly `n_harmonics * search_hi_hz`,
about 700 Hz for a 150mm span.

That widens the spectrum ~2.8x, which is what makes the phasor rewrite mandatory rather
than merely desirable: a 2048-point window at 700 Hz bandwidth costs ~135 ms on the
reference CB1 with the current trig-per-sample loop, versus ~23 ms rewritten. The same
change also makes phase 2's sweep viable, where 49 seconds of math for a 20-second
capture is otherwise unshippable.

The existing `TEST_RESONANCES` path stays in the tree untouched for v1. It is not wired
into the new flow.

## Architecture

Three new units, each testable in isolation:

| unit | responsibility | dependencies |
|---|---|---|
| `BeltStreamClient` | owns the klippy UDS socket, subscribes to `adxl345/dump_adxl345`, decodes `\x03`-framed batches into a ring buffer | libhv event loop; no LVGL |
| `PluckDetector` | tracks the noise floor, detects onsets, applies the strength gate, extracts ring-down windows | none - pure |
| `PitchEstimator` | PSD + harmonic product spectrum over a span-derived search window; returns f0 and a confidence | none - pure |

`PluckDetector` and `PitchEstimator` are pure functions over sample buffers. Every
threshold established above lives in exactly one of them, so the empirical findings are
unit-testable without a printer attached.

**Threading.** The socket reader must not be a bare `std::thread` - that is the
`EAGAIN` / `std::terminate` failure on AD5M and CC1. Preferred approach is attaching the
UDS descriptor to the libhv event loop `MoonrakerClient` already runs, so no new thread
exists at all. Decoding and PSD run off the main thread; results reach the UI through
`ui_queue_update()`. All existing rules apply: no LVGL from a background thread, no
synchronous deletes inside queued callbacks, `lifetime_.bg_cb` rather than bare
`tok.expired()` checks.

**Reused:** `ui_frequency_response_chart` for the spectrum strip. The widget already
exists, is platform-adaptive, and is what the belt panel was always meant to use.

## Measurement pipeline

1. Reject any onset below **9x** the noise floor; prompt "pluck harder"
2. Extract 500 ms of ring-down beginning 40 ms after onset (skipping the impact spike)
3. Harmonic product spectrum over a PSD computed with at least `n_harmonics * search_hi`
   of bandwidth, search window derived from span length at [0.70, 1.50] x the expected
   fundamental. Both bounds matter: too low a floor and HPS locks onto f0/2, too little
   bandwidth and it returns nothing. The measured window gets **7/7 real fixtures
   correct**, including one that a fixed 45 Hz floor gets wrong.
4. Running median of accepted plucks; **commit a number at 5**
5. Keep listening past 5. The count keeps climbing and the median keeps updating so the
   user can confirm stability for themselves

Naive peak-pick is never used on plucks. It is retained for the *sweep*, where comparing
two response curves is the correct operation and matches Klipper's documented method.

## Gating

- **Accelerometer present.** Today only the *entry row* is gated
  (`advanced_panel.xml:205` hides it when `printer_has_accelerometer == 0`); inside the
  panel nothing checks, so it renders "Not detected" and leaves Start clickable. Hiding a
  menu row is not a gate — the panel is still reachable by `ctl navigate`, by a deep link,
  and by a printer whose accelerometer drops out after entry. The START action itself must
  be disabled when no accelerometer is present, bound to the same subject, so the guarantee
  does not depend on the route the user took to get there.
- **Co-located:** the klippy UDS socket is reachable. Probed on panel entry.
- **Idle:** the printer is not printing.
- **Capable:** a one-time cached micro-benchmark of a real 2048-point PSD, gated on
  measured milliseconds. Explicitly *not* the `PlatformTier` enum: the CB1 has 986 MB and
  3 cores so it classifies as `BASIC`, and it is the reference machine for this feature.
  Tier says nothing about the thing that matters here, which is DSP throughput.

## UI

Panel states replace the current five:

| state | purpose |
|---|---|
| `START` | capability + hardware detection, explain the procedure |
| `POSITION` | park the gantry so the span is the nominal 150mm, show which segment to pluck |
| `LISTEN` | the live meter, one belt at a time |
| `COMPARE` | A vs B, delta, verdict |
| `VERIFY` | *(phase 2)* optional sweep, presented as curve similarity |
| `ERROR` | |

`LISTEN` uses the hero-readout layout: large live frequency, a match bar against the held
reference, and a compact live spectrum strip beneath. Pluck count and running median are
visible so convergence is legible.

`POSITION` never asks the user to measure anything. The gantry parks itself at the Y that
yields a nominal 150mm span, and the animation shows which segment that is. Reaching for
calipers is work we can do arithmetic instead.

`VERIFY` (phase 2) presents overlaid curves and a similarity percentage, never a
frequency next to the tuner's frequency. The two measurements are not comparable - a plucked span and a
driven gantry response are different physical quantities - and putting two numbers side
by side would invite users to reconcile them.

**Pluck instruction animation.** A looping illustration shown in `POSITION` and `LISTEN`.
Functional, not decorative: two of the three things a user can get wrong are plucking the
wrong run and plucking too softly, and both are far easier to show than to describe.

Direction was settled against mockups and reference photos of the reference machine. The
drawing itself is deferred to implementation, when there is a real panel and real pixel
dimensions; the decisions below are what those mockups bought.

*Style:* filled isometric, 30°, viewed from the front and above — the angle a user
actually stands at. Flat fills with clean outlines, the visual language commercial
printers use for filament-load prompts. This was chosen over flat vector line art. A hand
belongs in frame; a rotation arrow alone was considered and rejected.

*Geometry, confirmed against photographs of the machine:*

- the plucked span is **horizontal**, running front-to-back
- it lies alongside the **side** face of the extrusion, never on top of it
- the belt band **stands on edge** — its width is vertical, its toothed face points
  outward at the user
- there are **two parallel runs**, joined by a 180° wrap around an idler with a **vertical
  axis** at the front corner; the pulley's diameter is the spacing between them
- the user plucks the **outer** run, the one nearest them; the inner run is boxed in
  against the extrusion
- teeth face **outward** on that outer run
- the idler carries a black printed block and a **red** adjuster
- at the front of the machine there is one belt per side: **front-left is B, front-right
  is A**. The panel can therefore label the measurement itself rather than asking.

*The gesture:* the finger comes down from above, hooks over the **top edge**, and **pulls
the belt toward the user** — away from the extrusion. Pulling from the top edge makes the
top edge lead and the bottom lag, so the band twists as it is drawn out. The deflection
and the twist are one motion. It is emphatically not a poke, a press, or an inward push.

*Constraint discovered while mocking up:* a pinch between thumb and finger does not read
at 300 px in this style — three attempts failed. A single tapered fingertip hooking the
edge does read. Plan for the fingertip.

*Implementation note:* vector primitives with `lv_anim` keep asset weight off the 512 MB
targets and let the ring-down animate at the frequency actually being measured. Sprite
frames would look better but carry per-resolution assets and a fixed palette that will not
follow a theme change. Either way the twist must be **exaggerated** past physical accuracy
— at true amplitude it is invisible at this size.

*Projection caveat worth knowing before redrawing:* rolling the belt's top edge toward the
viewer turns its face away in isometric, so the rotation cannot be shown directly. What
reads instead is what the roll exposes — the lit thickness edge widening, a highlight
riding the top edge so its path visibly bows, and the teeth compressing as the face turns.

## Telemetry

Add `screen_locality: "local" | "remote"`, derived from the same UDS reachability probe
that gates the feature. Sits alongside the existing `cpu_cores` / `cpu_model` / `arch` /
`app_platform` fields. One probe serves both the gate and the metric, and it answers a
question we currently cannot: what fraction of installs run the display on the printer.

## Error handling

| condition | behavior |
|---|---|
| UDS socket absent or refused | feature hidden; telemetry records `remote` |
| printer starts printing mid-session | stop streaming, leave the panel with a clear message |
| accelerometer absent | existing `printer_has_accelerometer` gate applies |
| capability probe fails | feature hidden with an explanatory message, not silently |
| pluck below the gate | in-place "pluck harder" prompt; not an error state |
| no plucks for N seconds | idle hint, keep streaming |
| stream stalls | detect missing batches, surface a reconnect rather than freezing a stale number |

## Testing

**Golden data.** Today's real Voron captures become CSV fixtures. `PitchEstimator` must
reproduce A=86 Hz and B=82 Hz from actual plucks. This is a regression test built from
hardware measurements rather than from the implementation's own output.

**Synthetic cases.** Most importantly a signal whose 2nd harmonic dominates the
fundamental, which must return f0 and not 2·f0. That test fails against the code
currently shipping.

Also: HPS floor set too low must be caught (subharmonic lock); the strength gate must
reject a 3x transient and accept a 10x one; the median must not commit before 5 accepted
plucks; span-derived search windows must track the span input.

**Not unit-testable, needs hardware:** end-to-end streaming, and the capability probe's
threshold on real slow silicon.

## Risks and open questions

- **Remote installs get nothing.** Moonraker auto-registers every Klipper endpoint, so
  `printer.adxl345.dump_adxl345` is addressable, but Moonraker only handles
  `response_template` for its own status subscription. A proxied stream likely dead-ends.
  Untested. If a relay turns out to work, remote support is additive and costs no
  redesign.
- **Sample size on the Z conclusion** is thin: 2 plucks at long span, 4 at short. The
  qualitative gap (5/5 identical versus 4/4 different) is stark enough to scope on, but a
  gantry-mounted second accelerometer would be a different experiment with a possibly
  different answer.
- **Single machine.** Every number here comes from one Voron 2.4 with one belt type at
  one tension. Thresholds may need widening once a second CoreXY is measured.
- **The span offset is a per-model constant measured once.** `span ≈ Y + 35` holds on a
  300mm 2.4 and nowhere else yet. A wrong offset does not break A-vs-B matching, which is
  span-independent, but it does skew the absolute target: a 10mm error moves the 110 Hz
  target by about 7 Hz. Every supported model needs its own measured value, and models
  without one should show matching only and suppress the absolute target rather than
  guessing.
- **libhv loop integration** for the UDS socket is the preferred approach but not yet
  verified as clean.
