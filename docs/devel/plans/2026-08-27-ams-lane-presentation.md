# AMS Lane Presentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace four hand-written, drifted copies of the AMS lane presentation rule with one pure classifier feeding two graphic widgets wrapped in declarative XML chrome.

**Architecture:** C++ classifies a lane into `{Present, Ghosted, Empty}` and computes its fill level — both pure, no LVGL. That state lands on a per-lane subject. Two registered widgets (`ams_lane_bar`, `ams_spool`) take a `slot_index`, resolve their own subjects, and draw the graphic. Everything around the graphic — labels, badges, ghost styling, the Empty branch, the per-lane loop — is XML bound to the per-lane subjects `AmsState` already registers.

**Tech Stack:** C++17, LVGL 9.5, helix-xml (our fork), Catch2 v3, pure Makefile.

**Spec:** `docs/devel/plans/2026-08-27-ams-lane-presentation-design.md` — read it first. The plan argues from the spec.

## Global Constraints

- Worktree `.worktrees/ams-empty-lane-rule`, branch `feature/ams-empty-lane-rule`. Never work on `main`.
- Build: `make -j4 test` (tests), `make -j4` (app). Never pipe `make` through `tail` — the exit code becomes the filter's.
- Check for concurrent builds first: `pgrep -x -d' ' 'make|cc1plus'`. Never `pgrep -f`.
- Run `helix-tests` from the repo root or you get ~336 fake failures.
- Catch2 ANDs tags in one string; OR needs commas: `"[a],[b]"`.
- SPDX header on every new file: `// SPDX-License-Identifier: GPL-3.0-or-later`, preceded by `// Copyright (C) 2025-2026 356C LLC`.
- spdlog only. No `printf`/`cout`/`LV_LOG_*`.
- Design tokens only — `theme_manager_get_color("...")`, `#space_md` in XML. No `lv_color_hex()` literals in UI code.
- The imperative-UI gate ratchets: `python3 scripts/check_imperative_ui.py` must not report more than 379. This work should lower it.
- Let the pre-commit hook run clang-format; re-stage and commit if it rewrites.
- Never `git add -A` / `git add .` — always explicit pathspecs.

---

## Phase 1 — Foundation (Tasks 1-3)

### Task 1: Revert the rendering-prescription approach

Both prior commits encoded the wrong seam. Reverting first puts the tree back
in a known-good state so the new design is built on the original code rather
than layered on a half-migration.

**Files:**
- Revert: commits `f578a5f82`, `3f6b76cf5`
- Delete (via revert): `include/ams_slot_presentation.h`, `tests/unit/test_ams_slot_presentation.cpp`, `tests/unit/test_ams_slot_presentation_wiring.cpp`

**Interfaces:**
- Consumes: nothing
- Produces: a tree with `SlotInfo::is_present()`-based lane rendering restored in `ui_ams_slot.cpp`, `ui_ams_mini_status.cpp`, `ui_panel_ams_overview.cpp`, `ams_drawing_utils.cpp`

- [ ] **Step 1: Revert both commits, newest first**

```bash
git revert --no-edit f578a5f82
git revert --no-edit 3f6b76cf5
```

- [ ] **Step 2: Confirm the prescription type is gone**

```bash
test ! -f include/ams_slot_presentation.h && echo "header gone"
grep -rn "resolve_slot_presentation\|SlotPresentation" src/ include/ tests/ | grep -v "docs/" || echo "no references remain"
```

Expected: "header gone" and "no references remain".

- [ ] **Step 3: Keep the widget name — it is independently useful**

The revert removes `lv_obj_set_name(..., "spool_graphic")` from
`create_spool_visual()`. Put it back; `ams_drawing_utils.cpp` still creates the
spool visual until Task 8, and the name makes the body reachable from tests and
`helix-screen ctl`. In `src/ui/ams_drawing_utils.cpp`, in `create_spool_visual()`,
after `sv.canvas = canvas;` in the 3D branch and after
`sv.color_swatch = filament_ring;` in the flat branch, add:

```cpp
lv_obj_set_name(canvas, "spool_graphic");        // 3D branch
lv_obj_set_name(filament_ring, "spool_graphic"); // flat branch
```

Then restore the name-based lookup in `tests/unit/test_ui_ams_slot.cpp`'s
`inspect_spool_state()` — the reverted version selects "the first unnamed child",
which silently walks past a named spool body onto the error indicator:

```cpp
lv_obj_t* spool_visual = lv_obj_find_by_name(spool_container, "spool_graphic");
if (!spool_visual)
    return st;
```

- [ ] **Step 4: Build and run the AMS suites**

```bash
make -j4 test 2>&1 | tail -3
./build/bin/helix-tests "[ams],[ams_slot],[ams_draw],[filament_slot_override]" 2>&1 | tail -5
```

Expected: PASS. `[filament_slot_override]` should report 1048 assertions in 166 test cases — the pre-fold baseline.

- [ ] **Step 5: Commit**

```bash
git add src/ui/ams_drawing_utils.cpp tests/unit/test_ui_ams_slot.cpp
git commit -m "refactor(ams): keep the spool_graphic name through the revert"
```

---

### Task 2: The pure lane classifier

**Files:**
- Create: `include/ams_lane_state.h`
- Test: `tests/unit/test_ams_lane_state.cpp`

**Interfaces:**
- Consumes: `SlotStatus`, `SlotInfo` from `ams_types.h`
- Produces:
  - `enum class helix::ui::LaneState { Present, Ghosted, Empty }`
  - `constexpr LaneState helix::ui::classify_lane(SlotStatus, bool has_identity)`
  - `bool helix::ui::lane_has_identity(const SlotInfo&)`
  - `float helix::ui::lane_fill_level(const SlotInfo&)`
  - `inline constexpr float helix::ui::ASSUMED_FILL_LEVEL`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_ams_lane_state.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_lane_state.cpp
 * @brief The AMS lane classifier (pure).
 *
 * Three base states. Loaded-ness and error-ness are NOT here — they ride their
 * own subjects and are applied by the chrome, because a blocked lane still has
 * filament and an active lane is still Present.
 */

