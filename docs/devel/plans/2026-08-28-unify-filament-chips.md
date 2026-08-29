# Unify Filament Chips Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the stacked two-tone `filament_swatch` chip on every AMS backend by moving chip rendering into `FilamentMappingCard`, retiring `filament_mapping_pill` and the second detail-view card.

**Architecture:** `FilamentMappingCard` already owns `mappings_`, `tool_info_`, `available_slots_` and `used_tools_`. Today it renders pills from that data, while `PrintSelectDetailView::update_color_swatches()` renders the stacked chips from an independently re-derived copy of the same three things. This plan makes the card the single chip renderer, deletes the detail view's renderer and its card, and collapses the two mutually-exclusive visibility predicates into one.

**Tech Stack:** C++17, LVGL 9.5, helix-xml (runtime XML), Catch2 v3, pure Makefile.

**Spec:** This document. Design settled in session 2026-08-28; decisions recorded under "Settled decisions" below.

## Global Constraints

- Declarative UI rules (CLAUDE.md): no new `lv_obj_add_event_cb`, no new imperative visibility/text/styling outside the documented structural exceptions. Per-item payload on a C++-generated collection IS such an exception and must carry `// DECLARATIVE_OK: <reason>`.
- Design tokens only: `theme_manager_get_color("...")`, `#space_*`, `text_small`/`text_body`. No `lv_color_hex()` literals for themeable colors.
- spdlog only. SPDX header on every new file.
- `make -j6` (check `pgrep -x -d' ' 'make|cc1plus'` and `free -h` first; never a second `make` in the same tree).
- Tests must be proven able to fail: `make mutate-diff`, and name the mutation in the commit body.
- Commit style: subject + ~4-line paragraph. Backticks in `-m` are eaten; use `git commit -F`.
- Widget renames break `lv_obj_find_by_name` lookups AND "first unnamed child" test selectors. Grep `tests/` before renaming any widget.

## Settled decisions

| Question | Decision |
|---|---|
| Scope | All AMS backends. `filament_mapping_pill` and `filament_mapping_more_pill` are retired. |
| Where rendering lives | Inside `FilamentMappingCard`, replacing the pill body of `rebuild_compact_view()`. The card is kept, not removed. |
| Surviving card title | `FILAMENTS` |
| Chevron | Kept. It is the app-wide "this opens something" idiom (`setting_action_row.xml`, `ams_edit_overlay.xml`), and the MAPPING card has no cue today. |
| Sliced colors toggle | Moves out of the card header to its own row at the top of `options_section`, above the FILAMENTS card. It recolors the 3D preview, not the chips. |
| Surviving XML node | `filament_mapping_card`. `color_requirements_card` is deleted and its chrome (chevron, empty-tools warning icon, 40px skeleton) moves in. Keeping this name avoids churn in `lv_obj_find_by_name` lookups and matches the class name. |

## Why this fixes the K2 remap bug

Reported: on a K2 Plus (CFS), remapping T3/lane 4 to T2/lane 3 and accepting left the chip showing `4`.

Root cause: `FilamentMappingCard::set_mappings()` (`include/ui_filament_mapping_card.h:109`) stores new mappings and fires `on_mappings_changed_`, but never calls `rebuild_compact_view()`. The pill's lane number is written imperatively during that rebuild, so nothing repaints. The card's own modal callback (`src/ui/ui_filament_mapping_card.cpp:418-424`) does the identical three steps *with* the rebuild, but `PrintSelectDetailView` overrides the card's tap via `set_on_tap()` (`:332`), making that callback dead on this screen. `on_mappings_changed_` lands in `refresh_preview_colors_and_mismatch()`, which repaints swatches only `if (color_swatches_visible_ == 1)` — and that subject is `!mapping_visible` by construction, so on CFS the one surface refreshed is the one not on screen.

The U1 does not have the bug for the complementary reason: its card is hidden, so `color_swatches_visible_` is 1 and the swatch path repaints.

After Task 2 there is one store and one renderer, and every mutator repaints. Task 2 alone closes the bug; the rest is the consolidation.

## File Structure

| File | Change | Responsibility after |
|---|---|---|
| `include/filament_mapper.h` | Modify | Gains `resolve_mapped_slot()`; `mapped_lane_display_number()` becomes a thin caller of it. |
| `src/printer/filament_mapper.cpp` | Modify | Single implementation of "which AvailableSlot does this mapping point at". |
| `include/ui_filament_mapping_card.h` | Modify | `set_mappings()` repaints. Doc comments corrected. |
| `src/ui/ui_filament_mapping_card.cpp` | Modify | `rebuild_compact_view()` renders `filament_swatch`. `open_mapping_modal()` folded onto `set_mappings()`. `should_show()` gates merged. |
| `include/ui_print_select_detail_view.h` | Modify | Loses `update_color_swatches`, `color_swatches_row_`, `color_requirements_card_`, `color_swatches_visible_`, `swatches_card_visible_for`. |
| `src/ui/ui_print_select_detail_view.cpp` | Modify | Same. `render_authoritative_chips()` stops branching on two surfaces. |
| `ui_xml/print_file_detail.xml` | Modify | One FILAMENTS card. Toggle row moved above it. |
| `ui_xml/components/filament_mapping_pill.xml` | Delete | — |
| `ui_xml/components/filament_mapping_more_pill.xml` | Delete | — |
| `tests/unit/test_filament_mapping_used_filter.cpp` | Modify | Gains the repaint + unknown-color render tests. |
| `tests/unit/test_filament_mapping_visibility.cpp` | Create | Pins the merged `should_show()` predicate. |
| `tests/unit/test_print_select_detail_subjects.cpp` | Modify | Drops `color_swatches_visible`. |

---

### Task 1: Move the Sliced colors toggle out of the card header

The toggle drives `detail_prefer_sliced_colors_`, which `apply_preview_colors()` (`src/ui/ui_print_select_detail_view.cpp:1327`) uses to choose which mappings feed the 3D preview. It has no effect on the chips. It is pure XML: `on_toggle_sliced_colors` is registered globally (`src/ui/ui_panel_print_select.cpp:359`) and no C++ looks up `sliced_colors_toggle` by name, so this is a move with no C++ change.

**Files:**
- Modify: `ui_xml/print_file_detail.xml` (remove the toggle wrapper from the MAPPING header ~`:283-295`; insert a new row into `options_section` just before the FILAMENTS card ~`:178`)

