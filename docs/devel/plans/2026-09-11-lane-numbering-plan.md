# 1-Based Lane and Tool Numbering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove 0-based numbering from user-facing text so every physical position reads 1-based, while a gcode tool keeps its `T<n>` spelling.

**Architecture:** One new header, `include/display_numbering.h`, owns every index-to-display conversion, and the `+ 1` that converts a storage index to a display number exists in exactly one function inside it. A `LaneNoun` enum supplies the backend's word (Slot, Lane, Gate, Tool), resolved once by `active_lane_noun()`. `FilamentMapper::format_slot_label` is reimplemented on top of the new helpers rather than forked. A lint gate lands last and forbids the raw forms from coming back.

**Tech Stack:** C++17, LVGL 9.5, Catch2 (amalgamated), pure Makefile build, `lv_tr()` for translation, spdlog for logging.

**Spec:** `docs/devel/plans/2026-09-11-lane-numbering-design.md` - read it first; this plan argues from it.

## Global Constraints

- **Issue:** `prestonbrown/helixscreen#957`. Commit subjects cite it: `fix(ams): <thing> (prestonbrown/helixscreen#957)`.
- **New source files need a two-line header**, copied exactly: `// Copyright (C) 2025-2026 356C LLC` then `// SPDX-License-Identifier: GPL-3.0-or-later`.
- **Every new `src/` file must be classified for the ESP32 firmware build** or the pre-commit gate refuses the commit, listing files other branches added too. Add it to `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (compiled) or `app_srcs_excluded.txt` (not in the v1 Core+AMS cut). Anchor the insert on a neighbouring file **in the same directory**, not on lexical order: `app_srcs_excluded.txt` has a directory-wide block and a separate per-file block. Never run `--write-exclusions`; it answers "exclude" for every undecided file at once.
- **`APP_SRCS` and `TEST_SRCS` are globbed** (`Makefile:436`, `Makefile:1150`). A new `.cpp` under `src/` or `tests/unit/` compiles with no Makefile edit.
- **Never `git add` in this tree.** It is shared with other sessions and `git add` then `git commit` is not atomic. Commit pathspecs directly: `git commit -m "..." -- path/one path/two`.
- **spdlog only.** No `printf`, no `cout`, no `LV_LOG_*`.
- **No comment archaeology.** A comment explains the code as it is now. No commit SHAs, no "used to", no bug or review stories. Applies to tests and gates too.
- **Doc and comment citations name a place, not a line:** `` `src/printer/filament_mapper.cpp#format_slot_label` ``.
- **A green suite is not evidence.** Each task ends with `make mutate-diff` over its own hunks, and the commit body names the mutation that went red.
- **Build commands:** `make -j` builds only the app. `make test` builds only tests. Run tags from the repo root: `./build/bin/helix-tests "[tag]"`. Before compiling, check for a peer build: `pgrep -x -d' ' 'make|cc1plus'` (never `pgrep -f`).

---

### Task 1: The display_numbering helper

The foundation. No call sites change, nothing user-visible moves.

**Files:**
- Create: `include/display_numbering.h`
- Create: `src/ui/display_numbering.cpp`
- Create: `tests/unit/test_display_numbering.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `helix::ui::LaneNoun` (enum class: `Slot`, `Lane`, `Gate`, `Tool`), `std::string helix::ui::tool_label(int)`, `int helix::ui::lane_number(int)`, `std::string helix::ui::lane_number_text(int)`, `std::string helix::ui::noun_text(LaneNoun)`, `std::string helix::ui::lane_label(LaneNoun, int)`, `std::string helix::ui::lane_label(LaneNoun, std::string_view, int)`. `active_lane_noun()` is declared here but implemented in Task 4.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_display_numbering.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_numbering.cpp
 * @brief Unit tests for display_numbering - the single home of index-to-label conversion
 */

#include "display_numbering.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

TEST_CASE("tool_label spells a gcode tool 0-based", "[numbering]") {
    CHECK(tool_label(0) == "T0");
    CHECK(tool_label(1) == "T1");
    CHECK(tool_label(15) == "T15");
}

TEST_CASE("lane_number is the only + 1", "[numbering]") {
    CHECK(lane_number(0) == 1);
    CHECK(lane_number(3) == 4);
    CHECK(lane_number_text(0) == "1");
    CHECK(lane_number_text(3) == "4");
}

TEST_CASE("lane_label composes noun and number", "[numbering]") {
    CHECK(lane_label(LaneNoun::Slot, 0) == "Slot 1");
    CHECK(lane_label(LaneNoun::Lane, 1) == "Lane 2");
    CHECK(lane_label(LaneNoun::Gate, 2) == "Gate 3");
}

TEST_CASE("LaneNoun::Tool is an ordinary noun, never T<n>", "[numbering]") {
    // A tool changer's positions are physical, so they count from 1 like any
    // other position. The T<n> spelling belongs to the gcode domain only.
    CHECK(lane_label(LaneNoun::Tool, 0) == "Tool 1");
    CHECK(lane_label(LaneNoun::Tool, 3) == "Tool 4");
    CHECK(lane_label(LaneNoun::Tool, 0) != tool_label(0));
}

TEST_CASE("lane_label with a unit prefixes the unit name", "[numbering]") {
    CHECK(lane_label(LaneNoun::Slot, "Turtle 1", 1) == "Turtle 1 \xc2\xb7 Slot 2");
    // An empty unit name degrades to the single-unit form rather than emitting
    // a leading separator.
    CHECK(lane_label(LaneNoun::Slot, "", 1) == "Slot 2");
}

TEST_CASE("out-of-range indices do not produce a label", "[numbering]") {
    // A negative index means "no lane". Callers must not paint "Slot 0".
    CHECK(lane_number(-1) == -1);
    CHECK(lane_number_text(-1).empty());
    CHECK(lane_label(LaneNoun::Slot, -1).empty());
    CHECK(lane_label(LaneNoun::Slot, "Turtle 1", -1).empty());
    CHECK(tool_label(-1).empty());
}
```