#include "ams_lane_state.h"

#include "../catch_amalgamated.hpp"

using helix::ui::ASSUMED_FILL_LEVEL;
using helix::ui::classify_lane;
using helix::ui::lane_fill_level;
using helix::ui::lane_has_identity;
using helix::ui::LaneState;

namespace {
/// Every value SlotStatus can take, so "exhaustive" below is literal.
/// name_of()'s switch has no default arm, so adding a status makes the
/// compiler point here rather than letting it go untested.
constexpr SlotStatus ALL_STATUSES[] = {
    SlotStatus::UNKNOWN, SlotStatus::EMPTY,   SlotStatus::AVAILABLE,
    SlotStatus::LOADED,  SlotStatus::BLOCKED, SlotStatus::FROM_BUFFER,
};

const char* name_of(SlotStatus s) {
    switch (s) {
    case SlotStatus::UNKNOWN:     return "UNKNOWN";
    case SlotStatus::EMPTY:       return "EMPTY";
    case SlotStatus::AVAILABLE:   return "AVAILABLE";
    case SlotStatus::LOADED:      return "LOADED";
    case SlotStatus::BLOCKED:     return "BLOCKED";
    case SlotStatus::FROM_BUFFER: return "FROM_BUFFER";
    }
    return "?";
}
} // namespace

static_assert(classify_lane(SlotStatus::LOADED, false) == LaneState::Present);
static_assert(classify_lane(SlotStatus::EMPTY, true) == LaneState::Ghosted);
static_assert(classify_lane(SlotStatus::EMPTY, false) == LaneState::Empty);

TEST_CASE("Lane classification is exhaustively pinned", "[ams][lane_state]") {
    for (SlotStatus status : ALL_STATUSES) {
        for (bool identity : {false, true}) {
            INFO("status=" << name_of(status) << " identity=" << identity);
            const bool absent =
                (status == SlotStatus::EMPTY || status == SlotStatus::UNKNOWN);
            const LaneState expected = !absent  ? LaneState::Present
                                       : identity ? LaneState::Ghosted
                                                  : LaneState::Empty;
            CHECK(classify_lane(status, identity) == expected);
        }
    }
}

TEST_CASE("UNKNOWN is classified exactly as EMPTY", "[ams][lane_state]") {
    // UNKNOWN is the skeleton value every backend writes before firmware data
    // lands (ams_backend_qidi.cpp:72 and friends). Treating it as Present would
    // briefly show filament in a lane that has none; treating it as EMPTY makes
    // it inherit the identity split, so a lane we already have a material for
    // dims instead of blanking.
    for (bool identity : {false, true}) {
        INFO("identity=" << identity);
        CHECK(classify_lane(SlotStatus::UNKNOWN, identity) ==
              classify_lane(SlotStatus::EMPTY, identity));
    }
}

TEST_CASE("Any one identity field retains the lane", "[ams][lane_state]") {
    SECTION("nothing set") {
        SlotInfo s;
        CHECK_FALSE(lane_has_identity(s));
    }
    SECTION("spoolman_id alone") {
        SlotInfo s; s.spoolman_id = 7;
        CHECK(lane_has_identity(s));
    }
    SECTION("material alone") {
        SlotInfo s; s.material = "PLA";
        CHECK(lane_has_identity(s));
    }
    SECTION("brand alone") {
        SlotInfo s; s.brand = "Polymaker";
        CHECK(lane_has_identity(s));
    }
    SECTION("spool_name alone") {
        SlotInfo s; s.spool_name = "Galaxy Black #3";
        CHECK(lane_has_identity(s));
    }
    SECTION("a zero or negative spoolman id is not a handle") {
        SlotInfo s; s.spoolman_id = 0;
        CHECK_FALSE(lane_has_identity(s));
        s.spoolman_id = -1;
        CHECK_FALSE(lane_has_identity(s));
    }
}