**Interfaces:**
- Consumes: nothing.
- Produces: a row named `sliced_colors_row` at the top of `options_section`. Task 4 assumes the MAPPING card header no longer contains a toggle.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/test_print_select_detail_subjects.cpp`:

```cpp
TEST_CASE_METHOD(PrintSelectDetailSubjectsFixture,
                 "Sliced colors toggle sits outside the filament card",
                 "[print_select][detail][xml]") {
    // The toggle recolors the 3D preview, not the chips. Task 4 merges the two
    // cards and the header cannot carry the toggle, the chevron and two icons
    // at 480x272 — so the toggle must already live outside the card.
    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);

    lv_obj_t* const toggle = lv_obj_find_by_name(root, "sliced_colors_toggle");
    REQUIRE(toggle != nullptr);

    lv_obj_t* const card = lv_obj_find_by_name(root, "filament_mapping_card");
    REQUIRE(card != nullptr);

    // Walk up from the toggle: the filament card must not be an ancestor.
    for (lv_obj_t* p = lv_obj_get_parent(toggle); p != nullptr; p = lv_obj_get_parent(p)) {
        CHECK(p != card);
    }
}
```

**Verified, and this needs a spike first.** `tests/unit/test_print_select_detail_subjects.cpp` uses `LVGLUITestFixture` directly — there is no custom fixture and no `create_detail_view_root()`. More importantly, NO test in the tree instantiates the `print_file_detail` XML root today (the six files matching that string use it only as a Catch2 tag). `LVGLUITestFixture` does call the production `helix::register_xml_components()` (`tests/lvgl_ui_test_fixture.cpp:204`) and initialises subjects before components, so the root is *expected* to build — but that is untested.

So replace `create_detail_view_root()` with a local helper in the test file:

```cpp
namespace {
// No fixture builds this root today. LVGLUITestFixture registers every
// production component and initialises subjects first, so this should work;
// if it returns null, the detail view's own subjects are not part of the
// fixture's Phase 4 and this whole XML-structure approach is not viable.
lv_obj_t* make_detail_root(lv_obj_t* parent) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "print_file_detail", nullptr));
}
} // namespace
```

and open the test with `lv_obj_t* const root = make_detail_root(test_screen()); REQUIRE(root != nullptr);`.

**Spike before writing the rest:** compile just that REQUIRE and run it. If the root builds, continue as planned. If it returns null, do NOT fight the fixture — drop the XML-structure assertions from Task 1 and Task 4 entirely and rely on Task 5's `ctl ls` / `ctl geom` checks for the structural verification instead. The C++ behaviour in Tasks 2 and 3 is where the real coverage lives; these two are confirmation, not the safety net.

- [ ] **Step 2: Run it and watch it fail**

```bash
make -j6 test && ./build/bin/helix-tests "[print_select][detail][xml]"
```
Expected: FAIL — the toggle's ancestor chain currently includes `filament_mapping_card`.

- [ ] **Step 3: Move the toggle in XML**

Delete this wrapper from the MAPPING card header in `ui_xml/print_file_detail.xml`:

```xml
            <lv_obj width="content"
                    height="content" style_pad_all="0" style_pad_gap="#space_xs" flex_flow="row"
                    style_flex_cross_place="center" scrollable="false" clickable="false" event_bubble="true">
              <text_small name="sliced_colors_label"
                          text="Sliced colors" translation_tag="Sliced colors" clickable="false" event_bubble="true"/>
              <ui_switch name="sliced_colors_toggle" size="small">
                <bind_state_if_eq subject="detail_prefer_sliced_colors" state="checked" ref_value="1"/>
                <event_cb trigger="value_changed" callback="on_toggle_sliced_colors"/>
              </ui_switch>
            </lv_obj>
```

Insert this immediately after the `history_status_row` block (before the FILAMENTS card comment, ~`:178`):

```xml
        <!-- Sliced colors toggle. Belongs to the 3D PREVIEW, not the chips:
             apply_preview_colors() uses detail_prefer_sliced_colors to pick
             which mappings feed the viewer (sliced = each tool's own slicer
             color; actual = the matched-lane colors). It sits at the top of the
             right column because the preview owns the left column and is
             flex_grow=1 there — a row under it would steal render height. -->
        <lv_obj name="sliced_colors_row"
                width="100%" height="content" flex_flow="row" style_pad_all="0" style_pad_gap="#space_xs"
                style_flex_main_place="space_between" style_flex_cross_place="center"
                style_pad_bottom="#space_sm" scrollable="false">
          <text_small name="sliced_colors_label"
                      text="Sliced colors" translation_tag="Sliced colors"/>
          <ui_switch name="sliced_colors_toggle" size="small">
            <bind_state_if_eq subject="detail_prefer_sliced_colors" state="checked" ref_value="1"/>
            <event_cb trigger="value_changed" callback="on_toggle_sliced_colors"/>
          </ui_switch>
        </lv_obj>
```

Note the dropped `clickable="false" event_bubble="true"` on the label and wrapper: those existed so taps reached the tappable MAPPING card. This row is not inside a tappable card, so they are noise here.

- [ ] **Step 4: Run the test — it passes**

```bash
./build/bin/helix-tests "[print_select][detail][xml]"
```
XML is loaded at runtime, so no rebuild is needed for the XML change itself; the test binary only needs rebuilding for Step 1's new test.

- [ ] **Step 5: Eyeball it**

```bash
export HELIX_SOCK=/tmp/helix-unify-filament-chips.sock
export HELIX_CONFIG_DIR=/tmp/helix-config-unify-filament-chips
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-chips.log 2>&1 &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate print_select
./build/bin/helix-screen ctl -s "$HELIX_SOCK" geom sliced_colors_row
./build/bin/helix-screen ctl -s "$HELIX_SOCK" geom filament_mapping_card
```
Expected: `sliced_colors_row` sits above `filament_mapping_card` with no overlap. Kill the instance when done (`pkill` by the captured PID, never `pkill -f`).

- [ ] **Step 6: Commit**

```bash
git add ui_xml/print_file_detail.xml tests/unit/test_print_select_detail_subjects.cpp
git commit -F - <<'EOF'
refactor(print-detail): move sliced-colors toggle out of the mapping header

The toggle drives detail_prefer_sliced_colors, which apply_preview_colors()
uses to pick the mappings feeding the 3D viewer - it never touched the chips.
It lived in the MAPPING header on the theory that it "only has meaning
relative to the mapping shown below it", which is true but points at the wrong
widget. Moving it to its own row at the top of the right column frees the
header for the card merge and puts the control with what it recolors.
EOF
```

---

### Task 2: Render `filament_swatch` from inside `FilamentMappingCard`

This is the task that fixes the reported bug. At the end of it, CFS/AFC show the stacked chips and an accepted remap repaints.

Two behaviours must survive the port, both of which exist in exactly one of the two renderers today:

1. **Unknown gcode color renders as a transparent outline, not a grey fill.** The pill honours `GcodeToolInfo::color_known` (`src/ui/ui_filament_mapping_card.cpp:286-308`); `update_color_swatches()` just falls back to `text_muted`. That grey fill is the K2 Plus report the pill fixed: a grey dot pointing at a real black lane reads as "this file prints in grey", a claim nothing made.
2. **A mapping that outlived its lane shows no number.** `FilamentMapper::mapped_lane_display_number()` returns -1 when no slot matches (`src/printer/filament_mapper.cpp:60-65`); `update_color_swatches()` re-derives `resolved->local_slot_index + 1` inline with no such guard. Task 2 folds both onto one function.

**Files:**
- Modify: `include/filament_mapper.h`, `src/printer/filament_mapper.cpp` (add `resolve_mapped_slot`)
- Modify: `src/ui/ui_filament_mapping_card.cpp:249-395` (`rebuild_compact_view` body), `:412-426` (`open_mapping_modal`)
- Modify: `include/ui_filament_mapping_card.h:100-119` (`set_mappings`)
- Modify: `ui_xml/print_file_detail.xml` (`filament_mapping_rows` height)
- Create: `tests/mapping_card_render_fixture.h`
- Test: `tests/unit/test_filament_mapping_used_filter.cpp`

**Two controller rulings folded in (pre-flight scan):**

1. **`filament_mapping_rows` must change from `height="content"` to `height="32"` in this task**, not in Task 4. `filament_swatch` is `height="100%"` and collapses to zero inside a content-height parent, so without this the app renders zero-height chips between Task 2 and Task 4. The unit tests cannot catch it — they render into a container the fixture builds, not the XML one. Task 4's replacement block already carries `height="32"`, so the two agree.
2. **Extract `MappingCardRenderFixture` into a new `tests/mapping_card_render_fixture.h`** and have `tests/unit/test_filament_mapping_used_filter.cpp` include it. Task 3 includes the same header. Without this the fixture reaches a fourth hand-written copy, which the Global Constraints forbid. Move the struct verbatim — same members, same constructor/destructor bodies, same includes it depends on (`ui_filament_mapping_card.h`, `ui_update_queue.h`, `../lvgl_ui_test_fixture.h`, `ams_backend_mock.h`, `ams_state.h`) — and give the header an SPDX line plus `#pragma once`. Do not change its behaviour while moving it; the existing idempotent-rebuild test must stay green as proof the move was faithful.

