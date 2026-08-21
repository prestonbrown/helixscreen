# Home Widgets Batch 0: DRY Refactors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consolidate six duplicated or dead code paths that the seven planned home widgets would otherwise each have to copy, so the widget work extends single implementations instead of forking twins.

**Architecture:** Every task extracts an existing inline implementation into a named helper in an existing namespace, converts the current call sites to it, and deletes any dead twin. No new subsystems, no behavior change visible to the user except where a task explicitly fixes a bug (Task 5's unbounded pending-delta accumulation).

**Tech Stack:** C++17, LVGL 9.5 subjects, Catch2 (amalgamated), pure Makefile build, spdlog.

## Global Constraints

- **Branch:** `feature/home-widgets`, worktree `.worktrees/home-widgets`. Based on `fix/grid-cell-metrics`, NOT `main`.
- **Never `git add -A` or `git add .`** — the worktree has `lib/` symlinks that a bare add clobbers, and other sessions share this repo. Always stage explicit paths.
- **Never `git rm`** — it stages. Use plain `rm`.
- **spdlog only** for logging, never `printf`/`cout`/`LV_LOG_*`.
- **SPDX header** on every new file: `// SPDX-License-Identifier: GPL-3.0-or-later`
- **Formatting** is handled by the pre-commit hook (clang-format). Do not hand-format.
- **`make -j` builds only `helix-screen`; `make test` builds only `helix-tests`.** Building one does not rebuild the other — you will otherwise verify a stale binary.
- **Run `helix-tests` from the repo root** of this worktree. Any other cwd produces ~336 bogus UI failures and a SIGSEGV that reads like a real regression.
- **Never pipe `make test-run` through `tail`** — that reports the filter's exit code, so a red suite reads as green. Redirect to a file and grep.
- **Do not capture a build result with `grep -c error:`** — grep exits 1 on zero matches, so a clean build reports as a failed command. Capture `$?` from make separately.
- **This batch touches no `ui_xml` files**, so `make regen-tokens` is not required here. It becomes required in Batch 1.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `include/bed_coord_mapper.h` (new) | `helix::BedCoordMapper` — aspect-preserving mm→px with Y-flip and center-origin | 1 |
| `src/ui/bed_coord_mapper.cpp` (new) | its implementation, moved verbatim | 1 |
| `include/ui_exclude_object_map_view.h` | drops the nested `CoordMapper`, uses the shared one | 1 |
| `include/bed_dimensions.h` (new) | `helix::bed_dimensions(api)` — the build_volume → axis_bounds → 235x235 chain | 2 |
| `src/printer/bed_dimensions.cpp` (new) | its implementation | 2 |
| `include/tune_controller.h` (new) | `helix::tune::set_speed_percent` / `set_flow_percent` / `reset_both` | 3 |
| `src/ui/tune_controller.cpp` (new) | M220/M221 sends with the single agreed clamp | 3 |

`tune_controller.cpp` lives in `src/ui/`, not `src/printer/`, to match its direct
precedent: `TemperatureController` — the existing "single authority for sends" —
is `include/temperature_controller.h` + `src/ui/temperature_controller.cpp`, and
it calls `NOTIFY_ERROR`/`NOTIFY_INFO` from there. A send authority that surfaces
user-facing errors belongs on the UI side of the layering.
| `src/ui/ui_panel_controls.cpp` | delete the dead speed/flow handlers | 4 |
| `include/z_offset_utils.h` | add `adjust()` declaration | 5 |
| `src/ui/z_offset_utils.cpp` | add `adjust()` implementation | 5 |
| `src/printer/printer_motion_state.cpp` | reset the pending delta on save | 6 |
| `include/z_offset_utils.h` / `src/ui/z_offset_utils.cpp` | persisted step index + shared step table | 7 |
| `include/toolhead_homing.h` | add per-axis accessor | 8 |
| `docs/devel/ARCHITECTURE.md` | fix the wrong factory signature | 9 |

Task 1 and Task 2 are independent of everything else and of each other. Tasks 3→4 are ordered (delete the dead twin only after the replacement exists). Tasks 5→6→7 all touch `z_offset_utils` and should run in order. Tasks 8 and 9 are independent of everything.

**Deferred from the spec:** spec item D6 (dryer subject writers + mirror-unit fix) moves to the Batch 1 plan. It is the only Batch 0 item with exactly one consumer — the dryer widget — so it belongs with that widget rather than in a shared-prerequisites batch.

---

### Task 1: Lift the bed coordinate mapper out of ExcludeObjectMapView

`ExcludeObjectMapView::CoordMapper` is aspect-preserving mm→px with centering, a Y-flip, and center-origin support for delta/Voron beds. It has no LVGL dependency and the toolhead widget needs exactly this math. Moving it prevents a second copy.

**Files:**
- Create: `include/bed_coord_mapper.h`
- Create: `src/ui/bed_coord_mapper.cpp`
- Modify: `include/ui_exclude_object_map_view.h:34-52` (remove the nested class)
- Modify: `src/ui/ui_exclude_object_map_view.cpp:27-45` (remove the moved implementation)
- Test: `tests/unit/test_bed_coord_mapper.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `helix::BedCoordMapper` with constructor
  `BedCoordMapper(float bed_w_mm, float bed_h_mm, int viewport_w_px, int viewport_h_px, float origin_x = 0.0f, float origin_y = 0.0f)`,
  and members `std::pair<float,float> mm_to_px(float x_mm, float y_mm) const`,
  `PixelRect bbox_to_rect(glm::vec2 bbox_min, glm::vec2 bbox_max) const`,
  `float scale() const`. `helix::PixelRect` is `struct { float x, y, w, h; }`.
  Task 2 and the Batch 2 toolhead widget both consume this.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_bed_coord_mapper.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/bed_coord_mapper.h"

#include "../catch_amalgamated.hpp"

using helix::BedCoordMapper;

TEST_CASE("BedCoordMapper preserves aspect ratio on a non-square viewport",
          "[bed_coord_mapper]") {
    // 200x100mm bed into a 400x400px viewport: the limiting axis is X
    // (400/200 = 2.0) vs Y (400/100 = 4.0), so scale must be 2.0, not 4.0.
    BedCoordMapper m(200.0f, 100.0f, 400, 400);
    REQUIRE(m.scale() == Catch::Approx(2.0f));
}

TEST_CASE("BedCoordMapper flips Y so bed origin is bottom-left",
          "[bed_coord_mapper]") {
    // Square bed, square viewport, scale 1.0, no centering slack on X.
    BedCoordMapper m(100.0f, 100.0f, 100, 100);

    auto [x0, y0] = m.mm_to_px(0.0f, 0.0f);
    auto [x1, y1] = m.mm_to_px(0.0f, 100.0f);

    // y=0mm is the BOTTOM of the plate, so it maps to the LARGER pixel y.
    REQUIRE(y0 > y1);
    REQUIRE(y0 == Catch::Approx(100.0f));
    REQUIRE(y1 == Catch::Approx(0.0f));
    REQUIRE(x0 == Catch::Approx(x1));
}

TEST_CASE("BedCoordMapper centers the plate in the slack axis",
          "[bed_coord_mapper]") {
    // 100x100mm bed into 200x100px: scale = 1.0 (Y-limited), leaving 100px
    // of horizontal slack that must be split evenly.
    BedCoordMapper m(100.0f, 100.0f, 200, 100);
    REQUIRE(m.scale() == Catch::Approx(1.0f));

    auto [x_left, y_left] = m.mm_to_px(0.0f, 0.0f);
    REQUIRE(x_left == Catch::Approx(50.0f));
}

TEST_CASE("BedCoordMapper honours a center origin for delta beds",
          "[bed_coord_mapper]") {
    // A 200mm delta bed spans -100..+100. Its center (0,0) must land in the
    // middle of the viewport, not at a corner.
    BedCoordMapper m(200.0f, 200.0f, 200, 200, -100.0f, -100.0f);

    auto [cx, cy] = m.mm_to_px(0.0f, 0.0f);
    REQUIRE(cx == Catch::Approx(100.0f));
    REQUIRE(cy == Catch::Approx(100.0f));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
make test 2>&1 | tail -20
```

Expected: compile failure, `bed_coord_mapper.h: No such file or directory`.

- [ ] **Step 3: Create the shared header**

Create `include/bed_coord_mapper.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/vec2.hpp>

#include <utility>

namespace helix {

struct PixelRect {
    float x, y, w, h;
};

/// Aspect-preserving millimetre-to-pixel mapper for a top-down bed view.
///
/// Maps the coordinate range (origin_x .. origin_x + bed_w_mm) horizontally and
/// (origin_y .. origin_y + bed_h_mm) vertically onto a viewport, using a single
/// scale factor for both axes so the plate never stretches, and centering the
/// plate in whichever axis has slack.
///
/// Y is flipped: bed y=0 (front) maps to the BOTTOM of the viewport, matching
/// how a printer bed is drawn top-down. Callers must not flip again.
///
/// `origin_x`/`origin_y` default to 0 for cartesian beds. Delta and some Voron
/// kinematics report a center origin, e.g. a 200mm delta spans -100..+100, so
/// those pass origin_x = origin_y = -100.
class BedCoordMapper {
  public:
    BedCoordMapper(float bed_w_mm, float bed_h_mm, int viewport_w_px, int viewport_h_px,
                   float origin_x = 0.0f, float origin_y = 0.0f);

    std::pair<float, float> mm_to_px(float x_mm, float y_mm) const;
    PixelRect bbox_to_rect(glm::vec2 bbox_min, glm::vec2 bbox_max) const;

    float scale() const {
        return scale_;
    }

  private:
    float scale_{1.0f};
    float offset_x_{0.0f};
    float offset_y_{0.0f};
    float origin_x_{0.0f};
    float origin_y_{0.0f};
    int viewport_h_{0};
};

} // namespace helix
```

- [ ] **Step 4: Move the implementation verbatim**

Read the current body at `src/ui/ui_exclude_object_map_view.cpp:27-45`. Create
`src/ui/bed_coord_mapper.cpp` containing that exact arithmetic, renamed from
`ExcludeObjectMapView::CoordMapper::` to `helix::BedCoordMapper::` and with
`PixelRect` unqualified (it is now `helix::PixelRect`).

Do **not** rewrite the math while moving it. Copy it, then let the tests confirm
the behavior is unchanged. If a test from Step 1 fails after the move, the test
encodes a wrong assumption about the existing behavior — fix the test to match
the shipped behavior and note why in a comment, rather than changing the math.

- [ ] **Step 5: Point ExcludeObjectMapView at the shared mapper**

In `include/ui_exclude_object_map_view.h`: delete the nested `CoordMapper` class
(lines 34-52) and the nested `PixelRect` struct (lines 30-32), add
`#include "bed_coord_mapper.h"`, and add inside the class:

```cpp
    using PixelRect = helix::PixelRect;
    using CoordMapper = helix::BedCoordMapper;
```

The aliases keep every existing `ExcludeObjectMapView::CoordMapper` and
`::PixelRect` reference in the .cpp compiling untouched, so this task changes no
call sites. Delete the moved implementation from
`src/ui/ui_exclude_object_map_view.cpp:27-45`.

- [ ] **Step 6: Run the tests**

```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[bed_coord_mapper]"
./build/bin/helix-tests "[exclude]"
```

Expected: all PASS. The `[exclude]` run is the regression check that the move
did not change behavior.

- [ ] **Step 7: Mutation-check one test**

Change `scale_ = std::min(sx, sy)` to `std::max(sx, sy)` in
`src/ui/bed_coord_mapper.cpp`, rebuild, confirm the aspect-ratio test FAILS, then
restore it. **After restoring, `touch src/ui/bed_coord_mapper.cpp`** — restoring a
file via `mv` back leaves its mtime older than the object file, so make does
nothing and you would re-run the mutated binary and report a false pass.

- [ ] **Step 8: Commit**

```bash
git add include/bed_coord_mapper.h src/ui/bed_coord_mapper.cpp \
        include/ui_exclude_object_map_view.h src/ui/ui_exclude_object_map_view.cpp \
        tests/unit/test_bed_coord_mapper.cpp
git commit -m "refactor(bed): share one mm-to-pixel mapper for top-down bed views"
```

---

### Task 2: One bed-dimension resolver

The "what size is the bed" fallback is written twice today and the toolhead
widget would be the third copy.

**Files:**
- Create: `include/bed_dimensions.h`
- Create: `src/printer/bed_dimensions.cpp`
- Modify: `src/ui/ui_panel_print_status.cpp:1431-1440`
- Modify: `src/ui/ui_exclude_object_map_view.cpp:105-106`
- Test: `tests/unit/test_bed_dimensions.cpp`

**Interfaces:**
- Consumes: `helix::BedCoordMapper` from Task 1 (only conceptually — no code dependency).
- Produces: `helix::BedDimensions` = `struct { float w_mm; float h_mm; float origin_x; float origin_y; }`
  and `helix::BedDimensions bed_dimensions(IMoonrakerAPI* api, const PrinterState* ps)`.
  The Batch 2 toolhead widget consumes this.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_bed_dimensions.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/bed_dimensions.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("bed_dimensions falls back to 235x235 with no api and no state",
          "[bed_dimensions]") {
    auto d = helix::bed_dimensions(nullptr, nullptr);
    REQUIRE(d.w_mm == Catch::Approx(235.0f));
    REQUIRE(d.h_mm == Catch::Approx(235.0f));
    REQUIRE(d.origin_x == Catch::Approx(0.0f));
    REQUIRE(d.origin_y == Catch::Approx(0.0f));
}

TEST_CASE("bed_dimensions rejects a degenerate build volume",
          "[bed_dimensions]") {
    // A build volume that has not been populated yet reports x_max == x_min,
    // giving a zero-width bed. That must fall through to the default rather
    // than produce a zero scale in BedCoordMapper.
    helix::BedDimensions d = helix::bed_dimensions_from_volume(0.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE(d.w_mm == Catch::Approx(235.0f));
    REQUIRE(d.h_mm == Catch::Approx(235.0f));
}

TEST_CASE("bed_dimensions uses a populated build volume", "[bed_dimensions]") {
    helix::BedDimensions d = helix::bed_dimensions_from_volume(0.0f, 350.0f, 0.0f, 350.0f);
    REQUIRE(d.w_mm == Catch::Approx(350.0f));
    REQUIRE(d.h_mm == Catch::Approx(350.0f));
    REQUIRE(d.origin_x == Catch::Approx(0.0f));
}

TEST_CASE("bed_dimensions carries a negative origin through for delta beds",
          "[bed_dimensions]") {
    helix::BedDimensions d = helix::bed_dimensions_from_volume(-100.0f, 100.0f, -100.0f, 100.0f);
    REQUIRE(d.w_mm == Catch::Approx(200.0f));
    REQUIRE(d.h_mm == Catch::Approx(200.0f));
    REQUIRE(d.origin_x == Catch::Approx(-100.0f));
    REQUIRE(d.origin_y == Catch::Approx(-100.0f));
}
```

Splitting the pure arithmetic into `bed_dimensions_from_volume(x_min, x_max, y_min, y_max)`
keeps the fallback logic testable without constructing a mock API. `bed_dimensions()`
is then a thin resolver that picks a source and calls it.

- [ ] **Step 2: Run the test to verify it fails**

```bash
make test 2>&1 | tail -20
```

Expected: compile failure, `bed_dimensions.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/bed_dimensions.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class IMoonrakerAPI;

namespace helix {

class PrinterState;

struct BedDimensions {
    float w_mm;
    float h_mm;
    float origin_x;
    float origin_y;
};

/// Fallback used when no source reports a usable bed size.
inline constexpr float kDefaultBedSizeMm = 235.0f;

/// Pure form: derive dimensions from an axis range, falling back to the default
/// when the range is degenerate (zero or negative extent).
BedDimensions bed_dimensions_from_volume(float x_min, float x_max, float y_min, float y_max);

/// Resolve the bed size from the best available source, in order:
///   1. IMoonrakerAPI::hardware().build_volume() — from Klipper
///      configfile.settings.stepper_x/y position_min/position_max
///   2. PrinterState::get_axis_bounds() — toolhead.axis_minimum/axis_maximum,
///      the kinematic envelope, available earlier than (1) because it rides the
///      status subscription rather than a config query
///   3. 235x235 with a zero origin
///
/// Either argument may be null. Do NOT add printer_database.json's
/// `build_volume_range` as a source — that field is a detection heuristic, not
/// an authoritative bed size.
BedDimensions bed_dimensions(IMoonrakerAPI* api, const PrinterState* ps);

} // namespace helix
```

- [ ] **Step 4: Write the implementation**

Create `src/printer/bed_dimensions.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_dimensions.h"

#include "i_moonraker_api.h"
#include "printer_state.h"

namespace helix {

BedDimensions bed_dimensions_from_volume(float x_min, float x_max, float y_min, float y_max) {
    const float w = x_max - x_min;
    const float h = y_max - y_min;
    if (w > 0.0f && h > 0.0f) {
        return BedDimensions{w, h, x_min, y_min};
    }
    return BedDimensions{kDefaultBedSizeMm, kDefaultBedSizeMm, 0.0f, 0.0f};
}

BedDimensions bed_dimensions(IMoonrakerAPI* api, const PrinterState* ps) {
    if (api) {
        const auto& vol = api->hardware().build_volume();
        auto d = bed_dimensions_from_volume(vol.x_min, vol.x_max, vol.y_min, vol.y_max);
        if (d.w_mm != kDefaultBedSizeMm || d.h_mm != kDefaultBedSizeMm) {
            return d;
        }
    }
    if (ps) {
        const auto& b = ps->get_axis_bounds();
        if (b.has_x && b.has_y) {
            return bed_dimensions_from_volume(b.x_min, b.x_max, b.y_min, b.y_max);
        }
    }
    return BedDimensions{kDefaultBedSizeMm, kDefaultBedSizeMm, 0.0f, 0.0f};
}

} // namespace helix
```

Check `include/printer_motion_state.h:18-25` for the exact `AxisBounds` field
names before writing this — the plan assumes `x_min`/`x_max`/`y_min`/`y_max`/
`has_x`/`has_y`. Adjust to whatever the struct actually declares.

- [ ] **Step 5: Convert both existing call sites**

At `src/ui/ui_panel_print_status.cpp:1431-1440`, replace the inline block with:

```cpp
        const auto bed = helix::bed_dimensions(api_, &printer_state_);
        float bed_w = bed.w_mm, bed_h = bed.h_mm;
```

Confirm what `PrintStatusPanel` calls its `PrinterState` reference before writing
`&printer_state_`. Then do the same at `src/ui/ui_exclude_object_map_view.cpp:105-106`.

- [ ] **Step 6: Run the tests**

```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[bed_dimensions]"
./build/bin/helix-tests "[exclude]" "[print_status]"
```

Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add include/bed_dimensions.h src/printer/bed_dimensions.cpp \
        src/ui/ui_panel_print_status.cpp src/ui/ui_exclude_object_map_view.cpp \
        tests/unit/test_bed_dimensions.cpp
git commit -m "refactor(bed): resolve bed dimensions through one fallback chain"
```

---

### Task 3: One authority for speed and flow sends

There is no `SpeedFlowController` analogous to `TemperatureController`. Two call
sites format their own gcode with disagreeing clamps. This task adds the single
authority and converts the live call site; Task 4 deletes the dead one.

**Files:**
- Create: `include/tune_controller.h`
- Create: `src/ui/tune_controller.cpp`
- Modify: `src/ui/ui_print_tune_overlay.cpp:407-456`
- Test: `tests/unit/test_tune_controller.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: in namespace `helix::tune` —
  `constexpr int kSpeedMinPct = 50, kSpeedMaxPct = 200, kFlowMinPct = 75, kFlowMaxPct = 125;`
  `int clamp_speed_percent(int pct);`
  `int clamp_flow_percent(int pct);`
  `void set_speed_percent(IMoonrakerAPI* api, int pct);`
  `void set_flow_percent(IMoonrakerAPI* api, int pct);`
  Each setter clamps, sends, and reports failure via `NOTIFY_ERROR`. Returns void;
  the caller reads the applied value back from `clamp_*` if it needs it.
  The Batch 1 speed/flow widget consumes these.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_tune_controller.cpp`. Sends made *through `MoonrakerAPI`*
are captured with the `ExecuteGcodeFixture` idiom from
`tests/unit/test_gcode_verbatim.cpp:118-149` — a `MoonrakerClientMock` that the
API is constructed over, read back via `mock_client.last_send_script()`.

**Do not** try to capture by subclassing `MoonrakerClient::send_jsonrpc`. That
works only for the direct `MoonrakerClient::gcode_script()` path; it does not see
sends routed through `MoonrakerAPI::execute_gcode`, which is what this code uses.
`MoonrakerAPI`'s constructor takes `(MoonrakerClient&, PrinterState&)` — a
reference pair, not a pointer — and the client must be connected and Klippy READY
or the send is refused.

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/tune_controller.h"
#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class TuneFixture : public LVGLTestFixture {
  public:
    TuneFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~TuneFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    const std::string& last_sent() const {
        return mock_client.last_send_script();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

} // namespace

TEST_CASE("clamp_speed_percent bounds to the shipped range", "[tune_controller]") {
    REQUIRE(helix::tune::clamp_speed_percent(10) == 50);
    REQUIRE(helix::tune::clamp_speed_percent(100) == 100);
    REQUIRE(helix::tune::clamp_speed_percent(500) == 200);
}

TEST_CASE("clamp_flow_percent bounds to the shipped range", "[tune_controller]") {
    REQUIRE(helix::tune::clamp_flow_percent(10) == 75);
    REQUIRE(helix::tune::clamp_flow_percent(100) == 100);
    REQUIRE(helix::tune::clamp_flow_percent(500) == 125);
}

TEST_CASE_METHOD(TuneFixture, "set_speed_percent sends a clamped M220",
                 "[tune_controller][mock]") {
    helix::tune::set_speed_percent(api.get(), 500);
    REQUIRE(last_sent() == "M220 S200");
}

TEST_CASE_METHOD(TuneFixture, "set_flow_percent sends a clamped M221",
                 "[tune_controller][mock]") {
    helix::tune::set_flow_percent(api.get(), 10);
    REQUIRE(last_sent() == "M221 S75");
}

TEST_CASE_METHOD(TuneFixture, "an in-range value is sent unmodified",
                 "[tune_controller][mock]") {
    // Guards against a clamp that rewrites everything, which would make the
    // two clamping tests above pass for the wrong reason.
    helix::tune::set_speed_percent(api.get(), 120);
    REQUIRE(last_sent() == "M220 S120");
}

TEST_CASE("a null api sends nothing and does not crash", "[tune_controller]") {
    helix::tune::set_speed_percent(nullptr, 120);
    helix::tune::set_flow_percent(nullptr, 120);
    SUCCEED();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
make test 2>&1 | tail -20
```

Expected: compile failure, `tune_controller.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/tune_controller.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class IMoonrakerAPI;

namespace helix::tune {

/// The single agreed clamp for speed and flow overrides.
///
/// Before this existed, PrintTuneOverlay clamped speed to [50,200] and flow to
/// [75,125] while a dead ControlsPanel path used [10,200] and [50,150]. These
/// are the shipped values — the ones users have actually been getting.
inline constexpr int kSpeedMinPct = 50;
inline constexpr int kSpeedMaxPct = 200;
inline constexpr int kFlowMinPct = 75;
inline constexpr int kFlowMaxPct = 125;

int clamp_speed_percent(int pct);
int clamp_flow_percent(int pct);

/// Clamp and send M220. No-op when `api` is null. Errors surface via NOTIFY_ERROR.
void set_speed_percent(IMoonrakerAPI* api, int pct);

/// Clamp and send M221. No-op when `api` is null. Errors surface via NOTIFY_ERROR.
void set_flow_percent(IMoonrakerAPI* api, int pct);

} // namespace helix::tune
```

- [ ] **Step 4: Write the implementation**

Create `src/ui/tune_controller.cpp`, lifting the send bodies from
`src/ui/ui_print_tune_overlay.cpp:407-437` so the log text and error copy are
preserved:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tune_controller.h"

#include "i_moonraker_api.h"
#include "ui_error_reporting.h" // NOTIFY_ERROR

#include "lvgl/src/others/translation/lv_translation.h" // lv_tr

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

namespace helix::tune {

int clamp_speed_percent(int pct) {
    return std::clamp(pct, kSpeedMinPct, kSpeedMaxPct);
}

int clamp_flow_percent(int pct) {
    return std::clamp(pct, kFlowMinPct, kFlowMaxPct);
}

void set_speed_percent(IMoonrakerAPI* api, int pct) {
    if (!api) {
        return;
    }
    const int value = clamp_speed_percent(pct);
    api->execute_gcode(
        "M220 S" + std::to_string(value),
        [value]() { spdlog::debug("[tune] Speed set to {}%", value); },
        [](const MoonrakerError& err) {
            spdlog::error("[tune] Failed to set speed: {}", err.message);
            NOTIFY_ERROR(lv_tr("Failed to set print speed: {}"), err.user_message());
        });
}

void set_flow_percent(IMoonrakerAPI* api, int pct) {
    if (!api) {
        return;
    }
    const int value = clamp_flow_percent(pct);
    api->execute_gcode(
        "M221 S" + std::to_string(value),
        [value]() { spdlog::debug("[tune] Flow set to {}%", value); },
        [](const MoonrakerError& err) {
            spdlog::error("[tune] Failed to set flow: {}", err.message);
            NOTIFY_ERROR(lv_tr("Failed to set flow rate: {}"), err.user_message());
        });
}

} // namespace helix::tune
```

Verified include locations: `NOTIFY_ERROR` is defined in
`include/ui_error_reporting.h:70`; `lv_tr` comes from
`lvgl/src/others/translation/lv_translation.h`. The tune overlay reaches
`NOTIFY_ERROR` transitively through `ui_toast_manager.h`, so do not copy its
include block verbatim — include `ui_error_reporting.h` directly.

`IMoonrakerAPI::execute_gcode(const std::string&, SuccessCallback, ErrorCallback)`
is confirmed at `include/i_moonraker_api.h:198`, so passing a `std::string` built
with `+ std::to_string(value)` is correct.

- [ ] **Step 5: Convert PrintTuneOverlay**

Rewrite the three handlers at `src/ui/ui_print_tune_overlay.cpp:407-456` to keep
their local mirrors in sync while delegating the send:

```cpp
void PrintTuneOverlay::handle_speed_adjust(int delta) {
    speed_percent_ = helix::tune::clamp_speed_percent(speed_percent_ + delta);
    update_display();
    helix::tune::set_speed_percent(api_, speed_percent_);
}

void PrintTuneOverlay::handle_flow_adjust(int delta) {
    flow_percent_ = helix::tune::clamp_flow_percent(flow_percent_ + delta);
    update_display();
    helix::tune::set_flow_percent(api_, flow_percent_);
}

void PrintTuneOverlay::handle_reset() {
    speed_percent_ = 100;
    flow_percent_ = 100;
    update_display();
    helix::tune::set_speed_percent(api_, 100);
    helix::tune::set_flow_percent(api_, 100);
}
```

Note this changes the reset path's log lines and error copy from "reset" wording
to the generic set wording. That is intentional consolidation, not a regression.

**Then delete the overlay's now-duplicate clamp constants.**
`include/ui_print_tune_overlay.h:229-230` declares
`static constexpr double Z_OFFSET_MIN = -2.0; Z_OFFSET_MAX = 2.0;`. Once
`helix::zoffset::adjust` owns the clamp, those are a second copy of the same
numbers that can silently drift from the shared ones. Remove them and update any
remaining reference to use `helix::zoffset::kZOffsetMinMm` / `kZOffsetMaxMm`. If
something outside the adjust path still needs them (e.g. a slider range), point it
at the shared constants rather than keeping a local pair. Leaving two clamp
definitions defeats the point of this task.

- [ ] **Step 6: Run the tests**

```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[tune_controller]"
./build/bin/helix-tests "[tune]" "[print_tune]"
```

Expected: all PASS.

- [ ] **Step 7: Mutation-check one test**

Change `kFlowMaxPct` to `999`, rebuild, confirm the clamped-M221 test FAILS,
restore, `touch include/tune_controller.h`.

- [ ] **Step 8: Commit**

```bash
git add include/tune_controller.h src/ui/tune_controller.cpp \
        src/ui/ui_print_tune_overlay.cpp tests/unit/test_tune_controller.cpp
git commit -m "refactor(tune): send speed and flow overrides through one clamp"
```

---

### Task 4: Delete the dead ControlsPanel speed/flow path

`src/ui/ui_panel_controls.cpp:1555-1650` registers `on_controls_speed_up` /
`on_controls_flow_*` callbacks at `:272-275`, but **no XML references any of
them**. The flow handler tracks a function-local `static int current_flow = 100`
instead of reading the `flow_factor` subject, and `update_flow_display()` at
`:1546-1552` hardcodes 100%. Leaving it is a trap for whoever wires those
callbacks up next.

**Files:**
- Modify: `src/ui/ui_panel_controls.cpp` — remove handlers, registrations, and declarations
- Modify: `include/ui_panel_controls.h` — remove the member declarations

**Interfaces:**
- Consumes: `helix::tune` from Task 3 (as the replacement any future wiring must use).
- Produces: nothing.

- [ ] **Step 1: Prove the callbacks really are unreferenced**

```bash
grep -rn "on_controls_speed_up\|on_controls_speed_down\|on_controls_flow_up\|on_controls_flow_down" \
  ui_xml/ src/ include/ tests/
```

Expected: hits ONLY in `src/ui/ui_panel_controls.cpp` (definitions plus the
`lv_xml_register_event_cb` calls) and `include/ui_panel_controls.h`. **If any
`ui_xml/` file references one, STOP** — the premise is wrong, do not delete;
report back instead.

- [ ] **Step 2: Delete the handlers, trampolines, and registrations**

There are **five** groups to remove, not three. Missing the trampolines breaks the
build, because the macro expands to a function body that calls the `handle_*`
method you deleted:

1. **Method bodies** in `src/ui/ui_panel_controls.cpp`: `update_flow_display`
   (`:1546`), `handle_speed_up` (`:1555`), `handle_speed_down` (`:1578`),
   `handle_flow_up` (`:1601`), `handle_flow_down` (`:1630`).
2. **Trampoline generators** in the same file, `:1822-1825`:
   ```cpp
   PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, speed_up)
   PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, speed_down)
   PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, flow_up)
   PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, flow_down)
   ```
   This macro is what defines `ControlsPanel::on_speed_up` and friends — they have
   no longhand definition in the file, which is why a plain grep for `on_speed_up`
   finds only the registration table.
3. **Registration entries** at `:272-275` (the four `{"on_controls_speed_up", on_speed_up}`
   style rows) and the `// Speed/Flow override buttons` comment above them.
4. **Handler declarations** in `include/ui_panel_controls.h:506-509` plus
   `update_flow_display` at `:511`.
5. **Trampoline declarations** in `include/ui_panel_controls.h:580-583`
   (`static void on_speed_up(lv_event_t* e);` and its three siblings).

Then remove any member left unused by the above — e.g. a speed/flow display label
pointer — but only after confirming nothing else references it.

Line numbers are as of this plan's writing; verify each against the real file
before deleting, and delete by matching the code, not by line number.

- [ ] **Step 3: Build and run the controls tests**

```bash
make -j16 > /tmp/t4-build.log 2>&1; echo "EXIT=$?"
make test 2>&1 | tail -20
./build/bin/helix-tests "[controls]"
```

Expected: EXIT=0 and all PASS. A compile error here means something did
reference the deleted code — re-run Step 1's grep more broadly before proceeding.

- [ ] **Step 4: Verify the imperative-UI gate did not regress**

```bash
python3 scripts/check_imperative_ui.py --list | grep TOTAL
```

**Baseline before this task is `TOTAL 383`** (measured on this branch). Read the
`TOTAL` line, not a line count — `wc -l` counts the report's own output rows, not
violations.

Expected: `TOTAL` is 383 or LOWER. The gate ratchets — the count may fall, never
rise. If deleting the dead handlers drops it below 383, that is a win worth noting
in your report; if it somehow rises, you deleted something that was not the dead
path and must stop and report.

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui_panel_controls.cpp include/ui_panel_controls.h
git commit -m "refactor(controls): drop the unreachable speed and flow handlers"
```

---

### Task 5: Extract the Z-offset baby-step

Clamping, micron rounding, pending-delta bookkeeping, the optimistic subject
write, and the homed-axes `MOVE=1` decision all live inline in
`PrintTuneOverlay::handle_z_offset_changed`. The Batch 1 Z-offset widget needs
all of it and would drift if it reimplemented any part.

**Files:**
- Modify: `include/z_offset_utils.h` (add the declaration after `format_offset_compact`)
- Modify: `src/ui/z_offset_utils.cpp` (add the implementation)
- Modify: `src/ui/ui_print_tune_overlay.cpp:458-524`
- Test: `tests/unit/test_z_offset_adjust.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: in namespace `helix::zoffset` —
  `constexpr double kZOffsetMinMm = -2.0, kZOffsetMaxMm = 2.0;`
  `struct AdjustResult { double applied_delta_mm; double new_offset_mm; bool sent; };`
  `AdjustResult adjust(IMoonrakerAPI* api, PrinterState* ps, double current_offset_mm, double delta_mm);`
  The caller keeps ownership of any UI mirror and updates it from
  `result.new_offset_mm`. The Batch 1 Z-offset widget consumes this.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_z_offset_adjust.cpp`, using the same fixture idiom as
Task 3 (a `MoonrakerClientMock` the API is built over — **not** a
`send_jsonrpc` subclass, which cannot see sends routed through `MoonrakerAPI`).

Note every case passes the fixture's **real** `PrinterState`, not `nullptr`. The
homed/not-homed branch is the whole point of this function, and `homed_axes` is
the runtime input that selects it — testing it with a null state would exercise a
path the shipped code never takes.

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/z_offset_utils.h"
#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class ZOffsetFixture : public LVGLTestFixture {
  public:
    ZOffsetFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~ZOffsetFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    void set_homed(const char* axes) {
        lv_subject_copy_string(state.get_homed_axes_subject(), axes);
    }

    const std::string& last_sent() const {
        return mock_client.last_send_script();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

} // namespace

TEST_CASE_METHOD(ZOffsetFixture, "adjust clamps at the safe limit",
                 "[z_offset][adjust][mock]") {
    // Already at +1.99mm, asking for another +0.05 must stop at +2.0.
    auto r = helix::zoffset::adjust(api.get(), &state, 1.99, 0.05);

    REQUIRE(r.new_offset_mm == Catch::Approx(2.0));
    REQUIRE(r.applied_delta_mm == Catch::Approx(0.01));
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust refuses a no-op move at the limit",
                 "[z_offset][adjust][mock]") {
    auto r = helix::zoffset::adjust(api.get(), &state, 2.0, 0.05);

    REQUIRE(r.sent == false);
    REQUIRE(last_sent().find("Z_ADJUST") == std::string::npos);
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust rounds to the nearest micron",
                 "[z_offset][adjust][mock]") {
    // Repeated float addition would drift; the result must land on a micron.
    auto r = helix::zoffset::adjust(api.get(), &state, 0.0, 0.0123456);

    REQUIRE(r.new_offset_mm == Catch::Approx(0.012));
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust omits MOVE=1 when axes are not homed",
                 "[z_offset][adjust][mock]") {
    set_homed("xy"); // Z missing — MOVE=1 would make Klipper error

    helix::zoffset::adjust(api.get(), &state, 0.0, 0.05);

    REQUIRE(last_sent().find("MOVE=1") == std::string::npos);
    REQUIRE(last_sent().find("SET_GCODE_OFFSET Z_ADJUST=0.050") != std::string::npos);
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust appends MOVE=1 when all axes are homed",
                 "[z_offset][adjust][mock]") {
    set_homed("xyz");

    helix::zoffset::adjust(api.get(), &state, 0.0, 0.05);

    REQUIRE(last_sent() == "SET_GCODE_OFFSET Z_ADJUST=0.050 MOVE=1");
}

TEST_CASE_METHOD(ZOffsetFixture, "adjust accumulates the pending delta",
                 "[z_offset][adjust][mock]") {
    set_homed("xyz");

    helix::zoffset::adjust(api.get(), &state, 0.0, 0.05);
    helix::zoffset::adjust(api.get(), &state, 0.05, -0.01);

    // +50um then -10um = +40um still unsaved.
    REQUIRE(lv_subject_get_int(state.get_pending_z_offset_delta_subject()) == 40);
}
```

Verify the real accessor names `get_homed_axes_subject()` and
`get_pending_z_offset_delta_subject()` against `include/printer_state.h` before
writing them, and adjust if they differ.

- [ ] **Step 2: Run the test to verify it fails**

```bash
make test 2>&1 | tail -20
```

Expected: compile failure, no member named `adjust` in namespace `helix::zoffset`.

- [ ] **Step 3: Add the declaration**

In `include/z_offset_utils.h`, after `format_offset_compact` (line 26), add:

```cpp
/// Safe clamp for a baby-stepped Z offset, in millimetres.
inline constexpr double kZOffsetMinMm = -2.0;
inline constexpr double kZOffsetMaxMm = 2.0;

struct AdjustResult {
    double applied_delta_mm; ///< delta actually applied after clamping
    double new_offset_mm;    ///< resulting offset, rounded to the micron
    bool sent;               ///< false when clamped to a no-op or api was null
};

/// Apply a Z baby-step: clamp to +/-2mm, round to the micron, accumulate the
/// pending delta, optimistically publish gcode_z_offset, and send
/// SET_GCODE_OFFSET Z_ADJUST.
///
/// MOVE=1 is appended only when x, y and z are all homed — it makes the toolhead
/// move immediately, which is the point of baby-stepping during a print, but
/// Klipper errors on it when the axes are not homed. A null `ps` is treated as
/// not homed.
///
/// The caller owns any UI mirror of the offset and should update it from
/// `AdjustResult::new_offset_mm` rather than tracking its own running total.
///
/// @warning Main thread only — reads and writes LVGL subjects.
AdjustResult adjust(IMoonrakerAPI* api, PrinterState* ps, double current_offset_mm,
                    double delta_mm);
```

`PrinterState` needs a forward declaration in this header alongside `IMoonrakerAPI`.

- [ ] **Step 4: Write the implementation**

Add to `src/ui/z_offset_utils.cpp`, transcribed from
`src/ui/ui_print_tune_overlay.cpp:458-524` with the UI-specific parts (the label
buffer, the indicator flash) left behind in the overlay:

```cpp
AdjustResult adjust(IMoonrakerAPI* api, PrinterState* ps, double current_offset_mm,
                    double delta_mm) {
    double new_offset = current_offset_mm + delta_mm;
    if (new_offset < kZOffsetMinMm || new_offset > kZOffsetMaxMm) {
        spdlog::warn("[zoffset] {:.3f}mm clamped to [{}, {}]", new_offset, kZOffsetMinMm,
                     kZOffsetMaxMm);
        new_offset = std::clamp(new_offset, kZOffsetMinMm, kZOffsetMaxMm);
        delta_mm = new_offset - current_offset_mm;
        if (std::abs(delta_mm) < 0.0005) {
            return AdjustResult{0.0, current_offset_mm, false};
        }
    }

    // Round to the micron so repeated additions cannot drift.
    new_offset = std::round(new_offset * 1000.0) / 1000.0;

    if (ps) {
        ps->add_pending_z_offset_delta(static_cast<int>(std::lround(delta_mm * 1000.0)));
        // Publish immediately rather than waiting for Moonraker to broadcast.
        if (auto* subj = ps->get_gcode_z_offset_subject()) {
            lv_subject_set_int(subj, static_cast<int>(std::lround(new_offset * 1000.0)));
        }
    }

    if (!api) {
        return AdjustResult{delta_mm, new_offset, false};
    }

    bool all_homed = false;
    if (ps) {
        const char* axes = lv_subject_get_string(ps->get_homed_axes_subject());
        all_homed = axes && strchr(axes, 'x') && strchr(axes, 'y') && strchr(axes, 'z');
    }

    char gcode[96];
    std::snprintf(gcode, sizeof(gcode), "SET_GCODE_OFFSET Z_ADJUST=%.3f%s", delta_mm,
                  all_homed ? " MOVE=1" : "");
    const double sent_delta = delta_mm;
    api->execute_gcode(
        gcode, [sent_delta]() { spdlog::debug("[zoffset] adjusted {:+.3f}mm", sent_delta); },
        [](const MoonrakerError& err) {
            spdlog::error("[zoffset] adjust failed: {}", err.message);
            NOTIFY_ERROR(lv_tr("Z-offset failed: {}"), err.user_message());
        });

    return AdjustResult{delta_mm, new_offset, true};
}
```

- [ ] **Step 5: Convert PrintTuneOverlay to call it**

Replace the body of `handle_z_offset_changed` at
`src/ui/ui_print_tune_overlay.cpp:458-524` with:

```cpp
void PrintTuneOverlay::handle_z_offset_changed(double delta) {
    const auto r = helix::zoffset::adjust(api_, printer_state_, current_z_offset_, delta);
    if (r.applied_delta_mm == 0.0 && !r.sent) {
        return; // already at the limit
    }
    current_z_offset_ = r.new_offset_mm;

    helix::format::format_distance_mm(current_z_offset_, 3, tune_z_offset_buf_,
                                      sizeof(tune_z_offset_buf_));
    lv_subject_copy_string(&tune_z_offset_subject_, tune_z_offset_buf_);

    if (tune_panel_) {
        lv_obj_t* indicator = lv_obj_find_by_name(tune_panel_, "z_offset_indicator");
        if (indicator) {
            ui_z_offset_indicator_set_value(indicator,
                                            static_cast<int>(current_z_offset_ * 1000.0));
            ui_z_offset_indicator_flash_direction(indicator, r.applied_delta_mm > 0 ? 1 : -1);
        }
    }
}
```

- [ ] **Step 6: Run the tests**

```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[z_offset]"
./build/bin/helix-tests "[print_tune]" "[controls]"
```

Expected: all PASS.

- [ ] **Step 7: Mutation-check one test**

Flip `all_homed ? " MOVE=1" : ""` to `all_homed ? "" : " MOVE=1"`, rebuild,
confirm the not-homed test FAILS, restore, `touch src/ui/z_offset_utils.cpp`.

- [ ] **Step 8: Commit**

```bash
git add include/z_offset_utils.h src/ui/z_offset_utils.cpp \
        src/ui/ui_print_tune_overlay.cpp tests/unit/test_z_offset_adjust.cpp
git commit -m "refactor(z-offset): share the baby-step adjust path"
```

---

### Task 6: Reset the pending Z-offset delta

`PrinterMotionState::clear_pending_z_offset_delta` (`src/printer/printer_motion_state.cpp:230`)
has **zero non-test callers**. Nothing clears `pending_z_offset_delta` — not on
save, not on print end, not on disconnect — so the "unsaved adjustment" banner in
ControlsPanel accumulates for the whole session and keeps claiming there are
unsaved changes after the user has saved.

**Files:**
- Modify: `src/ui/z_offset_utils.cpp` (`apply_and_save` success path)
- Test: `tests/unit/test_z_offset_adjust.cpp` (add a case)

**Interfaces:**
- Consumes: `helix::zoffset::adjust` from Task 5.
- Produces: no new API. `apply_and_save` gains a trailing
  `PrinterState* ps = nullptr` parameter — it must be **last**, after
  `on_success` and `on_error`, so that defaulting it keeps any caller this plan
  does not know about compiling. New signature:
  `void apply_and_save(IMoonrakerAPI* api, ZOffsetCalibrationStrategy strategy, std::function<void()> on_success, std::function<void(const std::string&)> on_error, PrinterState* ps = nullptr);`

- [ ] **Step 1: Confirm the premise**

```bash
grep -rn "clear_pending_z_offset_delta" src/ include/ tests/
```

Expected: the definition in `src/printer/printer_motion_state.cpp`, its
declaration, and hits only in `tests/unit/test_printer_motion_char.cpp`. **If a
production caller exists, STOP** and report — the bug is already fixed elsewhere.

- [ ] **Step 2: Write the failing test**

Add to `tests/unit/test_z_offset_adjust.cpp`:

Reuse `ZOffsetFixture` from Task 5 — this case belongs in the same file and needs
the same connected mock:

```cpp
TEST_CASE_METHOD(ZOffsetFixture, "a saved offset clears the pending delta",
                 "[z_offset][adjust][mock]") {
    set_homed("xyz");

    helix::zoffset::adjust(api.get(), &state, 0.0, 0.05);
    REQUIRE(lv_subject_get_int(state.get_pending_z_offset_delta_subject()) == 50);

    bool saved = false;
    helix::zoffset::apply_and_save(
        api.get(), ZOffsetCalibrationStrategy::PROBE_CALIBRATE, [&]() { saved = true; },
        [](const std::string&) {}, &state);

    // apply_and_save chains Z_OFFSET_APPLY_PROBE then SAVE_CONFIG through the
    // mock client, which completes its RPCs synchronously.
    REQUIRE(saved);
    REQUIRE(lv_subject_get_int(state.get_pending_z_offset_delta_subject()) == 0);
}
```

**The `REQUIRE(saved)` line is load-bearing.** If the mock does not drive
`apply_and_save`'s success continuation synchronously, that assertion fails and
tells you so directly — rather than the pending-delta assertion passing or failing
for reasons unrelated to the reset you are adding. If `saved` is false, find how
the existing z-offset save tests drive the continuation and follow that; do not
delete the assertion to make the test green.

**Add a second case for the firmware-managed path**, which is guaranteed
synchronous — `apply_and_save` calls `on_success()` and returns without sending
anything (`src/ui/z_offset_utils.cpp:67-73`). This one cannot be defeated by mock
async behavior, so it pins the wrapper independently of how the RPC chain resolves:

```cpp
TEST_CASE_METHOD(ZOffsetFixture, "a firmware-managed save also clears the pending delta",
                 "[z_offset][adjust][mock]") {
    set_homed("xyz");
    helix::zoffset::adjust(api.get(), &state, 0.0, 0.05);
    REQUIRE(lv_subject_get_int(state.get_pending_z_offset_delta_subject()) == 50);

    bool saved = false;
    helix::zoffset::apply_and_save(
        api.get(), ZOffsetCalibrationStrategy::FIRMWARE_MANAGED, [&]() { saved = true; },
        [](const std::string&) {}, &state);

    REQUIRE(saved);
    REQUIRE(lv_subject_get_int(state.get_pending_z_offset_delta_subject()) == 0);
}
```

If the `PROBE_CALIBRATE` case turns out not to be drivable synchronously through
the mock, keep this firmware-managed case, report that limitation, and do not fake
the other one.

- [ ] **Step 3: Run the test to verify it fails**

```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[z_offset][adjust]"
```

Expected: FAIL — the pending delta is still 50.

- [ ] **Step 4: Add the reset**

Give `apply_and_save` a trailing `PrinterState* ps = nullptr` parameter.

**`apply_and_save` has TWO success paths, not one** (`src/ui/z_offset_utils.cpp:57-110`):
the `FIRMWARE_MANAGED` early return calls `on_success()` synchronously and returns
(`:67-73`), and the `Z_OFFSET_APPLY_PROBE`/`_ENDSTOP` → `SAVE_CONFIG` chain calls it
from the inner nested success lambda (`:94-99`). Putting the reset only in the
`SAVE_CONFIG` continuation would silently miss every firmware-managed printer —
where the offset genuinely *is* persisted, so the pending delta is exactly as stale.

Do it in **one** place instead, by wrapping the caller's continuation at the top of
the function, before any of the branching:

```cpp
    // Both success paths (FIRMWARE_MANAGED early return and the
    // APPLY -> SAVE_CONFIG chain) funnel through this wrapper, so the pending
    // delta is cleared once, wherever the save actually completed.
    auto on_saved = [ps, on_success = std::move(on_success)]() {
        if (ps) {
            ps->clear_pending_z_offset_delta();
        }
        if (on_success) {
            on_success();
        }
    };
```

then use `on_saved` everywhere the body currently uses `on_success`. Note the
existing code copies `on_success` into two nested lambdas, so keep it copyable.

Then pass the state from **all three** existing call sites — find them with
`grep -rn 'apply_and_save' src/ include/ | grep -v z_offset_utils` rather than
trusting this list:

- `src/ui/ui_print_tune_overlay.cpp:558`
- `src/ui/ui_panel_controls.cpp:1097`
- `src/ui/ui_panel_calibration_zoffset.cpp:717`

**The third one is the trap.** Because the new parameter is defaulted to
`nullptr`, any call site you miss keeps compiling and silently never clears the
pending delta — the exact bug this task exists to fix, still present on one of the
three paths a user can save from. Passing state from all three is the requirement;
a green build proves nothing here.

- [ ] **Step 5: Run the tests**

```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[z_offset]" "[controls]" "[print_tune]"
```

Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add src/ui/z_offset_utils.cpp include/z_offset_utils.h \
        src/ui/ui_print_tune_overlay.cpp src/ui/ui_panel_controls.cpp \
        tests/unit/test_z_offset_adjust.cpp
git commit -m "fix(z-offset): clear the pending delta once a save succeeds"
```

---

### Task 7: Persist the Z step size

`Z_STEP_AMOUNTS[] = {0.05, 0.025, 0.01, 0.005}` (`include/ui_print_tune_overlay.h:227`)
lives in RAM only, with the selection in `selected_z_step_idx_` initialised from an
existing named constant `Z_STEP_DEFAULT` (`:233`) — use that constant rather than
writing a bare `2`. It resets every launch, and the Batch 1 Z-offset widget needs
to share whatever step the user last chose rather than keeping a second one.

**Files:**
- Modify: `include/ui_print_tune_overlay.h` (no new state, just the persisted read)
- Modify: `src/ui/ui_print_tune_overlay.cpp:526+` (`handle_z_step_select`) and the init path
- Test: `tests/unit/test_z_offset_adjust.cpp` (add a case)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `Config` key `z_offset/step_index` (int, 0-3, default 2). The Batch 1
  Z-offset widget reads the same key.

- [ ] **Step 1: Use the verified Config idiom**

The dryer overlay persists two per-printer values this way
(`src/ui/ui_ams_environment_overlay.cpp:217-228` and `:721-722`) — this is the
form to copy, already verified:

```cpp
Config* config = Config::get_instance();
int v = config->get<int>(config->df() + "ams/dryer_last_temp", 55);
config->set<int>(config->df() + "ams/dryer_last_temp", v);
```

`df()` is the per-printer prefix, so the key you want is
`config->df() + "z_offset/step_index"` — per-printer, not global. Guard against a
null `Config::get_instance()` and fall back to `kZStepDefaultIndex` if it is null,
since these accessors are called from overlay init.

- [ ] **Step 2: Write the failing test**

Add to `tests/unit/test_z_offset_adjust.cpp`:

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "the z step index round-trips through Config",
                 "[z_offset][step]") {
    helix::zoffset::set_persisted_step_index(3);
    REQUIRE(helix::zoffset::persisted_step_index() == 3);

    // Out-of-range values must fall back to the default rather than index a
    // Z_STEP_AMOUNTS entry that does not exist.
    helix::zoffset::set_persisted_step_index(99);
    REQUIRE(helix::zoffset::persisted_step_index() == 2);

    helix::zoffset::set_persisted_step_index(-1);
    REQUIRE(helix::zoffset::persisted_step_index() == 2);
}
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
make test 2>&1 | tail -20
```

Expected: compile failure, no `persisted_step_index` in `helix::zoffset`.

- [ ] **Step 4: Add the accessors**

In `include/z_offset_utils.h`:

```cpp
/// Z baby-step increments, largest first. Index 2 (0.01mm) is the default.
inline constexpr double kZStepAmountsMm[] = {0.05, 0.025, 0.01, 0.005};
inline constexpr int kZStepDefaultIndex = 2;

/// Read the user's last-chosen step index from Config, clamped to a valid
/// index. Out-of-range or unset returns kZStepDefaultIndex.
int persisted_step_index();

/// Persist the chosen step index. Out-of-range values are ignored.
void set_persisted_step_index(int idx);
```

Implement both in `src/ui/z_offset_utils.cpp` against the `Config` key
`z_offset/step_index`, validating against `std::size(kZStepAmountsMm)`.

- [ ] **Step 5: Use them from PrintTuneOverlay**

Have `handle_z_step_select` call `set_persisted_step_index(idx)` after validating,
and have the overlay seed `selected_z_step_idx_` from `persisted_step_index()`
instead of the hardcoded default.

**Delete `Z_STEP_AMOUNTS` and `Z_STEP_DEFAULT` from
`include/ui_print_tune_overlay.h` outright** and use the shared
`kZStepAmountsMm` / `kZStepDefaultIndex` everywhere. Both are used only inside
this one overlay — the four references are `handle_z_step_select` (bounds check,
the active-subject loop, and its debug log) and the `Z_STEP_AMOUNTS[selected_z_step_idx_]`
read that produces the actual step. This is the same treatment Task 5 gave
`Z_OFFSET_MIN`/`Z_OFFSET_MAX`, and for the same reason: two copies of the step
table can silently disagree once the widget also reads it.

**Line numbers in this task are stale.** Task 5 removed ~60 lines from
`ui_print_tune_overlay.cpp`, so `handle_z_step_select` has moved from `:526` to
roughly `:450`, and the header constants from `:227-233` may also have shifted.
Locate everything by matching the code, not by line number.

- [ ] **Step 6: Run the tests**

```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[z_offset]" "[print_tune]"
```

Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add include/z_offset_utils.h src/ui/z_offset_utils.cpp \
        include/ui_print_tune_overlay.h src/ui/ui_print_tune_overlay.cpp \
        tests/unit/test_z_offset_adjust.cpp
git commit -m "feat(z-offset): remember the chosen baby-step size across launches"
```

---

### Task 8: Per-axis homing accessor

`homed_axes` is the raw Klipper string and is already decoded three ways:
ControlsPanel owns `x_homed`/`y_homed`/`xy_homed`/`z_homed`/`all_homed`
(`src/ui/ui_panel_controls.cpp:207-245`), MotionPanel makes prefixed copies to
avoid colliding with those (`src/ui/ui_panel_motion.cpp:178-180`), and
`helix::toolhead_is_homed()` exists. The Batch 2 toolhead widget needs per-axis
state and must not become the fourth copy.

**Files:**
- Modify: `include/toolhead_homing.h`
- Modify: `src/printer/toolhead_homing.cpp`
- Test: `tests/unit/test_toolhead_homing.cpp` (create if absent)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `enum class helix::Axis { X, Y, Z };` and
  `[[nodiscard]] bool axis_is_homed(const PrinterState& ps, Axis axis);`
  The Batch 2 toolhead widget consumes this.

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/printer_state.h"
#include "../../include/toolhead_homing.h"
#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "axis_is_homed reads each axis independently",
                 "[toolhead_homing]") {
    helix::PrinterState ps;
    ps.init_subjects(false); // false = skip XML registration (test default)

    lv_subject_copy_string(ps.get_homed_axes_subject(), "xy");

    REQUIRE(helix::axis_is_homed(ps, helix::Axis::X) == true);
    REQUIRE(helix::axis_is_homed(ps, helix::Axis::Y) == true);
    REQUIRE(helix::axis_is_homed(ps, helix::Axis::Z) == false);
    REQUIRE(helix::toolhead_is_homed(ps) == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "an empty homed_axes means nothing is homed",
                 "[toolhead_homing]") {
    helix::PrinterState ps;
    ps.init_subjects(false); // false = skip XML registration (test default)

    lv_subject_copy_string(ps.get_homed_axes_subject(), "");

    REQUIRE(helix::axis_is_homed(ps, helix::Axis::X) == false);
    REQUIRE(helix::axis_is_homed(ps, helix::Axis::Z) == false);
}
```

Match however existing tests construct `PrinterState` — prefer `XMLTestFixture`
if it owns one, per the test-isolation notes in CLAUDE.md.

- [ ] **Step 2: Run the test to verify it fails**

```bash
make test 2>&1 | tail -20
```

Expected: compile failure, no `axis_is_homed` in namespace `helix`.

- [ ] **Step 3: Add the declaration**

In `include/toolhead_homing.h`, before `toolhead_is_homed`:

```cpp
enum class Axis { X, Y, Z };

/**
 * @brief Whether one axis reports homed.
 *
 * Reads the same live `homed_axes` subject as toolhead_is_homed(). Use this
 * rather than ControlsPanel's x_homed/y_homed/z_homed subjects — those are owned
 * by that panel's SubjectManager, and MotionPanel already had to make prefixed
 * copies to avoid colliding with them.
 *
 * @warning Main thread only. This reads an LVGL subject.
 */
[[nodiscard]] bool axis_is_homed(const PrinterState& ps, Axis axis);
```

- [ ] **Step 4: Implement it**

In `src/printer/toolhead_homing.cpp`, matching how `toolhead_is_homed` at
`:18-27` reads the subject:

```cpp
bool axis_is_homed(const PrinterState& ps, Axis axis) {
    // Same const_cast as toolhead_is_homed() directly above: the subject
    // accessors are all non-const, but this only reads the string value.
    const char* axes =
        lv_subject_get_string(const_cast<PrinterState&>(ps).get_homed_axes_subject());
    if (axes == nullptr) {
        return false;
    }
    const char c = (axis == Axis::X) ? 'x' : (axis == Axis::Y) ? 'y' : 'z';
    return std::string(axes).find(c) != std::string::npos;
}
```

**`get_homed_axes_subject()` is non-const** (`include/printer_state.h:1012`), so
calling it on a `const PrinterState&` does not compile without the cast. The
existing `toolhead_is_homed` at `src/printer/toolhead_homing.cpp:17-31` uses
exactly this `const_cast` with a comment explaining why it is safe — follow it
rather than inventing a different approach, and match its `std::string::find`
idiom over `strchr` for consistency with the function directly above.

- [ ] **Step 5: Run the tests**

```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[toolhead_homing]"
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/toolhead_homing.h src/printer/toolhead_homing.cpp \
        tests/unit/test_toolhead_homing.cpp
git commit -m "feat(toolhead): read per-axis homing from the shared helper"
```

---

### Task 9: Fix the misleading widget factory signature in the docs

`docs/devel/ARCHITECTURE.md:452-540` shows `register_widget_factory` taking a
zero-argument lambda. The real `WidgetFactory` takes
`const std::string& instance_id` (`include/panel_widget_registry.h:17`). Anyone
following the doc writes code that does not compile. Its widget table also lists
8 widgets against 37 shipped, and Batch 1-3 will add 7 more.

**Files:**
- Modify: `docs/devel/ARCHITECTURE.md:452-540`

**Interfaces:** none.

- [ ] **Step 1: Read the current section and the real signature**

```bash
sed -n '452,540p' docs/devel/ARCHITECTURE.md
sed -n '10,70p' include/panel_widget_registry.h
```

- [ ] **Step 2: Correct the factory example**

Update the code sample so the lambda takes `const std::string& instance_id` and
matches the real `WidgetFactory` typedef. Copy a working call from
`src/ui/panel_widgets/temp_graph_widget.cpp:26-32` rather than composing one.

- [ ] **Step 3: Replace the stale widget table with a pointer**

The 8-entry table at `:458-467` cannot be kept accurate by hand against the 37
widgets currently in `s_widget_defs`, and Batches 1-3 will add seven more. It is
also wrong on specifics: it lists a `TemperatureWidget` class that does not exist —
the real one is `HeaterTempWidget`, used as three config-driven instances
(`temperature`, `bed_temperature`, `chamber_temperature`).

Replace the table with one sentence pointing at `s_widget_defs`
(`src/ui/panel_widget_registry.cpp:66-108`) as the authoritative list, and keep a
short prose description of what a widget is.

- [ ] **Step 3b: Fix the stale description of the home panel itself**

`:454` says the home panel "exposes a **row** of modular widgets". It has not been
a row for some time — it is a multi-page grid (up to 8 pages) with long-press edit
mode, drag-to-move, and edge-drag resize, laid out on half-cell tracks. Correct
that sentence. Do not attempt to document the whole grid system here; point at
`docs/devel/LAYOUT_SYSTEM.md` for the detail.

Still do **not** touch `docs/devel/LAYOUT_SYSTEM.md` itself — its § "Widget span
authoring" is stale in cell units, but that is Task 13's scope on the parent
`fix/grid-cell-metrics` branch and editing it here creates a conflict.

**Do not touch `docs/devel/LAYOUT_SYSTEM.md`.** Its § "Widget span authoring" is
also stale (cell units, not tracks) but is squarely Task 13's scope on the parent
`fix/grid-cell-metrics` branch. Editing it here creates a conflict.

- [ ] **Step 4: Commit**

```bash
git add docs/devel/ARCHITECTURE.md
git commit -m "docs(architecture): correct the panel widget factory signature"
```

---

## Verification before declaring Batch 0 done

- [ ] Full suite, not piped through a filter:

```bash
make test-run > /tmp/batch0-suite.log 2>&1; echo "EXIT=$?"
grep -nE "FAILED|error:|Segmentation" /tmp/batch0-suite.log | head -40
grep -nE "assertions? in .* test cases" /tmp/batch0-suite.log | tail -3
```

A SIGTERM in the log is usually another session's `pkill`, not a regression —
Catch2 counts it as FAILED. Check for that before hunting one.

- [ ] Confirm the app still builds and runs:

```bash
make -j16 > /tmp/batch0-app.log 2>&1; echo "EXIT=$?"
export HELIX_SOCK=/tmp/helix-home-widgets.sock
export HELIX_CONFIG_DIR=/tmp/helix-config-home-widgets
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv \
  --remote-socket "$HELIX_SOCK" > /tmp/helix-home-widgets.log 2>&1 &
sleep 5
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate controls
./build/bin/helix-screen ctl -s "$HELIX_SOCK" text controls_z_offset
grep -c "No subject was found" /tmp/helix-home-widgets.log || true
```

Expect `active_screen` to confirm the navigation actually happened, and zero
`No subject was found` lines. **Kill the instance afterward by its pinned socket**,
never with a broad `pkill helix-screen` — other sessions have instances running:

```bash
./build/bin/helix-screen ctl -s "$HELIX_SOCK" quit || pkill -f "$HELIX_SOCK"
pgrep -xl helix-screen
```

- [ ] Then write the Batch 1 plan (speed/flow, Z-offset, and dryer widgets, plus
  spec item D6's dryer subject writers).
