# Live Belt Tuner Phase 1 - DSP Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and unit-test the three pure DSP units the live belt tuner needs, fix the octave-error bug in the shipping pitch analysis, and delete the two dead code paths the design retired.

**Architecture:** One change to the existing `compute_psd`, then three new header/source pairs under `include/` and `src/calibration/`, each a pure function or small stateful class over sample buffers with no LVGL, no I/O, and no threading. They are validated against real accelerometer captures taken from a Voron 2.4, checked into `tests/fixtures/belt_plucks/`. No UI or streaming work happens in this plan - that is phase 2.

**Tech Stack:** C++17, Catch2 (`tests/unit/*.cpp`, auto-globbed), spdlog for logging, pure Makefile build.

## Global Constraints

- SPDX header on every new file: `// SPDX-License-Identifier: GPL-3.0-or-later`
- Logging is spdlog only. Never `printf`, `cout`, or `LV_LOG_*`.
- New `.cpp` under `src/*/` and new tests under `tests/unit/` are picked up by existing wildcards. **No Makefile edits are needed.**
- All new code lives in `namespace helix::calibration` to match `belt_tension_types.h`.
- Build the app with `make -j`. Build tests with `make test`. **These are separate targets** - `make -j` does not rebuild tests, so always `make test` before running them or you will test a stale binary.
- Run tests from the **repository root** or you get hundreds of spurious UI failures: `./build/bin/helix-tests "[tag]"`
- Before compiling, check for a concurrent build: `pgrep -x cc1plus`
- Do not run `git add -A` or `git add .`. Stage explicit paths only.
- Do not use `git rm`. Use plain `rm`, then stage the specific path.

## Reference Data

`tests/fixtures/belt_plucks/*.csv` are real ADXL345 ring-down captures from a Voron 2.4
(300mm, 151mm span, steppers energized, sample rate ~3091 Hz). Format is Klipper's raw
accelerometer CSV (`time,accel_x,accel_y,accel_z` with `#` comment lines), so
`parse_accel_csv()` from `belt_tension_types.h` reads them directly.

| fixture | ground truth | note |
|---|---|---|
| `a_belt_86hz_1/2/3.csv` | 86 Hz | 2nd harmonic dominates the fundamental by 3 dB |
| `b_belt_82hz_1/2/3.csv` | 82 Hz | fundamental dominates |
| `b_belt_82hz_hard_case.csv` | 82 Hz | weakest accepted pluck; a fixed 45 Hz search floor gets this wrong |
| `weak_pluck_reject.csv` | n/a | 1.4x noise floor; must be rejected before analysis |

## Critical Constraint Discovered During Planning

`compute_psd` caps its output at 250 Hz (`max_bin = min(n/2, 250*n/sample_rate)`).
A harmonic product spectrum over 4 harmonics needs bins out to `4 * f0` - about 344 Hz
for an 86 Hz belt - so with the 250 Hz cap **every real capture yields no candidate with
a complete harmonic series and the estimator returns nothing at all**. Verified against
all eight fixtures: 0.0 Hz at a 250 Hz cap, correct at 700 Hz.

Two consequences:

1. `compute_psd` must accept a bandwidth argument, and the pluck path must request
   roughly `n_harmonics * search_hi_hz`.
2. That widens the spectrum by ~2.8x, which puts a 2048-point window at ~135 ms on the
   reference CB1 with the current trig-per-sample loop. **The phasor rewrite is therefore
   required, not an optimisation** - with it the same window is ~23 ms.

This is why Task 1 is the `compute_psd` change: nothing downstream can be tested until
it lands.

## File Structure

| file | responsibility |
|---|---|
| `src/calibration/belt_tension_types.cpp` + `include/belt_tension_types.h` | modified: `compute_psd` gains a bandwidth parameter and a phasor inner loop |
| `include/pitch_estimator.h` / `src/calibration/pitch_estimator.cpp` | harmonic product spectrum, span-to-frequency helpers |
| `include/pluck_detector.h` / `src/calibration/pluck_detector.cpp` | noise floor, RMS strength gate, ring-down extraction |
| `include/pluck_aggregator.h` / `src/calibration/pluck_aggregator.cpp` | running median across plucks, commit threshold |
| `tests/unit/test_pitch_estimator.cpp` | new |
| `tests/unit/test_pluck_detector.cpp` | new |
| `tests/unit/test_pluck_aggregator.cpp` | new |

---

### Task 1: compute_psd - bandwidth parameter and phasor inner loop

Two changes to one function. `compute_psd` currently hard-caps its output at 250 Hz and
calls `sin`/`cos` once per sample per bin per axis. The cap makes harmonic analysis
impossible; the trig makes the widened spectrum too slow. Both must land before anything
downstream can be built.

**Files:**
- Modify: `include/belt_tension_types.h` (the `compute_psd` declaration, ~line 161)
- Modify: `src/calibration/belt_tension_types.cpp:262-339` (the `compute_psd` body)
- Test: `tests/unit/test_belt_tension_calibrator.cpp` (append; existing `[fft]` cases must keep passing unchanged)

**Interfaces:**
- Consumes: nothing new
- Produces: `std::vector<std::pair<float,float>> compute_psd(const std::vector<AccelSample>& samples, float sample_rate = 3200.0f, float max_freq_hz = 250.0f)`
  - The new third parameter is defaulted, so every existing call site keeps its current
    behaviour with no edit.

- [ ] **Step 1: Confirm the existing behaviour is green before touching it**

Run: `make test && ./build/bin/helix-tests "[fft]"`
Expected: PASS. This is the contract the change must preserve. If it is red before you
start, stop and find out why.

- [ ] **Step 2: Write the failing tests**

Append to `tests/unit/test_belt_tension_calibrator.cpp`:

```cpp
TEST_CASE("compute_psd default bandwidth stays at 250 Hz", "[belt_tension][fft]") {
    // Existing callers must not shift under them.
    const float sr = 3200.0f;
    const int n = 3200;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].time = t;
        samples[static_cast<size_t>(i)].x =
            std::sin(2.0f * static_cast<float>(M_PI) * 100.0f * t);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9.81f;
    }

    auto psd = compute_psd(samples, sr);
    REQUIRE(!psd.empty());
    CHECK(psd.back().first <= Catch::Approx(250.0f).margin(2.0f));
}

TEST_CASE("compute_psd honours a wider bandwidth request", "[belt_tension][fft]") {
    const float sr = 3200.0f;
    const int n = 3200;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].time = t;
        samples[static_cast<size_t>(i)].x =
            std::sin(2.0f * static_cast<float>(M_PI) * 100.0f * t) +
            0.3f * std::sin(2.0f * static_cast<float>(M_PI) * 600.0f * t);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9.81f;
    }

    auto narrow = compute_psd(samples, sr, 250.0f);
    auto wide = compute_psd(samples, sr, 700.0f);

    REQUIRE(!narrow.empty());
    REQUIRE(!wide.empty());
    CHECK(wide.size() > narrow.size());
    CHECK(wide.back().first > 650.0f);

    // The 600 Hz component is invisible at the narrow bandwidth and present at
    // the wide one. This is exactly why harmonic analysis needs the parameter.
    auto narrow_peak = find_peak_frequency(narrow, 500.0f, 700.0f);
    auto wide_peak = find_peak_frequency(wide, 500.0f, 700.0f);
    CHECK_FALSE(narrow_peak.found);
    REQUIRE(wide_peak.found);
    CHECK(wide_peak.frequency == Catch::Approx(600.0f).margin(4.0f));
}

TEST_CASE("compute_psd bandwidth is clamped by Nyquist", "[belt_tension][fft][edge_case]") {
    const float sr = 1000.0f;
    const int n = 1000;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        samples[static_cast<size_t>(i)].time = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].x = std::sin(static_cast<float>(i) * 0.1f);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9.81f;
    }
    // Asking for 5 kHz from a 1 kHz capture must not read past the array.
    auto psd = compute_psd(samples, sr, 5000.0f);
    REQUIRE(!psd.empty());
    CHECK(psd.back().first <= sr / 2.0f + 1.0f);
}

TEST_CASE("compute_psd phasor recurrence does not drift", "[belt_tension][fft][phasor]") {
    // Two tones, the lower one stronger. A phasor that accumulates rotation
    // error smears the peak; this catches that.
    const float sr = 3200.0f;
    const int n = 1024;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].time = t;
        samples[static_cast<size_t>(i)].x =
            std::sin(2.0f * static_cast<float>(M_PI) * 97.0f * t) +
            0.5f * std::sin(2.0f * static_cast<float>(M_PI) * 194.0f * t);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9810.0f;
    }

    auto psd = compute_psd(samples, sr, 400.0f);
    REQUIRE(psd.size() > 50);

    auto peak = find_peak_frequency(psd, 20.0f, 300.0f);
    REQUIRE(peak.found);
    // The 97 Hz tone has 4x the power of the 194 Hz one, so it must win.
    CHECK(peak.frequency == Catch::Approx(97.0f).margin(4.0f));
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[fft]"`
Expected: FAIL. "compute_psd honours a wider bandwidth request" fails because the third
argument does not compile yet.

- [ ] **Step 4: Update the declaration**

In `include/belt_tension_types.h`, replace the `compute_psd` declaration:

```cpp
/// Compute PSD via DFT from accelerometer samples
/// Returns vector of (frequency_hz, power) pairs
std::vector<std::pair<float, float>> compute_psd(const std::vector<AccelSample>& samples,
                                                 float sample_rate = 3200.0f);
```

with:

```cpp
/// Compute PSD via DFT from accelerometer samples.
///
/// Returns vector of (frequency_hz, power) pairs. Bin i sits at
/// (i+1)*resolution - there is no DC bin.
///
/// @param max_freq_hz Highest frequency to compute, clamped to Nyquist. The
///        default preserves the original 250 Hz behaviour. Harmonic analysis
///        needs roughly n_harmonics * f0 of bandwidth or the upper harmonics
///        fall outside the array; see pitch_estimator.h.
std::vector<std::pair<float, float>> compute_psd(const std::vector<AccelSample>& samples,
                                                 float sample_rate = 3200.0f,
                                                 float max_freq_hz = 250.0f);
```

- [ ] **Step 5: Update the implementation**

In `src/calibration/belt_tension_types.cpp`, change the signature to match, then replace
the `max_bin` computation:

```cpp
    size_t max_bin =
        std::min(n / 2, static_cast<size_t>(250.0f * static_cast<float>(n) / sample_rate));
```

with:

```cpp
    const float bandwidth = (max_freq_hz > 0.0f) ? max_freq_hz : 250.0f;
    size_t max_bin =
        std::min(n / 2, static_cast<size_t>(bandwidth * static_cast<float>(n) / sample_rate));
    if (max_bin == 0) {
        spdlog::warn("[BeltTension] Bandwidth {:.1f} Hz yields no bins at {:.1f} Hz sample rate",
                     bandwidth, sample_rate);
        return psd;
    }
```

Then replace the bin loop inside the `process_axis` lambda:

```cpp
        // DFT for this axis, accumulate power into psd
        for (size_t k = 1; k <= max_bin; ++k) {
            float real = 0.0f;
            float imag = 0.0f;
            float omega =
                2.0f * static_cast<float>(M_PI) * static_cast<float>(k) / static_cast<float>(n);

            for (size_t i = 0; i < n; ++i) {
                float angle = omega * static_cast<float>(i);
                real += signal[i] * std::cos(angle);
                imag -= signal[i] * std::sin(angle);
            }

            float power = (real * real + imag * imag) / (static_cast<float>(n) * sample_rate);
            psd[k - 1].second += power;
        }
```

with a rotating phasor - two trig calls per bin instead of two per sample per bin:

```cpp
        // DFT for this axis, accumulate power into psd.
        //
        // exp(-j*omega*i) is advanced by complex multiplication rather than
        // recomputed per sample. Measured 5.8x faster on an Allwinner H616.
        // The accumulators are double deliberately: a float phasor accumulates
        // enough rotation error over thousands of samples to smear the peak.
        for (size_t k = 1; k <= max_bin; ++k) {
            const double omega = 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n);
            const double step_cos = std::cos(omega);
            const double step_sin = std::sin(omega);

            double phasor_cos = 1.0; // angle 0
            double phasor_sin = 0.0;
            double real = 0.0;
            double imag = 0.0;

            for (size_t i = 0; i < n; ++i) {
                const double s = signal[i];
                real += s * phasor_cos;
                imag -= s * phasor_sin;
                const double next_cos = phasor_cos * step_cos - phasor_sin * step_sin;
                phasor_sin = phasor_cos * step_sin + phasor_sin * step_cos;
                phasor_cos = next_cos;
            }

            const double power =
                (real * real + imag * imag) / (static_cast<double>(n) * sample_rate);
            psd[k - 1].second += static_cast<float>(power);
        }
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[fft]"`
Expected: all pass, including the pre-existing cases that were green in Step 1.

- [ ] **Step 7: Mutation-check the phasor**

Change `double phasor_cos = 1.0;` to `float phasor_cos = 1.0f;` and both accumulators to
`float`, then rebuild and run `[phasor]`. The point is to confirm the test is sensitive
to precision at all. If it still passes, note that in the commit message - the test is
weaker than intended and the double accumulators are then a belt-and-braces choice rather
than a proven requirement.

Restore `double`, then `touch src/calibration/belt_tension_types.cpp` so make rebuilds -
restoring content alone can leave make with nothing to do, and you will re-run the
mutated binary and report a false pass.

- [ ] **Step 8: Commit**

```bash
git add include/belt_tension_types.h src/calibration/belt_tension_types.cpp \
        tests/unit/test_belt_tension_calibrator.cpp
git commit -m "perf(belt): phasor DFT and configurable bandwidth in compute_psd"
```

---

### Task 2: PitchEstimator

Harmonic product spectrum. The shipping analysis takes the largest PSD bin, which returns
the 2nd harmonic whenever it dominates - on the reference A belt that is every single
pluck, producing a confident answer exactly one octave sharp.

**Files:**
- Create: `include/pitch_estimator.h`
- Create: `src/calibration/pitch_estimator.cpp`
- Test: `tests/unit/test_pitch_estimator.cpp`