**Interfaces:**
- Consumes: `FilamentMappingCard` members `tool_info_`, `mappings_`, `available_slots_` (already present).
- Produces: `static const AvailableSlot* FilamentMapper::resolve_mapped_slot(const ToolMapping&, const std::vector<AvailableSlot>&)` — returns nullptr for auto/unmapped/outlived. Task 4 relies on the card rendering `filament_swatch` children into its rows container.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_filament_mapping_used_filter.cpp`. The `MappingCardRenderFixture` already defined in that file is reused (a fourth copy of it would be the twin CLAUDE.md forbids).

```cpp
// ============================================================================
// Stacked-swatch render + repaint
// ============================================================================

namespace {

// Lane number currently drawn in chip `idx`'s bottom band, or "" when hidden.
// Re-fetches the chip every call: a real rebuild reparents the old children
// away, so a cached pointer would report pre-rebuild text forever and the test
// would pass on a broken card.
std::string rendered_lane_number(lv_obj_t* rows, uint32_t idx) {
    lv_obj_t* const chip = lv_obj_get_child(rows, static_cast<int32_t>(idx));
    if (chip == nullptr) {
        return "<no chip>";
    }
    lv_obj_t* const band = lv_obj_find_by_name(chip, "bottom_band");
    if (band == nullptr) {
        return "<no bottom_band>";
    }
    lv_obj_t* const lbl = lv_obj_find_by_name(band, "slot_label");
    if (lbl == nullptr) {
        return "<no slot_label>";
    }
    if (lv_obj_has_flag(lbl, LV_OBJ_FLAG_HIDDEN)) {
        return "";
    }
    return lv_label_get_text(lbl);
}

// One explicit mapping per tool, so the rendered lane number is a pure function
// of the slot index chosen here rather than of the seeding heuristic.
std::vector<ToolMapping> mappings_to_slots(const std::vector<int>& slots) {
    std::vector<ToolMapping> m;
    m.reserve(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        ToolMapping tm;
        tm.tool_index = static_cast<int>(i);
        tm.mapped_slot = slots[i];
        tm.mapped_backend = 0;
        tm.is_auto = false;
        tm.reason = ToolMapping::MatchReason::FIRMWARE_MAPPING;
        m.push_back(tm);
    }
    return m;
}

} // namespace

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "FilamentMappingCard renders stacked filament_swatch chips",
                 "[filament_mapping][swatch]") {
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(card.should_show());
    REQUIRE(lv_obj_get_child_count(rows) == 2);

    // The stacked chip's structure, not the pill's. A pill has gcode_dot/arrow/
    // slot_dot; a swatch has top_band/divider/bottom_band.
    lv_obj_t* const chip = lv_obj_get_child(rows, 0);
    CHECK(lv_obj_find_by_name(chip, "top_band") != nullptr);
    CHECK(lv_obj_find_by_name(chip, "divider") != nullptr);
    CHECK(lv_obj_find_by_name(chip, "bottom_band") != nullptr);
    CHECK(lv_obj_find_by_name(chip, "gcode_dot") == nullptr);

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "FilamentMappingCard::set_mappings repaints the lane number",
                 "[filament_mapping][remap]") {
    // The native-remap flow does not go through the card's own modal callback:
    // the print-detail view overrides the card's tap handler, so an accepted
    // remap arrives here as a bare set_mappings() call. The lane number is
    // written imperatively during rebuild, so a setter that stores without
    // rebuilding leaves the old lane on screen while the print runs the new one.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(lv_obj_get_child_count(rows) == 2);

    // Tool 0 on the mock's 4th lane (local_slot_index 3 => displayed "4").
    card.set_mappings(mappings_to_slots({3, 1}));
    CHECK(rendered_lane_number(rows, 0) == "4");

    // The reported scenario: remap tool 0 down to the 3rd lane, same colour but
    // more filament. The chip must follow the mapping.
    card.set_mappings(mappings_to_slots({2, 1}));
    CHECK(rendered_lane_number(rows, 0) == "3");

    // Untouched tools keep their lane — a repaint, not a reset.
    CHECK(rendered_lane_number(rows, 1) == "2");

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "Unknown gcode colour draws no fill on the top band",
                 "[filament_mapping][swatch]") {
    // A K2 Plus report: painting the neutral stand-in reads as "this file prints
    // in grey", a claim about the file that nothing has made. The pill fixed this
    // via GcodeToolInfo::color_known; the swatch renderer never had the guard, so
    // it must not be lost in the move. Empty palette entry => colour unknown.
    card.update({"", ""}, {"PLA", "PETG"});
    REQUIRE(lv_obj_get_child_count(rows) == 2);

    lv_obj_t* const band = lv_obj_find_by_name(lv_obj_get_child(rows, 0), "top_band");
    REQUIRE(band != nullptr);
    CHECK(lv_obj_get_style_bg_opa(band, 0) == LV_OPA_TRANSP);

    process_lvgl(100);
}

