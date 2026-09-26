# Motion Panel: Settings and the Z Clamp — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make jog speeds and jog step distances user-configurable, and stop Z jogs
driving past the axis limits.

**Architecture:** Three existing single-source-of-truth points are pointed at settings
instead of constants: `send_jog_move`'s `constexpr` feedrates, `get_jog_mode_distances`'s
static table, and (for the clamp) the `clamp_jog_delta` call that X and Y already make but
Z never did. A new Settings → Printing → Motion overlay follows the row-descriptor pattern
in `ui_settings_machine_limits.cpp`. No new subsystems.

**Tech Stack:** C++17, LVGL 9.5, helix-xml, Catch2 (amalgamated), spdlog, pure Makefile.

**Spec:** `docs/devel/plans/2026-09-20-motion-panel-enhancements-design.md` (items 0, 1, 2,
the clamp half of 4, and the settings shortcut)

## Global Constraints

- **Never `git add` in this tree.** Commit pathspecs directly: `git commit -- <paths>`.
  `git add` then `git commit` is not atomic and a peer committing during your hook takes
  your staged files. See CLAUDE.md § Sharing This Tree.
- **Claim the tree before your first edit:** `scripts/helix-claim take worktree:<tree>`.
  A refused take must stop you, not be paired with an unconditional release.
- **spdlog only.** No `printf`, `cout`, or `LV_LOG_*`.
- **SPDX headers** on every new source file: `// SPDX-License-Identifier: GPL-3.0-or-later`
- **No comment archaeology.** Comments describe the code as it is now. No commit SHAs, no
  "used to", no narrated issue history. Bare issue refs like `(#865)` are fine.
- **New user-facing strings** must be wrapped in `lv_tr("...")` as a **literal** (the
  extractor scans for literals), then `make translation-sync && make translations`, then
  the generated `ui_xml/translations/*.xml` staged with the commit.
- **Citations in docs** use `path#symbol`, never a line number.
- **Check for a running build before compiling:** `pgrep -x -d' ' 'make|clang++|cc1plus'`
  and `free -h`. Never `pgrep -f`.
- **Defaults must preserve today's behaviour exactly.** Every setting added here defaults
  to the value currently hardcoded, so no printer changes behaviour on upgrade.
- **`CURRENT_CONFIG_VERSION` does not move.** These keys are additive and read their
  default when absent. Per `docs/devel/CONFIG_MIGRATION.md`, `get_default_config()` must
  still reflect them because fresh installs skip migrations.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `ui_xml/header_bar.xml` | Action button slot 2 gains slot 1's prop set | 1 |
| `ui_xml/micro/header_bar.xml` | Same change, parity-gated | 1 |
| `ui_xml/overlay_panel.xml` | Forwards the new props | 1 |
| `include/ui_panel_motion.h` | Per-axis edge latch; `get_jog_mode_distances` declaration only | 2, 5 |
| `src/ui/ui_panel_motion.cpp` | Z clamp, latch fold, settings reads, distance table | 2, 5, 6 |
| `include/settings_manager.h` | Jog speed + step distance accessors and subjects | 3, 4 |
| `src/system/settings_manager.cpp` | Their implementations and init | 3, 4 |
| `src/system/config.cpp` | `get_default_config()` entries | 3, 4 |
| `ui_xml/motion_settings_overlay.xml` | The new settings screen | 7 |
| `src/ui/ui_settings_motion.cpp` | Its controller | 7 |
| `include/ui_settings_motion.h` | Its interface | 7 |
| `ui_xml/settings_printing_overlay.xml` | Row that opens it | 7 |
| `ui_xml/motion_panel.xml` | The cog in the header | 8 |
| `tests/unit/test_motion_jog_clamp.cpp` | Clamp and latch behaviour | 2 |
| `tests/unit/test_settings_manager_motion.cpp` | Settings round-trip and clamping | 3, 4 |

---

### Task 1: `header_bar` action button slot 2 parity

Slot 2 currently takes only text, background colour and callback, and hardcodes
`style_min_width="90"`, which shapes it as a text button. An icon-only cog there would
render as a 90px-wide circle on a 22px micro header. Task 8 needs that cog.

**Files:**
- Modify: `ui_xml/header_bar.xml`
- Modify: `ui_xml/micro/header_bar.xml`
- Modify: `ui_xml/overlay_panel.xml`

**Interfaces:**
- Consumes: nothing
- Produces: `action_button_2_icon`, `action_button_2_icon_size`,
  `hide_action_button_2_icon`, `hide_action_button_2_text`, `action_button_2_icon_color`,
  `action_button_2_radius`, `action_button_2_width`, `action_button_2_pad`,
  `action_button_2_min_width`, `action_button_2_hidden_subject`,
  `action_button_2_disabled_subject` — all with defaults matching today's rendering.

- [ ] **Step 1: Capture the current rendering of both existing consumers**

