# Motor Ripple Speed Sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A "Vertical Artifact Check" calibration that sweeps each XY motor through print
speeds while streaming the accelerometer, draws ripple amplitude against speed live,
saves bands of speeds to avoid, warns before printing a file whose outer walls land in
one, and on FlashForge MCLib printers shows compensation off against on.

**Architecture:** Pure, LVGL-free modules do the work: the motion plan
(`motor_ripple_plan`), the analysis (`motor_ripple_analysis`) and serialisation
(`motor_ripple_store`). A runner state machine (`MotorRippleRunner`) sends one gcode script
per speed and slices the `IAccelBatchSource` stream into captures. An overlay panel
(`MotorRipplePanel`, shaped like `BeltTensionPanel`) shows it on an extended
`ui_frequency_response_chart`. A pure print-start gate reads the saved bands and the
outer-wall speed from the gcode footer.

**Tech Stack:** C++17 (firmware builds shared code as gnu++17: no C++20, no `std::span`),
LVGL 9.5 + helix-xml, Catch2, libhv (through the existing `BeltStreamClient`), Moonraker
database API.

**Spec:** `docs/devel/plans/2026-11-12-vfa-speed-sweep-design.md`. Read it first. This
plan does not repeat its reasoning.

## Global Constraints

- **Code rules:**
  - Code: `// SPDX-License-Identifier: GPL-3.0-or-later`, spdlog only, `kConstantName`
    constants.
  - Not allowed: exceptions (`json::parse(s, nullptr, false)`, `json_util::safe_*`),
    RTTI, `std::regex`.
  - JSON: `#include "hv/json.hpp"`.
  - No comment archaeology.
- **Vendor names** (`STEPPER_RESONANCE_DAMP_*`, FlashForge, Z-Mod, OpenCreator) appear
  only in `src/printer/stepper_resonance_calibration.cpp`. Generic code asks capability
  questions (`.claude/rules/vendor-abstraction.md`).
- **Threading:**
  - Stream batches arrive on the source's loop thread. Analysis runs there, or on
    `HttpExecutor`, never on the LVGL thread.
  - Results reach the UI only through `ui_queue_update()` or an `AsyncLifetimeGuard`.
  - No `std::thread(...).detach()` (`.claude/rules/threading.md`).
- **Motion safety:** refuse while `machine_motion_blocked`. Home through
  `ensure_homed_then`. Restore velocity limits and gcode state after success, error,
  cancel and panel teardown alike.
- **Fixed values** (from the spec):

  | Value | Setting |
  |---|---|
  | Speeds | from 20 mm/s, x1.08 per step, up to `min(300, 0.9 * max_velocity)` |
  | Cruise | 2 s per speed |
  | Pass length | `clamp(v * 1 s, 30 mm, 0.85 * usable diagonal)` |
  | Test accel | `min(10000, max_accel)`, `MINIMUM_CRUISE_RATIO=0` |
  | Z clearance | 20 mm, or the current Z if higher, capped at max Z |
  | Ceiling | `0.45 * measured sample rate` |
  | Band rule | ripple > 3.0x its own sweep median and > 500 mm/s^2 |
  | Database key | namespace `helix-screen`, key `speed_sweep` |

- **UI rules** (`.claude/rules/declarative-ui.md`):
  - Data in C++, appearance in XML, design tokens only.
  - One `bind_flag_if` per widget, with a combined `cond`.
  - New strings go through `lv_tr()` with English entries in `translations/`.
- **Test cadence:** one session owns the test plan (below). Implementers run only
  `make t F='[ripple]'`, or the task's own tags, once after each change. Never
  mutate-diff, sweeps or `land-gate` per task.

## Review Focus

1. **A stream that stalls or overflows mid-speed.** The runner must not record a speed from
   a capture with gaps as if it were clean. The speed gets one retry, then the run fails
   cleanly with limits restored. Test in Task 6.
