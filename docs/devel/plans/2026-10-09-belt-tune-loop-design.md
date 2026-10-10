# Belt Tension: guided tuning loop (design)

Tracks prestonbrown/helixscreen#1736. Builds on the shipped resonance comparison (#1721,
`docs/devel/BELT_TENSION.md`). Mockups: https://claude.ai/artifact/AYJMD8kG24LgxnnHts1tNK

## Goal

The comparison says how well the two CoreXY belt paths match. The loop turns that into
tuning: it says which belt to turn, which way and (where the hardware allows) how far, then
re-measures only the disagreeing frequency zone on both paths, and repeats until the peaks
line up. Each round is a short, motors-only sweep; hands are on the tensioner only between
sweeps.

Success: on a screw-tensioned CoreXY (Voron 2.4 first), a user starting from a Poor or Fair
match reaches a matched zone in a handful of rounds, and the confirming full sweep shows a
higher similarity than the starting one.

## Rulings (Preston, 2026-10-09)

What goes:

- **Advice is suggested, then verified.** A higher peak means a tighter belt. The loop
  suggests a direction and amount, measures what the turn actually did, learns Hz-per-turn
  for this machine, and corrects the next suggestion.
- **The looser path is adjusted by default**, tightened toward the tighter one; "Adjust A/B
  instead" swaps which path is adjusted and which is the reference.
- **Each round sweeps BOTH paths over the zone**, so drift between runs cancels: the gap is
  always computed from two curves of the same round.
- **Zones come from peak pairs, falling back to the tallest peaks.** When the main peaks are
  unpaired (further apart than `PAIR_MAX_HZ`, or no pair at all), the loop steers by each
  path's tallest in-band peak AND shows a shape warning that opens a modal listing the
  mechanical causes (gantry racking, loose pulley grub screw, belt rubbing a flange/idler/frame,
  unseated or twisted belt). Tuning continues.
- **Done** = the zone's gap is within `max(1 Hz, one bin)`; then the next bad zone, if any;
  then a confirming full two-path sweep landing on RESULTS with "was N%".
- **"Good enough" exits straight to RESULTS**, no confirm sweep.
- **Entry is a fourth RESULTS button**: "Tune belts" on Fair/Poor, "Fine-tune" on Good. The
  comparison on screen is round 0; no extra sweep.
- **Clockwise tightens.** The screw graphic, turn amount, ⅛-turn stepper and learned rate
  appear ONLY when the printer database says the printer has screw tensioners.

What stays as it is:

- The comparison itself: similarity, verdict thresholds, pairing, START/RUNNING/RESULTS/ERROR
  views, the gate, the RAM check, stall detection and Stop (`emergency_abort`).
- No absolute frequency target. The loop matches the paths; it does not tension to a number.
- Path names stay A (`AXIS=1,-1`) and B (`AXIS=1,1`).

## Tensioner kinds

New printer-database field `belt_tensioner`: `"screw"` or `"spring"`; absent means unknown.
Set it only where the hardware is confirmed. Voron 2.4 and Trident are screw. The K1 family
is believed spring; confirm before setting it.

| Kind | Instruction | Amount + stepper | Learned Hz/turn |
|------|-------------|------------------|-----------------|
| SCREW | screw graphic with clockwise / counter-clockwise arrow, "Tighten B" / "Loosen B" | yes, ⅛-turn steps | yes |
| UNKNOWN | words only: "Tighten B" / "Loosen B" | no | no |
| SPRING | "Release B's tensioner, let the spring settle, lock it" | no | no |

On a spring printer the spring sets the tension, so a mismatch that survives a re-seat
points at the mechanical causes; after two spring rounds without improvement, the
mechanical-causes line shows.

## Round mechanics

**Zone pick** (`pick_tune_zone`). Ignore peaks below `ZONE_MIN_AMPLITUDE_FRACTION` (0.20)
of the stronger path's in-band maximum. Among the remaining pairs not already matched, take
the one with the largest frequency gap greater than the matched tolerance. If each path's
tallest peak is unpaired, or there are no pairs, use tallest-A against tallest-B with
`source = TALLEST_UNPAIRED` (the shape warning). Window = midpoint ± max(`ZONE_HALF_MIN_HZ`
10, gap/2 + `ZONE_MARGIN_HZ` 5), clamped to the `[resonance_tester]` range. If no candidate
zone exists, the loop goes straight to the confirm offer.

**Narrow sweep.** `TEST_RESONANCES AXIS=<a|b> FREQ_START=<lo> FREQ_END=<hi>
OUTPUT=resonances NAME=helix_belt_<a|b> SWEEPING_PERIOD=0`, A then B. Progress percent and
the sweep cursor scale against the window, not the configured range. Stall detection, Stop
and the CSV read path are unchanged. A 32 Hz window at the default 1 Hz/s is about 30 s per
path. Confirm on hardware that both mainline Klipper and Kalico accept `FREQ_START` /
`FREQ_END` on `TEST_RESONANCES` (first hardware task).