XML is loaded at runtime, so no rebuild is needed to look at these.

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
echo $! > /tmp/helix-$TREE.pid
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate spoolman
./build/bin/helix-screen ctl -s "$HELIX_SOCK" geom action_button_2
```

Record the width, height and x/y. This is the number Step 5 must still produce.

- [ ] **Step 2: Add the props to `ui_xml/header_bar.xml`**

In the `<api>` block, beside the existing `action_button_2_*` props:

```xml
    <prop name="action_button_2_icon" type="string" default=""/>
    <prop name="action_button_2_icon_size" type="string" default="sm"/>
    <prop name="hide_action_button_2_icon" type="string" default="false"/>
    <prop name="hide_action_button_2_text" type="string" default="false"/>
    <prop name="action_button_2_icon_color" type="string" default="#text"/>
    <prop name="action_button_2_radius" type="string" default="#border_radius"/>
    <prop name="action_button_2_width" type="string" default="content"/>
    <prop name="action_button_2_pad" type="string" default="#space_sm"/>
    <!-- 90 is the width the text-shaped slot has always had; an icon-only
         consumer passes a smaller value so the button reads as a circle. -->
    <prop name="action_button_2_min_width" type="string" default="90"/>
    <prop name="action_button_2_hidden_subject" type="string" default=""/>
    <prop name="action_button_2_disabled_subject" type="string" default=""/>
```

Then replace the `action_button_2` view markup with:

```xml
    <ui_button name="action_button_2"
               width="$action_button_2_width" height="$action_button_height"
               style_min_width="$action_button_2_min_width"
               style_max_height="#header_button_height" style_radius="$action_button_2_radius"
               style_bg_color="$action_button_2_bg_color" hidden="$hide_action_button_2"
               icon="$action_button_2_icon" icon_size="$action_button_2_icon_size"
               text="$action_button_2_text" translation_tag="$action_button_2_text_tag">
      <bind_state_if_eq subject="$action_button_2_disabled_subject" state="disabled" ref_value="1"/>
      <bind_flag_if_eq subject="$action_button_2_hidden_subject" flag="hidden" ref_value="1"/>
      <event_cb trigger="clicked" callback="$action_button_2_callback"/>
    </ui_button>
```

- [ ] **Step 3: Make the identical change in `ui_xml/micro/header_bar.xml`**

Apply the same `<api>` additions and the same view markup. The micro variant is a
separate file and `check_variant_parity.py` fails the build if the two drift.

- [ ] **Step 4: Forward the props in `ui_xml/overlay_panel.xml`**

`overlay_panel` is a pass-through wrapper. Beside its existing
`action_button_2_text="$action_button_2_text"` forwarding, add one line per new prop,
and declare each in `overlay_panel`'s own `<api>` with the same defaults. A prop that is
declared but not forwarded silently does nothing.

- [ ] **Step 5: Verify the existing consumers are pixel-identical**

```bash
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate spoolman
./build/bin/helix-screen ctl -s "$HELIX_SOCK" geom action_button_2
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate settings
```

Expected: byte-identical geometry to Step 1. If the width changed, a default is wrong.

- [ ] **Step 6: Run the parity gate**

```bash
make t F='[xml]'
```

Expected: PASS, including `check_variant_parity.py`.

- [ ] **Step 7: Commit**

```bash
git commit -- ui_xml/header_bar.xml ui_xml/micro/header_bar.xml ui_xml/overlay_panel.xml \
  -m "refactor(xml): header_bar action button slot 2 reaches parity with slot 1"
```

Body should note that every new prop defaults to today's behaviour and that the two
existing consumers were verified unchanged with `ctl geom`.

---

### Task 2: Clamp Z jogs and fold the edge-warning latches

X and Y clamp against `AxisBounds` in `MotionPanel::on_jog`. `on_motion_z_button`
dispatches straight through, so a Z jog past the limit is refused by Klipper and surfaces
one error toast per attempt. The two existing `*_edge_warned_` latches become three
copies of one rule unless they are folded now.

**Files:**
- Modify: `include/ui_panel_motion.h`
- Modify: `src/ui/ui_panel_motion.cpp`
- Create: `tests/unit/test_motion_jog_clamp.cpp`

**Interfaces:**
- Consumes: `helix::clamp_jog_delta(double current, double uncommitted, double delta,
  double min, double max)` from `include/jog_coalescer.h`; `helix::AxisBounds` from
  `include/printer_motion_state.h`; `helix::axis_is_homed(const PrinterState&, Axis)` from
  `include/toolhead_homing.h`; `Axis` from `include/axis.h`.
- Produces: `double MotionPanel::clamp_axis_and_warn(Axis axis, double current,
  double uncommitted, double delta, float min, float max)` — returns the clamped delta,
  `0.0` when fully blocked, and owns the per-axis warn latch.

- [ ] **Step 1: Write the failing test**

The clamp rule is pure, so it is tested through `clamp_jog_delta` directly plus a
bed-moves sign case. Create `tests/unit/test_motion_jog_clamp.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/jog_coalescer.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("Z jog clamps at the axis maximum", "[motion_clamp]") {
    // At Z=248 on a 250mm axis, a +10 request may only travel 2.
    const double allowed = helix::clamp_jog_delta(248.0, 0.0, 10.0, 0.0, 250.0);
    REQUIRE(allowed == Catch::Approx(2.0));
}