2. **A low-rate accelerometer** (QIDI's `lis2dw` at ~387 Hz). Every ripple above the
   ceiling must be absent, not zero. The chart must show the gap, and the bands must be
   built only from measured values. Tests in Tasks 2 and 3.
3. **The panel closed or the printer disconnected mid-run.** Limits and gcode state are
   restored where the connection allows, and nothing touches a freed panel. Test in
   Task 6.
4. **A Prusa file whose `external_perimeter_speed` is a percentage,** or a file from a slicer
   whose key is missing. The value must be resolved, or treated as unknown. It must never
   be read as the raw number. Test in Task 8.
5. **A saved result from a different printer, or one with a damaged JSON record.** It loads
   as "no result" and never crashes the gate. Test in Task 7.

## Test plan (owned by the session running this plan)

- **While building:** each task runs its own tags once: `make t F='[ripple]'`, or the tags
  named in the task.
- **Merged result:** after all tasks merge, run `make t F='[ripple],[print_start],[gcode_footer],[frequency_chart],[stepper_resonance]'`
  once on the merged tree.
- **Finish:**
  1. One review of the whole branch.
  2. One headless screenshot per changed screen.
  3. ONE `make land-gate` on the fastest free machine.
- **Device check before landing:**
  - Voron at 192.168.1.112 and QIDI Q2 at 192.168.1.85: run the check end to end, with a
    `scripts/helix-claim` device claim.
  - Creator 5 Pro on Z-Mod at 192.168.30.130: the compensation-compare mode.
  - Ask Preston before any motion on a printer.
- **Series finished:** `make mutate-diff` once over the series, in the background, capped
  at 45 min.

---

### Task 1: Ripple types and the motion plan

**Files:**
- Create: `include/motor_ripple_types.h`, `include/motor_ripple_plan.h`, `src/calibration/motor_ripple_plan.cpp`
- Test: `tests/unit/test_motor_ripple_plan.cpp`

**Interfaces:**
- Produces (`include/motor_ripple_types.h`, namespace `helix::calibration::ripple`):

```cpp
inline constexpr float kSweepMinMmS = 20.0f;
inline constexpr float kSweepMaxMmS = 300.0f;
inline constexpr float kSweepStepRatio = 1.08f;
inline constexpr float kSweepVelocityFraction = 0.9f;
inline constexpr float kCruiseSeconds = 2.0f;
inline constexpr float kMinPassMm = 30.0f;
inline constexpr float kPassDiagonalFraction = 0.85f;
inline constexpr float kTestAccelMmS2 = 10000.0f;
inline constexpr float kClearanceZMm = 20.0f;
inline constexpr float kCeilingFraction = 0.45f;
inline constexpr float kBandFactor = 3.0f;
inline constexpr float kBandFloorMmS2 = 500.0f;
inline constexpr std::array<int, 3> kHarmonics = {1, 2, 4};

enum class Kinematics { CoreXY, Cartesian, Unsupported };
enum class MotorId { StepperX, StepperY };

struct MotorGeometry {
    float rotation_distance_mm = 0.0f;
    float gear_ratio = 1.0f;          ///< product of gear_ratio pairs; 1 when none
    int full_steps_per_rotation = 200;
};

struct SpeedResult {
    float speed_mm_s = 0.0f;
    std::array<std::optional<float>, 3> ripple; ///< mm/s^2, indexed like kHarmonics; nullopt above the ceiling
    float broadband = 0.0f;                      ///< relative only
    int frames_used = 0;
};

struct MotorSweep {
    MotorId motor = MotorId::StepperX;
    std::vector<SpeedResult> speeds; ///< ascending speed
};

struct RippleBand {
    MotorId motor = MotorId::StepperX;
    float lo_mm_s = 0.0f;
    float hi_mm_s = 0.0f;
    int harmonic = 1;
    float peak_speed_mm_s = 0.0f;
    float peak_mm_s2 = 0.0f;
    float freq_hz = 0.0f;
};

struct SkippedMotor { MotorId motor; std::string reason; }; ///< reason untranslated

struct SweepResult {
    float sample_rate_hz = 0.0f;
    float ceiling_hz = 0.0f;
    int64_t unix_time = 0;
    std::vector<MotorSweep> sweeps;              ///< compensation off, or the only run
    std::vector<MotorSweep> compensated_sweeps;  ///< compensation on; empty unless compare mode
    std::vector<RippleBand> bands;               ///< from compensated_sweeps when present, else sweeps
    std::vector<SkippedMotor> skipped;
};

[[nodiscard]] float electrical_cycle_mm(const MotorGeometry& g); ///< rd / gear * 4 / full_steps
[[nodiscard]] float ripple_freq_hz(float speed_mm_s, int harmonic, float electrical_mm);
```

- Produces (`include/motor_ripple_plan.h`):

```cpp
struct SweepPlanInputs {
    Kinematics kinematics = Kinematics::Unsupported;
    float x_min = 0, x_max = 0, y_min = 0, y_max = 0; ///< toolhead axis limits, machine coords
    bool has_xy_limits = false;
    std::optional<std::array<float, 3>> probe_point;  ///< resonance_tester probe point
    float origin_x = 0, origin_y = 0, origin_z = 0;   ///< gcode_move.homing_origin
    float max_velocity_mm_s = 0;
    float max_accel_mm_s2 = 0;
    float current_z = 0, max_z = 0;
    std::optional<float> minimum_cruise_ratio; ///< nullopt: firmware has max_accel_to_decel instead
    float max_accel_to_decel = 0;
    bool y_motor_measurable = true;            ///< false on a Cartesian without accel_chip_y
};

enum class SweepRefusal { None, UnsupportedKinematics, NoAxisLimits, NoVelocity, SpanTooSmall };

struct StepPlan {
    SweepRefusal refusal = SweepRefusal::None;
    std::string gcode;       ///< approach, passes, M400, G4 P250; empty when refused
    int passes = 0;          ///< even, >= 2
    float pass_mm = 0;       ///< toolhead travel per pass
    float seconds = 0;       ///< cruise time over all passes
    uint32_t timeout_ms = 0; ///< max(30000, 4 * total move time)
};

[[nodiscard]] std::vector<float> sweep_speeds(float max_velocity_mm_s);
[[nodiscard]] std::vector<MotorId> motors_to_measure(const SweepPlanInputs& in);
[[nodiscard]] StepPlan plan_step(const SweepPlanInputs& in, MotorId motor, float speed_mm_s);
[[nodiscard]] std::string begin_gcode(const SweepPlanInputs& in);   ///< SAVE_GCODE_STATE, G90, Z clearance, SET_VELOCITY_LIMIT
[[nodiscard]] std::string restore_gcode(const SweepPlanInputs& in); ///< limits back, RESTORE_GCODE_STATE
```

- [ ] **Step 1: Write the failing tests** (`[ripple][plan]`)
  - `electrical_cycle_mm({40, 1, 200}) == Approx(0.8)`, `({38.82, 1, 200}) == Approx(0.7764)`, gear ratio 3 divides by 3.
  - `ripple_freq_hz(86.3, 2, 0.8) == Approx(215.75)`.
  - `sweep_speeds(500)`: front 20, back 300, size 37, every ratio between neighbours <= 1.08 + 1e-3. `sweep_speeds(200)` ends at 180. `sweep_speeds(0)` is empty.
  - CoreXY, Voron 300x305 limits, probe point (150, 150, 20):
    - stepper_x passes run along (1, 1) and stepper_y along (1, -1). Parse the G1 endpoints out of `gcode` and check `dx == dy`, or `dx == -dy`.
    - Toolhead feedrate is `speed / sqrt(2) * 60`.
    - At 20 mm/s: `pass_mm == 30`, `passes == 2`.
    - At 300 mm/s: `pass_mm <= 0.85 *` diagonal span, and `passes * pass_mm / (300 / sqrt(2)) >= 2.0`.
  - Cartesian: stepper_x passes move X only. `motors_to_measure` drops StepperY when `y_motor_measurable` is false.
  - Refusals: `Kinematics::Unsupported`, no limits, zero velocity, and a 40 mm span all refuse with empty gcode.
  - `begin_gcode`:
    - contains `SAVE_GCODE_STATE NAME=helix_ripple_sweep`;
    - contains `SET_VELOCITY_LIMIT ACCEL=10000 MINIMUM_CRUISE_RATIO=0` when `max_accel` is 30000;
    - contains `ACCEL=5000` when `max_accel` is 5000;
    - uses `ACCEL_TO_DECEL` when `minimum_cruise_ratio` is nullopt;
    - raises Z to 20, or keeps a current Z of 40;
    - caps Z at `max_z`.
  - `restore_gcode` puts back the exact `max_accel` and `minimum_cruise_ratio` it was given, and ends in `RESTORE_GCODE_STATE NAME=helix_ripple_sweep`.
- [ ] **Step 2:** Run `make t F='[ripple][plan]'`. Expected: FAIL to compile (missing header).
- [ ] **Step 3:** Implement. Centre on the probe point, converted to gcode coordinates with the homing origin, as `belt_settle.cpp#plan_belt_settle` does. The usable diagonal is the largest segment along the motor's direction that keeps both ends inside the limits, inset by 10 mm.
- [ ] **Step 4:** Run `make t F='[ripple][plan]'`. Expected: PASS. Break the `passes` even-rounding once, see a test go red, restore.
- [ ] **Step 5:** Commit: `git add -N` the new files, then `git commit -m "feat(calibration): motor ripple sweep motion plan" -- <paths>`.

### Task 2: Capture analysis

**Files:**
- Create: `include/motor_ripple_analysis.h`, `src/calibration/motor_ripple_analysis.cpp`
- Test: `tests/unit/test_motor_ripple_analysis.cpp`

**Interfaces:**
- Consumes: Task 1 types; `AccelSample` from `include/belt_tension_types.h`.
- Produces:

```cpp
[[nodiscard]] float ceiling_hz(float sample_rate_hz); ///< kCeilingFraction * rate
[[nodiscard]] SpeedResult analyze_capture(const std::vector<AccelSample>& samples,
                                          float sample_rate_hz, float speed_mm_s,
                                          float electrical_mm);
```

- [ ] **Step 1: Write the failing tests** (`[ripple][analysis]`). The input is synthetic: 3 s at 3200 Hz, with sinusoids summed on x, y and z.
  - A 600 mm/s^2 sine at `ripple_freq_hz(86.3, 2, 0.8)` on x, plus 1% white noise. `ripple[1]` is within 5% of 600, and `ripple[0]` and `ripple[2]` are below 60.
  - The same amplitude split as 0.6 / 0.8 on x and y comes out within 5% of 600 (quadrature sum).
  - A capture with 0.5 s of near-silence at each end returns the same value within 5% (idle frames are excluded), and `frames_used` is below the frame count.
  - At 387 Hz, the speed is 130 mm/s with `electrical_mm` 0.7764: `ripple[0]` (167 Hz) has a value; `ripple[1]` and `ripple[2]` are nullopt.
  - An empty capture, or one shorter than one frame, gives `frames_used == 0` and every ripple nullopt.
- [ ] **Step 2:** Run `make t F='[ripple][analysis]'`. Expected: FAIL.
- [ ] **Step 3:** Implement as the spec describes:
  - Hann frames of `clamp(6 / f1, 0.16 s, 0.5 s)`, hopping a quarter frame.
  - Goertzel amplitude at each `f_k` per axis, combined in quadrature, `2|X| / sum(w)`.
  - Broadband: the RMS of the frame after a first-order 15 Hz high-pass.
  - Moving frames: broadband above 0.3x the 90th percentile.
  - The result is the median over moving frames.
  - Do not allocate per frame.
- [ ] **Step 4:** Run the tags. Expected: PASS. Then check the speed: on thelio, time `analyze_capture` on 3 s at 3200 Hz (a `[.][ripple][bench]` case). Record the figure in the commit body. The target is under 50 ms on thelio, a proxy for keeping up on MIPS.
- [ ] **Step 5:** Commit.

### Task 3: Bands and slicer advice

**Files:**
- Modify: `include/motor_ripple_analysis.h`, `src/calibration/motor_ripple_analysis.cpp`
- Test: `tests/unit/test_motor_ripple_bands.cpp`. Fixtures: `tests/fixtures/motor_ripple/voron24_tmc2240_adxl345.json` and `qidi_q2_lis2dw.json`, already committed. They hold per-speed `r1`/`r2`/`r4` (null above the ceiling) for each motor, from the spike.

**Interfaces:**
- Produces:

```cpp
[[nodiscard]] std::vector<RippleBand> find_bands(const MotorSweep& sweep, float electrical_mm,
                                                 float factor = kBandFactor,
                                                 float floor_mm_s2 = kBandFloorMmS2);
/// Union over motors, sorted by lo; overlapping bands from different motors merge.
[[nodiscard]] std::vector<RippleBand> merge_bands(std::vector<RippleBand> bands);
[[nodiscard]] const RippleBand* band_containing(const std::vector<RippleBand>& bands, float speed_mm_s);
/// Nearest speed outside every band, in whole mm/s, or nullopt past the sweep range.
[[nodiscard]] std::optional<int> nearest_clear_speed(const std::vector<RippleBand>& bands,
                                                     float speed_mm_s, bool below);
```

- [ ] **Step 1: Write the failing tests** (`[ripple][bands]`). Each fixture is loaded into a `MotorSweep`.
  - Voron stepper_x gives exactly 3 bands: [77.0, 97.0] with harmonic 2 and peak 86.3; [113.1, 122.1] with harmonic 2; [166.2, 179.4] with harmonic 1 and peak 172.5. Tolerance is ±0.2 mm/s.
  - Voron stepper_y gives [41.6, 44.9] with harmonic 4, [77.0, 97.0] with harmonic 2, and [153.9, 179.4] with harmonic 1.
  - The Q2 stepper_x bands include one containing 43.5 and one containing 101.4. No band is built from a null value.
  - A flat sweep (every ripple 100) gives no bands. A sweep whose peak is 3.5x the median but only 400 mm/s^2 gives no bands (the floor).
  - `freq_hz` of the Voron 2x band is within 1 Hz of 215.75.
  - Merging the Voron X and Y bands gives one band covering 77-97. `band_containing(merged, 86)` is non-null, and `band_containing(merged, 100)` is null.
  - `nearest_clear_speed(voron_merged, 86, true) == 76` and `nearest_clear_speed(voron_merged, 86, false) == 98`.
- [ ] **Step 2:** Run the tags. Expected: FAIL.
- [ ] **Step 3:** Implement the rule exactly as the spec's "Bands" section states.
  - Read the fixtures through the existing test-fixture path helper used by `tests/unit/test_gcode_footer_summary.cpp` or the AFC tests.
- [ ] **Step 4:** Run the tags. Expected: PASS. Change 3.0 to 2.5 once, watch the stepper_x count go red, restore.
- [ ] **Step 5:** Commit.

### Task 4: Discovery reads stepper geometry and accelerometer roles

**Files:**
- Modify: `include/printer_discovery.h`, `src/printer/printer_discovery_parse.cpp`
- Test: the existing discovery parse test file, which `grep -l "has_accelerometer" tests/unit` finds.

**Interfaces:**
- Produces, on `PrinterDiscovery`:

```cpp
[[nodiscard]] std::optional<ripple::MotorGeometry> motor_geometry(ripple::MotorId m) const; ///< from configfile stepper_x / stepper_y
[[nodiscard]] const std::string& accel_chip_for(ripple::MotorId m) const; ///< accel_chip_x/_y, else accel_chip; empty when none
[[nodiscard]] bool has_axis_accel_chip_y() const;                         ///< resonance_tester names accel_chip_y
```

- [ ] **Step 1:** Write the failing tests (`[discovery][ripple]`), each a configfile JSON in the shape the existing tests use.
  - Voron: `rotation_distance` 40, `full_steps_per_rotation` 200, no `gear_ratio` → `{40, 1, 200}`.
  - `gear_ratio: [[3, 1]]` → gear 3.
  - A missing `stepper_y` → nullopt.
  - Accelerometer chips:
    - `accel_chip: adxl345`: both motors get "adxl345".
    - `accel_chip_x: adxl345 hotend` plus `accel_chip_y: adxl345 bed`: each motor gets its own.
    - No resonance_tester: an empty string.
  - The `has_axis_accel_chip_y` flag.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Parse in the same pass that sets `has_accelerometer_`. Reset in the same reset function (`printer_discovery_parse.cpp` near line 1155).
- [ ] **Step 4:** Run them. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 5: Compensation toggle capability

**Files:**
- Modify: `include/stepper_resonance_calibration.h`, `src/printer/stepper_resonance_calibration.cpp`
- Test: `tests/unit/test_stepper_resonance_calibration.cpp`

**Interfaces:**
- Produces:

```cpp
struct CompensationToggle {
    std::string enable_gcode;  ///< empty: no toggle
    std::string disable_gcode;
    bool applies_factory_values = false; ///< enable restores fixed numbers, not a calibration
    [[nodiscard]] bool present() const { return !enable_gcode.empty() && !disable_gcode.empty(); }
};
[[nodiscard]] CompensationToggle compensation_toggle(const PrinterDiscovery& hw);
```

- [ ] **Step 1:** Write the failing tests (`[stepper_resonance]`):
  - Both `STEPPER_RESONANCE_DAMP_ENABLE` and `_DISABLE` macros present → present, with those gcodes.
  - Only one present → absent.
  - `applies_factory_values` is true exactly when the macros are present AND the routine row that resolves is the stock `stepper_resonance_tester` row. It is false for the Z-Mod and OpenCreator rows.
  - A printer with no macros → absent.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement beside `resolve()`, reusing its detection predicates. No vendor string leaves this file.
- [ ] **Step 4:** Run them. Expected: PASS. Then run `scripts/check_vendor_branches.py`: no new counts outside this file.
- [ ] **Step 5:** Commit.

### Task 6: The runner

**Files:**
- Create: `include/motor_ripple_runner.h`, `src/calibration/motor_ripple_runner.cpp`
- Test: `tests/unit/test_motor_ripple_runner.cpp` (uses `tests/mocks` / `FakeMoonrakerClient` and a scripted `IAccelBatchSource`)

**Interfaces:**
- Consumes: Tasks 1-5; `IAccelBatchSource` and its `SourceFactory` type from `include/live_sweep_preview.h`; `ensure_homed_then` (`src/printer/toolhead_homing.cpp`); `IMoonrakerAPI` gcode execution, with the timeout taken from `StepPlan::timeout_ms`.
- Produces:

```cpp
class MotorRippleRunner {
  public:
    enum class Mode { Sweep, CompensationCompare };
    enum class Phase { Idle, Homing, Preparing, Measuring, Restoring, Done, Failed, Cancelled };
    struct Request {
        Mode mode = Mode::Sweep;
        SweepPlanInputs plan;
        std::map<MotorId, MotorGeometry> geometry;
        std::map<MotorId, std::string> accel_chip;
        std::string klippy_socket;
        CompensationToggle toggle;      ///< used only in CompensationCompare
        std::string tool_select_gcode;  ///< empty: none; the panel fills it on tool changers
        std::string tool_return_gcode;
    };
    struct Callbacks { // all fire on the LVGL thread
        std::function<void(Phase)> on_phase;
        std::function<void(bool compensated, MotorId, const SpeedResult&, size_t index, size_t total)> on_speed;
        std::function<void(const SweepResult&)> on_done;
        std::function<void(const std::string& reason)> on_failed; ///< untranslated
    };
    MotorRippleRunner(IMoonrakerAPI& api, LiveSweepPreview::SourceFactory source_factory);
    ~MotorRippleRunner(); ///< cancels; sends restore_gcode if Measuring
    bool start(Request req, Callbacks cb); ///< false (with no motion) on a refused plan or busy runner
    void cancel();
    [[nodiscard]] Phase phase() const;
};
```

- [ ] **Step 1:** Write the failing tests (`[ripple][runner]`). The scripted source emits a synthetic ripple for whichever speed's script is "running", and `FakeMoonrakerClient` records the gcode.
  - **Happy path, Sweep mode, CoreXY:**
    - The sent order is the homing check, `begin_gcode`, then one script per speed for X, then for Y, then `restore_gcode`.
    - `on_speed` fires 74 times.
    - `on_done` gets bands, with `ceiling_hz` equal to `0.45 *` the source rate.
  - **CompensationCompare:**
    - The order is `disable_gcode`, the X+Y sweep, `enable_gcode`, the X+Y sweep, `disable_gcode`, then `restore_gcode`.
    - The bands come from `compensated_sweeps`.
  - **A batch with overflows during a speed:** that speed is retried once. A second dirty capture fails the run, and `restore_gcode` is still the last gcode sent.
  - **Cancel mid-speed:** no further speed scripts are sent, `restore_gcode` is sent, and `Phase::Cancelled` is reached. In CompensationCompare, `disable_gcode` is sent before restore.
  - **Destructor during Measuring:** `restore_gcode` is sent, and no callback fires afterwards.
  - **Busy or refusals:** `start` while `machine_motion_blocked` returns false and sends nothing. A refused plan returns false.
  - **Stream stall:** no batch for longer than `BeltStreamClient::kStallTimeoutMs` fails the run, with restore sent.
- [ ] **Step 2:** Run the tests. Expected: FAIL.
- [ ] **Step 3:** Implement.
  - **Segmenting:** a capture runs from the sample count when its script is sent to the sample count when its RPC returns.
  - **Analysis:** run `analyze_capture` off the LVGL thread. Hand each result over through `AsyncLifetimeGuard` (`lifetime_.bg_cb`).
  - **Gcode sends:** chain them through the API callbacks, one at a time.
- [ ] **Step 4:** Run the tests. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 7: Saved results

**Files:**
- Create: `include/motor_ripple_store.h`, `src/calibration/motor_ripple_store.cpp`
- Test: `tests/unit/test_motor_ripple_store.cpp`

**Interfaces:**
- Produces:

```cpp
inline constexpr const char* kRippleDbNamespace = "helix-screen";
inline constexpr const char* kRippleDbKey = "speed_sweep";
inline constexpr int kRippleSchemaVersion = 1;
[[nodiscard]] json ripple_result_to_json(const SweepResult& r);
[[nodiscard]] std::optional<SweepResult> ripple_result_from_json(const json& j); ///< nullopt on a wrong version or shape
```

  Plus a small `RippleResultCache`, owned like the other per-printer caches:
  - `load(IMoonrakerAPI&)` runs on connect.
  - `save(IMoonrakerAPI&, const SweepResult&)` runs only for a completed run.
  - `bands()` serves the gate.

- [ ] **Step 1:** Write the failing tests (`[ripple][store]`):
  - A round trip keeps nullopt ripples as JSON null.
  - Version 2 → nullopt.
  - A missing `bands`, `"speeds": "x"` and a truncated document → nullopt, with no throw. Parse with `json::parse(s, nullptr, false)`.
  - The cache: `save` is never called for a Failed or Cancelled run (the runner test drives it); `bands()` is empty before `load`.
- [ ] **Step 2:** Run the tests. Expected: FAIL.
- [ ] **Step 3:** Implement with `json_util::safe_*`.
- [ ] **Step 4:** Run the tests. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 8: Outer-wall speed from the footer, and the print-start gate

**Files:**
- Modify: `include/gcode_footer_summary.h`, `src/rendering/gcode_footer_summary.cpp`, `include/print_start_checks.h`, `src/printer/print_start_checks.cpp` (gate table near line 1068), `src/printer/print_start_inputs.cpp#fill_file_context`
- Test: `tests/unit/test_gcode_footer_summary.cpp`, the existing print-start-checks test file

**Interfaces:**
- Produces:
  - `GcodeFooterSummary::outer_wall_speed_mm_s` (`std::optional<float>`).
  - `PrintStartContext::ripple_bands` (`std::vector<ripple::RippleBand>`) and `PrintStartContext::outer_wall_speed_mm_s` (`std::optional<float>`).
  - `CheckResult gate_outer_wall_speed(const PrintStartContext&)`, registered as `{"outer_wall_speed", gate_outer_wall_speed}`.

- [ ] **Step 1:** Write the failing tests.
  - **Footer** (`[gcode_footer]`):
    - `; outer_wall_speed = 200` → 200.
    - `; external_perimeter_speed = 25` → 25.
    - `; external_perimeter_speed = 50%` with `; perimeter_speed = 60` → 30.
    - `50%` with no `perimeter_speed` → nullopt.
    - `; outer_wall_speed = 0` → nullopt.
    - No key → nullopt.
    - The 7.8 KB-from-the-end placement in `assets/test_gcodes/vaso_voronoi_V2_abajo.gcode`, read through `read_file_tail` with the default window → 200.
  - **Gate** (`[print_start]`), using the merged Voron bands from Task 3:
    - Speed 86 → Warn. The body names "86", the band "77-97" and the suggestions "76" and "98".
    - Speed 120 → Pass.
    - No bands → Pass.
    - No speed → Pass.
    - It never returns Block.
- [ ] **Step 2:** Run `make t F='[gcode_footer],[print_start]'`. Expected: FAIL.
- [ ] **Step 3:** Implement.
  - **The warning text:** "Your file asks for outer walls at {speed} mm/s. This printer shook hardest at {lo}-{hi} mm/s in its last Vertical Artifact Check, which can print fine vertical lines on walls along X or Y. Try {below} or {above} mm/s (Orca: Outer wall speed; PrusaSlicer: External perimeters)." Use `lv_tr()` with format placeholders, as other gates do.
  - **Bands into the context:** `fill_file_context` fills the bands from `RippleResultCache`.
- [ ] **Step 4:** Run the tags. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 9: Chart extensions

**Files:**
- Modify: `include/ui_frequency_response_chart.h`, `src/ui/ui_frequency_response_chart.cpp`
- Test: `tests/unit/test_frequency_response_chart.cpp`

**Interfaces:**
- Produces:

```cpp
void ui_frequency_response_chart_set_x_unit(ui_frequency_response_chart_t* chart, const char* unit); ///< default "Hz"
struct FrChartBand { float x_lo; float x_hi; };
void ui_frequency_response_chart_set_bands(ui_frequency_response_chart_t* chart, const FrChartBand* bands, size_t count);
```

  The "before" series reuse `set_series_muted`. Do not add a dashed style.

- [ ] **Step 1:** Write the failing tests (`[frequency_chart]`):
  - The x labels read "mm/s" after `set_x_unit("mm/s")`.
  - Two bands give two band rects inside the plot, at the right x positions within one pixel at 800x480. Read their geometry the way the existing marker tests do.
  - `set_bands(nullptr, 0)` clears them.
  - Bands survive `set_freq_range`.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement. The band fill is a design token (the warning colour at low opacity), not a literal.
- [ ] **Step 4:** Run them. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 10: Mock ripple stream and compensation macros

**Files:**
- Modify: `include/sweep_synth.h`, `src/mock/accel_batch_source_mock.cpp`, the `creator5_zmod` mock persona (`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` names where personas live), `src/mock/moonraker_client_mock.cpp`
- Test: `tests/unit/test_motor_ripple_mock.cpp`

**Interfaces:**
- Produces:
  - A ripple synth mode: for the speed whose script is running, it emits sinusoids at `f_k`. Their amplitudes follow one or two Lorentzian resonances (default 216 Hz, Q 8). Running the toggle's `enable_gcode` scales them by 0.25.
  - `HELIX_MOCK_ACCEL_RATE` (default 3200) sets the rate the stream reports and uses. Document it in `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`.
  - The `creator5_zmod` persona gets `STEPPER_RESONANCE_DAMP_ENABLE` / `_DISABLE` macros.
- [ ] **Step 1:** Write the failing test (`[ripple][mock]`). `MotorRippleRunner` over the mock source finds a 2x band containing 86 mm/s at a 216 Hz resonance with rotation distance 40. At `HELIX_MOCK_ACCEL_RATE=387`, there are no 2x or 4x values above 174 Hz.
- [ ] **Step 2:** Run the test. Expected: FAIL.
- [ ] **Step 3:** Implement. The mock must know which speed is running: parse the feedrate from the last speed script it received.
- [ ] **Step 4:** Run the test. Expected: PASS.
- [ ] **Step 5:** Commit.

### Task 11: The panel, its entry row and "Check the result"

**Files:**
- Create: `include/ui_panel_motor_ripple.h`, `src/ui/calibration/ui_panel_motor_ripple.cpp`, `ui_xml/motor_ripple_panel.xml`
- Modify: `ui_xml/advanced_panel.xml` (a row beside Belt Tension), `src/printer/printer_capabilities_state.cpp` (a `printer_supports_ripple_check` subject), `src/ui/calibration/ui_stepper_resonance_cal_modal.cpp` (the post-restart "Check the result" action), `translations/` English strings, `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` if a demo knob is added
- Test: `tests/ui/` panel test following the Belt Tension panel tests

**Interfaces:**
- Consumes:
  - Tasks 4-10.
  - `BeltTensionPanel`'s co-location probe and `ViewState` pattern (`include/ui_panel_belt_tension.h`).
  - `ShaperSweepChart`'s live hand-off.
- Produces: `MotorRipplePanel` (an `OverlayBase`) with `ViewState { Start, Running, Results, Error }`, opened from the Advanced row and from the compensation modal.

- [ ] **Step 1: Measure first.**
  - Run the mock with the pinned socket recipe from root `CLAUDE.md`, headless. Run `ctl geom` on the Belt Tension results view at 800x480 and at the smallest breakpoint.
  - Choose one chart with a motor toggle, or two charts, from those numbers. Record the numbers in the commit body.
- [ ] **Step 2:** Write the failing UI tests (`[ui][ripple]`):
  - The row is hidden without an accelerometer, on unsupported kinematics, and when the host is not co-located.
  - Start shows the clear-bed confirm, and Start is disabled until it is ticked.
  - Running shows a progress count of speeds over 74 that advances with mock speeds.
  - Results show one band list entry per merged band, with the Orca hint.
  - On the `creator5_zmod` persona, results show before and after with a reduction per ripple.
  - With `applies_factory_values` true, the after series is labelled "as your printer applies it", never "calibrated".
  - A skipped Y motor on a Cartesian mock shows its reason.
  - Closing during Running cancels the runner, and the mock records `restore_gcode`.
- [ ] **Step 3:** Run them. Expected: FAIL.
- [ ] **Step 4:** Implement.
  - **Appearance:** XML with tokens only; C++ owns subjects and data.
  - **Tool changers:** the tool picker reuses the compensation modal's tool dropdown pattern. Fill `tool_select_gcode` / `tool_return_gcode` from the existing tool-change abstraction (`ToolState`), never from vendor names.
  - **Teardown:** pair `push_overlay` with `register_overlay_instance`.
- [ ] **Step 5:** Run them. Expected: PASS. Take headless screenshots of Start, Running, Results (sweep) and Results (compare) at 800x480, plus Results at the smallest breakpoint.
- [ ] **Step 6:** Commit.

### Task 12: Docs, and removing the plan

**Files:**
- Create: `docs/devel/MOTOR_RIPPLE_CHECK.md`, covering:
  - the method;
  - the band rule and its fixtures;
  - why table tuning is out (the spike numbers from the spec);
  - module map;
  - mock knobs.
- Modify:
  - `docs/user/guide/calibration.md`: a "Vertical Artifact Check" section beside Vertical Artifact Compensation, in user voice, with Orca and PrusaSlicer setting names.
  - The doc indexes (`docs/devel/CLAUDE.md`, `docs/README.md`).
  - `docs/devel/CHANGELOG_1_1_DRAFT.md` or the 1.2 notes file, whichever main uses for unreleased notes at the time.
- Delete: both plan files, `docs/devel/plans/2026-11-12-vfa-speed-sweep-design.md` and `docs/devel/plans/2026-11-12-vfa-speed-sweep.md`.

- [ ] **Step 1:** Write the docs.
- [ ] **Step 2:** Commit through the hook, which checks doc references and index routing: `git commit -m "docs: motor ripple speed sweep" -- <paths>`.