- [ ] **Step 2: Run it and confirm it fails**

```bash
make test -j"$(scripts/helix-claim jobs)" 2>&1 | tail -20
```

Expected: compile error, `display_numbering.h: No such file or directory`. That is the correct failure for this step.

- [ ] **Step 3: Write the header**

Create `include/display_numbering.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace helix::ui {

/**
 * @brief The word a backend uses for one physical filament position.
 *
 * An enum rather than a string so a caller cannot compare the wrong spelling
 * and silently fall through to the default.
 */
enum class LaneNoun {
    Slot, ///< Bambu-style AMS, K2 CFS, ACE, QIDI Box, AD5X IFS, Snapmaker U1
    Lane, ///< AFC
    Gate, ///< Happy Hare
    Tool, ///< Tool changer: each position carries its own toolhead
};

/**
 * @brief Spell a G-code tool.
 *
 * 0-based and T-prefixed, because this is the number a user types into a
 * console and the number the slicer emitted. Never renumbered.
 *
 * @param gcode_tool 0-based tool index; negative yields an empty string
 */
std::string tool_label(int gcode_tool);

/**
 * @brief Convert a storage index to the number a user sees.
 *
 * The only + 1 in the codebase. Every display path routes through this so a
 * missing conversion is a call that is not here, rather than one correct
 * expression among many hand-written ones.
 *
 * @param index 0-based storage index
 * @return 1-based display number, or -1 when @p index is negative
 */
int lane_number(int index);

/// @brief lane_number() as text, or empty when @p index is negative.
std::string lane_number_text(int index);

/// @brief The translated word for @p noun ("Slot", "Lane", "Gate", "Tool").
std::string noun_text(LaneNoun noun);

/**
 * @brief A physical position: "Slot 1", "Lane 2", "Tool 4".
 * @return empty when @p index is negative
 */
std::string lane_label(LaneNoun noun, int index);

/**
 * @brief A physical position inside a named unit: "Turtle 1 · Slot 2".
 *
 * @p unit_display_name is user-configured (an AFC unit name) and is not
 * translated. An empty unit degrades to the single-unit form.
 * @return empty when @p index is negative
 */
std::string lane_label(LaneNoun noun, std::string_view unit_display_name, int index);

/**
 * @brief The noun for the printer currently connected.
 *
 * Resolved in one place so no widget derives it for itself. The nozzle badge
 * exists on machines with no AMS backend at all, which is why this falls back
 * rather than requiring a backend.
 *
 * @return the active AMS backend's lane_noun(), else LaneNoun::Slot
 */
LaneNoun active_lane_noun();

} // namespace helix::ui
```

- [ ] **Step 4: Write the implementation**

Create `src/ui/display_numbering.cpp`. Note `lv_translation.h` is included the same way `src/printer/filament_mapper.cpp` includes it, so this adds no new dependency direction.

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_numbering.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <cstdio>

namespace helix::ui {

namespace {
/// U+00B7 MIDDLE DOT, the separator between a unit name and its position.
constexpr const char* kUnitSeparator = "\xc2\xb7";
} // namespace

std::string tool_label(int gcode_tool) {
    if (gcode_tool < 0)
        return {};
    return "T" + std::to_string(gcode_tool);
}

int lane_number(int index) {
    if (index < 0)
        return -1;
    return index + 1;
}

std::string lane_number_text(int index) {
    const int n = lane_number(index);
    if (n < 0)
        return {};
    return std::to_string(n);
}

std::string noun_text(LaneNoun noun) {
    switch (noun) {
    case LaneNoun::Lane:
        return lv_tr("Lane");
    case LaneNoun::Gate:
        return lv_tr("Gate");
    case LaneNoun::Tool:
        return lv_tr("Tool");
    case LaneNoun::Slot:
        break;
    }
    return lv_tr("Slot");
}

std::string lane_label(LaneNoun noun, int index) {
    const int n = lane_number(index);
    if (n < 0)
        return {};
    return noun_text(noun) + " " + std::to_string(n);
}

std::string lane_label(LaneNoun noun, std::string_view unit_display_name, int index) {
    if (unit_display_name.empty())
        return lane_label(noun, index);
    const std::string body = lane_label(noun, index);
    if (body.empty())
        return {};
    return std::string(unit_display_name) + " " + kUnitSeparator + " " + body;
}

} // namespace helix::ui
```

`active_lane_noun()` is declared but not defined here; Task 4 defines it. Until then nothing calls it, so the link stays clean.

- [ ] **Step 5: Classify the new file for the ESP32 build**

`src/printer/filament_mapper.cpp` and `src/ui/ui_format_utils.cpp` are both already in `app_srcs.txt`, and both will depend on this file, so it is compiled:

```bash
grep -n 'src/ui/ui_format_utils.cpp' firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
```

Insert `src/ui/display_numbering.cpp` beside that entry, inside the `src/ui/` block. Do not sort the file as a whole.

- [ ] **Step 6: Run the tests and confirm they pass**

```bash
make test -j"$(scripts/helix-claim jobs)" && ./build/bin/helix-tests "[numbering]"
```

Expected: all sections pass.

- [ ] **Step 7: Prove the tests can fail**

```bash
make mutate-diff
```

Expected: reverting the `+ 1` in `lane_number` turns `[numbering]` red. If a hunk survives, the test for it is vacuous; fix the test, not the gate.

- [ ] **Step 8: Commit**

```bash
git commit -m "feat(ui): add display_numbering, one home for index-to-label conversion (prestonbrown/helixscreen#957)" -- include/display_numbering.h src/ui/display_numbering.cpp tests/unit/test_display_numbering.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
```

Body: one short paragraph saying the `+ 1` now has one home, and naming the mutation that went red.

---

### Task 2: Reimplement FilamentMapper on the helper

Pure refactor. `format_slot_label`'s existing six test sections are the regression net, so they must pass **unchanged**.

**Files:**
- Modify: `src/printer/filament_mapper.cpp#FilamentMapper::format_slot_label`, `#FilamentMapper::mapped_lane_display_number`
- Modify: `include/filament_mapper.h` (doc comments only)
- Test: `tests/unit/test_filament_mapper.cpp` (existing, unchanged)