TEST_CASE("Z jog clamps at the axis minimum", "[motion_clamp]") {
    const double allowed = helix::clamp_jog_delta(1.0, 0.0, -10.0, 0.0, 250.0);
    REQUIRE(allowed == Catch::Approx(-1.0));
}

TEST_CASE("Z jog at the limit is fully blocked", "[motion_clamp]") {
    const double allowed = helix::clamp_jog_delta(250.0, 0.0, 10.0, 0.0, 250.0);
    REQUIRE(std::abs(allowed) <= helix::AxisMove::EPSILON_MM);
}

TEST_CASE("Z jog accounts for an uncommitted pending move", "[motion_clamp]") {
    // 240 now, 8mm already queued, so only 2 of a further 10 may go.
    const double allowed = helix::clamp_jog_delta(240.0, 8.0, 10.0, 0.0, 250.0);
    REQUIRE(allowed == Catch::Approx(2.0));
}

TEST_CASE("A bed-moves printer clamps the gcode-space delta, not the button delta",
          "[motion_clamp]") {
    // On a bed-moves printer the UP button produces gcode Z-. At Z=1 that must
    // clamp to -1, NOT to +2 as it would if the clamp ran before the inversion.
    const double button_delta = 10.0;   // "up"
    const double gcode_delta = -button_delta; // after bed_moves_ inversion
    const double allowed = helix::clamp_jog_delta(1.0, 0.0, gcode_delta, 0.0, 250.0);
    REQUIRE(allowed == Catch::Approx(-1.0));
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
make t F='[motion_clamp]'
```

Expected: FAIL at compile or link, because `test_motion_jog_clamp.cpp` is new and the
tag matches nothing yet. If the build system globs `tests/unit/*.cpp` these compile and
pass immediately, since `clamp_jog_delta` already exists — that is expected and fine.
These tests pin the contract the production change must honour. **The behavioural
regression test is Step 6.**

- [ ] **Step 3: Replace the two latches with a per-axis array**

In `include/ui_panel_motion.h`, delete:

```cpp
    bool x_edge_warned_ = false; // dedupe "blocked at bed edge" warnings
    bool y_edge_warned_ = false;
```

and add, with the helper declaration:

```cpp
    /// Per-axis "blocked at limit" latch, indexed by Axis. Rows arriving while
    /// already blocked must not each raise a toast, and hold-to-repeat makes
    /// that a flood rather than a nuisance.
    bool edge_warned_[3] = {false, false, false};

    /// Clamp one axis against its bounds and raise at most one warning per
    /// approach. Returns the permitted delta, 0.0 when fully blocked.
    double clamp_axis_and_warn(Axis axis, double current, double uncommitted, double delta,
                               float min, float max);
```

- [ ] **Step 4: Implement the helper in `src/ui/ui_panel_motion.cpp`**

```cpp
double MotionPanel::clamp_axis_and_warn(Axis axis, double current, double uncommitted,
                                        double delta, float min, float max) {
    const double allowed =
        helix::clamp_jog_delta(current, uncommitted, delta, static_cast<double>(min),
                               static_cast<double>(max));
    const auto idx = static_cast<size_t>(axis);

    if (std::abs(allowed) > helix::AxisMove::EPSILON_MM) {
        edge_warned_[idx] = false;
        return allowed;
    }

    if (!edge_warned_[idx]) {
        // Three literals rather than a built string: the translation extractor
        // scans for lv_tr() literals and cannot see a runtime-assembled key.
        switch (axis) {
        case Axis::X:
            NOTIFY_WARNING(lv_tr("X jog blocked at bed edge"));
            break;
        case Axis::Y:
            NOTIFY_WARNING(lv_tr("Y jog blocked at bed edge"));
            break;
        case Axis::Z:
            NOTIFY_WARNING(lv_tr("Z jog blocked at axis limit"));
            break;
        }
        edge_warned_[idx] = true;
    }
    return 0.0;
}
```

Replace the inline X and Y blocks in `on_jog` with calls to it, and reset the array in
`on_activate` where `x_edge_warned_` / `y_edge_warned_` are reset today.

- [ ] **Step 5: Apply the clamp to Z, after the inversion**

In `on_motion_z_button`, the `bed_moves_` inversion already happens before
`dispatch_jog`. Insert the clamp between them, so it acts on the G-code-space delta:

```cpp
    if (bed_moves_) {
        distance = -distance;
        spdlog::debug("[{}] Bed-moves printer: inverted Z direction for bed movement",
                      get_name());
    }

    // Bounds are in gcode space, so this must follow the inversion above.
    const auto bounds = get_printer_state().get_axis_bounds();
    if (bounds.has_z && helix::axis_is_homed(get_printer_state(), Axis::Z)) {
        distance = clamp_axis_and_warn(Axis::Z, current_z_, jog_coalescer_.uncommitted_z(),
                                       distance, bounds.z_min, bounds.z_max);
        if (distance == 0.0) {
            return;
        }
    }

    spdlog::debug("[{}] Z jog: {:+.2f}mm (bed_moves={})", get_name(), distance, bed_moves_);
    dispatch_jog({0.0, 0.0, distance});
```

- [ ] **Step 6: Add the regression test that fails without the production change**

Append to `tests/unit/test_motion_jog_clamp.cpp`:

```cpp
#include "../../include/ui_panel_motion.h"

TEST_CASE("MotionPanel warns once per approach, then again after retreating",
          "[motion_clamp]") {
    MotionPanel panel;
    // Blocked: first call warns, second is silent.
    REQUIRE(panel.clamp_axis_and_warn(Axis::Z, 250.0, 0.0, 10.0, 0.0f, 250.0f) == 0.0);
    REQUIRE(panel.clamp_axis_and_warn(Axis::Z, 250.0, 0.0, 10.0, 0.0f, 250.0f) == 0.0);
    // Moving away clears the latch.
    REQUIRE(panel.clamp_axis_and_warn(Axis::Z, 100.0, 0.0, -10.0, 0.0f, 250.0f)
            == Catch::Approx(-10.0));
    // Approaching again warns again.
    REQUIRE(panel.clamp_axis_and_warn(Axis::Z, 250.0, 0.0, 10.0, 0.0f, 250.0f) == 0.0);
}
```

`clamp_axis_and_warn` must be public, or the test declared a friend. Prefer making it
public: it is a pure-ish decision function and testing it directly is the point.

- [ ] **Step 7: Run the tests**

```bash
make t F='[motion_clamp]'
```

Expected: PASS, all six cases.

- [ ] **Step 8: Sync translations for the new string**

`"Z jog blocked at axis limit"` is new.

```bash
make translation-sync
make translations
```

- [ ] **Step 9: Prove the tests can fail**

```bash
make mutate-diff
```

Expected: reverting the clamp hunk in `on_motion_z_button` turns `[motion_clamp]` red.
Name the surviving or killed mutation in the commit body. A green suite is not evidence.

- [ ] **Step 10: Commit**

```bash
git commit -- include/ui_panel_motion.h src/ui/ui_panel_motion.cpp \
  tests/unit/test_motion_jog_clamp.cpp \
  ui_xml/translations/translations.xml ui_xml/translations \
  -m "fix(motion): clamp Z jogs to the axis limits and warn once per approach"
```

Body: one paragraph noting the clamp runs after the `bed_moves_` inversion because bounds
are in G-code space, that the two per-axis latches folded into one indexed form, and the
mutation that proved the test.

---

### Task 3: Jog speed settings

**Files:**
- Modify: `include/settings_manager.h`
- Modify: `src/system/settings_manager.cpp`
- Modify: `src/system/config.cpp`
- Create: `tests/unit/test_settings_manager_motion.cpp`

**Interfaces:**
- Consumes: nothing
- Produces: `int SettingsManager::get_jog_speed_xy() const` and `get_jog_speed_z()`,
  both **mm/min**; `void set_jog_speed_xy(int mm_per_min)` and `set_jog_speed_z(int)`,
  both clamping to `[60, 60000]`; subjects `settings_jog_speed_xy` and
  `settings_jog_speed_z`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_settings_manager_motion.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/settings_manager.h"

#include "../catch_amalgamated.hpp"
#include "../helix_test_fixture.h"

TEST_CASE_METHOD(HelixTestFixture, "Jog speeds default to today's hardcoded values",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    REQUIRE(s.get_jog_speed_xy() == 6000);
    REQUIRE(s.get_jog_speed_z() == 600);
}

TEST_CASE_METHOD(HelixTestFixture, "Jog speeds round-trip", "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.set_jog_speed_z(1500);
    REQUIRE(s.get_jog_speed_z() == 1500);
}

TEST_CASE_METHOD(HelixTestFixture, "Jog speeds clamp to a sane range", "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.set_jog_speed_xy(0);
    REQUIRE(s.get_jog_speed_xy() == 60);
    s.set_jog_speed_xy(999999);
    REQUIRE(s.get_jog_speed_xy() == 60000);
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
make t F='[settings_motion]'
```

Expected: FAIL, compile error, `get_jog_speed_xy` is not a member.

- [ ] **Step 3: Declare the accessors and subjects**

In `include/settings_manager.h`, beside the extrude-speed block:

```cpp
    /** @brief Get XY jog feedrate in mm/min (default 6000, range 60-60000) */
    int get_jog_speed_xy() const;
    /** @brief Set XY jog feedrate in mm/min (clamped 60-60000, persisted) */
    void set_jog_speed_xy(int mm_per_min);
    lv_subject_t* subject_jog_speed_xy() {
        return &jog_speed_xy_subject_;
    }

    /** @brief Get Z jog feedrate in mm/min (default 600, range 60-60000) */
    int get_jog_speed_z() const;
    /** @brief Set Z jog feedrate in mm/min (clamped 60-60000, persisted) */
    void set_jog_speed_z(int mm_per_min);
    lv_subject_t* subject_jog_speed_z() {
        return &jog_speed_z_subject_;
    }
```

and in the members section:

```cpp
    lv_subject_t jog_speed_xy_subject_{};
    lv_subject_t jog_speed_z_subject_{};
```

- [ ] **Step 4: Implement them**

In `src/system/settings_manager.cpp`, in the init function beside the extrude-speed
block:

```cpp
    // Jog feedrates in mm/min. Defaults are the values MotionPanel used as
    // constants, so an upgrade changes nothing until the user asks.
    int jog_speed_xy = config->get<int>(config->df() + "motion/jog_speed_xy", 6000);
    jog_speed_xy = std::clamp(jog_speed_xy, 60, 60000);
    UI_MANAGED_SUBJECT_INT(jog_speed_xy_subject_, jog_speed_xy, "settings_jog_speed_xy",
                           subjects_);

    int jog_speed_z = config->get<int>(config->df() + "motion/jog_speed_z", 600);
    jog_speed_z = std::clamp(jog_speed_z, 60, 60000);
    UI_MANAGED_SUBJECT_INT(jog_speed_z_subject_, jog_speed_z, "settings_jog_speed_z",
                           subjects_);
```

and the accessors, following `get_extrude_speed` / `set_extrude_speed` exactly:

```cpp
int SettingsManager::get_jog_speed_xy() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&jog_speed_xy_subject_));
}

