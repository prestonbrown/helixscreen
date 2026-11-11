# Toolchanger usability fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a toolchanger owner aim the Filament panel, Controls and the temperature overlay at any tool without mounting it, and clean up the six smaller rough edges reported for Medusa HC and U1.

**Architecture:** One pure `TargetToolSelection` rule (follows the mounted tool on a tool change, otherwise keeps the user's pick) owned once per surface; temperature cards repoint to that tool's per-extruder subjects the way `TempGraphOverlay::repoint_nozzle_card()` already does. Moves that need the tool on the carriage go through the existing `offer_tool_change()` and wait for ToolState to report the tool before sending anything. The small fixes are XML plus pure decision functions.

**Tech Stack:** C++17 (firmware builds share `src/`: no C++20), LVGL 9.5 + helix-xml, Catch2, `make t F='[tag]'`.

**Spec:** `docs/devel/plans/2026-11-10-toolchanger-feedback-design.md` (read it first).

**Branch:** `feature/toolchanger-feedback` off `origin/release/1.1`, worktree `.worktrees/toolchanger-feedback`. On release/1.1 the UI sources are flat (`src/ui/ui_panel_filament.cpp`, not `src/ui/filament/...`); the port to `main` translates paths.

## Global Constraints

- Temperature sends only through `TemperatureController::set_target(extruder_name, deg, opts)` / `set_target(HeaterType, ...)` (lint-enforced).
- Declarative UI (`.claude/rules/declarative-ui.md`): new visibility, labels and enabled state bind to subjects; no new `lv_obj_add_event_cb`, imperative `HIDDEN` flags or `lv_label_set_text`. Imperative-UI ratchet may fall, never rise.
- No vendor names in panels; capability questions only (`has_multiple_toolheads()`, `supports_tool_park()`, `get_topology()`).
- Every user string through `lv_tr()` plus `ui_xml/translations/*.xml` entries (`make` regenerates; the translation coverage gate runs at push).
- Threading: LVGL only on the main thread; `on_failed` hooks may run off it and must marshal (`helix::ui::run_on_main`). A raw `lv_timer_t*` is cancelled in the destructor too (`lv_timer_cancel_safe`).
- Constants `kCamelCase`; no exceptions, no RTTI, no `std::regex`.
- Fits 800x480 and the smallest breakpoint: `ctl geom` the Multi-Filament card, Motion's bottom row, the AMS header and the Controls nozzle row before placing controls.
- Test cadence (root `CLAUDE.md` § Verification Cadence): each task runs only its own tags with `make t F='[tag]'`; no `unit-sweep`, `full-test-run`, `mutate-diff` or `land-gate` until Task 10. Each new test goes red once by a hand break of the line it pins, then restore.

## Review Focus

1. **A `G1 E` after a pickup must never reach the previous nozzle**, including when the swap fails, times out, or the user declines the confirm dialog. Pinned in Task 6 (`[filament][target_tool]` "extrude waits for the target tool").
2. **No-backend multi-extruder / IDEX:** Load and Unload run macros against the active extruder, so with a non-active target they would feed the wrong tool. My call: on printers with no AMS backend, Load and Unload pick up the target first, the same way Extrude does. Pinned in Task 6.
3. **A heating macro configured for material preheat** heats whatever it heats (usually the active extruder). My call: when the target tool is not the active one, presets skip the macro and send the target extruder and bed directly. Pinned in Task 5.
4. **Rediscovery drops the target tool** (tool count shrinks, extruder renamed): every surface falls back to `current(tool_count)` (T0) and never reads a destroyed subject. Pinned in Task 4 and Task 5.
5. **The panel opens with nothing mounted at boot**: dropdown shows T0 (not "None"), temp card shows T0, Pick up offered. Pinned in Task 5.

Cool Down keeps today's behavior (turns every heater off, which covers the target tool); the spec line about Cool Down is satisfied by that. (My call.)

---

### Task 1: Picker rows say only what is known

**Files:**
- Modify: `src/ui/ui_batch_filament_modal.cpp` (`contents_suffix`)
- Test: `tests/unit/test_batch_filament_modal.cpp`, `tests/unit/test_toolchanger_ui_gating.cpp:178`

**Interfaces:** Consumes and produces nothing new; `BatchFilamentModal::row_label(LaneNoun, int, const SlotInfo&, std::optional<bool> present, bool at_toolhead)` keeps its signature.

- [ ] **Step 1: Failing test** `"BatchFilamentModal row status claims only what the sensor knows"` `[ams][batch]`, one SECTION per row of the spec table, material "PLA", slot 0, `LaneNoun::Slot`:
  - `at_toolhead=true`, present any → label ends `"(PLA - loaded)"`
  - `present=true` → ends `"(PLA - ready to load)"`
  - `present=false` → ends `"(PLA - empty)"`
  - `present=nullopt` → ends `"(PLA)"`
  - material empty, `present=false` → ends `"(Empty)"`
  Update the existing "a full lane not yet fed says ready to load" SECTION to pass `present=true`, and `test_toolchanger_ui_gating.cpp:178` to the state its mock actually reports.
- [ ] **Step 2:** `make t F='[batch]'` → the `nullopt` and `false` sections FAIL.
- [ ] **Step 3:** Rewrite `contents_suffix(info, present, at_toolhead)` to the table. New literal `lv_tr("empty")` already exists in the hint list; reuse it.
- [ ] **Step 4:** `make t F='[batch]'` and `make t F='[toolchanger]'` PASS.
- [ ] **Step 5:** Commit `fix(ams): picker rows claim ready to load only when the sensor sees filament`.

### Task 2: "All" row in the Load/Unload picker

**Files:**
- Modify: `include/ui_batch_filament_modal.h`, `src/ui/ui_batch_filament_modal.cpp`, `ui_xml/batch_filament_modal.xml`
- Test: `tests/unit/test_batch_filament_modal.cpp`

**Interfaces:**
- Produces: `static bool BatchFilamentModal::show_select_all(size_t enabled_rows, size_t cap, bool single_select);` (true when `!single_select && enabled_rows >= 2 && (cap == 0 || cap >= enabled_rows)`).
- Subjects (process-wide, beside `batch_filament_title_text`): `batch_filament_all_visible` (int 0/1), `batch_filament_all_checked` (int 0/1). Callback `batch_filament_all_toggled`.

- [ ] **Step 1: Failing tests** `"BatchFilamentModal offers All only when every row fits the cap"` `[ams][batch]`: `(6,0,false)`→true, `(6,6,false)`→true, `(6,4,false)`→false, `(1,0,false)`→false, `(6,0,true)`→false. And a UI test (LVGLUITestFixture, mock toolchanger with 6 heads): toggling All selects every enabled row and disabled rows stay unselected; unchecking one row sets `batch_filament_all_checked` to 0; checking the last unchecked row sets it to 1.
- [ ] **Step 2:** `make t F='[batch]'` → FAIL.
- [ ] **Step 3:** Implement. XML: a checkbox row labelled `All` above `batch_multiselect`, hidden by `batch_filament_all_visible`, checked state bound to `batch_filament_all_checked`. C++: `on_show()` publishes visibility from `show_select_all(enabled count, view.max_heads_per_op, single_select(op_))`; the toggle calls `multiselect_.select_all()` / `deselect_all()` (select only enabled rows); `multiselect_.set_on_change` keeps `all_checked` true iff every enabled row is selected.
- [ ] **Step 4:** `make t F='[batch]'` PASS.
- [ ] **Step 5:** Commit `feat(ams): All row in the load and unload picker`.

### Task 3: Settings to a header gear; "Mounted" line on multi-head printers

**Files:**
- Modify: `ui_xml/components/ams_sidebar.xml` (remove `btn_settings` from the grid), `ui_xml/ams_panel.xml` (gear button at the right of `overlay_header`, hidden by `ams_has_device_ops`, same callback the old button used), `ui_xml/components/ams_loaded_card.xml` (one `text_body` line above the existing details), `src/ui/ui_ams_sidebar.cpp`, `include/ui_ams_sidebar.h`
- Create: none
- Test: `tests/unit/test_ams_sidebar_action_grid.cpp`, new cases in `tests/unit/test_ams_loaded_card_buffer.cpp` or a new `tests/unit/test_ams_mounted_line.cpp`

**Interfaces:**
- Produces: `std::string helix::ui::mounted_tool_text(LaneNoun tool_noun, int active_tool);` in `src/ui/ui_ams_sidebar.cpp` (declared in `include/ui_ams_sidebar.h`): `active_tool >= 0` → `"Mounted: " + active_tool_lane_label(tool_noun, active_tool)` (translated "Mounted: {}"), else `lv_tr("No tool mounted")`.
- Subjects: `ams_mounted_text` (string), `ams_show_mounted` (int, 1 iff `ToolState::has_multiple_toolheads()`), both updated from the active-tool and tool-list observers.

- [ ] **Step 1: Failing tests:** `mounted_tool_text(LaneNoun::Tool, 2)` contains `"2"` and the tool noun's label for index 2 (use `active_tool_lane_label` as the oracle); `-1` → `"No tool mounted"`. Grid test: no `btn_settings` under `action_buttons_container`; a gear named `btn_ams_settings` exists in the header and is hidden when `ams_has_device_ops` is 0. Loaded card: mounted line hidden on a single-head mock (AFC), shown with `"No tool mounted"` on the toolchanger mock with no tool, clog/buffer widgets still present.
- [ ] **Step 2:** `make t F='[ams]'` (the sidebar files' tags) → FAIL.
- [ ] **Step 3:** Implement; the existing "a row the actions do not fill is centred" case keeps passing for odd action counts.
- [ ] **Step 4:** `make t F='[ams]'` PASS; `ctl geom` the header at 800x480 and the smallest breakpoint, gear must not push the back button or name.
- [ ] **Step 5:** Commit `feat(ams): settings in the panel header and the mounted tool on multi-head printers`.

### Task 4: `TargetToolSelection` and `decide_target_tool_action`

**Files:**
- Create: `include/target_tool_selection.h` (as below, syntax-checked), `src/ui/target_tool_selection.cpp`
- Test: `tests/unit/test_target_tool_selection.cpp` `[target_tool]`

**Interfaces (produces, used by Tasks 5-8):**

```cpp
namespace helix::ui {
class TargetToolSelection {
  public:
    void on_active_tool_changed(int active_tool); // >= 0 moves the selection; -1 keeps the pick
    void pick(int tool);
    [[nodiscard]] int current(int tool_count) const; // clamped; 0 when unset or out of range
  private:
    int selected_ = -1;
};
enum class TargetToolAction : uint8_t { None, PickUp, Park };
[[nodiscard]] TargetToolAction decide_target_tool_action(int target_tool, int active_tool,
                                                         bool multiple_heads, bool supports_park);
}
```

Also in this task: `class ToolArrivalWait` in the same header, main-thread only:
`void start(int tool, uint32_t timeout_ms, std::function<void()> on_arrived, std::function<void()> on_timeout);` `void cancel();` `~ToolArrivalWait()` cancels. Fires `on_arrived` as soon as ToolState's active tool subject reads `tool` (immediately if it already does), `on_timeout` after `kToolArrivalTimeoutMs = 120000` otherwise; never both; `cancel()` fires neither. Observer via `observe<int>(ToolState::instance().get_active_tool_subject(), ..., ToolState::instance().get_subjects_lifetime())`; timer cancelled in `cancel()` and the destructor (`lv_timer_cancel_safe`).

- [ ] **Step 1: Failing tests:**
  - selection: fresh → `current(6)==0`; `pick(3)` → 3; `on_active_tool_changed(1)` → 1; `pick(4)` then `on_active_tool_changed(-1)` → 4; `pick(5)` then `current(3)` → 0.
  - action: `(3,1,true,true)`→PickUp; `(3,-1,true,true)`→PickUp; `(3,3,true,true)`→Park; `(3,3,true,false)`→None; `(3,1,true,false)`→PickUp; `(3,1,false,true)`→None.
  - wait (LVGLTestFixture, drive `ToolState` active index): arrives → only `on_arrived`; already active → `on_arrived` at once; time advanced past timeout → only `on_timeout`; `cancel()` then arrival → neither; destroyed while armed → no callback, no crash.
- [ ] **Step 2:** `make t F='[target_tool]'` → FAIL (link errors count).
- [ ] **Step 3:** Implement `src/ui/target_tool_selection.cpp`.
- [ ] **Step 4:** `make t F='[target_tool]'` PASS.
- [ ] **Step 5:** Commit `feat(ui): one rule for which tool a surface targets`.

### Task 5: Filament panel aims at the target tool (select-only dropdown, temp card, pencil, presets)

**Files:**
- Modify: `include/ui_panel_filament.h`, `src/ui/ui_panel_filament.cpp` (`handle_extruder_changed`, `populate_extruder_dropdown`, `sync_tool_dropdown_text`, the `active_tool_observer_` lambda, the nozzle half of `temp_observers_`, `send_nozzle_target`, `show_printer_nozzle_target`, `handle_preset_button`, `handle_nozzle_temp_tap`, `update_nozzle_label`)
- Test: new `tests/unit/test_filament_panel_target_tool.cpp` `[filament][target_tool]`; existing `test_filament_panel_op_slot.cpp`, `test_filament_manage_row_controls.cpp` must stay green.

**Interfaces:**
- Consumes: Task 4 `TargetToolSelection`.
- Produces: `FilamentPanel::target_tool_` (`TargetToolSelection`), `int FilamentPanel::target_tool() const;` `const std::string& FilamentPanel::target_extruder_name() const;` (that tool's `ToolInfo::extruder_name`, else `"extruder"`), `void FilamentPanel::repoint_nozzle_observers();`.

Behavior: the dropdown only `pick()`s, never issues a tool change on any topology (delete the PARALLEL / no-backend change branch). The dropdown's selected index and closed text follow `target_tool()` (never "None"). The nozzle current/target observers subscribe to `get_extruder_temp_subject(name, lifetime)` / `get_extruder_target_subject(name, lifetime)` for the target extruder and are re-made on every target change and extruder-version change (pattern: `TempGraphOverlay::repoint_nozzle_card`). The mini graph switches extruder with the target (`temp_control_panel_->switch_active_extruder`). `send_nozzle_target()` and the keypad confirm send `set_target(target_extruder_name(), ...)`. Presets: when the target extruder is the active one, today's path (macro or direct); otherwise skip the macro and send the target extruder and bed directly. `update_nozzle_label()` names the target tool.

- [ ] **Step 1: Failing tests** (mock toolchanger, 6 heads, ToolState active = 1):
  - choosing index 3 in the dropdown sends no gcode (mock API records nothing) and `target_tool()==3`
  - the temp card's target subject reflects extruder3's target after the mock sets it, not extruder1's
  - keypad confirm 215 sends `set_target("extruder3", 215)` (assert via the temperature controller's recorded calls / mock gcode `SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=215`)
  - PLA preset with target 3 heats extruder3 and bed, not extruder1
  - active becomes 4 → `target_tool()==4`; active becomes -1 → stays 4
  - boot with active -1 → dropdown text is the T0 label, not "None"
  - tool list shrinks to 3 while target is 5 → `target_tool()==0`, no crash
- [ ] **Step 2:** `make t F='[target_tool]'` → FAIL.
- [ ] **Step 3:** Implement as described.
- [ ] **Step 4:** `make t F='[target_tool]'`, `make t F='[filament]'` PASS.
- [ ] **Step 5:** Commit `feat(filament): the tool selector aims the panel without changing tools`.

### Task 6: Pick up / Park button, and pickup before moves that need the carriage

**Files:**
- Modify: `include/ui_panel_filament.h`, `src/ui/ui_panel_filament.cpp` (`handle_extrude_button`, `handle_retract_button`, `handle_purge_button`, `handle_load_button`, `handle_unload_button`), `ui_xml/filament_panel.xml` (button beside `extruder_dropdown`)
- Test: `tests/unit/test_filament_panel_target_tool.cpp`

**Interfaces:**
- Consumes: Task 4 `decide_target_tool_action`, `ToolArrivalWait`; Task 5 `target_tool()`; `offer_tool_change(printer_state, tool, ToolChangeHooks)`; `request_tool_park(printer_state)`.
- Produces: subject `filament_target_tool_action` (int = `TargetToolAction`), callback `on_filament_target_tool_action`; `void FilamentPanel::with_target_mounted(void (FilamentPanel::*then)());`

`with_target_mounted(then)`: target already active → `then` now. Otherwise `offer_tool_change(target)` with `owner_token` from the panel lifetime; `on_success` arms `ToolArrivalWait` (`on_arrived` → `then`; `on_timeout` → warning toast "{tool} did not arrive" and nothing sent); `on_failed` / `on_declined` → nothing sent. A second tap while waiting cancels the wait (reuse `operation_guard_` gating). Extrude, Retract and Purge always go through it before `run_after_preheat`. Load and Unload go through it only when `AmsState::get_backend()` is null (Review Focus 2).

Button: label "Pick up" / "Park" by subject, hidden for `None`. Pick up → `offer_tool_change(target)`; Park → `request_tool_park(printer_state_)`.

- [ ] **Step 1: Failing tests:**
  - target 3, active 1, Extrude: no `G1 E` until ToolState active reads 3; then exactly one extrude gcode
  - same, change fails (`on_failed`) → no `G1 E`; confirm dismissed (`on_declined`) → no `G1 E`; arrival times out → no `G1 E` and a warning toast
  - target == active → extrude sent at once, no tool change requested
  - no-backend 2-extruder mock, target 1, active 0, Load → tool change to 1 requested before the load macro
  - button subject: (target 3, active 1) PickUp; (3,3, park supported) Park; (3,3, no park) None; hub AFC mock → None
- [ ] **Step 2:** `make t F='[target_tool]'` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[target_tool]'`, `make t F='[filament]'` PASS; `ctl geom` the Multi-Filament card at 800x480 and smallest breakpoint, dropdown plus button fit.
- [ ] **Step 5:** Commit `feat(filament): pick up and park from the panel, and pick up before extruding`.

### Task 7: Controls nozzle row targets a chosen tool

**Files:**
- Modify: `include/ui_panel_controls.h`, `src/ui/ui_panel_controls.cpp` (`handle_nozzle_target_edit`, `handle_custom_nozzle_confirmed`, nozzle temp observers, `controls_nozzle_label`), `ui_xml/controls_panel.xml` (chooser in the nozzle row, hidden unless `has_multiple_toolheads`; look is the builder's call within the measured row), `ui_xml/micro/controls_panel.xml` only if the chooser fits there, else hidden on micro
- Test: new `tests/unit/test_controls_nozzle_target.cpp` `[controls][target_tool]`

**Interfaces:**
- Consumes: Task 4 `TargetToolSelection`; same repoint approach as Task 5 (factor `repoint_nozzle_observers` into a shared helper only if both bodies come out the same; otherwise keep them separate with a one-line reason).
- Produces: `void ControlsPanel::select_nozzle_tool(int tool);` (the chooser's callback calls it).

- [ ] **Step 1: Failing tests:** multi-head mock active 1: `select_nozzle_tool(3)` → row shows extruder3's temps; pencil keypad confirm 220 → `set_target("extruder3", 220)`; tool change to 5 → row follows to 5; single-head mock → chooser hidden and pencil edits the active extruder exactly as today.
- [ ] **Step 2:** `make t F='[controls]'` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[controls]'` PASS.
- [ ] **Step 5:** Commit `feat(controls): the nozzle row edits the tool you choose`.

### Task 8: Temperature overlay keeps its nozzle pick

**Files:**
- Modify: `include/ui_overlay_temp_graph.h`, `src/ui/ui_overlay_temp_graph.cpp` (`open()`, `select_extruder`, an active-tool observer)
- Test: `tests/unit/test_temp_graph_overlay.cpp` `[temp_graph]`

Behavior: `open()` no longer clears `picked_extruder_`. A tool change clears it (the card then shows the newly mounted tool), matching `TargetToolSelection`. The vanished-extruder fallback in `repoint_nozzle_card()` stays.

- [ ] **Step 1: Failing tests:** pick extruder3, close, `open(Mode::Nozzle)` → `displayed_extruder_name()=="extruder3"`; then active tool change → `"extruder"` of the new active tool; pick extruder5, rediscovery drops it → active extruder.
- [ ] **Step 2:** `make t F='[temp_graph]'` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[temp_graph]'` PASS.
- [ ] **Step 5:** Commit `feat(temp): the temperature overlay remembers the nozzle you picked`.

### Task 9: Per-axis home bar on Motion, shared with Controls

**Files:**
- Create: `ui_xml/components/home_axis_bar.xml` (the `button_group` from `controls_panel.xml` § "Home row", parameterized by props: callback names for all/x/y/xy/z, the busy subject, and whether XY shows)
- Modify: `ui_xml/controls_panel.xml` (use the component, XY on), `ui_xml/motion_panel.xml` (place it in the bottom row area, XY off, All/X/Y/Z), `src/ui/ui_panel_motion.cpp` (register `on_motion_home_x/y/z` calling the existing `home('X')` etc.; reuse `on_motion_bed_home` for All). Homing feedback stays per panel (Controls' quick-action spinners vs Motion's toasts are deliberately different).
- Test: `tests/unit/test_motion_panel_homing_indicators.cpp` `[motion]`, Controls home-bar test if one exists (else add one case)

- [ ] **Step 1: Failing tests:** Motion panel built from XML has `btn_home_x`, `btn_home_y`, `btn_home_z`, no `btn_home_xy`; clicking `btn_home_x` sends `G28 X` on the mock; X hidden when `printer_has_individual_xyz_homing` is 0; Controls still has all five buttons wired to its own callbacks.
- [ ] **Step 2:** `make t F='[motion]'` → FAIL.
- [ ] **Step 3:** Implement; `ctl geom` Motion's bottom row at 800x480 and the smallest breakpoint first and place the bar where it fits without shrinking the jog pad or the speed buttons below their current touch size.
- [ ] **Step 4:** `make t F='[motion]'`, `make t F='[controls]'` PASS.
- [ ] **Step 5:** Commit `feat(motion): home single axes from the motion panel`.

### Task 10: Finish (orchestrator, once)

- [ ] Translations: every new key present in `ui_xml/translations/*.xml` (`make` regenerates; check the coverage gate output).
- [ ] One review of the whole branch (`docs/devel/REVIEW_RUBRIC.md`); a second round only for crash, lifetime or wrong-tool-command findings.
- [ ] One headless screenshot per changed screen on the mock toolchanger (Filament panel with T3 targeted, Pick up shown; Controls nozzle row; Motion with the home bar; AMS panel header gear and Mounted line; Load picker with All): show them to Preston.
- [ ] One `make land-gate` on the fastest free machine (thelio, then zeus), watched with `watch-gate.sh`.
- [ ] Delete both plan files in the landing change; durable notes go to `docs/devel/TOOL_ABSTRACTION.md` (target tool rule) and `docs/devel/FILAMENT_MANAGEMENT.md` (picker rows, All row).
- [ ] Land on `release/1.1`, port to `main` (path translation to `src/ui/<group>/`), post "please test" to topi and wildtang3nt at the next beta tag.