**Interfaces:**
- Consumes: `helix::ui::lane_label`, `helix::ui::lane_number`, `helix::ui::LaneNoun` from Task 1.
- Produces: no signature change. `format_slot_label(const AvailableSlot&)` and `mapped_lane_display_number(...)` keep their exact declarations.

- [ ] **Step 1: Run the existing tests and record the baseline**

```bash
./build/bin/helix-tests "[filament_mapper]"
```

Expected: PASS. Write down the assertion count; it must not drop.

- [ ] **Step 2: Reimplement format_slot_label**

Replace the body in `src/printer/filament_mapper.cpp`. The current version hand-builds four snprintf branches around `slot.local_slot_index + 1`; the material suffix is the only part that is genuinely this function's job.

```cpp
std::string FilamentMapper::format_slot_label(const AvailableSlot& slot) {
    std::string label =
        helix::ui::lane_label(helix::ui::LaneNoun::Slot, slot.unit_display_name,
                              slot.local_slot_index);

    const char* material_str = nullptr;
    if (slot.is_empty) {
        material_str = lv_tr("Empty");
    } else if (!slot.material.empty()) {
        material_str = slot.material.c_str();
    }
    if (material_str)
        label += std::string(": ") + material_str;

    return label;
}
```

Add `#include "display_numbering.h"` to the include block.

The noun is hardcoded to `Slot` here on purpose: this overload takes an `AvailableSlot`, which carries no backend identity. Task 4 does not change that; callers that know their backend use `lane_label` directly.

- [ ] **Step 3: Reimplement mapped_lane_display_number**

Replace its `s->local_slot_index + 1` with `helix::ui::lane_number(s->local_slot_index)`. Keep the existing `-1` returns for unmapped and missing lanes, and keep the explanatory comment about local versus global index, which is still a true statement about the system.

- [ ] **Step 4: Run the existing tests and confirm no behaviour change**

```bash
make test -j"$(scripts/helix-claim jobs)" && ./build/bin/helix-tests "[filament_mapper][numbering]"
```

Expected: PASS, with the assertion count from Step 1 or higher. A drop means a section stopped running.

- [ ] **Step 5: Commit**

```bash
git commit -m "refactor(ams): build slot labels from display_numbering (prestonbrown/helixscreen#957)" -- src/printer/filament_mapper.cpp include/filament_mapper.h
```

---

### Task 3: Route the 17 correct sites through lane_number

Every one of these is correct today. They are folded in so a missing conversion becomes a missing call rather than one correct expression among many, which is what makes Task 10's gate enforceable.

**Files (modify, all are `+ 1` on a position index):**
- `src/ui/ui_filament_mapping_card.cpp` (bottom-band lane label)
- `src/ui/ui_filament_mapping_modal.cpp#FilamentMappingModal::get_slot_display_text`
- `src/ui/ui_ams_tool_text.cpp#update_toolchange_text`
- `src/ui/ams_drawing_utils.cpp#create_lane_badge` caller in `src/ui/ui_ams_mini_status.cpp#ui_ams_mini_status_set_slot_full`
- `src/ui/ui_ams_slot.cpp`, `src/ui/ui_ams_context_menu.cpp#AmsContextMenu::build_backup_options_for`
- `src/ui/ui_ams_edit_overlay.cpp`, `src/ui/ui_ams_detail.cpp`, `src/ui/ui_panel_ams.cpp`, `src/ui/ui_panel_ams_overview.cpp`
- `src/ui/ui_zone_presentation.cpp`, `src/ui/ui_ams_zone_overview_overlay.cpp`, `src/ui/ams_drawing_utils.cpp` (`unit_index + 1`)
- `src/ui/ui_exclude_object_map_view.cpp`, `src/ui/ui_exclude_object_side_list.cpp`
- `src/ui/panel_widgets/temp_graph_widget.cpp#TempGraphWidget::TempGraphConfigModal::sensor_display_name`
- `src/ui/panel_widgets/print_status_widget.cpp#PrintStatusWidget::build_nozzle_tool_options`
- `src/printer/printer_temperature_state.cpp` (`ExtruderInfo::display_name`)