void SettingsManager::set_jog_speed_xy(int mm_per_min) {
    mm_per_min = std::clamp(mm_per_min, 60, 60000);
    spdlog::info("[SettingsManager] set_jog_speed_xy({} mm/min)", mm_per_min);

    auto old_val = std::to_string(lv_subject_get_int(&jog_speed_xy_subject_));
    lv_subject_set_int(&jog_speed_xy_subject_, mm_per_min);

    Config* config = Config::get_instance();
    config->set<int>(config->df() + "motion/jog_speed_xy", mm_per_min);
    config->save();

    TelemetryManager::instance().notify_setting_changed("jog_speed_xy", old_val,
                                                        std::to_string(mm_per_min));
}
```

Repeat for `jog_speed_z` with key `motion/jog_speed_z` and telemetry name `jog_speed_z`.

- [ ] **Step 5: Add the defaults to `get_default_config()`**

Fresh installs skip migrations and read `get_default_config()` directly, so the keys must
appear there with the same values (6000 and 600) under a `motion` section. Do **not** bump
`CURRENT_CONFIG_VERSION`: these keys are additive and existing configs read the default
when the key is absent.

- [ ] **Step 6: Run the tests**

```bash
make t F='[settings_motion]'
```

Expected: PASS, three cases.

- [ ] **Step 7: Commit**

```bash
git commit -- include/settings_manager.h src/system/settings_manager.cpp \
  src/system/config.cpp tests/unit/test_settings_manager_motion.cpp \
  -m "feat(settings): jog feedrates for XY and Z become settings"