**Interfaces:**
- Consumes: `compute_psd()`, `parse_accel_csv()`, `AccelSample` from `include/belt_tension_types.h`
- Produces:
  - `float helix::calibration::expected_frequency_for_span(float span_mm)`
  - `bool helix::calibration::search_window_for_span(float span_mm, float* lo_hz, float* hi_hz)`
  - `struct helix::calibration::PitchEstimate { float frequency_hz; bool valid; }`
  - `PitchEstimate helix::calibration::estimate_pitch(const std::vector<std::pair<float,float>>& psd, float search_lo_hz, float search_hi_hz, int n_harmonics = 4)`
  - `float helix::calibration::required_bandwidth_hz(float search_hi_hz, int n_harmonics = 4)`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_pitch_estimator.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pitch_estimator.cpp
 * @brief Harmonic-aware fundamental estimation for belt plucks
 *
 * Ground truth comes from real Voron 2.4 captures in tests/fixtures/belt_plucks/.
 * The critical case is a signal whose 2nd harmonic is stronger than its
 * fundamental: peak-picking returns 2*f0, which is the bug this replaces.
 */

#include "../../include/belt_tension_types.h"
#include "../../include/pitch_estimator.h"

#include "../catch_amalgamated.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace helix::calibration;

namespace {

/// Fixtures are Klipper-format CSV; tests run from the repository root.
std::vector<AccelSample> load_fixture(const std::string& name) {
    std::ifstream in("tests/fixtures/belt_plucks/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_accel_csv(ss.str());
}

float fixture_sample_rate(const std::vector<AccelSample>& s) {
    REQUIRE(s.size() > 1);
    const float span = s.back().time - s.front().time;
    REQUIRE(span > 0.0f);
    return static_cast<float>(s.size() - 1) / span;
}

/// Synthetic pluck with the harmonic profile measured on a real A belt:
/// h1 -3 dB, h2 0 dB, h3 -14 dB, h4 -13 dB. The 2nd harmonic dominates the
/// fundamental, which is what defeats peak-picking. h3 and h4 must be present
/// or the harmonic product is degenerate and the test proves nothing.
std::vector<AccelSample> make_harmonic_heavy(float f0, float sample_rate, int count) {
    static const float amps[4] = {0.708f, 1.0f, 0.200f, 0.224f};
    std::vector<AccelSample> out(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / sample_rate;
        float v = 0.0f;
        for (int h = 0; h < 4; ++h) {
            v += amps[h] * std::sin(2.0f * static_cast<float>(M_PI) * f0 *
                                    static_cast<float>(h + 1) * t);
        }
        // Deterministic low-level broadband content so the upper harmonics of a
        // wrong candidate are not exactly zero.
        v += 0.01f * std::sin(static_cast<float>(i) * 12.9898f);
        out[static_cast<size_t>(i)].time = t;
        out[static_cast<size_t>(i)].x = v;
        out[static_cast<size_t>(i)].y = 0.0f;
        out[static_cast<size_t>(i)].z = 9810.0f; // gravity, must be removed as DC
    }
    return out;
}

} // namespace

TEST_CASE("expected_frequency_for_span follows the Voron 110Hz@150mm reference",
          "[belt_tension][pitch]") {
    CHECK(expected_frequency_for_span(150.0f) == Catch::Approx(110.0f).margin(0.01f));
    CHECK(expected_frequency_for_span(300.0f) == Catch::Approx(55.0f).margin(0.01f));
    CHECK(expected_frequency_for_span(75.0f) == Catch::Approx(220.0f).margin(0.01f));
}

TEST_CASE("expected_frequency_for_span rejects nonsense spans", "[belt_tension][pitch]") {
    CHECK(expected_frequency_for_span(0.0f) == 0.0f);
    CHECK(expected_frequency_for_span(-10.0f) == 0.0f);
}

TEST_CASE("search_window_for_span brackets the expected fundamental",
          "[belt_tension][pitch]") {
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(150.0f, &lo, &hi));
    CHECK(lo == Catch::Approx(77.0f).margin(0.5f));
    CHECK(hi == Catch::Approx(165.0f).margin(0.5f));

    // The floor MUST sit above half the expected fundamental, or the harmonic
    // product locks onto f0/2 instead of f0.
    CHECK(lo > expected_frequency_for_span(150.0f) * 0.5f);
}

TEST_CASE("search_window_for_span rejects nonsense spans", "[belt_tension][pitch]") {
    float lo = 0.0f, hi = 0.0f;
    CHECK_FALSE(search_window_for_span(0.0f, &lo, &hi));
    CHECK_FALSE(search_window_for_span(150.0f, nullptr, &hi));
}

TEST_CASE("estimate_pitch returns the fundamental when the 2nd harmonic dominates",
          "[belt_tension][pitch]") {
    const float sr = 3200.0f;
    const float search_lo = 63.0f, search_hi = 135.0f;
    auto samples = make_harmonic_heavy(90.0f, sr, 1600);
    // Bandwidth must cover 4*f0 or no candidate has a complete harmonic series.
    auto psd = compute_psd(samples, sr, required_bandwidth_hz(search_hi));
    REQUIRE(!psd.empty());

    // Peak-picking is wrong here by construction - it finds 180 Hz.
    auto naive = find_peak_frequency(psd, 20.0f, 300.0f);
    REQUIRE(naive.found);
    CHECK(naive.frequency == Catch::Approx(180.0f).margin(4.0f));

    auto est = estimate_pitch(psd, search_lo, search_hi);
    REQUIRE(est.valid);
    CHECK(est.frequency_hz == Catch::Approx(90.0f).margin(4.0f));
}

TEST_CASE("estimate_pitch recovers 86 Hz from real A-belt captures",
          "[belt_tension][pitch][golden]") {
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(151.0f, &lo, &hi));

    for (const auto* name : {"a_belt_86hz_1.csv", "a_belt_86hz_2.csv", "a_belt_86hz_3.csv"}) {
        auto samples = load_fixture(name);
        REQUIRE(samples.size() > 1000);
        auto psd = compute_psd(samples, fixture_sample_rate(samples),
                               required_bandwidth_hz(hi));
        auto est = estimate_pitch(psd, lo, hi);
        INFO("fixture " << name);
        REQUIRE(est.valid);
        CHECK(est.frequency_hz == Catch::Approx(86.0f).margin(2.0f));
    }
}

TEST_CASE("estimate_pitch recovers 82 Hz from real B-belt captures",
          "[belt_tension][pitch][golden]") {
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(151.0f, &lo, &hi));

    for (const auto* name : {"b_belt_82hz_1.csv", "b_belt_82hz_2.csv", "b_belt_82hz_3.csv",
                             "b_belt_82hz_hard_case.csv"}) {
        auto samples = load_fixture(name);
        REQUIRE(samples.size() > 1000);
        auto psd = compute_psd(samples, fixture_sample_rate(samples),
                               required_bandwidth_hz(hi));
        auto est = estimate_pitch(psd, lo, hi);
        INFO("fixture " << name);
        REQUIRE(est.valid);
        CHECK(est.frequency_hz == Catch::Approx(82.0f).margin(2.0f));
    }
}