**Interfaces:**
- Consumes: `helix::ui::lane_number` from Task 1.
- Produces: no new API.

- [ ] **Step 1: Replace each `+ 1` with a call**

For each site, `x + 1` becomes `helix::ui::lane_number(x)` and `#include "display_numbering.h"` is added. Do not change any output string. This task must be invisible at runtime.

Two sites are **not** part of this and must be left alone, because their `+ 1` is not an index conversion:
- `src/printer/filament_slot_override_store.cpp#format_lane_key` builds a persistence key.
- `src/rendering/gcode_tool_remapper.cpp` rewrites gcode.

- [ ] **Step 2: Fix the "Nozzle" fallback asymmetry while here**

`print_status_widget.cpp#build_nozzle_tool_options` and `temp_graph_widget.cpp#sensor_display_name` both read:

```cpp
opt.label = index == 0 ? std::string(lv_tr("Nozzle"))
                       : std::string(lv_tr("Nozzle")) + " " + std::to_string(index + 1);
```

On a multi-extruder printer this labels extruder 0 as bare "Nozzle" beside "Nozzle 2" and "Nozzle 3", reading as though Nozzle 1 is missing. The authoritative path at `src/printer/printer_temperature_state.cpp` gates on `multi` and emits "Nozzle 1" correctly. Make both fallbacks unconditional:

```cpp
opt.label = std::string(lv_tr("Nozzle")) + " " + helix::ui::lane_number_text(index);
```

- [ ] **Step 3: Build and run the affected tags**

```bash
make test -j"$(scripts/helix-claim jobs)" && ./build/bin/helix-tests "[ams],[numbering],[filament_mapper]"
```

Expected: PASS with no output changes.

- [ ] **Step 4: Commit**

```bash
git commit -m "refactor(ui): route position numbering through lane_number (prestonbrown/helixscreen#957)" -- src/ui src/printer/printer_temperature_state.cpp
```

Body: note that Step 2 is the one behaviour change in the task, and why.

---

### Task 4: LaneNoun per backend

**Files:**
- Modify: `include/ams_backend.h` (new virtual)
- Modify: `src/printer/ams_backend_afc.cpp` + header (`Lane`), `src/printer/ams_backend_happy_hare.cpp` + header (`Gate`), `src/printer/ams_backend_toolchanger.cpp` + header (`Tool`)
- Modify: `src/ui/display_numbering.cpp` (define `active_lane_noun`)
- Modify: `translations/en.yml` and the other 8 locale YAMLs (via `make translation-sync`)
- Test: `tests/unit/test_display_numbering.cpp` (extend)

**Interfaces:**
- Consumes: `helix::ui::LaneNoun` from Task 1.
- Produces: `virtual helix::ui::LaneNoun AmsBackend::lane_noun() const`, default `LaneNoun::Slot`. `helix::ui::active_lane_noun()` now has a definition.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_display_numbering.cpp`, and add
`#include "lvgl/src/others/translation/lv_translation.h"` to its include block,
since these cases call `lv_tr` directly:

```cpp
TEST_CASE("noun_text covers every LaneNoun", "[numbering]") {
    // A new enumerator with no case falls through to Slot, which would be a
    // silent wrong word rather than a build failure, so pin all four.
    CHECK(noun_text(LaneNoun::Slot) == std::string(lv_tr("Slot")));
    CHECK(noun_text(LaneNoun::Lane) == std::string(lv_tr("Lane")));
    CHECK(noun_text(LaneNoun::Gate) == std::string(lv_tr("Gate")));
    CHECK(noun_text(LaneNoun::Tool) == std::string(lv_tr("Tool")));
}
```

Add a backend test in `tests/unit/test_ams_backend_afc.cpp` and its Happy Hare and tool-changer siblings:

```cpp
TEST_CASE("AFC names its positions lanes", "[ams][afc][numbering]") {
    AmsBackendAfc backend;
    CHECK(backend.lane_noun() == helix::ui::LaneNoun::Lane);
}
```

- [ ] **Step 2: Run and confirm it fails**

```bash
make test -j"$(scripts/helix-claim jobs)" 2>&1 | tail -20
```

Expected: `'class AmsBackendAfc' has no member named 'lane_noun'`.

- [ ] **Step 3: Add the virtual**

In `include/ams_backend.h`, beside `should_hide_slot_tool_badge()`, following that file's existing virtual style:

```cpp
    /**
     * @brief The word this backend's hardware uses for one filament position.
     *
     * Display code composes it with a 1-based number. Returned as an enum so a
     * caller cannot compare the wrong spelling and silently get the default.
     *
     * @return the backend's noun; Slot unless overridden
     */
    [[nodiscard]] virtual helix::ui::LaneNoun lane_noun() const {
        return helix::ui::LaneNoun::Slot;
    }
```

Add `#include "display_numbering.h"` to `include/ams_backend.h`.

Override in the three backends that differ. Every other backend inherits the default; do not add no-op overrides.

- [ ] **Step 4: Define active_lane_noun**

In `src/ui/display_numbering.cpp`:

```cpp
LaneNoun active_lane_noun() {
    const auto* backend = AmsState::instance().active_backend();
    return backend ? backend->lane_noun() : LaneNoun::Slot;
}
```

Confirm the accessor's real name first (`grep -n 'active_backend\|current_backend' include/ams_state.h`) and use whatever it is. A printer with no AMS backend, which is the common case for the nozzle badge, takes the `Slot` fallback.

- [ ] **Step 5: Add the two new translation keys**

`Slot` and `Tool` already exist. `Lane` and `Gate` are new:

```bash
make translation-sync
make translations
```

Stage the 9 YAMLs **and** the generated `ui_xml/translations/*.xml`, which are tracked and not auto-staged.

- [ ] **Step 6: Run and confirm pass**

```bash
make test -j"$(scripts/helix-claim jobs)" && ./build/bin/helix-tests "[numbering],[ams]"
```

- [ ] **Step 7: Commit**

```bash
git commit -m "feat(ams): give each backend its own word for a filament position (prestonbrown/helixscreen#957)" -- include/ams_backend.h include/display_numbering.h src/printer src/ui/display_numbering.cpp tests/unit translations ui_xml/translations
```

---

### Task 5: Split gcode identity from physical label on ToolInfo

**Files:**
- Modify: `include/tool_state.h` (add `ToolInfo::display_label`, remove `ToolTopology::tool_name_prefix`)
- Modify: `src/printer/tool_state.cpp#ToolState::init_tools`, `#ToolState::set_ams_topology`
- Modify: `src/printer/ams_state.cpp` (drops `topo.tool_name_prefix = "T";`)
- Modify: `src/ui/panel_widgets/tool_switcher_widget.cpp` (`#rebuild_pills`, `#rebuild_compact`, `#ToolPicker::on_created`), `src/ui/panel_widgets/nozzle_temps_widget.cpp#create_extruder_row`, `src/ui/panel_widgets/preheat_widget.cpp`, `src/ui/ui_panel_filament.cpp#FilamentPanel::populate_extruder_dropdown`
- Test: `tests/unit/test_tool_state_ams_topology.cpp`

**Interfaces:**
- Consumes: `helix::ui::tool_label`, `helix::ui::lane_label`, `helix::ui::active_lane_noun` from Tasks 1 and 4.
- Produces: `ToolInfo::display_label` (`std::string`, 1-based physical label). `ToolInfo::name` keeps its existing meaning and its `"T0"` default. `ToolTopology::tool_name_prefix` no longer exists.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("ToolInfo keeps gcode identity and physical label apart", "[tool_state][numbering]") {
    // name is what you type into a console; display_label is what is written on
    // the machine. A widget picking the wrong one is the bug this split exists
    // to make visible at the call site.
    auto& ts = ToolState::instance();
    // ... existing fixture setup for a 4-tool topology ...
    const auto* first = ts.tool_at(0);
    REQUIRE(first != nullptr);
    CHECK(first->name == "T0");
    CHECK(first->display_label == "Tool 1");
}
```

Use the fixture and accessor the neighbouring cases in that file already use; do not invent `tool_at` if the file spells it differently.

- [ ] **Step 2: Run and confirm it fails**

Expected: `'struct ToolInfo' has no member named 'display_label'`.

- [ ] **Step 3: Add the field**

In `include/tool_state.h`, directly under `name`:

```cpp
    std::string name = "T0";      ///< G-code identity, 0-based ("T0"). What a user types.
    std::string display_label;    ///< Physical label, 1-based ("Tool 1"). What is on the machine.
```

- [ ] **Step 4: Populate it**

In `src/printer/tool_state.cpp`, both `init_tools` branches and `set_ams_topology` set it:

```cpp
t.name = helix::ui::tool_label(i);
t.display_label = helix::ui::lane_label(helix::ui::active_lane_noun(), i);
```

Remove `ToolTopology::tool_name_prefix` and its four assignments. It builds names as `fmt::format("{}{}", prefix, i)`, so a backend supplying `"Lane"` would silently produce `Lane0`, and no search for `T{}` would ever find it. Every current assignment is `"T"`, so removing it changes no output.

- [ ] **Step 5: Point the five widgets at display_label**

Each currently renders `ToolInfo::name` for a physical thing:

| Site | Change |
|---|---|
| `tool_switcher_widget.cpp#rebuild_pills` | `tools[i].name` to `tools[i].display_label` |
| `tool_switcher_widget.cpp#rebuild_compact` | same, and the `"T?"` literal fallback becomes an empty string |
| `tool_switcher_widget.cpp#ToolPicker::on_created` | the `"tool_text"` attr value |
| `nozzle_temps_widget.cpp#create_extruder_row` | the **short** label only; the long label is already "Nozzle 1" and stays |
| `ui_panel_filament.cpp#populate_extruder_dropdown` | the `"T0\nT1\nT2"` option list |

`preheat_widget.cpp#update_tool_target_label` is `"T%d"` built directly and is handled in Task 6.

- [ ] **Step 6: Run and confirm pass**

```bash
make test -j"$(scripts/helix-claim jobs)" && ./build/bin/helix-tests "[tool_state],[numbering]"
```

- [ ] **Step 7: Commit**

```bash
git commit -m "feat(tools): separate gcode identity from physical tool label (prestonbrown/helixscreen#957)" -- include/tool_state.h src/printer/tool_state.cpp src/printer/ams_state.cpp src/ui tests/unit
```