```

---

### Task 4: Jog step distance settings

**Files:**
- Modify: `include/settings_manager.h`
- Modify: `src/system/settings_manager.cpp`
- Modify: `src/system/config.cpp`
- Modify: `tests/unit/test_settings_manager_motion.cpp`

**Interfaces:**
- Consumes: `helix::JogMode` from `include/ui_panel_motion.h`
- Produces: `float SettingsManager::get_jog_distance(helix::JogMode mode, bool outer) const`
  and `void set_jog_distance(helix::JogMode mode, bool outer, float mm)`, clamping to
  `[0.01, 200.0]`; `void reset_jog_distances()` restoring 0.1/1, 1/10, 10/50.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_settings_manager_motion.cpp`:

```cpp
#include "../../include/ui_panel_motion.h"

TEST_CASE_METHOD(HelixTestFixture, "Jog distances default to today's table",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    REQUIRE(s.get_jog_distance(helix::JogMode::Fine, false) == Catch::Approx(0.1f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Fine, true) == Catch::Approx(1.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Coarse, false) == Catch::Approx(1.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Coarse, true) == Catch::Approx(10.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, false) == Catch::Approx(10.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(50.0f));
}

TEST_CASE_METHOD(HelixTestFixture, "Jog distances round-trip and clamp",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.set_jog_distance(helix::JogMode::Turbo, true, 25.0f);
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(25.0f));
    s.set_jog_distance(helix::JogMode::Turbo, true, 0.0f);
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(0.01f));
}

TEST_CASE_METHOD(HelixTestFixture, "Reset restores the shipped distances",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.set_jog_distance(helix::JogMode::Fine, false, 5.0f);
    s.reset_jog_distances();
    REQUIRE(s.get_jog_distance(helix::JogMode::Fine, false) == Catch::Approx(0.1f));
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
make t F='[settings_motion]'
```

Expected: FAIL, `get_jog_distance` is not a member.

- [ ] **Step 3: Implement**

Six config keys under `motion/`: `fine_inner`, `fine_outer`, `coarse_inner`,
`coarse_outer`, `turbo_inner`, `turbo_outer`. Store as float. Because these are read on
every jog rather than bound to a widget, plain config reads with a small cache are enough;
they do not each need an `lv_subject_t`. The settings overlay re-reads on open.

Key naming is derived in one place so the getter and setter cannot disagree:

```cpp
namespace {
/// The one place a (mode, ring) pair becomes a config key.
std::string jog_distance_key(helix::JogMode mode, bool outer) {
    const char* mode_name = "coarse";
    switch (mode) {
    case helix::JogMode::Fine:
        mode_name = "fine";
        break;
    case helix::JogMode::Coarse:
        mode_name = "coarse";
        break;
    case helix::JogMode::Turbo:
        mode_name = "turbo";
        break;
    }
    return std::string("motion/") + mode_name + (outer ? "_outer" : "_inner");
}

float jog_distance_default(helix::JogMode mode, bool outer) {
    switch (mode) {
    case helix::JogMode::Fine:
        return outer ? 1.0f : 0.1f;
    case helix::JogMode::Coarse:
        return outer ? 10.0f : 1.0f;
    case helix::JogMode::Turbo:
        return outer ? 50.0f : 10.0f;
    }
    return outer ? 10.0f : 1.0f;
}
} // namespace
```

Getter clamps on read as well as write, so a hand-edited config cannot produce a
zero-length jog.

- [ ] **Step 4: Add the six defaults to `get_default_config()`**

Same section as Task 3. `CURRENT_CONFIG_VERSION` still does not move.

- [ ] **Step 5: Run the tests**

```bash
make t F='[settings_motion]'
```

Expected: PASS, six cases total in the file.

- [ ] **Step 6: Commit**

```bash
git commit -- include/settings_manager.h src/system/settings_manager.cpp \
  src/system/config.cpp tests/unit/test_settings_manager_motion.cpp \
  -m "feat(settings): jog step distances become settings (#865)"
```

---

### Task 5: Point `get_jog_mode_distances` at the settings

**Files:**
- Modify: `include/ui_panel_motion.h`
- Modify: `src/ui/ui_panel_motion.cpp`

**Interfaces:**
- Consumes: `SettingsManager::get_jog_distance` from Task 4
- Produces: `helix::JogModeDistances get_jog_mode_distances(helix::JogMode mode)` — now
  **by value**, declared in the header and defined in the `.cpp`.

- [ ] **Step 1: Move the definition out of the header**

`get_jog_mode_distances` is currently `inline` in `include/ui_panel_motion.h` returning a
reference to a static table, and `src/ui/ui_jog_pad.cpp` includes it. Reading settings
from a header inline would pull `SettingsManager` into every translation unit that
includes the panel header.

In the header, leave only:

```cpp
/// Inner and outer ring distances for a jog mode, in millimetres.
/// Values come from settings; the labels are formatted for display.
helix::JogModeDistances get_jog_mode_distances(helix::JogMode mode);
```

- [ ] **Step 2: Define it in `src/ui/ui_panel_motion.cpp`**

```cpp
helix::JogModeDistances get_jog_mode_distances(helix::JogMode mode) {
    auto& settings = helix::SettingsManager::instance();
    helix::JogModeDistances d{};
    d.inner = settings.get_jog_distance(mode, /*outer=*/false);
    d.outer = settings.get_jog_distance(mode, /*outer=*/true);
    format_distance_label(d.inner_label, sizeof(d.inner_label), d.inner);
    format_distance_label(d.outer_label, sizeof(d.outer_label), d.outer);
    return d;
}
```

`JogModeDistances` currently holds `const char*` labels pointing at string literals. With
runtime values those must become owned buffers, so change the struct to
`char inner_label[16]` / `char outer_label[16]` and add:

```cpp
/// Trim trailing zeros so 0.1 reads "0.1" and 10.0 reads "10", matching the
/// labels the shipped table used.
static void format_distance_label(char* buf, size_t n, float mm) {
    if (mm == std::floor(mm)) {
        std::snprintf(buf, n, "%.0f", static_cast<double>(mm));
    } else {
        std::snprintf(buf, n, "%g", static_cast<double>(mm));
    }
}
```

- [ ] **Step 3: Check both call sites still compile**

`src/ui/ui_panel_motion.cpp` and `src/ui/ui_jog_pad.cpp` both bind with
`const auto& mode_dist = get_jog_mode_distances(...)`, which extends the returned
temporary's lifetime, so neither needs editing. Confirm without a full build:

```bash
scripts/syntax_check.py src/ui/ui_panel_motion.cpp src/ui/ui_jog_pad.cpp
```

Expected: no errors.

- [ ] **Step 4: Make `send_jog_move` read the speed settings**

Replace the two `constexpr` values:

```cpp
void MotionPanel::send_jog_move(const helix::AxisMove& move) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        jog_coalescer_.on_error();
        return;
    }
    auto& settings = helix::SettingsManager::instance();
    const double xy_feedrate = settings.get_jog_speed_xy();
    const double z_feedrate = settings.get_jog_speed_z();

    api->motion().move_relative(move.dx, move.dy, move.dz, xy_feedrate, z_feedrate,
```

- [ ] **Step 5: Verify behaviour is unchanged at defaults**

```bash
make -j"$(scripts/helix-claim jobs)"
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate motion
./build/bin/helix-screen ctl -s "$HELIX_SOCK" text jog_mode_coarse
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click z_up_large
grep -E 'G0 Z|F[0-9]+' /tmp/helix-$TREE.log | tail -3
```

Expected: the emitted G-code still reads `F600` for Z and the mode labels still read
`1mm` / `10mm`. Anything else means a default drifted.