TEST_CASE("resolve_mapped_slot: a mapping that outlived its lane resolves to nothing",
          "[filament][mapping][mapper]") {
    // Unit unplugged between the mapping and the render. Better to show no
    // number than a stale one — and the swatch renderer used to re-derive
    // local_slot_index + 1 inline, with no such guard.
    std::vector<helix::AvailableSlot> slots;
    helix::AvailableSlot s{};
    s.slot_index = 0;
    s.backend_index = 0;
    s.local_slot_index = 0;
    slots.push_back(s);

    ToolMapping gone;
    gone.tool_index = 0;
    gone.mapped_slot = 7; // no such lane
    gone.mapped_backend = 0;
    CHECK(helix::FilamentMapper::resolve_mapped_slot(gone, slots) == nullptr);
    CHECK(helix::FilamentMapper::mapped_lane_display_number(gone, slots) == -1);

    ToolMapping automatic;
    automatic.tool_index = 0;
    automatic.is_auto = true;
    CHECK(helix::FilamentMapper::resolve_mapped_slot(automatic, slots) == nullptr);

    ToolMapping ok;
    ok.tool_index = 0;
    ok.mapped_slot = 0;
    ok.mapped_backend = 0;
    REQUIRE(helix::FilamentMapper::resolve_mapped_slot(ok, slots) == &slots[0]);
    CHECK(helix::FilamentMapper::mapped_lane_display_number(ok, slots) == 1);
}
```

- [ ] **Step 2: Run them and watch them fail**

```bash
make -j6 test && ./build/bin/helix-tests "[filament_mapping][swatch]" "[filament_mapping][remap]" "[mapper]"
```
Expected: the swatch tests fail because the card renders pills (`top_band` is null, `gcode_dot` is not); the mapper test fails to compile until Step 3 adds `resolve_mapped_slot`. Compile first, then run.

- [ ] **Step 3: Extract `resolve_mapped_slot` in FilamentMapper**

In `include/filament_mapper.h`, next to `mapped_lane_display_number`:

```cpp
    /// Resolve a mapping to the AvailableSlot it points at, or nullptr.
    /// nullptr covers all three "no lane to name" cases: an explicit "auto"
    /// (the firmware picks at print time), an unmapped tool, and a mapping that
    /// outlived its lane (unit unplugged between the mapping and the render).
    /// The returned pointer aliases `slots` and is invalidated by any mutation
    /// of that vector.
    [[nodiscard]] static const AvailableSlot*
    resolve_mapped_slot(const ToolMapping& mapping, const std::vector<AvailableSlot>& slots);
```

In `src/printer/filament_mapper.cpp`, replace the body of `mapped_lane_display_number` and add the new function:

```cpp
const AvailableSlot* FilamentMapper::resolve_mapped_slot(const ToolMapping& mapping,
                                                         const std::vector<AvailableSlot>& slots) {
    // "Auto" is a deliberate absence of a mapping - the firmware picks at print
    // time - so there is no lane to name yet.
    if (mapping.is_auto || mapping.mapped_slot < 0) {
        return nullptr;
    }
    for (const auto& s : slots) {
        if (s.slot_index == mapping.mapped_slot && s.backend_index == mapping.mapped_backend) {
            return &s;
        }
    }
    // A mapping that outlived its lane. Better to show nothing than a stale lane.
    return nullptr;
}