---

### Task 6: The display sweep

The user-visible change lands here. Every site below is a `T<n>` or bare index that should be a 1-based physical label.

**Files (modify):** as listed in the two tables.

**Interfaces:**
- Consumes: everything from Tasks 1, 4 and 5.
- Produces: no new API.

- [ ] **Step 1: Write the failing tests first**

Nothing currently asserts any of these strings, so each fix needs its assertion written before the fix. Minimum set:

```cpp
TEST_CASE("the nozzle badge counts from 1", "[ams][slot][numbering]") {
    // Gated on has_multiple_extruders(), so this only renders on a genuinely
    // multi-nozzle printer. A bare "0" reads as an ordinal, which is the bug.
    // ... drive ToolState to a 2-extruder topology, active tool 0 ...
    CHECK(std::string(lv_subject_get_string(ts.get_tool_badge_text_subject())) == "1");
}

TEST_CASE("print-start warnings name the gcode tool and the physical slot", "[print_start][numbering]") {
    // T0 is the slicer's tool; Slot 1 is the lane. Both appear, each correct.
    const std::string msg = build_empty_lane_message(/* tool 0, slot 0 */);
    CHECK(msg.find("T0") != std::string::npos);
    CHECK(msg.find("Slot 1") != std::string::npos);
    CHECK(msg.find("Tool 0") == std::string::npos);
}
```

- [ ] **Step 2: Run and confirm they fail**

Expected: the badge case reports `"0" != "1"`; the print-start case fails on `Tool 0` still being present.

- [ ] **Step 3: Fix the physical-position sites**

These become 1-based. The value shown is a thing you can point at on the machine.

| `path#function` | Now | Becomes |
|---|---|---|
| `src/ui/ui_ams_tool_text.cpp#update_tool_badge` | `snprintf(buf, sizeof(buf), "%d", tool->index)` | `lane_number_text(tool->index)` |
| `src/ui/ui_filament_path_canvas.cpp#helix::ui::fpath::format_tool_badge_label` | `"E%d"` from `extruder_tool[lane]` | 1-based; keep the `E` prefix, it names an extruder |
| `src/ui/ams_drawing_utils.cpp#compute_tool_badge_labels` | picks prefix `'E'` vs `'T'`, numbers raw | 1-based for the `'E'` path; this is the policy chokepoint for the canvases |
| `src/ui/ui_system_path_canvas.cpp#ui_system_path_canvas_set_total_tools` and `#ui_system_path_canvas_set_tool_label_prefix` and `#ui_system_path_canvas_set_tool_virtual_numbers` | `"%c%d"` with raw `i` | 1-based |
| `src/printer/ams_state.cpp#AmsState::sync_current_loaded_from_backend` | `lv_tr("Current: Tool %d"), slot_index` | `lane_label(active_lane_noun(), slot_index)`, giving `Current: Tool 1` |
| `src/ui/ui_ams_slot.cpp#apply_tool_badge` | `"T%d", mapped_tool` | keep `T<n>`: this badge names which gcode tool the lane feeds |
| `src/ui/ui_print_tune_overlay.cpp#PrintTuneOverlay::update_tool_z_displays` | `"T%d"` and `"T%d %s"` | `display_label`: a per-tool Z offset belongs to a physical toolhead |
| `src/ui/panel_widgets/preheat_widget.cpp#PreheatWidget::update_tool_target_label` | `"T%d", tool_target_` | `display_label` |
| `src/ui/panel_widgets/print_status_widget.cpp#DetailedFormatter::update_tool_label` | `"T%d", idx` | `display_label` |
| `src/ui/ui_ams_tool_text.cpp` current-tool observer | `"T%d", tool` | `display_label`; the `ams_current_tool_text` buffer comment in `include/ams_state.h` must be updated to match |

The `ams_current_tool_text` subject's documented contract lives in three places that must agree: the buffer comment in `include/ams_state.h`, the `<!-- -->` block in `ui_xml/ams_current_tool.xml`, and the `text="T0"` design-time placeholder in that same file. Update all three.

- [ ] **Step 4: Fix the gcode-tool sites**

These stay 0-based and keep `T<n>`, but stop being hand-built. Each `"T%d"` / `"T" + std::to_string(...)` becomes `helix::ui::tool_label(...)`:

| `path#function` |
|---|
| `src/printer/print_start_checks.cpp#gate_material_compatibility` (Material Mismatch) |
| `src/printer/print_start_checks.cpp#grade_change_warning` (Filament Grade Mismatch) |
| `src/printer/print_start_checks.cpp#gate_unresolved_tools` (Color Mismatch) |
| `src/printer/print_start_checks.cpp#build_empty_lane_message` (both branches: `Tool 0` becomes `T0`, slot stays 1-based) |
| `src/printer/print_start_checks.cpp#gate_insufficient_lane_weight` (`tool 0` becomes `T0`) |
| `src/ui/ui_filament_mapping_card.cpp#FilamentMappingCard::rebuild_compact_view` (tool pill) |
| `src/ui/ui_filament_mapping_modal.cpp#FilamentMappingModal::create_tool_row` |
| `src/ui/modals/ui_preflight_check_modal.cpp#PreflightCheckModal::create_tool_row` and `#on_show` |
| `src/ui/ui_ams_context_menu.cpp#AmsContextMenu::build_tool_options` (dropdown) |
| `src/ui/ui_ams_edit_overlay.cpp#AmsEditOverlay::update_ui` (Remap Tool dropdown) |
| `include/printer_discovery.h` tool-changer discovery block |
| `src/printer/ams_backend_toolchanger.cpp#AmsBackendToolChanger::initialize_tools` (`slot.spool_name`) |