- [ ] **Step 6: Run the motion tests**

```bash
make t F='[motion_clamp],[jog_coalescer]'
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git commit -- include/ui_panel_motion.h src/ui/ui_panel_motion.cpp \
  -m "feat(motion): jog speeds and step distances come from settings"
```

Body: note that `get_jog_mode_distances` moved out of the header to keep
`SettingsManager` out of every translation unit that includes it, and that the emitted
G-code was verified unchanged at default settings.

---

### Task 6: The Settings → Motion overlay

**Files:**
- Create: `ui_xml/motion_settings_overlay.xml`
- Create: `include/ui_settings_motion.h`
- Create: `src/ui/ui_settings_motion.cpp`
- Modify: `ui_xml/settings_printing_overlay.xml`
- Modify: `src/ui/ui_settings_printing.cpp`

**Interfaces:**
- Consumes: Tasks 3 and 4's accessors; `ui_keypad_show` from
  `include/ui_component_keypad.h`
- Produces: `void show_motion_settings_overlay()` — the single opener, called by this
  task's settings row and by Task 8's header cog.

- [ ] **Step 1: Read the two exemplars before writing anything**

```bash
sed -n 1,120p src/ui/ui_settings_machine_limits.cpp   # FIELD_SPECS row-descriptor table
sed -n 280,340p src/ui/ui_overlay_retraction_settings.cpp  # tappable field into keypad
```

The overlay is two sliders (jog speed XY, jog speed Z) plus six keypad rows (the
distances) plus a reset button. Follow `FIELD_SPECS` for the sliders and the retraction
overlay for the keypad rows.

- [ ] **Step 2: Write the XML**

`ui_xml/motion_settings_overlay.xml`, extending `overlay_panel` with
`title="Motion" title_tag="Motion"`. Two `setting_slider_row`-shaped entries named
`jog_speed_xy_slider` and `jog_speed_z_slider`, six tappable value rows named
`fine_inner_row` … `turbo_outer_row`, and a `reset_distances_btn`. Every user-facing
string needs a `translation_tag`.

- [ ] **Step 3: Write the controller**

`src/ui/ui_settings_motion.cpp` with the SPDX header. Sliders display **mm/s** while the
settings store **mm/min**, so convert at the boundary and only there:

```cpp
// Settings store mm/min because that is what the motion API takes; the UI
// speaks mm/s to match Extrude Speed and the web interfaces.
static int mm_min_to_mm_s(int mm_per_min) {
    return mm_per_min / 60;
}
static int mm_s_to_mm_min(int mm_per_sec) {
    return mm_per_sec * 60;
}
```

The slider maximum comes from the printer's reported limit, so the UI cannot offer a
speed the machine rejects:

```cpp
    const auto limits = api->get_safety_limits();
    const int max_mm_s = static_cast<int>(limits.max_feedrate_mm_min / 60.0);
```

Distance rows open the keypad with coupled bounds, so inner can never exceed outer:

```cpp
    ui_keypad_config_t config = {
        .initial_value = current,
        .min_value = is_outer ? paired_inner : 0.01f,
        .max_value = is_outer ? 200.0f : paired_outer,
        .title_label = row_title,
        .unit_label = "mm",
        .allow_decimal = true,
        .allow_negative = false,
        .callback = on_distance_entered,
        .user_data = this,
    };
    ui_keypad_show(&config);
```

- [ ] **Step 4: Add the row that opens it**

In `ui_xml/settings_printing_overlay.xml`, beside `row_machine_limits`:

```xml
        <setting_action_row name="row_motion"
                            title="Motion" title_tag="Motion"
                            description="Jog speeds and step distances"
                            description_tag="Jog speeds and step distances"
                            callback="on_motion_settings_clicked"/>
```

Register `on_motion_settings_clicked` in `src/ui/ui_settings_printing.cpp`'s callback
table. It calls `show_motion_settings_overlay()` — the same function Task 8 calls. Do not
add a second opener; `on_machine_limits_clicked` is already registered separately by both
`SettingsPanel` and `PrintingSettingsOverlay`, and a third copy of that shape is what this
avoids.

- [ ] **Step 5: Drive it and verify each control**

```bash
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate settings
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click row_printing
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click row_motion
./build/bin/helix-screen ctl -s "$HELIX_SOCK" ls
./build/bin/helix-screen ctl -s "$HELIX_SOCK" text jog_speed_z_slider
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click turbo_outer_row
./build/bin/helix-screen ctl -s "$HELIX_SOCK" ls
```

Expected: the overlay lists all nine controls, and the last `ls` shows the keypad
overlay pushed on top.

- [ ] **Step 6: Verify a changed setting reaches the G-code**

```bash
./build/bin/helix-screen ctl -s "$HELIX_SOCK" set_value jog_speed_z_slider 25
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate motion
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click z_up_large
grep -E 'G0 Z' /tmp/helix-$TREE.log | tail -2
```

Expected: `F1500`, not `F600`. This is the Discord report closing.

- [ ] **Step 7: Sync translations**

```bash
make translation-sync
make translations
```