TEST_CASE("Fill level: tracked ratio, else the assumed constant",
          "[ams][lane_state][fill]") {
    SECTION("tracked weights give the real ratio") {
        SlotInfo s;
        s.status = SlotStatus::AVAILABLE;
        s.total_weight_g = 1000.0f;
        s.remaining_weight_g = 250.0f;
        CHECK(lane_fill_level(s) == Catch::Approx(0.25f));
    }
    SECTION("present but weightless uses the shared assumed constant") {
        // The case that must NOT be a bare 1.0f duplicated per call site.
        SlotInfo s;
        s.status = SlotStatus::AVAILABLE;
        s.material = "PETG";
        CHECK(lane_fill_level(s) == Catch::Approx(ASSUMED_FILL_LEVEL));
    }
    SECTION("a ghosted lane keeps its last known fill") {
        // Deliberate reversal of a106413f6 (#1071). Safe only because the whole
        // cell is ghosted — see the wiring test in Task 5.
        SlotInfo s;
        s.status = SlotStatus::EMPTY;
        s.material = "PETG";
        s.total_weight_g = 1000.0f;
        s.remaining_weight_g = 600.0f;
        CHECK(lane_fill_level(s) == Catch::Approx(0.6f));
    }
    SECTION("a ghosted lane with no weights falls back to the assumed constant") {
        SlotInfo s;
        s.status = SlotStatus::EMPTY;
        s.material = "PETG";
        CHECK(lane_fill_level(s) == Catch::Approx(ASSUMED_FILL_LEVEL));
    }
    SECTION("an unassigned empty lane has no fill") {
        SlotInfo s;
        s.status = SlotStatus::EMPTY;
        CHECK(lane_fill_level(s) == Catch::Approx(0.0f));
    }
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
make -j4 test 2>&1 | grep -c "error:"
```

Expected: non-zero — `ams_lane_state.h` does not exist.

- [ ] **Step 3: Write the header**

Create `include/ams_lane_state.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

namespace helix::ui {

/**
 * @brief What a lane fundamentally is, for every surface that draws one.
 *
 * Three states, not five. "Loaded" and "error" are NOT here: a blocked lane
 * still has filament and an active lane is still Present, so they are
 * decorations laid over a base state, driven by their own subjects
 * (slot_active_loaded, slot.error) and applied by the chrome.
 */
enum class LaneState {
    /// Has filament. Draw it at lane_fill_level().
    Present,
    /// Ejected but still carries an identity (#1071 keeps the override).
    /// Draw it at its last known fill with the WHOLE cell dimmed.
    Ghosted,
    /// No filament and no identity. The spool rendering shows a placeholder
    /// and "Empty"; the bar rendering draws nothing and leaves the gap.
    Empty,
};

/// Fill level for a lane that has filament but no weight data. One named
/// constant instead of the bare 1.0f that used to sit in display_fill_level()
/// and three places in ui_spool_canvas.cpp.
inline constexpr float ASSUMED_FILL_LEVEL = 1.0f;

/**
 * @brief Does this lane still carry an identity after being ejected?
 *
 * Spoolman link, material, brand or spool name — deliberately NOT cleared on
 * eject (#1071), so a lane with one is "assigned, not present".
 */
[[nodiscard]] inline bool lane_has_identity(const SlotInfo& slot) {
    return slot.spoolman_id > 0 || !slot.material.empty() || !slot.brand.empty() ||
           !slot.spool_name.empty();
}

/**
 * @brief Classify a lane. Pure — testable with no display.
 *
 * UNKNOWN is treated exactly as EMPTY. It is not a steady state on any backend:
 * every SlotStatus::UNKNOWN assignment is skeleton construction before firmware
 * data arrives (ams_backend_qidi.cpp:72, ams_backend_snapmaker.cpp:263,
 * ams_backend_happy_hare.cpp:1325, ams_backend_ace.cpp:1317,
 * ams_backend_afc.cpp:4339), plus one QIDI fallback for an unrecognised value.
 * Treating it as EMPTY lets it inherit the identity split, so a lane whose
 * material is already known dims rather than blanking during startup.
 */
[[nodiscard]] constexpr LaneState classify_lane(SlotStatus status, bool has_identity) {
    const bool absent = (status == SlotStatus::EMPTY || status == SlotStatus::UNKNOWN);
    if (!absent) {
        return LaneState::Present;
    }
    return has_identity ? LaneState::Ghosted : LaneState::Empty;
}

/**
 * @brief How full to draw this lane, 0.0-1.0.
 *
 * A ghosted lane keeps its last known fill. That reverses a106413f6 (#1071),
 * where an emptied lane rendered a full-strength 75% bar and read as loaded.
 * It is safe here ONLY because Ghosted dims the entire cell — the dimming is
 * the disclaimer. Do not reuse this value without the ghost.
 */
[[nodiscard]] inline float lane_fill_level(const SlotInfo& slot) {
    if (classify_lane(slot.status, lane_has_identity(slot)) == LaneState::Empty) {
        return 0.0f;
    }
    if (slot.total_weight_g > 0.0f && slot.remaining_weight_g >= 0.0f) {
        return slot.remaining_weight_g / slot.total_weight_g;
    }
    return ASSUMED_FILL_LEVEL;
}

} // namespace helix::ui
```

- [ ] **Step 4: Run to verify it passes**

```bash
make -j4 test 2>&1 | tail -2
./build/bin/helix-tests "[lane_state]" 2>&1 | tail -4
```

Expected: PASS.

- [ ] **Step 5: Mutation-verify one branch**

Change `classify_lane`'s `absent` to `(status == SlotStatus::EMPTY)` (dropping
UNKNOWN), rebuild, and confirm "UNKNOWN is classified exactly as EMPTY" goes
RED. Echo the build exit code and `grep` the mutated line back out of the source
before believing the result — a failed build leaves a stale binary that still
passes. Then restore and `touch include/ams_lane_state.h`.

- [ ] **Step 6: Commit**

```bash
git add include/ams_lane_state.h tests/unit/test_ams_lane_state.cpp
git commit -m "feat(ams): add the pure lane classifier and fill level"
```

---

### Task 3: Per-lane `lane_state` subject

**Files:**
- Modify: `include/ams_state.h` — add `lv_subject_t slot_lane_states_[MAX_SLOTS];` beside `slot_active_loaded_` (~line 1850) and declare `get_slot_lane_state_subject(int)` beside `get_slot_status_subject` (~line 1050)
- Modify: `src/printer/ams_state.cpp` — init in the per-slot loop (~line 425), accessor beside `get_slot_status_subject`, write in the status-sync path beside `slot_statuses_`
- Test: `tests/unit/test_ams_lane_state_subject.cpp`

**Interfaces:**
- Consumes: `classify_lane`, `lane_has_identity` from Task 2
- Produces: `lv_subject_t* AmsState::get_slot_lane_state_subject(int slot_index)` holding `static_cast<int>(LaneState)`, XML-registered as `ams_slot_<n>_lane_state`

- [ ] **Step 1: Write the failing test**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_lane_state.h"
#include "ams_state.h"
#include "ams_backend_mock.h"
#include "../test_fixtures.h"
#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(XMLTestFixture, "lane_state subject tracks the backend",
                 "[ams][lane_state][subject]") {
    auto owned = std::make_unique<AmsBackendMock>(4);
    owned->set_operation_delay(0);
    auto* mock = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    // Lane 0: assigned + present. Lane 1: ejected but assigned. Lane 2: bare.
    {
        SlotInfo i0 = mock->get_slot_info(0);
        i0.material = "PLA";
        REQUIRE(mock->set_slot_info(0, i0).success());
        mock->force_slot_status(0, SlotStatus::AVAILABLE);

        SlotInfo i1 = mock->get_slot_info(1);
        i1.material = "PETG";
        REQUIRE(mock->set_slot_info(1, i1).success());
        mock->force_slot_status(1, SlotStatus::EMPTY);

        SlotInfo i2 = mock->get_slot_info(2);
        i2.material.clear(); i2.brand.clear(); i2.spool_name.clear();
        i2.spoolman_id = 0;
        REQUIRE(mock->set_slot_info(2, i2).success());
        mock->force_slot_status(2, SlotStatus::EMPTY);
    }

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    process_lvgl(20);

    auto state_of = [](int i) {
        lv_subject_t* s = AmsState::instance().get_slot_lane_state_subject(i);
        REQUIRE(s != nullptr);
        return static_cast<helix::ui::LaneState>(lv_subject_get_int(s));
    };

    CHECK(state_of(0) == helix::ui::LaneState::Present);
    CHECK(state_of(1) == helix::ui::LaneState::Ghosted);
    CHECK(state_of(2) == helix::ui::LaneState::Empty);

    AmsState::instance().clear_backends();
}

TEST_CASE_METHOD(XMLTestFixture, "lane_state subject is XML-registered per lane",
                 "[ams][lane_state][subject]") {
    AmsState::instance().init_subjects(true);
    // Name shape must match what the chrome binds to: ams_slot_<n>_lane_state.
    CHECK(lv_xml_get_subject(nullptr, "ams_slot_0_lane_state") != nullptr);
    CHECK(lv_xml_get_subject(nullptr, "ams_slot_3_lane_state") != nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "lane_state subject bounds-checks its index",
                 "[ams][lane_state][subject]") {
    AmsState::instance().init_subjects(true);
    CHECK(AmsState::instance().get_slot_lane_state_subject(-1) == nullptr);
    CHECK(AmsState::instance().get_slot_lane_state_subject(AmsState::MAX_SLOTS) == nullptr);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
make -j4 test 2>&1 | grep "error:" | head -3
```

Expected: `get_slot_lane_state_subject` is not a member of `AmsState`.

- [ ] **Step 3: Add the subject**

In `include/ams_state.h`, beside `slot_active_loaded_`:

```cpp
lv_subject_t slot_lane_states_[MAX_SLOTS]; // int: helix::ui::LaneState
```

and beside `get_slot_status_subject`:

```cpp
/// Per-lane LaneState (helix::ui::classify_lane). THE presentation input for
/// every surface that draws a lane. XML name: ams_slot_<n>_lane_state.
[[nodiscard]] lv_subject_t* get_slot_lane_state_subject(int slot_index);
```

In `src/printer/ams_state.cpp`, in the per-slot init loop beside `slot_statuses_`:

```cpp
lv_subject_init_int(&slot_lane_states_[i], static_cast<int>(helix::ui::LaneState::Empty));
subjects_.register_subject(&slot_lane_states_[i]);
if (register_xml) {
    snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_lane_state", i);
    lv_xml_register_subject(nullptr, name_buf, &slot_lane_states_[i]);
}
```

Accessor, beside `get_slot_status_subject`:

```cpp
lv_subject_t* AmsState::get_slot_lane_state_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_lane_states_[slot_index];
}
```

In the status-sync path, wherever `slot_statuses_[i]` is written, write the
classification from the same `SlotInfo`. Use the existing thread-safe setter
path — this runs off the WebSocket thread, so it must go through the same
`ui_queue_update()`/`set_*_internal()` mechanism the sibling subjects use. Do
not call `lv_subject_set_int` directly from a background thread.

```cpp
const auto lane_state = helix::ui::classify_lane(
    slot_info.status, helix::ui::lane_has_identity(slot_info));
// ... set alongside slot_statuses_[i], through the same queued path
```

Add `#include "ams_lane_state.h"` to `src/printer/ams_state.cpp`.

- [ ] **Step 4: Run to verify it passes**

```bash
make -j4 test 2>&1 | tail -2
./build/bin/helix-tests "[lane_state]" 2>&1 | tail -4
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/ams_state.h src/printer/ams_state.cpp tests/unit/test_ams_lane_state_subject.cpp
git commit -m "feat(ams): publish per-lane LaneState as a subject"
```

---

## Phase 2 — Bar rendering (Tasks 4-6)

Bar first: it is the simpler of the two renderings, it has the clearer
"draws nothing" case, and it covers two of the four call sites.

### Task 4: `ams_lane_bar` widget

**Files:**
- Create: `include/ui_ams_lane_bar.h`, `src/ui/ui_ams_lane_bar.cpp`
- Test: `tests/unit/test_ams_lane_bar.cpp`

**Interfaces:**
- Consumes: `LaneState`, `lane_fill_level`, `AmsState::get_slot_lane_state_subject`
- Produces: XML widget `ams_lane_bar` taking `slot_index`; registered by `ui_ams_lane_bar_register()`. Named children: `bar_bg`, `bar_fill`, `status_line`.

Model it on `ui_ams_slot.cpp`: `lv_xml_register_widget("ams_lane_bar", create, apply)`,
`apply` reads `slot_index`, and a `setup_observers()` resolves
`AmsState::get_slot_lane_state_subject(i)`, `get_slot_color_subject(i)`,
`get_slot_fill_subject(i)`, `get_slot_active_loaded_subject(i)` and observes
each with `observe_int_sync<lv_obj_t>` from `observer_factory.h`, passing the
`SubjectLifetime` where the accessor offers a token'd overload.

**Rendering contract, tested below:**

| LaneState | bar_fill | bar_bg border | cell opacity |
|-----------|----------|---------------|--------------|
| `Present` | visible, height = fill % | `LV_OPA_50` | `LV_OPA_COVER` |
| `Ghosted` | visible, height = fill % | `LV_OPA_50` | ghost token |
| `Empty`   | hidden | hidden | n/a — nothing drawn |

Active adds a 2px `text`-coloured border at `LV_OPA_80`. Error shows `status_line`.

- [ ] **Step 1: Write the failing test**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_lane_bar.h"
#include "ams_lane_state.h"
#include "ams_state.h"
#include "ams_backend_mock.h"
#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
lv_obj_t* make_bar(lv_obj_t* parent, int slot_index) {
    const std::string idx = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", idx.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_lane_bar", attrs));
}
bool visible(lv_obj_t* root, const char* name) {
    lv_obj_t* o = lv_obj_find_by_name(root, name);
    REQUIRE(o != nullptr);
    return !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: an Empty lane draws nothing",
                 "[ams][lane_bar]") {
    // The whole point of the bar rendering's Empty case: no fill, no outline.
    // The layout gap remains so lanes stay countable, but nothing is painted.
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Empty));

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);

    CHECK_FALSE(visible(bar, "bar_fill"));
    CHECK_FALSE(visible(bar, "bar_bg"));
    lv_obj_delete(bar);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: a Ghosted lane keeps its fill, dimmed",
                 "[ams][lane_bar]") {
    // The #1071 reversal. BOTH halves are asserted: the fill comes back AND the
    // cell is dimmed. Without the second, this is the bug a106413f6 fixed.
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Ghosted));
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), 60);

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);

    CHECK(visible(bar, "bar_fill"));
    CHECK(lv_obj_get_style_opa(bar, LV_PART_MAIN) < LV_OPA_COVER);
    lv_obj_delete(bar);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: a Present lane is full strength",
                 "[ams][lane_bar]") {
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), 60);

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);

    CHECK(visible(bar, "bar_fill"));
    CHECK(lv_obj_get_style_opa(bar, LV_PART_MAIN) == LV_OPA_COVER);
    lv_obj_delete(bar);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: decorations do not alter the base state",
                 "[ams][lane_bar]") {
    // Active and error are laid OVER a base state, not alternatives to it.
    // A blocked lane still has filament; an active lane is still Present.
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), 60);

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);
    REQUIRE(visible(bar, "bar_fill"));

    // Going active must not hide or dim the fill.
    lv_subject_set_int(AmsState::instance().get_slot_active_loaded_subject(0), 1);
    process_lvgl(20);
    CHECK(visible(bar, "bar_fill"));
    CHECK(lv_obj_get_style_opa(bar, LV_PART_MAIN) == LV_OPA_COVER);
    // ...and it does add its own mark.
    CHECK(lv_obj_get_style_border_width(lv_obj_find_by_name(bar, "bar_bg"),
                                        LV_PART_MAIN) == 2);

    lv_obj_delete(bar);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_bar: the lane_state subject drives repaint",
                 "[ams][lane_bar]") {
    // Proves the observer is wired, not just the initial apply.
    ui_ams_lane_bar_register();
    AmsState::instance().init_subjects(true);
    lv_subject_t* st = AmsState::instance().get_slot_lane_state_subject(0);
    lv_subject_set_int(st, static_cast<int>(helix::ui::LaneState::Present));
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), 60);

    lv_obj_t* bar = make_bar(test_screen(), 0);
    REQUIRE(bar != nullptr);
    process_lvgl(20);
    REQUIRE(visible(bar, "bar_fill"));

    lv_subject_set_int(st, static_cast<int>(helix::ui::LaneState::Empty));
    process_lvgl(20);
    CHECK_FALSE(visible(bar, "bar_fill"));
    lv_obj_delete(bar);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