int FilamentMapper::mapped_lane_display_number(const ToolMapping& mapping,
                                               const std::vector<AvailableSlot>& slots) {
    const AvailableSlot* const s = resolve_mapped_slot(mapping, slots);
    return s ? s->local_slot_index + 1 : -1;
}
```

- [ ] **Step 4: Replace the pill body of `rebuild_compact_view()` with the swatch render**

In `src/ui/ui_filament_mapping_card.cpp`, keep everything above the per-tool loop unchanged — the `rows_container_` guard, the `scoped_freeze` + `drain()`, the post-drain re-check (#1221), the fingerprint compute and the early return, and `safe_clean_children()`. Replace only the per-tool creation loop (and delete the overflow/`more_pill` block that follows it) with:

```cpp
    const bool multi_tool = tool_info_.size() > 1;
    const lv_color_t neutral = theme_manager_get_color("text_muted");

    for (size_t i = 0; i < tool_info_.size(); ++i) {
        const auto& tool = tool_info_[i];
        auto* chip =
            static_cast<lv_obj_t*>(lv_xml_create(rows_container_, "filament_swatch", nullptr));
        if (!chip) {
            continue;
        }
        // Fix the chip width in code: a numeric width on a component <view> root
        // is not honoured by lv_xml_create (only "content"/"%"), and the band
        // labels use flex_grow (which contributes 0 to content width), so without
        // this the whole chip collapses to 0.
        lv_obj_set_width(chip, 40); // DECLARATIVE_OK: lv_xml_create width limitation

        // mappings_ is built parallel to tool_info_ and compacted in lockstep by
        // apply_used_tools_filter, so index i is this tool's mapping. Fall back to
        // a default mapping rather than indexing past the end if that ever drifts.
        const ToolMapping mapping = (i < mappings_.size()) ? mappings_[i] : ToolMapping{};
        const helix::AvailableSlot* const resolved =
            helix::FilamentMapper::resolve_mapped_slot(mapping, available_slots_);

        // Every mutation below is per-item payload on a C++-generated collection:
        // the card builds one chip per used tool from runtime data, so there is no
        // XML instance per tool to hang a bind on.

        // TOP band: the gcode file's intended colour for this tool.
        if (auto* top = lv_obj_find_by_name(chip, "top_band")) {
            if (tool.color_known) {
                lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0); // DECLARATIVE_OK: see above
                lv_obj_set_style_bg_color(top, lv_color_hex(tool.color_rgb), 0);
            } else {
                // No fill: painting the neutral stand-in reads as "this file
                // prints in grey", a claim nothing has made. (K2 Plus report.)
                lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0); // DECLARATIVE_OK: see above
            }
            if (auto* tool_lbl = lv_obj_find_by_name(top, "tool_label")) {
                if (multi_tool) {
                    lv_label_set_text_fmt(tool_lbl, "T%d", tool.tool_index);
                    // Contrast is computed against the fill; with no fill there is
                    // nothing to contrast against, so take the normal text colour.
                    lv_obj_set_style_text_color(
                        tool_lbl,
                        tool.color_known
                            ? theme_manager_get_contrast_color(lv_color_hex(tool.color_rgb))
                            : theme_manager_get_color("text"),
                        0);
                    lv_obj_remove_flag(tool_lbl, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(tool_lbl, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        // BOTTOM band: the present colour of the effective mapped lane.
        const bool slot_empty = resolved && resolved->is_empty;
        const lv_color_t slot_color =
            (resolved && !resolved->is_empty) ? lv_color_hex(resolved->color_rgb) : neutral;
        if (auto* bottom = lv_obj_find_by_name(chip, "bottom_band")) {
            if (slot_empty) {
                // Declarative empty_slot style (warning border, reduced opacity)
                // declared in filament_swatch.xml under selector user_1.
                lv_obj_add_state(bottom, LV_STATE_USER_1);
            } else if (resolved) {
                lv_obj_set_style_bg_color(bottom, slot_color, 0); // DECLARATIVE_OK: see above
            } else {
                // No lane chosen yet: naming one would be a claim the mapping has
                // not made, so the band stays blank.
                lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0); // DECLARATIVE_OK: see above
            }
            if (auto* slot_lbl = lv_obj_find_by_name(bottom, "slot_label")) {
                const int lane_number =
                    helix::FilamentMapper::mapped_lane_display_number(mapping, available_slots_);
                if (lane_number > 0) {
                    lv_label_set_text_fmt(slot_lbl, "%d", lane_number);
                    // An empty lane draws no fill, so there is nothing to contrast
                    // against - take the warning colour that the band border uses.
                    lv_obj_set_style_text_color(slot_lbl,
                                                slot_empty
                                                    ? theme_manager_get_color("warning")
                                                    : theme_manager_get_contrast_color(slot_color),
                                                0);
                    lv_obj_remove_flag(slot_lbl, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(slot_lbl, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        // Divider: a colour that reads against BOTH band fills. Blend the two
        // 50/50 and take the contrast of the blend, so the rule stays visible
        // whether the bands are light, dark or mixed.
        if (auto* divider = lv_obj_find_by_name(chip, "divider")) {
            const lv_color_t top_color =
                tool.color_known ? lv_color_hex(tool.color_rgb) : neutral;
            lv_obj_set_style_bg_color(
                divider,
                theme_manager_get_contrast_color(lv_color_mix(top_color, slot_color, LV_OPA_50)),
                0); // DECLARATIVE_OK: see above
        }
    }
```

Delete the `if (overflow) { ... }` block and the `visible`/`overflow`/`count` locals that fed it. The 40px chip is a third the width of a 48% pill, so a 6-chip cap no longer earns its complexity; if a very wide palette ever overflows, the row wraps (`flex_flow="row_wrap"` on `filament_mapping_rows`).

Confirm `#include "filament_mapper.h"` and `#include "theme_manager.h"` are already present in this file; add whichever is missing.

- [ ] **Step 5: Make `set_mappings()` repaint, and fold the twin onto it**

In `include/ui_filament_mapping_card.h`, replace the `set_mappings` doc block and body:

```cpp
    /**
     * @brief Replace the current tool→slot mappings and repaint.
     *
     * The single mapping-store writer. Stores, re-renders the chips, then fires
     * on_mappings_changed_ so downstream consumers (preview colours, pre-flight
     * gate) re-evaluate. The repaint is not optional: the lane number inside each
     * chip is written imperatively during rebuild_compact_view(), so a store
     * without a rebuild leaves the pre-remap lane on screen while the print runs
     * the new one. Safe to call before create() — rebuild_compact_view() returns
     * early when rows_container_ is null.
     */
    void set_mappings(std::vector<helix::ToolMapping> mappings) {
        mappings_ = std::move(mappings);
        rebuild_compact_view();
        if (on_mappings_changed_) {
            on_mappings_changed_();
        }
    }
```

Then in `src/ui/ui_filament_mapping_card.cpp:417-423`, collapse the internal modal callback onto that one writer so the two cannot drift again:

```cpp
    mapping_modal_.set_on_mappings_updated(
        [this](auto mappings) { set_mappings(std::move(mappings)); });
```

Also correct the stale comment at `src/ui/ui_print_select_detail_view.cpp:337-342` — "The card already refreshed its own slot/mapping state from the user's edit" was true only of the internal-modal path that `set_on_tap` disabled. Replace with:

```cpp
    filament_mapping_card_.set_on_mappings_changed([this]() {
        // Fired by set_mappings() AFTER it repaints the chips, so this is only
        // the downstream work: re-colour the preview and re-gate pre-flight.
        // Synchronous, on the main thread, so a direct call is safe.
        refresh_preview_colors_and_mismatch();
    });
```

- [ ] **Step 6: Run the tests — all pass**

```bash
make -j6 test && ./build/bin/helix-tests "[filament_mapping]" "[mapper]" "[filament]"
```
Expected: PASS. `test_filament_mapping_drain_reentrancy.cpp` and the idempotent-rebuild case must stay green — the fingerprint and the freeze/drain sequence were deliberately left untouched.

- [ ] **Step 7: Prove the tests can fail**

```bash
make mutate-diff
```
Expected: reverting the `rebuild_compact_view()` line inside `set_mappings` turns `[filament_mapping][remap]` red; reverting the `color_known` branch turns `[filament_mapping][swatch]` red. Name one of those mutations in the commit body.

- [ ] **Step 8: Commit**

```bash
git add include/filament_mapper.h src/printer/filament_mapper.cpp \
        include/ui_filament_mapping_card.h src/ui/ui_filament_mapping_card.cpp \
        src/ui/ui_print_select_detail_view.cpp tests/unit/test_filament_mapping_used_filter.cpp
git commit -F - <<'EOF'
fix(print-detail): repaint the filament chip when a remap is accepted

set_mappings() stored the accepted mapping and fired on_mappings_changed_ but
never rebuilt, so the lane number - written imperatively during the rebuild -
kept showing the pre-remap lane. The card's own modal callback did the same
three steps with the rebuild, but the detail view overrides the card tap, so
that path is dead on this screen. Folded both onto one writer and moved the
stacked filament_swatch render into the card, which also restores the
unknown-colour outline and the outlived-lane guard that only one renderer had.
Mutation: dropping rebuild_compact_view() from set_mappings turns the remap
test red.
EOF
```

---

### Task 3: Merge the two visibility predicates

Today the card and the swatch row are mutually exclusive and gated by different rules. One card now serves every backend, so the union has to be deliberate.

| Rule | Source | Fate |
|---|---|---|
| AMS available | `should_show()` | Keep |
| **any backend editable** | `should_show()` | **Drop.** It exists to avoid a dead control, which is now the *tap affordance's* job — `color_card_opens_remap()` already gates that correctly. Keeping it would hide the card on U1/ACE, which is the whole point of the merge. |
| NOT(bypass && ≤1 lane) | `should_show()` | Keep. A K2 user read the chips as "this maps to lane 2", tapped to confirm, and printed off the bypass spool. |
| tool_info non-empty | `should_show()` | Keep |
| multi-tool printer → tools>0, else tools>1 | `swatches_card_visible_for()` | Keep, moved into the card. |

**Files:**
- Modify: `src/ui/ui_filament_mapping_card.cpp:62-150` (`update`), `include/ui_filament_mapping_card.h`
- Modify: `src/ui/ui_print_select_detail_view.cpp:2064-2071` (delete `swatches_card_visible_for`)
- Test: `tests/unit/test_filament_mapping_visibility.cpp` (create)

**Interfaces:**
- Consumes: `FilamentMapper::resolve_mapped_slot` from Task 2.
- Produces: `should_show()` true on non-editable backends. Task 4 relies on `filament_mapping_visible` being the only visibility subject.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_filament_mapping_visibility.cpp`:

**Controller ruling (pre-flight scan):** do NOT hand-write a fixture here. Task 2 extracted `MappingCardRenderFixture` into `tests/mapping_card_render_fixture.h`; include that and use it. The `VisibilityFixture` shown below is exactly the copy-paste twin the Global Constraints forbid — it is reproduced only so you can see which members the tests need. Replace it with `#include "../mapping_card_render_fixture.h"` and `TEST_CASE_METHOD(MappingCardRenderFixture, ...)`.

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapping_visibility.cpp
 * @brief Pins the merged FilamentMappingCard::should_show() predicate.
 *
 * The card used to hide itself on backends whose tool mapping is not editable
 * (Snapmaker U1, ACE), because a second surface - the print-detail FILAMENTS
 * card - rendered the same information there. That second surface is gone, so
 * hiding on non-editable backends would show the user nothing at all. The
 * dead-control concern it existed for now lives entirely on the tap affordance
 * (PrintSelectDetailView::color_card_opens_remap).
 *
 * The two gates that MUST survive the merge are pinned here too: the bypass
 * single-lane suppression (a K2 user printed off the bypass spool after reading
 * a chip as a lane claim) and the single-extruder tools>1 rule.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentMappingCard;

namespace {

struct VisibilityFixture : LVGLUITestFixture {
    FilamentMappingCard card;
    lv_obj_t* card_widget = nullptr;
    lv_obj_t* rows = nullptr;
    lv_obj_t* warning = nullptr;
    AmsBackendMock* mock = nullptr;

    VisibilityFixture() {
        auto& ams = AmsState::instance();
        ams.init_subjects(false);
        auto owned = std::make_unique<AmsBackendMock>(4);
        mock = owned.get();
        mock->set_operation_delay(0);
        ams.set_backend(std::move(owned));
        mock->start();

        card_widget = lv_obj_create(test_screen());
        rows = lv_obj_create(card_widget);
        warning = lv_obj_create(card_widget);
        card.create(card_widget, rows, warning);
    }

    ~VisibilityFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        if (mock) {
            mock->stop();
        }
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "Card shows on a backend whose mapping is not editable",
                 "[filament_mapping][visibility]") {
    // snapmaker_mode_ makes get_tool_mapping_capabilities() report
    // {supported=true, editable=false} — the U1 case. Before the merge this
    // hid the card and a second surface drew the chips; that surface is gone.
    mock->set_snapmaker_mode(true);
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK(card.should_show());
    CHECK(lv_obj_get_child_count(rows) == 2);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "Card hides a single-lane print while bypass is engaged",
                 "[filament_mapping][visibility]") {
    // With bypass engaged a single-tool print takes filament from the external
    // spool, so offering a lane mapping claims something the print will not do.
    REQUIRE(mock->enable_bypass() == AmsError::None);
    REQUIRE(mock->is_bypass_active());
    card.update({"#FF0000"}, {"PLA"});
    CHECK_FALSE(card.should_show());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "Card shows a single-tool file on a multi-tool printer",
                 "[filament_mapping][visibility]") {
    // The reachable half of the tool-count rule moved from
    // swatches_card_visible_for(): on a multi-tool printer ANY referenced tool
    // is worth showing, because which lane it routes to is the whole question.
    // (The single-extruder tools>1 branch needs a 1-slot backend the 4-slot mock
    // cannot present; it is covered by Task 5's manual verification instead of a
    // test that cannot fail.)
    card.set_used_tools(std::set<int>{0});
    card.update({"#FF0000"}, {"PLA"});
    CHECK(card.should_show());
    CHECK(lv_obj_get_child_count(rows) == 1);
    process_lvgl(100);
}
```

Verified against `include/ams_backend_mock.h`: `set_snapmaker_mode(bool)` exists at `:533`; there is NO `set_bypass_active` — bypass is engaged through the `AmsBackend` override `enable_bypass()` at `:179`, checked with `is_bypass_active()` at `:181`. If `AmsError::None` is spelled differently in this tree, grep `include/ams_backend.h` for the success enumerator. If the single-extruder branch cannot be reached with a 1-slot mock, DELETE that third case rather than ship one that cannot fail.

- [ ] **Step 2: Run and watch the first case fail**

```bash
make -j6 test && ./build/bin/helix-tests "[filament_mapping][visibility]"
```
Expected: "Card shows on a backend whose mapping is not editable" FAILS — `should_show()` is false because of the `any_editable` gate.

- [ ] **Step 3: Drop the `any_editable` gate and fold in the tool-count rule**

In `src/ui/ui_filament_mapping_card.cpp::update()`, delete this block (`:78-94`):

```cpp
    bool any_editable = false;
    for (int i = 0, n = ams.backend_count(); i < n; ++i) {
        auto* backend = ams.get_backend(i);
        if (!backend) {
            continue;
        }
        auto caps = backend->get_tool_mapping_capabilities();
        if (caps.supported && caps.editable) {
            any_editable = true;
            break;
        }
    }
    if (!any_editable) {
        should_show_ = false;
        return;
    }
```

Replace it with the comment explaining why it is gone:

```cpp
    // NO editable-backend gate here. It once existed to avoid a dead control on
    // Snapmaker U1 / ACE, back when a second surface (the print-detail FILAMENTS
    // card) drew the same chips there. That surface is gone, so hiding here would
    // show the user nothing at all. Whether a TAP does anything is a separate
    // question, answered by PrintSelectDetailView::color_card_opens_remap().
```

Then, after the `tool_info_.empty()` check, add the tool-count rule moved from `swatches_card_visible_for()`:

```cpp
    // Moved from PrintSelectDetailView::swatches_card_visible_for(): on a
    // multi-tool printer any referenced tool is worth showing (lane identity
    // matters); on a single extruder it takes 2+ tools to be a manual-swap
    // multi-colour file rather than an ordinary single-colour print.
    const int ams_slots = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    const bool is_multi_tool_printer =
        helix::ToolState::instance().is_multi_tool() || ams_slots > 1;
    const size_t tool_count = used_tools_ ? used_tools_->size() : tool_info_.size();
    if (!(is_multi_tool_printer ? tool_count > 0 : tool_count > 1)) {
        should_show_ = false;
        return;
    }
```

Add `#include "tool_state.h"` if absent.

- [ ] **Step 4: Run the tests — all pass**

```bash
./build/bin/helix-tests "[filament_mapping]"
```

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui_filament_mapping_card.cpp include/ui_filament_mapping_card.h \
        tests/unit/test_filament_mapping_visibility.cpp
git commit -F - <<'EOF'
refactor(print-detail): one visibility rule for the filament card

The card hid itself on backends with non-editable tool mapping because a second
surface drew the same chips there. With one renderer that gate would show the
user nothing, so it moves to the tap affordance, where color_card_opens_remap
already answers it. The bypass single-lane suppression and the single-extruder
tools>1 rule both survive, now in one predicate instead of two that had to stay
mutually exclusive by construction.
EOF
```

---

### Task 4: Collapse the two XML cards into one FILAMENTS card

**Files:**
- Modify: `ui_xml/print_file_detail.xml` (delete `color_requirements_card` ~`:185-239`; retitle and re-chrome `filament_mapping_card` ~`:263-316`)
- Modify: `src/ui/ui_print_select_detail_view.cpp` (`:296`, `:306-320`, `:943-944`, `:1026-1156`, `:1358-1370`, `:1460-1476`)
- Modify: `include/ui_print_select_detail_view.h` (`:558-562`, `:682`, `:1012`, `:1024`)
- Delete: `ui_xml/components/filament_mapping_pill.xml`, `ui_xml/components/filament_mapping_more_pill.xml`
- Modify: `tests/unit/test_print_select_detail_subjects.cpp`

**Interfaces:**
- Consumes: Task 3's merged `should_show()`.
- Produces: one `ui_card name="filament_mapping_card"` titled FILAMENTS; `color_swatches_visible` subject retired; `color_card_remappable` retained and now published from `filament_mapping_card_.should_show()`.

- [ ] **Step 1: Write the failing test**

In `tests/unit/test_print_select_detail_subjects.cpp`:

```cpp
TEST_CASE_METHOD(PrintSelectDetailSubjectsFixture,
                 "One filament card, titled FILAMENTS, with the tap chevron",
                 "[print_select][detail][xml]") {
    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);

    // The second card is gone: one surface for the chips on every backend.
    CHECK(lv_obj_find_by_name(root, "color_requirements_card") == nullptr);
    CHECK(lv_obj_find_by_name(root, "color_swatches_row") == nullptr);

    lv_obj_t* const card = lv_obj_find_by_name(root, "filament_mapping_card");
    REQUIRE(card != nullptr);
    // Chrome absorbed from the retired card: the tap cue and the empty-lane
    // warning, plus the mismatch icon this card already had.
    CHECK(lv_obj_find_by_name(card, "color_card_remap_chevron") != nullptr);
    CHECK(lv_obj_find_by_name(card, "empty_tools_warning_icon") != nullptr);
    CHECK(lv_obj_find_by_name(card, "filament_mismatch_icon") != nullptr);
}
```

Also delete any existing assertion in that file that names the `color_swatches_visible` subject.

- [ ] **Step 2: Run and watch it fail**

```bash
make -j6 test && ./build/bin/helix-tests "[print_select][detail][xml]"
```
Expected: FAIL — `color_requirements_card` still exists.

- [ ] **Step 3: Rewrite the XML**

Delete the whole `color_requirements_card` block. Replace the `filament_mapping_card` header row and skeleton so the surviving card reads:

```xml
        <!-- Filament card. ONE chip surface for every AMS backend: the card
             renders stacked filament_swatch chips from its own mappings store.
             Visibility is the C++-owned filament_mapping_visible subject,
             published after each FilamentMappingCard::update(). Tapping opens
             the remap picker; the chevron says so, and both it and the card's
             clickable flag come from color_card_remappable so the card never
             swallows a tap it would silently drop. -->
        <ui_card name="filament_mapping_card"
                 width="100%" style_pad_hor="0" style_pad_ver="#space_md" style_pad_gap="#space_xs"
                 style_border_width="0" flex_flow="column">
          <bind_flag_if_eq subject="filament_mapping_visible" flag="hidden" ref_value="0"/>
          <bind_flag_if_eq subject="color_card_remappable" flag="clickable" ref_value="1"/>
          <lv_obj width="100%"
                  height="content" style_pad_all="0" style_pad_gap="#space_xs" flex_flow="row"
                  style_flex_cross_place="center" scrollable="false" clickable="false" event_bubble="true">
            <text_small text="FILAMENTS" translation_tag="FILAMENTS" clickable="false" event_bubble="true"/>
            <icon name="filament_mismatch_icon"
                  src="alert" size="sm" variant="warning" clickable="false" event_bubble="true">
              <bind_flag_if_eq subject="filament_mismatch" flag="hidden" ref_value="0"/>
            </icon>
            <icon name="empty_tools_warning_icon"
                  src="alert" size="sm" variant="warning" clickable="false" event_bubble="true">
              <bind_flag_if_eq subject="empty_tools_warning" flag="hidden" ref_value="0"/>
            </icon>
            <!-- Spacer: pushes the tap cue to the trailing edge without giving
                 the label a width that would fight the icons. -->
            <lv_obj name="color_card_header_spacer"
                    width="content" height="1" flex_grow="1" style_pad_all="0" style_bg_opa="0"
                    style_border_width="0" scrollable="false" clickable="false" event_bubble="true"/>
            <!-- The tap cue. Same chevron every tappable row in the app uses
                 (setting_action_row.xml, ams_edit_overlay.xml), so it reads as
                 "this opens something" without a label to translate. -->
            <icon name="color_card_remap_chevron"
                  src="chevron_right" size="sm" variant="secondary" clickable="false" event_bubble="true">
              <bind_flag_if_eq subject="color_card_remappable" flag="hidden" ref_value="0"/>
            </icon>
          </lv_obj>
          <lv_obj name="filament_mapping_rows"
                  width="100%" height="32" style_pad_all="0" style_pad_gap="#space_xs" flex_flow="row_wrap"
                  style_pad_top="#space_xs" scrollable="false" clickable="false" event_bubble="true">
            <bind_flag_if_eq subject="detail_mapping_ready" flag="hidden" ref_value="0"/>
            <!-- Chips added dynamically in C++ (clickable=false + event_bubble=true
                 on filament_swatch) so a tap reaches this card. -->
          </lv_obj>
          <!-- Skeleton: two chip-sized placeholders shown only until the
               authoritative tool set exists — a cache hit never shows this. -->
          <!-- SIZE_OK: skeleton row height mirrors filament_mapping_rows' literal 32 -->
          <lv_obj name="mapping_skeleton"
                  width="100%" height="32" style_pad_all="0" style_pad_gap="#space_xs" flex_flow="row"
                  style_pad_top="#space_xs" scrollable="false" clickable="false" event_bubble="true">
            <bind_flag_if_eq subject="detail_mapping_ready" flag="hidden" ref_value="1"/>
            <!-- SIZE_OK: chip width mirrors filament_swatch.xml's literal 40 -->
            <lv_obj width="40"
                    height="100%" style_radius="4" style_bg_color="#card_bg" style_bg_opa="100"
                    clickable="false" event_bubble="true"/>
            <!-- SIZE_OK: chip width mirrors filament_swatch.xml's literal 40 -->
            <lv_obj width="40"
                    height="100%" style_radius="4" style_bg_color="#card_bg" style_bg_opa="100"
                    clickable="false" event_bubble="true"/>
          </lv_obj>
        </ui_card>
```

`filament_mapping_rows` gains a fixed `height="32"` (was `content`) because `filament_swatch` is `height="100%"` and needs a definite parent height, exactly as `color_swatches_row` provided. Keep whatever `filament_mapping_warning` child the old card had, unchanged.

- [ ] **Step 4: Point the C++ at the one card and delete the dead renderer**

In `src/ui/ui_print_select_detail_view.cpp`:

1. Delete the `color_swatches_row_` lookup (`:296`) and its null-out (`:943`).
2. Move the tap handler from `color_requirements_card_` to the surviving card. Replace `:306-320` with a lookup of `filament_mapping_card` into a single member (reuse `color_requirements_card_`'s slot but rename it `filament_card_`), keeping the existing `lv_obj_add_event_cb(..., LV_EVENT_CLICKED, this)` and its comment about the clickable flag coming from `color_card_remappable`. The card is looked up twice today (`:306` and `:322`); collapse to one lookup and pass it to both the event_cb and `filament_mapping_card_.create()`.
3. Delete `update_color_swatches()` entirely (`:1026-1156`) and its declaration (`include/ui_print_select_detail_view.h:1012`).
4. Delete `swatches_card_visible_for()` (`:2064-2071`) and its declaration (`:1024`) — the rule moved into the card in Task 3.
5. Delete the `color_swatches_visible_` subject: its `UI_MANAGED_SUBJECT_INT` (`:186`), its reset (`:447`), and the member (`include/ui_print_select_detail_view.h:682`).
6. In `refresh_preview_colors_and_mismatch()` (`:1358-1370`), delete the `if (lv_subject_get_int(&color_swatches_visible_) == 1) { update_color_swatches(...); }` block. The card repaints itself from `set_mappings()` now, so this function is purely downstream work.
7. In `render_authoritative_chips()` (`:1460-1476`), replace the two-surface branch with:

```cpp
    // One chip surface for every backend: the card renders from its own store.
    const bool card_visible = filament_mapping_card_.should_show();
    lv_subject_set_int(&filament_mapping_visible_, card_visible ? 1 : 0);
    // Published from the same place, against the same backend snapshot: a card
    // shown without this would advertise nothing, and a chevron shown without
    // the card would point at a control that is not there.
    lv_subject_set_int(&color_card_remappable_,
                       card_visible && color_card_opens_remap() ? 1 : 0);
```

Verified: there is NO `publish_mapping_visibility()` helper. `filament_mapping_visible_` is registered at `:180` and set inline at three sites — `:433` (the reset path), `:1402`, and `:1996`. `render_authoritative_chips()` does not touch it today. Setting it inline as shown above therefore makes this a FOURTH site. Fold all four onto one private `publish_card_visibility()` as part of this step (the copy-paste-twin rule), and call it from each. Check whether `:1402`'s call still has a distinct meaning once the card is the only surface — if it is now redundant with `render_authoritative_chips()`, delete it rather than route it through the helper.

8. Delete `ui_xml/components/filament_mapping_pill.xml` and `ui_xml/components/filament_mapping_more_pill.xml` with plain `rm` (never `git rm` — it stages, and a concurrent session has swept staged deletions before). Grep for any remaining `lv_xml_create(..., "filament_mapping_pill"` / `"filament_mapping_more_pill"` and for registrations of those component names before deleting.

- [ ] **Step 5: Build and run the full suite**

```bash
pgrep -x -d' ' 'make|cc1plus'; free -h
make -j6 test && make test-run
```
Expected: green. Compare shard pass count and CASE count against a pre-change run — the assertion total is unstable and proves nothing.

- [ ] **Step 6: Run the gates**

```bash
scripts/check_imperative_ui.py --list | wc -l
make regen-doc-links
scripts/quality-checks.sh
```
Expected: the imperative-UI count must not RISE. It should FALL — `update_color_swatches()` deleted more imperative sites than the card's render gained.

- [ ] **Step 7: Commit**

```bash
rm ui_xml/components/filament_mapping_pill.xml ui_xml/components/filament_mapping_more_pill.xml
git add -u ui_xml/ src/ include/ tests/
git commit -F - <<'EOF'
refactor(print-detail): one FILAMENTS card, one chip renderer

The detail view carried two mutually exclusive chip surfaces - a MAPPING card of
dot-arrow-dot pills for editable backends and a FILAMENTS card of stacked
two-tone chips for the rest - each with its own renderer, visibility rule and
copy of the lane-number lookup. The card now renders the stacked chip for every
backend, so the second card, update_color_swatches(), the color_swatches_visible
subject and both pill components are gone. Header keeps the chevron and both
warning icons; the sliced-colours toggle moved out in an earlier commit.
EOF
```

---

### Task 5: Verify on the mock at 480x272 and refresh screenshots

**Files:**
- Modify: screenshot fixtures under whatever path `scripts/screenshot-recipes.sh` writes to, if the print-detail token is covered there.

- [ ] **Step 1: Drive the CFS/editable case**

```bash
export HELIX_SOCK=/tmp/helix-unify-filament-chips.sock
export HELIX_CONFIG_DIR=/tmp/helix-config-unify-filament-chips
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-chips.log 2>&1 &
CHIPS_PID=$!
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate print_select
# open a multi-colour file, then:
./build/bin/helix-screen ctl -s "$HELIX_SOCK" ls filament_mapping_card
./build/bin/helix-screen ctl -s "$HELIX_SOCK" geom filament_mapping_rows
./build/bin/helix-screen ctl -s "$HELIX_SOCK" text filament_mapping_card
```
Expected: chips present, `filament_mapping_rows` 32px tall with 40px children, no horizontal overflow past the card. Prefer `geom`/`text` over reading a screenshot — a screenshot only proves what a scroll position exposed.

- [ ] **Step 2: Drive the remap and confirm the number moves**

Open the remap modal from the card tap, change a tool to a different lane, accept, then re-read:
```bash
./build/bin/helix-screen ctl -s "$HELIX_SOCK" text filament_mapping_card
```
Expected: the lane digit reflects the new lane. This is the reported bug, verified end to end rather than only at the unit seam.

- [ ] **Step 3: Confirm the non-editable case still renders**

Restart under a Snapmaker-style mock (see `HELIX_MOCK_*` in `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` for the U1 preset) and repeat Step 1. Expected: the same card, the same chips, and `color_card_remappable` still 1 (U1 supports native remap).

- [ ] **Step 4: Kill the instance**

```bash
kill "$CHIPS_PID"
```
Never `pkill -f` — it self-matches.

- [ ] **Step 5: Refresh screenshots if the print-detail token is covered**

```bash
grep -n "print_detail\|print_select" scripts/screenshot-recipes.sh
```
If a recipe exists, regenerate it and commit the new PNG. If not, skip — do not add a new recipe as part of this change.

- [ ] **Step 6: Ask for a visual check**

Drive to the print-detail screen on both backends and tell Preston exactly what to compare: chip width, band split, divider contrast, and whether the header reads cleanly with two warning icons plus the chevron at 480x272. That judgment is the one thing `ctl` cannot answer.

---

## Self-review notes

- **Spec coverage:** all six settled decisions map to a task — scope (2, 4), renderer location (2), title (4), chevron (4), toggle (1), surviving node name (4). The reported bug is closed by Task 2 alone.
- **Type consistency:** `resolve_mapped_slot` is introduced in Task 2 Step 3 and consumed in Task 2 Step 4 and the Task 2 tests; `mapped_lane_display_number` keeps its existing signature. `should_show()` keeps its signature; only its body changes (Task 3).
- **All three unknowns were resolved before hand-off, and two of the three assumptions were wrong.** `set_bypass_active` does not exist (use `enable_bypass()`); `publish_mapping_visibility()` does not exist (the publish is inline at three sites, and the plan's edit would have made a fourth, so Task 4 now folds them); no test builds the `print_file_detail` root, so Task 1 carries a spike step and an explicit fallback. `set_snapmaker_mode(bool)` does exist as assumed.
- **Deliberately out of scope:** `PrintStatusPanel` has no filament chips (grepped: zero hits in `ui_panel_print_status.cpp` and every `print_status_*.xml`), so nothing there changes.