- [ ] **Step 8: Commit**

```bash
git commit -- ui_xml/motion_settings_overlay.xml include/ui_settings_motion.h \
  src/ui/ui_settings_motion.cpp ui_xml/settings_printing_overlay.xml \
  src/ui/ui_settings_printing.cpp ui_xml/translations \
  -m "feat(settings): add a Motion section for jog speeds and step distances"
```

---

### Task 7: The header cog

**Files:**
- Modify: `ui_xml/motion_panel.xml`
- Modify: `src/ui/ui_panel_motion.cpp`

**Interfaces:**
- Consumes: Task 1's `action_button_2_icon` props; Task 6's
  `show_motion_settings_overlay()`
- Produces: nothing

- [ ] **Step 1: Add the cog to the header**

In `ui_xml/motion_panel.xml`'s `header_bar`, beside the existing E-stop action button
attributes:

```xml
                hide_action_button_2="false" action_button_2_icon="cog"
                action_button_2_icon_size="sm" hide_action_button_2_text="true"
                action_button_2_bg_color="#elevated_bg"
                action_button_2_radius="9999" action_button_2_width="#button_height_sm"
                action_button_2_min_width="0"
                action_button_2_callback="on_motion_settings_clicked"
```

`action_button_2_min_width="0"` is what Task 1's prop exists for: without it the slot
keeps its 90px text width and the cog renders as a stadium rather than a circle.

- [ ] **Step 2: Hide it on the two smallest tiers**

The micro header caps buttons at 22px and the title already flex-grows, so the cog is not
worth the width there. Add inside the `header_bar` element:

```xml
      <bind_flag_if_eq subject="ui_breakpoint" flag="hidden" ref_value="0"/>
```

bound to the action button 2 slot via `action_button_2_hidden_subject`, using the same
breakpoint mechanism `motion_panel.xml` already uses for its layout branches.

- [ ] **Step 3: Register the callback in `MotionPanel`**

```cpp
    lv_xml_register_event_cb(nullptr, "on_motion_settings_clicked",
                             on_motion_settings_clicked);
```

```cpp
static void on_motion_settings_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionPanel] on_motion_settings_clicked");
    (void)e;
    show_motion_settings_overlay();
    LVGL_SAFE_EVENT_CB_END();
}
```

- [ ] **Step 4: Verify it opens and back returns to Motion**

XML is loaded at runtime, but the callback registration is C++, so rebuild first.

```bash
make -j"$(scripts/helix-claim jobs)"
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate motion
./build/bin/helix-screen ctl -s "$HELIX_SOCK" geom action_button_2
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click action_button_2
./build/bin/helix-screen ctl -s "$HELIX_SOCK" current
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click header_back
./build/bin/helix-screen ctl -s "$HELIX_SOCK" current
```

Expected: `geom` reports a square button (width equals height, so the radius reads as a
circle); `current` after the click is the motion settings overlay; `current` after back is
the **motion panel**, not the settings tree.

- [ ] **Step 5: Run the full gate**

```bash
make full-test-run
```

Expected: PASS. Nothing else runs the bats suite locally, and Task 1 touched shared XML.

- [ ] **Step 6: Commit**

```bash
git commit -- ui_xml/motion_panel.xml src/ui/ui_panel_motion.cpp \
  -m "feat(motion): add a settings shortcut to the panel header"
```

- [ ] **Step 7: Release the claim and clean up**

```bash
kill "$(cat /tmp/helix-$TREE.pid)"   # never pkill: the name is shared
scripts/helix-claim release worktree:"$TREE"
```

---

## Self-Review

**Spec coverage.** Item 0 is Task 1. Item 1 is Tasks 3 and 5. Item 2 is Tasks 4 and 5.
The clamp half of item 4 is Task 2. The settings shortcut is Tasks 6 and 7. Items 3, 5, 6,
7 and the repeat half of item 4 are deliberately **not** in this plan; they are plans 2
and 3.

**Known gaps to resolve during execution, not silently:**

- Task 6 Step 3 assumes an `api->get_safety_limits()` accessor reaching
  `SafetyLimits::max_feedrate_mm_min`. `is_safe_feedrate` reads it from
  `safety_limits_` inside the motion API. If no public accessor exists, add one rather
  than duplicating the max-velocity read, and say so in the commit.
- Task 6 Step 2 names `setting_slider_row`. Confirm the actual component name from
  `ui_xml/machine_limits_overlay.xml` before writing; the slider rows there are the
  authority.
- Task 2's `Axis` enum is assumed to be indexable 0/1/2. Verify in `include/axis.h`; if it
  is not a plain contiguous enum, switch the latch to a small `std::array` keyed by an
  explicit mapping rather than casting.

**Type consistency.** `get_jog_speed_xy` / `get_jog_speed_z` return `int` mm/min
throughout. `get_jog_distance(JogMode, bool outer)` returns `float` mm throughout.
`clamp_axis_and_warn` returns `double` and takes `float` bounds, matching `AxisBounds`
(float) and `clamp_jog_delta` (double) at the one conversion point inside the helper.
`get_jog_mode_distances` returns by value in every reference after Task 5.