TEST_CASE("required_bandwidth_hz covers the whole harmonic series",
          "[belt_tension][pitch]") {
    // A 151mm span searches up to ~164 Hz; four harmonics of that is ~656 Hz.
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(151.0f, &lo, &hi));
    const float bw = required_bandwidth_hz(hi);
    CHECK(bw > hi * 4.0f);
    CHECK(bw > 600.0f);
    // The shipping default of 250 Hz is nowhere near enough - that is the bug.
    CHECK(bw > 250.0f);

    CHECK(required_bandwidth_hz(0.0f) == 0.0f);
    CHECK(required_bandwidth_hz(160.0f, 0) == 0.0f);
}

TEST_CASE("estimate_pitch rejects degenerate input", "[belt_tension][pitch][edge_case]") {
    std::vector<std::pair<float, float>> empty;
    CHECK_FALSE(estimate_pitch(empty, 70.0f, 170.0f).valid);

    auto samples = load_fixture("a_belt_86hz_1.csv");
    auto psd = compute_psd(samples, fixture_sample_rate(samples), 700.0f);
    CHECK_FALSE(estimate_pitch(psd, 170.0f, 70.0f).valid); // inverted window
    CHECK_FALSE(estimate_pitch(psd, 70.0f, 170.0f, 0).valid); // no harmonics
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: compile error, `pitch_estimator.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `include/pitch_estimator.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

/**
 * @file pitch_estimator.h
 * @brief Harmonic-aware fundamental estimation for plucked belts
 *
 * A plucked belt produces a harmonic series. Which harmonic carries the most
 * energy varies with belt path and geometry, so taking the largest PSD bin
 * returns 2*f0 whenever the 2nd harmonic dominates. Measured on a Voron 2.4,
 * that was every pluck on one belt and no pluck on the other, from the same
 * printer minutes apart.
 *
 * The harmonic product spectrum multiplies the spectrum by its own decimations,
 * which reinforces a true fundamental and suppresses a lone harmonic. It is
 * sensitive to its search range: extend the floor below f0/2 and it locks onto
 * the subharmonic instead. Derive the range from span length via
 * search_window_for_span().
 */

namespace helix::calibration {

/// Voron reference point: a 150 mm span at correct tension rings at 110 Hz.
inline constexpr float REFERENCE_SPAN_MM = 150.0f;
inline constexpr float REFERENCE_FREQUENCY_HZ = 110.0f;

/// Search window as a fraction of the expected fundamental. Validated against
/// every capture in tests/fixtures/belt_plucks/; widening the floor below
/// ~0.65 reintroduces subharmonic lock on the weakest captures.
inline constexpr float SEARCH_WINDOW_LO_FRACTION = 0.70f;
inline constexpr float SEARCH_WINDOW_HI_FRACTION = 1.50f;

struct PitchEstimate {
    float frequency_hz = 0.0f;
    bool valid = false;
};

/**
 * @brief Expected fundamental for a belt span at reference tension
 * @param span_mm Free span length in mm
 * @return Expected Hz, or 0.0f if span_mm <= 0
 */
float expected_frequency_for_span(float span_mm);

/**
 * @brief Search window for the fundamental, derived from span length
 * @param span_mm Free span length in mm
 * @param lo_hz Out: lower bound
 * @param hi_hz Out: upper bound
 * @return false if span_mm <= 0 or either pointer is null
 */
bool search_window_for_span(float span_mm, float* lo_hz, float* hi_hz);

/**
 * @brief Bandwidth compute_psd must cover for a complete harmonic series
 *
 * estimate_pitch() skips any candidate whose harmonics fall outside the PSD
 * array. With the default 250 Hz cap that is every realistic belt frequency,
 * and the estimator returns nothing at all.
 *
 * @param search_hi_hz Top of the fundamental search window
 * @param n_harmonics Harmonics estimate_pitch() will multiply
 * @return Hz of bandwidth to request from compute_psd(), with 5% margin
 */
float required_bandwidth_hz(float search_hi_hz, int n_harmonics = 4);

/**
 * @brief Estimate the fundamental via harmonic product spectrum
 * @param psd Output of compute_psd(); bin i is at frequency (i+1)*resolution
 * @param search_lo_hz Lower bound of the fundamental search
 * @param search_hi_hz Upper bound of the fundamental search
 * @param n_harmonics Harmonics to multiply, including the fundamental
 * @return Estimate with valid=false if input is degenerate or nothing is in range
 */
PitchEstimate estimate_pitch(const std::vector<std::pair<float, float>>& psd,
                             float search_lo_hz, float search_hi_hz, int n_harmonics = 4);

} // namespace helix::calibration
```

- [ ] **Step 4: Write the implementation**

Create `src/calibration/pitch_estimator.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pitch_estimator.h"

#include <spdlog/spdlog.h>

namespace helix::calibration {

float expected_frequency_for_span(float span_mm) {
    if (span_mm <= 0.0f) {
        return 0.0f;
    }
    return REFERENCE_FREQUENCY_HZ * REFERENCE_SPAN_MM / span_mm;
}

bool search_window_for_span(float span_mm, float* lo_hz, float* hi_hz) {
    if (lo_hz == nullptr || hi_hz == nullptr) {
        return false;
    }
    const float expected = expected_frequency_for_span(span_mm);
    if (expected <= 0.0f) {
        return false;
    }
    *lo_hz = expected * SEARCH_WINDOW_LO_FRACTION;
    *hi_hz = expected * SEARCH_WINDOW_HI_FRACTION;
    return true;
}

float required_bandwidth_hz(float search_hi_hz, int n_harmonics) {
    if (search_hi_hz <= 0.0f || n_harmonics < 1) {
        return 0.0f;
    }
    return search_hi_hz * static_cast<float>(n_harmonics) * 1.05f;
}

PitchEstimate estimate_pitch(const std::vector<std::pair<float, float>>& psd,
                             float search_lo_hz, float search_hi_hz, int n_harmonics) {
    PitchEstimate out;
    if (psd.size() < 4 || search_hi_hz <= search_lo_hz || n_harmonics < 1) {
        return out;
    }

    const size_t bins = psd.size();
    // compute_psd() emits bin i at frequency (i+1)*resolution - there is no DC
    // bin - so harmonic h of candidate index i lives at index h*(i+1)-1.
    double best_product = -1.0;
    for (size_t i = 0; i < bins; ++i) {
        const float freq = psd[i].first;
        if (freq < search_lo_hz || freq > search_hi_hz) {
            continue;
        }

        double product = 1.0; // double: four multiplied PSD bins underflow float
        bool series_complete = true;
        for (int h = 1; h <= n_harmonics; ++h) {
            const size_t harmonic_index = static_cast<size_t>(h) * (i + 1) - 1;
            if (harmonic_index >= bins) {
                series_complete = false;
                break;
            }
            product *= static_cast<double>(psd[harmonic_index].second);
        }
        if (!series_complete) {
            continue;
        }

        if (product > best_product) {
            best_product = product;
            out.frequency_hz = freq;
            out.valid = true;
        }
    }

    if (out.valid) {
        spdlog::debug("[PitchEstimator] f0={:.1f} Hz (window {:.1f}-{:.1f}, {} harmonics)",
                      out.frequency_hz, search_lo_hz, search_hi_hz, n_harmonics);
    }
    return out;
}

} // namespace helix::calibration
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[pitch]"`
Expected: all assertions pass, including the six golden fixtures

- [ ] **Step 6: Mutation-check the golden tests**

Temporarily change `SEARCH_WINDOW_LO_FRACTION` to `0.40f`, then:

Run: `make test && ./build/bin/helix-tests "[golden]"`
Expected: **FAIL** on `b_belt_82hz_hard_case.csv` - that fixture is in the suite
specifically because a low floor makes HPS return a subharmonic.

Restore `0.70f`, then `touch include/pitch_estimator.h` so make actually rebuilds
(restoring a file by content alone can leave make with nothing to do and you will
re-run the mutated binary and report a false pass).

Run: `make test && ./build/bin/helix-tests "[golden]"`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add include/pitch_estimator.h src/calibration/pitch_estimator.cpp \
        tests/unit/test_pitch_estimator.cpp tests/fixtures/belt_plucks
git commit -m "feat(belt): harmonic-aware pitch estimation for plucked belts"
```

---

### Task 3: PluckDetector

The strength gate. Measured on 60 real captures: below 5x the noise floor nothing was a
pluck at all, and above 9x the estimator is right 95% of the time. Gating matters more
than averaging - ungated, the median never exceeded 48% accuracy at any sample count.

**Files:**
- Create: `include/pluck_detector.h`
- Create: `src/calibration/pluck_detector.cpp`
- Test: `tests/unit/test_pluck_detector.cpp`

**Interfaces:**
- Consumes: `AccelSample` from `include/belt_tension_types.h`
- Produces:
  - `struct helix::calibration::PluckWindow { std::vector<AccelSample> samples; float rms_ratio; }`
  - `class helix::calibration::PluckDetector` with `window_rms`, `set_noise_floor`,
    `noise_floor`, `learn_noise_floor`, `rms_ratio`, `passes_gate`, `extract_ringdown`
  - `PluckDetector::MIN_RMS_RATIO`, `::SKIP_MS`, `::ANALYZE_MS`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_pluck_detector.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pluck_detector.cpp
 * @brief Noise floor, strength gating, and ring-down extraction
 */

#include "../../include/belt_tension_types.h"
#include "../../include/pluck_detector.h"

#include "../catch_amalgamated.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace helix::calibration;

namespace {

std::vector<AccelSample> load_fixture(const std::string& name) {
    std::ifstream in("tests/fixtures/belt_plucks/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_accel_csv(ss.str());
}

/// Flat noise at a chosen amplitude, plus a constant gravity offset on Z that
/// must be removed as DC or every RMS reading is wrong.
std::vector<AccelSample> make_noise(float amplitude, int count) {
    std::vector<AccelSample> out(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float phase = static_cast<float>(i);
        out[static_cast<size_t>(i)].time = static_cast<float>(i) / 3200.0f;
        out[static_cast<size_t>(i)].x = amplitude * std::sin(phase);
        out[static_cast<size_t>(i)].y = amplitude * std::cos(phase * 1.7f);
        out[static_cast<size_t>(i)].z = 9810.0f + amplitude * std::sin(phase * 0.3f);
    }
    return out;
}

/// Quiet run, then a decaying burst starting at `onset`.
std::vector<AccelSample> make_transient(int count, int onset, float quiet, float burst) {
    auto out = make_noise(quiet, count);
    for (int i = onset; i < count; ++i) {
        const float decay = std::exp(-static_cast<float>(i - onset) / 400.0f);
        const float t = static_cast<float>(i) / 3200.0f;
        out[static_cast<size_t>(i)].x +=
            burst * decay * std::sin(2.0f * static_cast<float>(M_PI) * 86.0f * t);
    }
    return out;
}

} // namespace

TEST_CASE("window_rms removes DC before measuring", "[belt_tension][pluck_detect]") {
    auto quiet = make_noise(10.0f, 512);
    const float rms = PluckDetector::window_rms(quiet.data(), quiet.size());
    // Gravity is 9810 on Z; if DC were not removed this would be ~9810.
    CHECK(rms < 100.0f);
    CHECK(rms > 0.0f);
}

TEST_CASE("window_rms handles degenerate input", "[belt_tension][pluck_detect][edge_case]") {
    CHECK(PluckDetector::window_rms(nullptr, 0) == 0.0f);
    std::vector<AccelSample> one(1);
    CHECK(PluckDetector::window_rms(one.data(), 0) == 0.0f);
}

TEST_CASE("learn_noise_floor establishes a positive baseline",
          "[belt_tension][pluck_detect]") {
    PluckDetector det;
    auto quiet = make_noise(10.0f, 2048);
    REQUIRE(det.learn_noise_floor(quiet));
    CHECK(det.noise_floor() > 0.0f);
    CHECK(det.noise_floor() == Catch::Approx(PluckDetector::window_rms(quiet.data(),
                                                                       quiet.size()))
                                   .epsilon(0.01));
}

TEST_CASE("learn_noise_floor rejects an empty buffer",
          "[belt_tension][pluck_detect][edge_case]") {
    PluckDetector det;
    std::vector<AccelSample> empty;
    CHECK_FALSE(det.learn_noise_floor(empty));
}

TEST_CASE("gate rejects a weak transient and accepts a firm one",
          "[belt_tension][pluck_detect]") {
    PluckDetector det;
    det.set_noise_floor(10.0f);

    auto weak = make_noise(30.0f, 2048);   // ~3x floor
    auto firm = make_noise(200.0f, 2048);  // well past 9x

    CHECK_FALSE(det.passes_gate(weak.data(), weak.size()));
    CHECK(det.passes_gate(firm.data(), firm.size()));
}

TEST_CASE("gate threshold is exactly MIN_RMS_RATIO", "[belt_tension][pluck_detect]") {
    PluckDetector det;
    auto probe = make_noise(100.0f, 2048);
    const float rms = PluckDetector::window_rms(probe.data(), probe.size());

    det.set_noise_floor(rms / (PluckDetector::MIN_RMS_RATIO * 1.05f));
    CHECK(det.passes_gate(probe.data(), probe.size()));

    det.set_noise_floor(rms / (PluckDetector::MIN_RMS_RATIO * 0.95f));
    CHECK_FALSE(det.passes_gate(probe.data(), probe.size()));
}

TEST_CASE("gate rejects the real weak-pluck capture",
          "[belt_tension][pluck_detect][golden]") {
    // Captured at 1.4x the noise floor. Analysing it yields 112 Hz, which is
    // meaningless - the gate is what stops that number reaching a user.
    auto weak = load_fixture("weak_pluck_reject.csv");
    REQUIRE(weak.size() > 1000);

    PluckDetector det;
    det.set_noise_floor(265.0f); // the floor measured during that session
    CHECK_FALSE(det.passes_gate(weak.data(), weak.size()));
}

TEST_CASE("gate accepts the real firm captures", "[belt_tension][pluck_detect][golden]") {
    PluckDetector det;
    det.set_noise_floor(265.0f);
    for (const auto* name : {"a_belt_86hz_1.csv", "b_belt_82hz_1.csv"}) {
        auto s = load_fixture(name);
        INFO("fixture " << name);
        CHECK(det.passes_gate(s.data(), s.size()));
    }
}

TEST_CASE("extract_ringdown skips the impact spike", "[belt_tension][pluck_detect]") {
    const float sr = 3200.0f;
    const int onset = 1000;
    auto buffer = make_transient(6000, onset, 5.0f, 500.0f);

    PluckWindow win;
    REQUIRE(PluckDetector::extract_ringdown(buffer, sr, &win));

    const size_t expected_len = static_cast<size_t>(sr * PluckDetector::ANALYZE_MS / 1000.0f);
    CHECK(win.samples.size() == expected_len);

    // The window must start after the onset, not before it.
    const float skip_s = PluckDetector::SKIP_MS / 1000.0f;
    CHECK(win.samples.front().time >= buffer[static_cast<size_t>(onset)].time + skip_s * 0.9f);
}

TEST_CASE("extract_ringdown fails on a buffer that is too short",
          "[belt_tension][pluck_detect][edge_case]") {
    auto tiny = make_noise(10.0f, 64);
    PluckWindow win;
    CHECK_FALSE(PluckDetector::extract_ringdown(tiny, 3200.0f, &win));
    CHECK_FALSE(PluckDetector::extract_ringdown(tiny, 3200.0f, nullptr));
    CHECK_FALSE(PluckDetector::extract_ringdown(tiny, 0.0f, &win));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: compile error, `pluck_detector.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `include/pluck_detector.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <cstddef>
#include <vector>

/**
 * @file pluck_detector.h
 * @brief Onset detection and strength gating for belt plucks
 *
 * Thresholds here are measured, not chosen. Across 60 real captures on a
 * Voron 2.4: below 5x the noise floor nothing was a pluck at all, between
 * 5-9x pitch estimation was right 64% of the time, and above 9x it was right
 * 95% of the time. Rejecting weak strikes contributes more accuracy than
 * averaging does - ungated, a median never exceeded 48% at any sample count.
 */

namespace helix::calibration {

/// A ring-down segment ready for spectral analysis.
struct PluckWindow {
    std::vector<AccelSample> samples;
    float rms_ratio = 0.0f;
};

class PluckDetector {
public:
    /// Minimum strength, as a multiple of the noise floor, for a strike to count.
    static constexpr float MIN_RMS_RATIO = 9.0f;
    /// Skip past the impact transient before analysing.
    static constexpr float SKIP_MS = 40.0f;
    /// Length of ring-down to analyse.
    static constexpr float ANALYZE_MS = 500.0f;

    /// Broadband RMS with per-axis DC removed. Static so callers can measure a
    /// buffer without owning a detector.
    static float window_rms(const AccelSample* samples, size_t count);

    void set_noise_floor(float rms) {
        noise_floor_ = rms;
    }
    [[nodiscard]] float noise_floor() const {
        return noise_floor_;
    }

    /// Measure the floor from a buffer captured while the machine is still.
    bool learn_noise_floor(const std::vector<AccelSample>& quiet);

    /// Strength of a window as a multiple of the noise floor. 0 if no floor set.
    [[nodiscard]] float rms_ratio(const AccelSample* samples, size_t count) const;

    /// True if this window is strong enough to analyse.
    [[nodiscard]] bool passes_gate(const AccelSample* samples, size_t count) const;

    /// Locate the strongest transient in `buffer` and extract the ring-down
    /// beginning SKIP_MS after it.
    static bool extract_ringdown(const std::vector<AccelSample>& buffer, float sample_rate,
                                 PluckWindow* out);

private:
    float noise_floor_ = 0.0f;
};

} // namespace helix::calibration
```

- [ ] **Step 4: Write the implementation**

Create `src/calibration/pluck_detector.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pluck_detector.h"

#include <spdlog/spdlog.h>

#include <cmath>

namespace helix::calibration {

float PluckDetector::window_rms(const AccelSample* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0f;
    }

    double mean_x = 0.0, mean_y = 0.0, mean_z = 0.0;
    for (size_t i = 0; i < count; ++i) {
        mean_x += samples[i].x;
        mean_y += samples[i].y;
        mean_z += samples[i].z;
    }
    const double n = static_cast<double>(count);
    mean_x /= n;
    mean_y /= n;
    mean_z /= n;

    double accum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double dx = samples[i].x - mean_x;
        const double dy = samples[i].y - mean_y;
        const double dz = samples[i].z - mean_z;
        accum += dx * dx + dy * dy + dz * dz;
    }
    return static_cast<float>(std::sqrt(accum / n));
}

bool PluckDetector::learn_noise_floor(const std::vector<AccelSample>& quiet) {
    if (quiet.empty()) {
        return false;
    }
    noise_floor_ = window_rms(quiet.data(), quiet.size());
    spdlog::debug("[PluckDetector] noise floor {:.2f}, gate at {:.2f}", noise_floor_,
                  noise_floor_ * MIN_RMS_RATIO);
    return noise_floor_ > 0.0f;
}

float PluckDetector::rms_ratio(const AccelSample* samples, size_t count) const {
    if (noise_floor_ <= 0.0f) {
        return 0.0f;
    }
    return window_rms(samples, count) / noise_floor_;
}

bool PluckDetector::passes_gate(const AccelSample* samples, size_t count) const {
    return rms_ratio(samples, count) >= MIN_RMS_RATIO;
}

bool PluckDetector::extract_ringdown(const std::vector<AccelSample>& buffer, float sample_rate,
                                     PluckWindow* out) {
    if (out == nullptr || sample_rate <= 0.0f || buffer.size() < 256) {
        return false;
    }

    const size_t analyze_len = static_cast<size_t>(sample_rate * ANALYZE_MS / 1000.0f);
    const size_t skip_len = static_cast<size_t>(sample_rate * SKIP_MS / 1000.0f);
    if (analyze_len == 0 || buffer.size() < analyze_len) {
        return false;
    }

    // Onset is the largest instantaneous deviation from the buffer mean.
    double mean_x = 0.0, mean_y = 0.0, mean_z = 0.0;
    for (const auto& s : buffer) {
        mean_x += s.x;
        mean_y += s.y;
        mean_z += s.z;
    }
    const double n = static_cast<double>(buffer.size());
    mean_x /= n;
    mean_y /= n;
    mean_z /= n;

    size_t onset = 0;
    double peak = -1.0;
    for (size_t i = 0; i < buffer.size(); ++i) {
        const double dx = buffer[i].x - mean_x;
        const double dy = buffer[i].y - mean_y;
        const double dz = buffer[i].z - mean_z;
        const double mag = dx * dx + dy * dy + dz * dz;
        if (mag > peak) {
            peak = mag;
            onset = i;
        }
    }

    size_t begin = onset + skip_len;
    if (begin + analyze_len > buffer.size()) {
        // Not enough ring-down after the onset; take the tail instead of
        // silently analysing pre-pluck silence.
        if (buffer.size() < analyze_len) {
            return false;
        }
        begin = buffer.size() - analyze_len;
    }

    out->samples.assign(buffer.begin() + static_cast<std::ptrdiff_t>(begin),
                        buffer.begin() + static_cast<std::ptrdiff_t>(begin + analyze_len));
    out->rms_ratio = 0.0f;
    return true;
}

} // namespace helix::calibration
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[pluck_detect]"`
Expected: all pass

- [ ] **Step 6: Commit**

```bash
git add include/pluck_detector.h src/calibration/pluck_detector.cpp \
        tests/unit/test_pluck_detector.cpp
git commit -m "feat(belt): strength-gated pluck detection and ring-down extraction"
```

---

### Task 4: PluckAggregator

Commit a number after five accepted plucks, but keep accepting them afterwards so the
user can watch the reading hold steady.

**Files:**
- Create: `include/pluck_aggregator.h`
- Create: `src/calibration/pluck_aggregator.cpp`
- Test: `tests/unit/test_pluck_aggregator.cpp`

**Interfaces:**
- Consumes: nothing
- Produces: `class helix::calibration::PluckAggregator` with `add(float)`, `reset()`,
  `count()`, `committed()`, `median()`, and `PluckAggregator::COMMIT_AFTER`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_pluck_aggregator.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pluck_aggregator.cpp
 * @brief Running median across plucks, with a commit threshold
 */

#include "../../include/pluck_aggregator.h"

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

TEST_CASE("aggregator starts empty and uncommitted", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    CHECK(agg.count() == 0);
    CHECK_FALSE(agg.committed());
    CHECK(agg.median() == 0.0f);
}

TEST_CASE("aggregator does not commit before the threshold", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    for (size_t i = 1; i < PluckAggregator::COMMIT_AFTER; ++i) {
        agg.add(86.0f);
        INFO("after " << i << " plucks");
        CHECK(agg.count() == i);
        CHECK_FALSE(agg.committed());
    }
    agg.add(86.0f);
    CHECK(agg.count() == PluckAggregator::COMMIT_AFTER);
    CHECK(agg.committed());
}

TEST_CASE("median ignores a single outlier", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    agg.add(86.0f);
    agg.add(86.0f);
    agg.add(56.0f); // the kind of miss a single HPS pass produces
    agg.add(86.0f);
    agg.add(86.0f);
    REQUIRE(agg.committed());
    CHECK(agg.median() == Catch::Approx(86.0f));
}

TEST_CASE("median of an even count averages the middle pair",
          "[belt_tension][aggregate]") {
    PluckAggregator agg;
    agg.add(82.0f);
    agg.add(86.0f);
    CHECK(agg.median() == Catch::Approx(84.0f));
}

TEST_CASE("aggregator keeps accepting past the commit threshold",
          "[belt_tension][aggregate]") {
    PluckAggregator agg;
    for (int i = 0; i < 12; ++i) {
        agg.add(86.0f);
    }
    CHECK(agg.count() == 12);
    CHECK(agg.committed());
    CHECK(agg.median() == Catch::Approx(86.0f));
}

TEST_CASE("reset clears everything", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    for (int i = 0; i < 6; ++i) {
        agg.add(86.0f);
    }
    agg.reset();
    CHECK(agg.count() == 0);
    CHECK_FALSE(agg.committed());
    CHECK(agg.median() == 0.0f);
}

TEST_CASE("aggregator ignores non-positive frequencies",
          "[belt_tension][aggregate][edge_case]") {
    PluckAggregator agg;
    agg.add(0.0f);
    agg.add(-5.0f);
    CHECK(agg.count() == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: compile error, `pluck_aggregator.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `include/pluck_aggregator.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <vector>

/**
 * @file pluck_aggregator.h
 * @brief Running median of per-pluck frequency estimates
 *
 * A single gated pluck is right about 95% of the time; a median of five is
 * effectively always right. The aggregator keeps collecting past the commit
 * threshold so a user can keep plucking and watch the value hold steady.
 */

namespace helix::calibration {

class PluckAggregator {
public:
    /// Accepted plucks required before the median is trustworthy enough to show
    /// as a committed result.
    static constexpr size_t COMMIT_AFTER = 5;

    /// Record an estimate. Non-positive values are ignored.
    void add(float frequency_hz);

    void reset();

    [[nodiscard]] size_t count() const {
        return samples_.size();
    }
    [[nodiscard]] bool committed() const {
        return samples_.size() >= COMMIT_AFTER;
    }

    /// Median of everything recorded, or 0 if nothing has been.
    [[nodiscard]] float median() const;

private:
    std::vector<float> samples_;
};

} // namespace helix::calibration
```

- [ ] **Step 4: Write the implementation**

Create `src/calibration/pluck_aggregator.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pluck_aggregator.h"

#include <algorithm>

namespace helix::calibration {

void PluckAggregator::add(float frequency_hz) {
    if (frequency_hz <= 0.0f) {
        return;
    }
    samples_.push_back(frequency_hz);
}

void PluckAggregator::reset() {
    samples_.clear();
}

float PluckAggregator::median() const {
    if (samples_.empty()) {
        return 0.0f;
    }
    std::vector<float> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 1) {
        return sorted[mid];
    }
    return 0.5f * (sorted[mid - 1] + sorted[mid]);
}

} // namespace helix::calibration
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[aggregate]"`
Expected: all pass

- [ ] **Step 6: Commit**

```bash
git add include/pluck_aggregator.h src/calibration/pluck_aggregator.cpp \
        tests/unit/test_pluck_aggregator.cpp
git commit -m "feat(belt): running-median aggregation across plucks"
```

---

### Task 5: Delete the strobe path

Strobe was a placeholder that never locked anything: both handlers logged and raised a
toast. The live meter supersedes it, and the reference Voron has only neopixels, which
cannot be driven at belt frequencies.

**Files:**
- Modify: `include/ui_panel_belt_tension.h` (remove `STROBE` state, `handle_lock_a_clicked`, `handle_lock_b_clicked`, strobe members)
- Modify: `src/ui/ui_panel_belt_tension.cpp` (remove the handlers at ~534-542, their callback registrations at ~116-119, and all `STROBE` state handling)
- Modify: `include/belt_tension_calibrator.h` (remove `start_strobe`, `set_strobe_frequency`, `STROBE_MODE`, `strobe_frequency_`)
- Modify: `src/calibration/belt_tension_calibrator.cpp:~420-470` (remove both method bodies)
- Modify: `include/moonraker_advanced_api.h:512` (remove the virtual `set_strobe_frequency`)
- Modify: `src/api/moonraker_advanced_api.cpp:2372` (remove the implementation)
- Modify: `ui_xml/panel_belt_tension.xml` (remove the `state_strobe` block, ~lines 171-231, and the `btn_strobe` button at ~161)
- Modify: `tests/unit/test_belt_tension_calibrator.cpp` (remove strobe test cases)
- Check: any mock implementing `IMoonrakerAdvancedAPI` must drop the override too, or the `[compile][drift]` tests fail

**Interfaces:**
- Consumes: nothing
- Produces: nothing. This is pure removal.

- [ ] **Step 1: Enumerate every reference before deleting**

Run:
```bash
grep -rn "strobe\|Strobe\|STROBE" src/ include/ tests/ ui_xml/ --include=*.cpp --include=*.h --include=*.xml
```
Expected: roughly 127 hits across the files listed above. Work through them file by file.
Note any hit in a file **not** listed above and handle it too - the list was built from a
snapshot and mocks in particular are easy to miss.

- [ ] **Step 2: Remove the API layer first**

Delete `set_strobe_frequency` from `include/moonraker_advanced_api.h` and
`src/api/moonraker_advanced_api.cpp`, plus any mock override.

Run: `make test 2>&1 | tail -30`
Expected: compile errors from `belt_tension_calibrator.cpp`, which still calls it. That
is the compiler enumerating your remaining work.

- [ ] **Step 3: Remove the calibrator layer**

Delete `start_strobe`, `set_strobe_frequency`, `State::STROBE_MODE`, and
`strobe_frequency_` from `include/belt_tension_calibrator.h` and
`src/calibration/belt_tension_calibrator.cpp`.

Also remove `has_pwm_led` and `pwm_led_pin` from `BeltTensionHardware` in
`include/belt_tension_types.h` **only if** this grep comes back empty after the above:
```bash
grep -rn "has_pwm_led\|pwm_led_pin" src/ include/ tests/
```

- [ ] **Step 4: Remove the panel and XML layers**

Delete from `include/ui_panel_belt_tension.h` and `src/ui/ui_panel_belt_tension.cpp`:
`handle_lock_a_clicked`, `handle_lock_b_clicked`, their entries in the callback
registration table, the `STROBE` enumerator, and any `update_strobe_display` code.

Delete from `ui_xml/panel_belt_tension.xml`: the `<lv_obj name="state_strobe">` block and
the `btn_strobe` button that navigates to it.

- [ ] **Step 5: Remove strobe tests**

Delete the strobe test cases from `tests/unit/test_belt_tension_calibrator.cpp`.

- [ ] **Step 6: Verify the tree is clean and builds**

Run:
```bash
grep -rn "strobe\|Strobe\|STROBE" src/ include/ tests/ ui_xml/ --include=*.cpp --include=*.h --include=*.xml
```
Expected: no output.

Run: `make -j 2>&1 | tail -5 && make test && ./build/bin/helix-tests "[belt_tension]"`
Expected: clean build, all belt tension tests pass.

- [ ] **Step 7: Verify the panel still renders**

XML loads at runtime, so a malformed edit will not show up in the build. Launch and drive
to the panel, pinning the socket and config dir so this cannot collide with another
session:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate advanced
```
Then check the log for XML errors:
```bash
grep -iE "No subject was found|failed to (load|parse)|unknown (widget|attribute)" /tmp/helix-$TREE.log
```
Expected: no output. Kill the instance afterwards:
```bash
pkill -f "remote-socket $HELIX_SOCK"; pgrep -xl helix-screen
```

- [ ] **Step 8: Commit**

```bash
git add include/ui_panel_belt_tension.h src/ui/ui_panel_belt_tension.cpp \
        include/belt_tension_calibrator.h src/calibration/belt_tension_calibrator.cpp \
        include/belt_tension_types.h include/moonraker_advanced_api.h \
        src/api/moonraker_advanced_api.cpp ui_xml/panel_belt_tension.xml \
        tests/unit/test_belt_tension_calibrator.cpp
git commit -m "refactor(belt): remove the strobe placeholder"
```

---

### Task 6: Delete the Z-belt path

`start_z_belt_listening()` is implemented, unit-tested, and unreachable - nothing in the
panel or XML calls it. Measurement on real hardware showed why it should stay that way:
four plucks of one Z belt returned 228, 164, 92 and 58 Hz, against five identical
readings on an A/B belt. A toolhead-mounted accelerometer cannot hear the Z belts through
the gantry, and `TEST_RESONANCES` cannot sweep Z either. Wiring this up would present
four corners of authoritative-looking noise.

**Files:**
- Modify: `include/belt_tension_calibrator.h:156` (remove `start_z_belt_listening`)
- Modify: `src/calibration/belt_tension_calibrator.cpp:505-610` (remove the body)
- Modify: `include/belt_tension_types.h` (remove `ZBeltCorner`, `ZBeltMeasurement`,
  `BeltTensionResult::z_belts`, `has_z_results()`, and `BeltTensionHardware::has_belted_z`)
- Modify: `src/calibration/belt_tension_types.cpp` (remove any `has_belted_z` detection)
- Modify: `tests/unit/test_belt_tension_calibrator.cpp:491-560` (remove the `[z_belt]` cases)

**Interfaces:**
- Consumes: nothing
- Produces: nothing. Pure removal.

- [ ] **Step 1: Enumerate every reference**

Run:
```bash
grep -rn "z_belt\|ZBelt\|has_belted_z\|z_belts\|has_z_results" src/ include/ tests/ ui_xml/
```
Expected: roughly 46 hits, confined to the files above.

- [ ] **Step 2: Record the finding in the header before removing the code**

Add to the file comment block of `include/belt_tension_types.h` so nobody re-adds it:

```cpp
 * Z belts are deliberately not supported. Measured on a Voron 2.4: four plucks
 * of one Z belt returned 228, 164, 92 and 58 Hz, versus five identical readings
 * on an A/B belt, with the signal 2-5x the noise floor against 12-14x. The
 * gantry decouples Z belt vibration from a toolhead-mounted accelerometer, and
 * Klipper's TEST_RESONANCES cannot sweep Z at all (resonance_tester.py
 * _parse_axis accepts only x, y, or a two-component XY vector). Supporting Z
 * needs a different sensor location, not different code.
```

- [ ] **Step 3: Remove the calibrator method**

Delete `start_z_belt_listening` from the header and its body from the source.

- [ ] **Step 4: Remove the types**

Delete `ZBeltCorner`, `ZBeltMeasurement`, `BeltTensionResult::z_belts`,
`BeltTensionResult::has_z_results()`, and `BeltTensionHardware::has_belted_z` plus its
detection code.

- [ ] **Step 5: Remove the tests**

Delete every `[z_belt]`-tagged case from `tests/unit/test_belt_tension_calibrator.cpp`,
and any `z_belts.push_back` usage in the result-construction tests.

- [ ] **Step 6: Verify**

Run:
```bash
grep -rn "z_belt\|ZBelt\|has_belted_z\|z_belts\|has_z_results" src/ include/ tests/ ui_xml/
```
Expected: no output except the explanatory comment added in Step 2.

Run: `make -j 2>&1 | tail -5 && make test && ./build/bin/helix-tests "[belt_tension]"`
Expected: clean build, all pass.

- [ ] **Step 7: Commit**

```bash
git add include/belt_tension_calibrator.h src/calibration/belt_tension_calibrator.cpp \
        include/belt_tension_types.h src/calibration/belt_tension_types.cpp \
        tests/unit/test_belt_tension_calibrator.cpp
git commit -m "refactor(belt): remove the unreachable Z-belt path"
```

---

## Done criteria

- `./build/bin/helix-tests "[belt_tension]"` green from the repository root
- `./build/bin/helix-tests "[golden]"` green - real Voron captures resolve to 86 Hz and 82 Hz
- `grep -rn "strobe\|z_belt" src/ include/ tests/ ui_xml/` returns only the explanatory comment
- `make -j` clean
- The belt tension panel still renders with no XML errors in the log

## What phase 2 covers

`BeltStreamClient` (Klipper UDS subscription on the libhv loop), the reworked panel
states, the pluck instruction animation, the capability probe, and the `screen_locality`
telemetry field. None of it is started here - this plan deliberately produces only
testable pure units plus the deletions, so it can land on its own.