**Peak in window** (`refine_peak_in_window`). The local maximum inside the window, refined by
parabolic interpolation over its neighbours; bins are about 1.5 Hz on Kalico, and the
matched tolerance is tighter than that. When the window holds two maxima, take the one
nearest the path's peak from the previous round.

**Gap** = adjusted peak − reference peak, from the same round. Negative: adjusted is looser,
tighten it. Positive: loosen it.

**Amount** (SCREW only). Round 1: ¼ turn. Afterwards `|gap| / hz_per_turn`, rounded to ⅛,
clamped to [⅛, 1]. `hz_per_turn` is the median of `|Δ adjusted peak| / |turns|` over
rounds whose move was at least `MIN_MOVE_HZ` (0.5). The stepper records what the user
actually turned; it defaults to the suggestion.

**Verdict on each round** (`BeltTuneRound::outcome`):

| Outcome | Condition | Message |
|---------|-----------|---------|
| CLOSER | gap shrank, same sign | "last turn: 5.5 Hz closer" |
| OVERSHOT | gap changed sign | advice flips to the other direction, smaller amount |
| WRONG_WAY | adjusted peak moved opposite to the advised direction by at least `MIN_MOVE_HZ` | "B's peak went down after tightening. Check you turned Path B's tensioner." |
| NO_MOVEMENT | moved less than `MIN_MOVE_HZ` | "Moved 0.2 Hz. Try a bigger turn." |
| MATCHED | \|gap\| within tolerance | gauge green, Next zone / Confirm |

After `STALL_ROUNDS` (8) in one zone without MATCHED, the mechanical-causes line shows.

## UI

New view state `4 = TUNING` in `panel_belt_tension.xml`, with subject `bt_tune_phase`
(0 ADJUST, 1 SWEEPING, 2 MATCHED). Measured surfaces: at 800×480 the panel content is
676×404 (hero 86 px, chart 676×195, buttons 52 px); at 480×272 it is 414 wide (hero 23 px,
chart 414×127, buttons 26 px).

- **Hero, 800×480:** left is the instruction (screw graphic + amount stepper for SCREW,
  words otherwise). Centre is the gap gauge: a big "2.4 Hz" coloured by distance (green
  within tolerance, warning within 3 Hz, danger beyond), "B looser than A" under it, then a
  track with a green matched band at zero, the current needle and a hollow needle for last
  round. The track carries NO end labels: they read as peak labels on the chart below.
  Right is round number, zone range, learned Hz/turn and the last move.
- **Chart:** the existing frequency-response chart with x range set to the zone; reference and
  adjusted curves solid, the adjusted path's two previous rounds as muted series, a dashed
  line at the reference peak, the sweep cursor while sweeping.
- **Shape warning:** one tappable line above the buttons, "Curves differ in shape. Steering by
  each path's tallest peak. Why? ›", opening a modal with the causes list.
- **Buttons:** ADJUST: Good enough · Adjust A/B instead · Re-measure (~1 min). SWEEPING: Stop.
  MATCHED: Keep tuning · Next zone (hidden when none) · Confirm (full, ~N min).
- **480×272:** hero becomes one line ("↻ Tighten B ¼ · 2.4 Hz apart · R3"), the gauge an
  8 px bar under it, the stepper moves into the button row, and tapping the hero line swaps
  the adjusted path. RESULTS folds Re-test A/B into one "Re-test ▾" menu so four buttons fit.
- **RESULTS after Confirm:** both path notes carry "· was N%" (the round-0 similarity) and the
  facts line adds "Tuned in N rounds" plus, for SCREW, the total turn.

## Interfaces