make -j4 test 2>&1 | grep "error:" | head -3
```

Expected: `ui_ams_lane_bar.h` not found.

- [ ] **Step 3: Implement the widget**

`include/ui_ams_lane_bar.h` declares `void ui_ams_lane_bar_register();`.

The crux is one function. Everything else is plumbing copied from
`ui_ams_slot.cpp`:

```cpp
/// THE bar rendering. One switch, no opacity arithmetic at the call site.
static void apply_lane_state(LaneBarData* d, helix::ui::LaneState state) {
    if (!d || !d->bar_bg || !d->bar_fill)
        return;

    // Empty draws nothing at all. The widget stays in the layout so the lane
    // remains countable, but neither the outline nor the fill is painted.
    if (state == helix::ui::LaneState::Empty) {
        lv_obj_add_flag(d->bar_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(d->bar_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(d->root, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    lv_obj_remove_flag(d->bar_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(d->bar_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(d->bar_fill, LV_PCT(d->fill_pct));
    lv_obj_align(d->bar_fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Ghost goes on the ROOT so every child dims together. This is the whole
    // mechanism: a per-element opacity is what made the previous attempt
    // unreadable, and it is what makes reversing #1071 safe.
    lv_obj_set_style_opa(d->root,
                         state == helix::ui::LaneState::Ghosted
                             ? static_cast<lv_opa_t>(theme_manager_get_spacing("ghost_opacity"))
                             : LV_OPA_COVER,
                         LV_PART_MAIN);
}
```

**Add the `ghost_opacity` token first.** There is no opacity accessor; px-typed
tokens are read with `theme_manager_get_spacing()`, and `modal_backdrop_opacity`
(`src/generated/theme_token_table.cpp:167`) is the precedent for an opacity
carried that way. Add `ghost_opacity` (value `51`, i.e. `LV_OPA_20`) to
`ui_xml/globals.xml` and regenerate the token table.

Do this rather than writing `LV_OPA_20` inline: design tokens are mandatory, the
value is now shared by both renderings, and the XML chrome in Tasks 5, 8 and 9
needs it as `#ghost_opacity`. A literal here plus a token there is exactly the
duplication this whole rework exists to remove.

The rest of `src/ui/ui_ams_lane_bar.cpp` follows `ui_ams_slot.cpp`'s shape:

- a `LaneBarData` struct in a registry keyed by `lv_obj_t*` (see
  `register_slot_data`/`unregister_slot_data` in `ui_ams_slot.cpp`) — never
  `lv_obj_set_user_data` on a widget that may already carry payload
- `create` builds `bar_bg` with a `bar_fill` child and a `status_line`, all
  named, using `theme_manager_get_color()` and spacing tokens
- `apply` reads `slot_index`, resets observers if it changed, then calls
  `setup_observers()`
- `setup_observers()` resolves and observes the four subjects; store each
  `ObserverGuard` on the data struct and `reset()` (never `release()`) in the
  `LV_EVENT_DELETE` handler
- one `apply_lane_state(LaneBarData*, LaneState)` applies the table above,
  setting the ghost with `lv_obj_set_style_opa()` on the widget **root** so the
  dimming covers every child at once

Register it from `SubjectInitializer` alongside the other widget registrations.

- [ ] **Step 4: Run to verify it passes**

```bash
make -j4 test 2>&1 | tail -2
./build/bin/helix-tests "[lane_bar]" 2>&1 | tail -4
```

Expected: PASS.

- [ ] **Step 5: Mutation-verify the ghost**

Remove the root-opacity line from `apply_lane_state`, rebuild (echo the exit
code, grep the source), and confirm "a Ghosted lane keeps its fill, dimmed"
goes RED on the opacity assertion. Restore and `touch` the file.

- [ ] **Step 6: Commit**

```bash
git add include/ui_ams_lane_bar.h src/ui/ui_ams_lane_bar.cpp tests/unit/test_ams_lane_bar.cpp
git commit -m "feat(ams): add the ams_lane_bar widget"
```

---

### Task 5: Convert the mini-status bar mode to XML

**Files:**
- Create: `ui_xml/components/ams_lane_bar_row.xml`
- Modify: `src/ui/ui_ams_mini_status.cpp` — delete `rebuild_bars()`, `apply_slot_style()`, `SlotBarData`
- Test: extend `tests/unit/test_ui_ams_mini_status.cpp`

**Interfaces:**
- Consumes: `ams_lane_bar` widget, `ams_slot_<n>_lane_state` subjects
- Produces: bar mode rendered from XML; `SlotBarData` no longer exists

- [ ] **Step 1: Write the XML**

Create `ui_xml/components/ams_lane_bar_row.xml`:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<component>
  <!--
    One row of AMS lane bars. The <repeat> tracks ams_slot_count, so adding or
    removing a unit re-expands the row and nothing else has to know. Each bar
    resolves its own subjects from slot_index - see ui_ams_lane_bar.cpp.
  -->
  <view name="ams_lane_bar_row"
        extends="lv_obj" width="content" height="100%"
        flex_flow="row" style_flex_cross_place="end"
        style_pad_column="#space_xxs" style_pad_all="0"
        style_bg_opa="0" style_border_width="0" scrollable="false">

    <repeat count="ams_slot_count">
      <ams_lane_bar name="lane_bar_${i}" slot_index="${i}"/>
    </repeat>
  </view>
</component>
```

Confirm `ams_slot_count` is the registered XML name for the lane-count subject
(`AmsState::get_slot_count_subject()`, `ams_state.h:651`); if it registers under
a different name, use that one — `<repeat count="...">` needs the XML name, not
the accessor.

- [ ] **Step 2: Write the failing test**

Extend `test_ui_ams_mini_status.cpp` with a case that seeds a mock backend with
one ejected-but-assigned lane and one bare empty lane, renders the strip narrow
(`ui_ams_mini_status_set_width(w, 130)`), and asserts via
`UITest::find_by_name` that the ghosted lane's bar exists with a visible fill
while the bare lane's bar draws nothing. Assert the **gap** survives by checking
the container's child count still equals the lane count.

- [ ] **Step 3: Run to verify it fails**

```bash
./build/bin/helix-tests "[ams_mini]" 2>&1 | tail -5
```

- [ ] **Step 4: Replace the C++ loop**

Delete `rebuild_bars()`, `apply_slot_style()` and the `SlotBarData` array.
`sync_from_ams_state()` stops writing bar caches — the subjects already carry
everything. Bar mode becomes: instantiate `ams_lane_bar_row` once, let the
`<repeat>` track `ams_slot_count`.

- [ ] **Step 5: Run to verify it passes, and check the gate moved the right way**

```bash
make -j4 test 2>&1 | tail -2
./build/bin/helix-tests "[ams_mini],[lane_bar]" 2>&1 | tail -4
python3 scripts/check_imperative_ui.py 2>&1 | tail -1
```

Expected: PASS, and the imperative count **below** 379.

- [ ] **Step 6: Commit**

```bash
git add ui_xml/components/ams_lane_bar_row.xml src/ui/ui_ams_mini_status.cpp tests/unit/test_ui_ams_mini_status.cpp
git commit -m "refactor(ams): render mini-status bars from XML"
```

---

### Task 6: Convert the overview mini-bars

**Files:**
- Modify: `src/ui/ui_panel_ams_overview.cpp:530-556` — replace the `BarStyleParams` loop
- Modify: `src/ui/ams_drawing_utils.cpp` / `include/ui/ams_drawing_utils.h` — delete `style_slot_bar()` and `BarStyleParams`. Leave `create_spool_visual()` and the `spool_visual_set_*` family alone for now; Task 7 moves them into the spool widget.
- Modify: `tests/unit/test_ams_drawing_utils.cpp` — delete the `[slot_bar]` cases (superseded by `[lane_bar]`)

**Interfaces:**
- Consumes: `ams_lane_bar`
- Produces: `style_slot_bar` and `BarStyleParams` no longer exist

- [ ] **Step 1: Replace the loop with the widget**

The overview's per-unit card builds its own bars. Swap the
`create_slot_column` + `style_slot_bar` pair for `ams_lane_bar` instances
carrying the lane's **global** index, since the overview iterates units and
`unit.first_slot_global_index + s` is the subject index.

- [ ] **Step 2: Delete the dead styler**

```bash
grep -rn "style_slot_bar\|BarStyleParams" src/ include/ tests/ || echo "gone"
```

Expected: "gone" after the deletions.

- [ ] **Step 3: Run the suites**

```bash
make -j4 test 2>&1 | tail -2
./build/bin/helix-tests "[ams],[ams_draw],[lane_bar]" 2>&1 | tail -4
```

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_panel_ams_overview.cpp src/ui/ams_drawing_utils.cpp include/ui/ams_drawing_utils.h tests/unit/test_ams_drawing_utils.cpp
git commit -m "refactor(ams): render overview mini-bars from the shared lane widget"
```

---

## Phase 3 — Spool rendering (Tasks 7-9)

### Task 7: `ams_spool` widget

**Files:**
- Create: `include/ui_ams_spool.h`, `src/ui/ui_ams_spool.cpp`
- Test: `tests/unit/test_ams_spool.cpp`

**Interfaces:**
- Consumes: `LaneState`, the per-lane subjects, `ui_spool_canvas.h`
- Produces: XML widget `ams_spool` taking `slot_index`. Named children: `spool_graphic`, `empty_placeholder`.

This absorbs `create_spool_visual()` — it owns the 3D-canvas-vs-flat-rings
choice internally (`resolve_3d_spool_style()`), so no caller branches on style.

**Rendering contract:**

| LaneState | spool_graphic | empty_placeholder | cell opacity |
|-----------|---------------|-------------------|--------------|
| `Present` | visible, fill % | hidden | `LV_OPA_COVER` |
| `Ghosted` | visible, fill % | hidden | ghost token |
| `Empty`   | hidden | visible | `LV_OPA_COVER` |

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_ams_spool.cpp` with these cases. Do not assume you have
read Task 4 — the shapes are similar but the assertions differ, because the
spool has a placeholder and the bar does not.

| Test case | Setup | Assert |
|-----------|-------|--------|
| `an Empty lane shows the placeholder` | `lane_state = Empty` | `spool_graphic` hidden, `empty_placeholder` visible, root opa `LV_OPA_COVER` |
| `a Ghosted lane keeps its spool, dimmed` | `lane_state = Ghosted`, fill 60 | `spool_graphic` visible, `empty_placeholder` hidden, root opa `< LV_OPA_COVER` |
| `a Present lane is full strength` | `lane_state = Present`, fill 60 | `spool_graphic` visible, `empty_placeholder` hidden, root opa `== LV_OPA_COVER` |
| `the lane_state subject drives repaint` | create Present, then set `Empty` and `process_lvgl(20)` | `spool_graphic` flips to hidden — proves the observer is live, not just the initial apply |
| `the ghost covers the label too` | `lane_state = Ghosted` | the material label's `text_opa` equals the root opa |

Use the same helpers as Task 4's test (`make_spool` via `lv_xml_create` with a
`slot_index` attr; a `visible(root, name)` wrapper over `lv_obj_find_by_name` +
`LV_OBJ_FLAG_HIDDEN`), written out in this file rather than shared — test files
in this tree are self-contained.

The last case is the one the bar does not have, and it is the load-bearing one:
it asserts the ghost is a **whole-cell** modifier. Reversing #1071 is safe only
because of it. If the label can be full strength while the graphic is dimmed,
the reversal reintroduces the original bug.

- [ ] **Step 2: Run to verify it fails**

- [ ] **Step 3: Implement, lifting from `create_spool_visual()`**

Move the body of `ams_draw::create_spool_visual()`
(`src/ui/ams_drawing_utils.cpp:788-880`) into the widget's `create`. It already
branches on `resolve_3d_spool_style()` and already builds `empty_placeholder`;
keep both. Keep `lv_obj_set_name(..., "spool_graphic")` on the canvas (3D
branch) and the filament ring (flat branch) — one style-independent handle.

The state application is the same single-switch shape as Task 4's
`apply_lane_state`, with the placeholder taking the branch the bar spends on
hiding its outline:

```cpp
static void apply_lane_state(SpoolData* d, helix::ui::LaneState state) {
    if (!d)
        return;
    const bool empty = (state == helix::ui::LaneState::Empty);

    ams_draw::spool_visual_set_empty(d->sv, empty);  // graphic vs placeholder

    // Ghost on the ROOT, so the graphic, the material label and the percent
    // dim as one cell. A dimmed spool beside a full-strength label reads as
    // two different lanes.
    lv_obj_set_style_opa(d->root,
                         state == helix::ui::LaneState::Ghosted
                             ? static_cast<lv_opa_t>(theme_manager_get_spacing("ghost_opacity"))
                             : LV_OPA_COVER,
                         LV_PART_MAIN);
}
```

- [ ] **Step 4: Run to verify it passes**

```bash
./build/bin/helix-tests "[ams_spool]" 2>&1 | tail -4
```

- [ ] **Step 5: Commit**

---

### Task 8: Convert `ams_slot` chrome to XML

**Files:**
- Modify: `ui_xml/ams_slot_view.xml` — add the bindings; its header comment currently admits "Dynamic styling based on slot state (highlight, opacity)" lives in C++. That stops being true.
- Modify: `src/ui/ui_ams_slot.cpp` — delete `apply_slot_status()`, `apply_slot_material()`, `refresh_slot_material_label()`, `slot_has_retained_identity()`, `create_spool_visualization()`
- Test: `tests/unit/test_ui_ams_slot.cpp`, `tests/unit/test_ams_slot_empty_lane_label.cpp`

**Interfaces:**
- Consumes: `ams_spool`, per-lane subjects
- Produces: `ams_slot` reduced to layout + the click/context-menu payload

- [ ] **Step 1: Bind the chrome**

In `ams_slot_view.xml`: `<ams_spool slot_index="${i}"/>` inside `spool_container`;
`bind_text` the material label; `<if cond="...">` for the Empty branch showing
`lv_tr("Empty")`; one `bind_style` on the view root for the ghost.

Note rule 6: an inline `style_*` attribute overrides `bind_style`. If the ghost
does not take, check for an inline opacity on the same element and use two
`bind_style`s rather than mixing.

- [ ] **Step 2: Keep the existing behavioural tests green**

`test_ams_slot_empty_lane_label.cpp` already pins first paint for both empty
shapes and that a later material notify does not undo "Empty". Those assertions
stay valid and are the regression net for this conversion — do not rewrite them
to match new behaviour. If they fail, the conversion is wrong.

- [ ] **Step 3: Delete the imperative appliers**

- [ ] **Step 4: Run**

```bash
make -j4 test 2>&1 | tail -2
./build/bin/helix-tests "[ams],[ams_slot],[ams_spool]" 2>&1 | tail -4
python3 scripts/check_imperative_ui.py 2>&1 | tail -1
```

- [ ] **Step 5: Commit**

---

### Task 9: Convert the mini-status spool mode

**Files:**
- Create: `ui_xml/components/ams_lane_spool_row.xml`
- Modify: `src/ui/ui_ams_mini_status.cpp` — delete `rebuild_spools()`, `spool_material_text()`, `spool_visual_set_ghost_opa()`, `SpoolCellData`, `rendered_cells`

**Interfaces:**
- Consumes: `ams_spool`
- Produces: spool mode rendered from XML; the render-signature cache is gone

- [ ] **Step 1: Write the XML** — `<repeat>` over `ams_spool` plus the material and percent labels, mirroring Task 5's row.

- [ ] **Step 2: Keep `measure_widest_material()` honest** — it sizes the text column from the strings about to be drawn. If the label text now comes from a binding, the measurement must read the same subject, or the column and the text drift. This is the one place where deleting `spool_material_text()` can silently break layout.

- [ ] **Step 3: Delete the C++ render loop and its cache**

- [ ] **Step 4: Run, including the widget-size suite**

```bash
./build/bin/helix-tests "[ams_mini],[ams_spool],[widget_size]" 2>&1 | tail -4
```

- [ ] **Step 5: Commit**

---

## Phase 4 — Close out (Task 10)

### Task 10: Sweep, verify, resolve #1367

- [ ] **Step 1: Confirm the deletion list is complete**

```bash
# style_slot_bar and the caches must be gone outright.
grep -rn "style_slot_bar\|BarStyleParams\|SpoolCellData\|SlotBarData\|apply_slot_status\|apply_slot_material" src/ include/ tests/ || echo "gone"

# create_spool_visual and the spool_visual_set_* helpers move INTO ui_ams_spool.cpp
# as private statics (Task 7), so they must no longer be referenced anywhere else.
grep -rn "create_spool_visual\|spool_visual_set_" src/ include/ tests/ \
  | grep -v "src/ui/ui_ams_spool.cpp" || echo "no external references"
```

- [ ] **Step 2: Confirm the duplicated constant is gone**

```bash
grep -n "1.0f" include/ams_types.h src/ui/ui_spool_canvas.cpp | grep -i "fill"
```

Expected: `display_fill_level()` and the canvas defaults now reference
`helix::ui::ASSUMED_FILL_LEVEL`.

- [ ] **Step 3: Full verification**

```bash
make -j4 test-run > /tmp/ams-suite.log 2>&1; echo "EXIT=$?"
grep -cE "had test failures|One or more test shards failed" /tmp/ams-suite.log
make test-hidden > /tmp/ams-hidden.log 2>&1; echo "EXIT=$?"
```

Expected: both exit 0, zero failure lines. `test-hidden` is the only suite that
catches exit-time and static-destruction crashes — others print green then
SIGSEGV. Compare shard **case** count against the pre-change baseline, not the
assertion total, which is unstable.

- [ ] **Step 4: Visual check**

```bash
export HELIX_SOCK=/tmp/helix-lane.sock HELIX_CONFIG_DIR=/tmp/helix-config-lane
mkdir -p "$HELIX_CONFIG_DIR"
HELIX_MOCK_AMS=multi SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv \
  -s 480x272 --remote-socket "$HELIX_SOCK" > /tmp/helix-lane.log 2>&1 &
```

`HELIX_MOCK_AMS=multi` gives a real ejected-but-assigned lane: Box Turtle lane 3
is `PLA`/`Overture`/`EMPTY` (`ams_backend_mock.cpp:1891`). Screenshot bar mode
at 480x272 and spool mode at a width above `w_normal()`, and confirm the ghosted
lane is legible in both. **Kill the instance when done.**

- [ ] **Step 5: Resolve #1367**

Its severity claim ("QIDI, Snapmaker, AFC, Happy Hare and ACE all publish
UNKNOWN") is wrong — those are skeleton-construction sites, not steady state.
Correct the body and close it if `display_fill_level()` now routes through
`lane_fill_level()`, or leave it open scoped to whatever remains.

- [ ] **Step 6: Delete both plan files**

Per `docs/CLAUDE.md`, plans are scaffolding. Durable knowledge goes to
`docs/devel/FILAMENT_MANAGEMENT.md` and the architecture chapter; delete
`2026-08-27-ams-lane-presentation-design.md` and this file in the shipping
commit.

- [ ] **Step 7: Final commit**