- [ ] **Step 5: Update the XML defaults**

`ui_xml/ams_current_tool.xml`, `ui_xml/components/tool_picker_button.xml` and `ui_xml/components/nozzle_temp_row.xml` carry `T0` as a design-time default. These are runtime-overwritten and cosmetic, but they encode the convention and should match. No rebuild is needed for XML; relaunch to see it.

- [ ] **Step 6: Run the tests and verify in the running app**

```bash
make -j"$(scripts/helix-claim jobs)" && make test -j"$(scripts/helix-claim jobs)"
./build/bin/helix-tests "[numbering],[ams],[print_start],[tool_state]"
```

Then drive the real UI, pinning the socket so a peer session is not driven by accident:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate ams
./build/bin/helix-screen ctl -s "$HELIX_SOCK" text ams_current_tool
```

`ctl text` is exact; a screenshot only proves what a scroll position exposed. Kill the PID captured at launch, never `pkill helix-screen`, which reaps every other session's instance.

- [ ] **Step 7: Commit**

```bash
git commit -m "fix(ui): count lanes and nozzles from 1 (prestonbrown/helixscreen#957)" -- src/ui src/printer ui_xml include tests/unit
```

---

### Task 7: Re-key the 8 translated strings

**Files:**
- Modify: the call sites listed below
- Modify: `translations/*.yml` (9 files), `ui_xml/translations/*.xml` (generated)

**Interfaces:**
- Consumes: `tool_label`, `lane_label` from earlier tasks.
- Produces: no new API.

- [ ] **Step 1: Change each key to take a label, not a number**

Embedding the number in the key means every future change to how a tool or lane is spelled orphans the translations again. Taking a `%s` pays the bill once.

| Old key | New key |
|---|---|
| `Current: Tool %d` | `Current: %s` |
| `Preheat: T{} + bed set` | `Preheat: {} + bed set` |
| `Switched to T{}` | `Switched to {}` |
| `T%d has no filament loaded - this print will run out.` | `%s has no filament loaded - this print will run out.` |
| `T%d needs filament in slot %d, which is empty - this print will run out.` | `%s needs filament in %s, which is empty - this print will run out.` |
| `Tool {} -> Slot {}: no filament loaded.` | `{} -> {}: no filament loaded.` |
| `T{} shares slot with T{}` | `{} shares slot with {}` |
| `Slot %d has about %.0fg but tool %d needs about %.0fg. Start anyway?` | `%s has about %.0fg but %s needs about %.0fg. Start anyway?` |

- [ ] **Step 2: Regenerate and stage**

```bash
make translation-sync
make translations
```

The 8 old keys orphan 64 entries across the 8 non-English locales. New keys land as empty-string placeholders in all 9 languages; English is filled in by the call-site change.

- [ ] **Step 3: Verify the format-string gate passes**

```bash
scripts/quality-checks.sh
```

`qc_translation_fmt` and `qc_translation_coverage` both inspect these files and will catch a placeholder-count mismatch between a key and its translations.

- [ ] **Step 4: Commit**

```bash
git commit -m "i18n: take a label rather than a tool number in 8 keys (prestonbrown/helixscreen#957)" -- src translations ui_xml/translations
```

---

### Task 8: Collapse the eight "Tool N out of range" copies

**Files:**
- Modify: `include/ams_error_helper.h` or the existing `AmsErrorHelper` home (confirm with `grep -rn 'class AmsErrorHelper\|namespace AmsErrorHelper' include/`)
- Modify: `src/printer/ams_backend_afc.cpp` (2 sites), `ams_backend_cfs.cpp`, `ams_backend_happy_hare.cpp` (2), `ams_backend_toolchanger.cpp`, `ams_backend_mock.cpp` (2), `ams_backend_ad5x_ifs.cpp` (the `"Tool T<n>"` near-twin)

**Interfaces:**
- Consumes: `helix::ui::tool_label`.
- Produces: one helper returning the error; name it for what it means, e.g. `AmsErrorHelper::tool_out_of_range(int tool_number)`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("every backend reports an out-of-range tool identically", "[ams][numbering]") {
    // Eight hand-written copies of one sentence agree by convention until they
    // silently do not. Folding them onto one function is also how the drift
    // gets found.
    const auto err = AmsErrorHelper::tool_out_of_range(7);
    CHECK(err.detail.find("T7") != std::string::npos);
}
```

- [ ] **Step 2: Run and confirm it fails**

Expected: no such member.

- [ ] **Step 3: Add the helper and replace all eight call sites**

The AD5X IFS one already spells it `"Tool T<n>"` while the others say `"Tool <n>"`. That disagreement is the reason to fold them; pick the `T<n>` spelling, since this names a gcode tool.

- [ ] **Step 4: Run and commit**

```bash
make test -j"$(scripts/helix-claim jobs)" && ./build/bin/helix-tests "[ams]"
git commit -m "refactor(ams): one out-of-range tool error, not eight copies (prestonbrown/helixscreen#957)" -- include src/printer tests/unit
```

---

### Task 9: Replace firmware text with authored copy

Touches the resume path, which is print-critical. It lands last of the behaviour changes and can be dropped without blocking anything before it.

**Files:**
- Modify: `include/ams_types.h` or wherever `AmsError` lives (confirm with `grep -rn 'struct AmsError' include/`)
- Modify: the backend `prepare_for_resume` implementations
- Modify: `src/ui/ui_resume_dispatch.cpp#show_restart_required_modal`, `src/ui/ui_panel_print_status.cpp#recompute_paused_overlay_visibility`, `src/ui/ui_panel_ams.cpp#AmsPanel::show_loading_error_modal`, `src/ui/recovery_modal_presenter.cpp#RecoveryModalPresenter::present`

**Interfaces:**
- Consumes: `lane_label`, `active_lane_noun`.
- Produces: `AmsError::user_msg` (`std::string`), populated by `prepare_for_resume`. Empty means no authored copy was supplied and the caller falls back to the firmware string.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("a resume failure carries authored copy, not firmware text", "[ams][resume]") {
    // Snapmaker firmware writes print_stats.message as "Filament Sensor
    // e0_filament: Runout Detected". A modal that renders that verbatim shows
    // the user a 0-based extruder name we do not control.
    // ... drive a backend to a runout with firmware text set ...
    CHECK(err.user_msg.find("e0_filament") == std::string::npos);
    CHECK(err.user_msg.find("Slot 1") != std::string::npos);
}
```

- [ ] **Step 2: Run and confirm it fails**

- [ ] **Step 3: Populate the field and prefer it at the four sites**

Each site prefers the authored message and falls back to the firmware string only when none was supplied, so a backend that has not been taught yet degrades to today's behaviour rather than to an empty modal.

Do not add a regex normalizer over `e(\d+)_filament`. It needs a per-backend pattern table that rots as firmware wording changes, and it cannot produce a backend-specific noun.

- [ ] **Step 4: Verify on a real paused print**

```bash
HELIX_MOCK_AUTO_PRINT=1 SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test --sim-speed 6 -vv \
  --remote-socket "$HELIX_SOCK" > /tmp/helix-resume.log 2>&1 &
```

Drive to a pause with `ctl` and read the overlay text with `ctl text`, not a screenshot.

- [ ] **Step 5: Commit**

```bash
git commit -m "fix(ams): author resume failure copy instead of echoing firmware (prestonbrown/helixscreen#957)" -- include src tests/unit
```

---

### Task 10: The lint gate

Last, so it is written against a tree that already passes.

**Files:**
- Modify: `tests/shell/test_code_lint.bats`

**Interfaces:**
- Consumes: nothing at runtime.
- Produces: a gate.

- [ ] **Step 1: Write the meta-test first**

A gate whose green has never been checked against a red case proves nothing. Assert it fires:

```bash
@test "tool label gate rejects a hand-built T<n>" {
    local tmp="$BATS_TEST_TMPDIR/offender.cpp"
    printf 'void f(int i) { snprintf(b, 8, "T%%d", i); }\n' > "$tmp"
    run bash -c "SCAN_ROOT='$BATS_TEST_TMPDIR' bash '$BATS_TEST_DIRNAME/../../scripts/check_tool_labels.sh'"
    [ "$status" -ne 0 ]
}
```

Note for the implementer: a mid-body `[[ ]]` is inert under macOS bash 3.2, where only the last statement's status counts. Use `[ ]` or put the assertion last.

- [ ] **Step 2: Run and confirm it fails**

Expected: the script does not exist yet.

- [ ] **Step 3: Write the gate**

Forbid, outside the allowlist:
- a `T` glued to an integer: `"T%d"`, `"T{}"`, `"T" + std::to_string(...)`
- a bare `+ 1` applied to an identifier named `*slot_index`, `*lane*` or `*unit_index` inside a formatting call

Allowlist: `src/ui/display_numbering.cpp`, the gcode emitters under `src/printer/ams_backend_*`, `src/rendering/gcode_tool_remapper.cpp`, and `src/printer/filament_slot_override_store.cpp#format_lane_key`. Escape hatch comment for a one-off, matching the style of the existing `// RTTI_OK:` hatch.

- [ ] **Step 4: Confirm the gate passes on the real tree and fails on the offender**

```bash
bats tests/shell/test_code_lint.bats
```

Both directions must be shown. A gate that only goes green has not been tested.

- [ ] **Step 5: Commit**

```bash
git commit -m "test(lint): forbid hand-built tool labels and position offsets (prestonbrown/helixscreen#957)" -- tests/shell scripts
```

---

## Closing out

- [ ] Run the full suite, not a tag filter. A green tag filter is not a regression check.

```bash
make test-run
```

- [ ] Run the mutation gate over the whole change.

```bash
scripts/zeus-run.sh mutate --tests '[ams],[numbering],[print_start],[tool_state],[filament_mapper]'
```

The tag union must be well above the shard count (~94) or Catch2 exits nonzero for shards matching zero tests and the baseline reads red.

- [ ] Delete this plan and the design doc in the commit that ships the work. They are scaffolding; durable knowledge belongs in `docs/devel/FILAMENT_MANAGEMENT.md` and in the code.
- [ ] Update `docs/devel/FILAMENT_MANAGEMENT.md`: it says "slot" throughout, which is now inconsistent with an AFC or Happy Hare machine. File a follow-up issue if it grows past a paragraph.