```cpp
// include/belt_tension_types.h
struct BeltSweepWindow {
    float freq_start_hz = 0.0f;
    float freq_end_hz = 0.0f;
};
enum class BeltTensionerKind { UNKNOWN, SCREW, SPRING };

// include/i_moonraker_sub_apis.h: test_belt_resonance gains a trailing optional window.
// No window = today's full sweep; a window adds FREQ_START/FREQ_END and the collector
// scales progress against it.
[[nodiscard]] virtual BeltRunCancel test_belt_resonance(const std::string& axis_param,
        const std::string& output_name, BeltSweepProgressCallback on_progress,
        BeltCurveCallback on_complete, ErrorCallback on_error,
        std::optional<BeltSweepWindow> window = std::nullopt) = 0;

// include/belt_tension_calibrator.h: same optional window on measure_path.
void measure_path(BeltPath path, std::function<void(int percent, float freq_hz)> on_progress,
                  std::function<void(BeltCurve)> on_complete, BeltErrorCallback on_error,
                  std::optional<BeltSweepWindow> window = std::nullopt);

// include/printer_detector.h
static BeltTensionerKind get_belt_tensioner(const std::string& printer_name);

// include/belt_tune_session.h (new, pure: no LVGL, no I/O)
namespace helix::calibration {
enum class BeltZoneSource { PAIRED, TALLEST_UNPAIRED };
struct BeltTuneZone {
    BeltSweepWindow window;
    BeltZoneSource source = BeltZoneSource::PAIRED;
    float ref_hz = 0.0f;  // reference path's peak that defined the zone
    float adj_hz = 0.0f;  // adjusted path's peak that defined the zone
};
[[nodiscard]] std::optional<BeltTuneZone> pick_tune_zone(const BeltComparison& cmp,
        BeltPath adjusted, float band_min_hz, float band_max_hz,
        const std::vector<float>& matched_zone_centres_hz);
[[nodiscard]] std::optional<float> refine_peak_in_window(const BeltCurve& curve,
        BeltSweepWindow window, std::optional<float> near_hz);
[[nodiscard]] float matched_tolerance_hz(const BeltCurve& curve);  // max(1, bin width)

enum class BeltTuneOutcome { CLOSER, OVERSHOT, WRONG_WAY, NO_MOVEMENT, MATCHED };
enum class BeltTurnDirection { TIGHTEN, LOOSEN };
struct BeltTuneAdvice {
    BeltPath path;
    BeltTurnDirection direction;
    std::optional<float> turns;  // SCREW only, multiple of 0.125
};
struct BeltTuneRound {
    float ref_hz, adj_hz, gap_hz;
    std::optional<float> turns_made;  // what the stepper recorded before this round
    BeltTuneOutcome outcome;
};

class BeltTuneSession {
  public:
    BeltTuneSession(const BeltCurve& a, const BeltCurve& b, const BeltComparison& round0,
                    float band_min_hz, float band_max_hz, BeltTensionerKind kind);
    [[nodiscard]] const std::optional<BeltTuneZone>& zone() const;
    [[nodiscard]] BeltPath adjusted_path() const;
    void swap_adjusted_path();                  // re-derives zone and advice
    [[nodiscard]] float gap_hz() const;          // signed, adjusted - reference
    [[nodiscard]] BeltTuneAdvice advice() const;
    void record_turns(float turns);              // stepper value; SCREW only
    const BeltTuneRound& add_round(const BeltCurve& a_window, const BeltCurve& b_window);
    [[nodiscard]] bool zone_matched() const;
    bool next_zone();                            // false when no unmatched zone remains
    [[nodiscard]] std::optional<float> hz_per_turn() const;
    [[nodiscard]] bool show_mechanical_hint() const;  // TALLEST_UNPAIRED or stalled
    [[nodiscard]] const std::vector<BeltTuneRound>& rounds() const;  // current zone
};
}
```

Every header above gets a `scripts/syntax_check.py` pass before the plan executes.

## Mock

`MoonrakerClientMock::dispatch_test_resonances_response` honours `FREQ_START`/`FREQ_END`
(progress lines over the window). `HELIX_MOCK_BELT_TUNE_SCRIPT="102.1,107.6,109.8"` gives
the adjusted path's peak for successive narrow sweeps, so `--test` walks a deterministic
loop to MATCHED without coupling the panel to the mock. The default mock (98 vs 110 Hz,
unpaired) exercises the TALLEST_UNPAIRED path and the shape warning.

## Tests

- `BeltTuneSession` against synthetic Lorentzian curves and the `tests/fixtures/belt_sweeps/`
  captures: zone pick (paired, unpaired fallback, amplitude floor, clamp to range, no
  zone), parabolic refinement off-bin, gap sign, default adjusted path, swap, every outcome
  row above, rate median ignoring sub-`MIN_MOVE_HZ` rounds, amount rounding and clamp,
  UNKNOWN/SPRING never yielding turns, stall hint at round 8, next_zone exhaustion.
- `test_belt_resonance` with a window emits `FREQ_START=`/`FREQ_END=` and scales progress
  against it; without one, the G-code is byte-identical to today's.
- `get_belt_tensioner` for screw, spring, absent and an unknown string.
- Panel: Tune button label by verdict; TUNING phases driven through the mock script;
  Good enough lands on RESULTS without a sweep; Confirm lands on RESULTS with "was N%".
- `make mutate-diff` once at the end of the branch.

## Out of scope

- Persisting Hz/turn across sessions or printers.
- Spring-tensioner detection from Klipper config (there is no signal there).
- Any absolute tension target or Z belts.

## Hardware verification (before merge)

1. `FREQ_START`/`FREQ_END` accepted on Kalico (Voron .112) and mainline (K1C or AD5M).
2. On the Voron: does a ¼ turn move the belt peak at least 0.5 Hz, and does the broad
   119-133 Hz hump move as one piece? If it smears, peak refinement needs a centroid
   instead of the maximum.
3. A full loop on the Voron with Preston at the tensioner; then ask the #1721 reporter for a
   run on their 350.
