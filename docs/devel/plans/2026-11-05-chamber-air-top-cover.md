# Chamber Air Control (U1 Top Cover) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. The project's Verification Cadence (CLAUDE.md) overrides every skill: each task runs only its own tags; review, screenshots and one `make land-gate` happen once at the end.

**Goal:** A generic chamber air capability (modes with live readback and targets, provider-owned fans, a firmware filter counter, presence-gated) with the Snapmaker U1 Top Cover as its second provider, at parity with the stock screen.

**Architecture:** `helix::chamber_air` stays the one provider-table module and grows pure capability questions over status JSON. `PrinterState` feeds each status frame through it into a small live state that publishes capability-named subjects. UI surfaces (fan overlay, settings row, Maintenance Filter meter, first-fit/removal modals, resume check) bind those subjects and call the module for gcode; none names a firmware.

**Tech Stack:** C++17 (firmware builds gnu++17, `-fno-exceptions -fno-rtti`), LVGL 9.5 + helix-xml, Catch2, the mock Moonraker client.

**Spec:** `docs/devel/plans/2026-11-05-chamber-air-top-cover-design.md`

## Global Constraints

- Branch `feature/u1-top-cover` off `release/1.1`; one landing on `release/1.1`, then port to main (the tree layout differs: main moved UI files into `src/ui/<group>/`).
- Firmware-shared code: no exceptions (`json_util::safe_*`, `json::parse(s, nullptr, false)`), no RTTI, no C++20, no `= default` comparisons.
- Vendor names (`purifier`, `Snapmaker`, `U1`) only in `src/printer/chamber_air_modes.cpp` and `src/printer/enclosure_top.cpp`; `scripts/check_vendor_branches.py` must not rise.
- Declarative UI: visibility and checked state by XML binds on subjects; no `lv_obj_add_event_cb`, no `lv_label_set_text` in new code.
- All user strings via `lv_tr`/`TR_NOOP`, US English, added through the translation pipeline (`docs/devel/TRANSLATION_SYSTEM.md`).
- Constants `kCamelCase`. Every new log line through spdlog.
- No control may be reachable while presence is false; presence is followed live.
- U1 values: modes 0 Idle, 1 Cool, 2 Preheat, 3 Hot; Cool defaults 42 °C / 0.6 fan; `ALARM_TEMP` = target + 1.0 (firmware `DESIRED_CRITICAL_MIN_TEMP_DIFF`); guard floor 10 %; fan speeds 0..1 in status, `SPEED=` takes 0..1; filter `inner_fan_work_time` in seconds; reset `SET_PURIFIER FAN=inner WORK=0`.

## Review Focus

1. **Cover pulled mid-session with the fan overlay open** - the row, owned dials and settings row disappear in place; nothing sends afterwards. Test in Task 4.
2. **A delta frame without `purifier`** - every reading keeps its last value (the enclosure_top nullopt rule); nothing reads absence as "removed" or "Off". Test in Task 1.
3. **Extended Firmware: real `fan_generic exhaust_fan` present** - no synthetic dial appears, and the real dial hides without the cover. Test in Task 4.
4. **Firmware without `mode` / `inner_fan_work_time` (1.3 layout)** - plain action buttons, no highlight; Filter meter keeps print hours. Test in Task 1 and Task 6.
5. **Boot with cover already fitted** - first-fit notice once if never shown, no removal warning at boot. Test in Task 7.

---

### Task 1: Extend the `chamber_air` provider contract (pure)

**Files:**
- Modify: `include/chamber_air_modes.h`, `src/printer/chamber_air_modes.cpp`
- Test: `tests/unit/test_chamber_air_modes.cpp` (tag `[chamber_air]`)

**Interfaces (Produces):**
```cpp
namespace helix::chamber_air {
struct ModeTargets {            // what a mode sends; providers ignore fields they don't take
    double target_c = 42.0;     ///< Exhaust (cool) target
    int exhaust_pct = 60;       ///< Exhaust fan speed while cooling
    int recirculate_pct = 60;   ///< Inner fan speed while recirculating
};
enum class FanRole { Exhaust, Recirculate };
struct OwnedFan {
    FanRole role;
    const char* id;               ///< stable id for UI user_data: "chamber_air_exhaust" / "chamber_air_recirculate"
    const char* equivalent_object;///< a real fan object that IS this fan when present ("fan_generic exhaust_fan"), or nullptr
};
struct Reading {                  ///< one status frame's news; every field nullopt = frame said nothing
    std::optional<bool> present;
    std::optional<AirMode> mode;          ///< 2 and 3 both map to Recirculate
    std::optional<double> exhaust_speed;  ///< 0..1
    std::optional<double> recirculate_speed;
    std::optional<double> exhaust_threshold, recirculate_threshold; ///< 0..1
    std::optional<double> critical_temp_c;
    std::optional<double> filter_seconds;
};
bool presence_is_live(const PrinterDiscovery& hw);       ///< false: always present (Z-Mod)
bool reports_mode(const PrinterDiscovery& hw);           ///< provider has a readback field
bool takes_targets(const PrinterDiscovery& hw);
std::string mode_gcode(const PrinterDiscovery& hw, AirMode mode, const ModeTargets& t); // replaces the 2-arg form; Z-Mod ignores t
std::vector<OwnedFan> owned_fans(const PrinterDiscovery& hw);
std::string fan_speed_gcode(const PrinterDiscovery& hw, FanRole role, int pct);   ///< "" if unsupported
std::string filter_reset_gcode(const PrinterDiscovery& hw);                        ///< "" if no counter
int guard_floor_pct(const PrinterDiscovery& hw);                                    ///< 0 when none; U1 10
nlohmann::json required_status_objects(const PrinterDiscovery& hw);                 ///< {object: [fields]}
Reading read_status(const PrinterDiscovery& hw, const nlohmann::json& status);
/// Which owned fan the mode drives (gets the floor), and which it disables. nullopt: none.
std::optional<FanRole> driven_fan(AirMode mode);
std::optional<FanRole> disabled_fan(AirMode mode);
}
```
U1 row: detect = `enclosure_top::firmware_reports_enclosure_top(hw)` (no second marker list); presence via `enclosure_top::fitted_from_status`. Gcode: Recirculate `SET_PURIFIER_MODE MODE=3 FAN_SPEED=0.60`; Exhaust `SET_PURIFIER_MODE MODE=1 FAN_SPEED=0.60 DESIRE_TEMP=42 ALARM_TEMP=43`; Off `SET_PURIFIER_MODE MODE=0`; fan `SET_PURIFIER FAN=exhaust SPEED=0.50`; equivalents `fan_generic exhaust_fan`, `fan_generic circulation_fan`. `driven_fan(Exhaust)=Exhaust`, `disabled_fan(Exhaust)=Recirculate`, `driven_fan(Recirculate)=Recirculate`, `disabled_fan(Recirculate)=Exhaust`, Off: none.

- [ ] **Step 1: Write failing tests** (`[chamber_air]`):
  - `"chamber air: the U1 top cover offers all three modes and reads them back"` - U1 discovery (`purifier`, `fm175xx_reader`); `modes()` = {Recirculate, Exhaust, Off}; `reports_mode` true; `presence_is_live` true.
  - `"chamber air: U1 mode gcode carries the targets"` - `mode_gcode(hw, Exhaust, {38.0, 70, 50})` == `"SET_PURIFIER_MODE MODE=1 FAN_SPEED=0.70 DESIRE_TEMP=38 ALARM_TEMP=39"`; Recirculate == `"SET_PURIFIER_MODE MODE=3 FAN_SPEED=0.50"`; Off == `"SET_PURIFIER_MODE MODE=0"`.
  - `"chamber air: U1 status maps modes 2 and 3 to recirculate"` - `read_status` of `{"purifier":{"mode":2}}` and `{"mode":3}` give Recirculate, `1` Exhaust, `0` Off, `7` nullopt.
  - `"chamber air: a frame without the object says nothing"` - `read_status(hw, {"toolhead":{}})` has every field nullopt.
  - `"chamber air: the 1.3 layout has no mode or filter counter"` - `{"purifier":{"power_detected":true,"exhaust_fan":{"speed":0.5},"inner_fan":{"speed":0,"work":12}}}`: present true, mode nullopt, filter_seconds nullopt, exhaust_speed 0.5.
  - `"chamber air: owned fans name their Extended Firmware equivalents"`, `"chamber air: fan and filter gcode"` (`fan_speed_gcode(hw, Recirculate, 50)` == `"SET_PURIFIER FAN=inner SPEED=0.50"`, `filter_reset_gcode` == `"SET_PURIFIER FAN=inner WORK=0"`), `"chamber air: Z-Mod keeps action-only modes"` (existing tests updated to the 3-arg `mode_gcode`; `reports_mode`/`presence_is_live`/`takes_targets` false, `owned_fans` empty, `guard_floor_pct` 0).
- [ ] **Step 2:** `make t F='[chamber_air]'` - FAIL (undeclared).
- [ ] **Step 3:** Implement; widen the `Provider` struct with optional fields, Z-Mod row untouched in behaviour. `scripts/syntax_check.py include/chamber_air_modes.h` after editing the header.
- [ ] **Step 4:** `make t F='[chamber_air]'` PASS; break one mapping (mode 2) by hand, see red, restore.
- [ ] **Step 5:** Commit `feat(chamber-air): providers report presence, mode, targets, owned fans and a filter counter (prestonbrown/helixscreen#2015)`.

### Task 2: Live state and subjects in PrinterState; one merged subscription

**Files:**
- Create: `include/chamber_air_state.h`, `src/printer/chamber_air_state.cpp`
- Modify: `src/api/moonraker_discovery_sequence.cpp#...subscription` (merge field lists), `src/printer/printer_state.cpp#update_from_status`, `include/printer_state.h`, `src/printer/printer_capabilities_state.cpp` (row flags unchanged)
- Test: `tests/unit/test_chamber_air_state.cpp` (`[chamber_air_state]`)

**Interfaces:**
- Consumes: Task 1 `read_status`, `Reading`, `presence_is_live`.
- Produces: class `helix::ChamberAirState` owned by `PrinterState` (`get_printer_state().chamber_air()`), main-thread only:
  - `void set_hardware(const PrinterDiscovery& hw);` resets everything for a new printer.
  - `void apply(const Reading& r);` merges non-nullopt fields.
  - Subjects (ints, registered as XML globals): `chamber_air_available` (provider matched && present), `chamber_air_mode` (-1 unknown, else `int(AirMode)`), `chamber_air_exhaust_pct`, `chamber_air_recirculate_pct` (0-100), `chamber_air_has_targets`.
  - Accessors: `bool available() const; std::optional<AirMode> mode() const; std::optional<double> filter_seconds() const; std::optional<double> critical_temp_c() const; std::optional<int> threshold_pct(FanRole) const;`
  - Edge hook for Task 7: `using PresenceEdge = std::function<void(bool present, bool at_startup)>; void set_presence_edge_handler(PresenceEdge);` fired on every change of `available`; the first reading after `set_hardware` fires with `at_startup = true`.
- A shared helper `merge_status_objects(json& into, const json& add)` unions field lists per object (both enclosure_top and chamber_air subscribe `purifier`); put it beside the subscription builder and use it for both modules.

- [ ] **Step 1: Failing tests:** apply `{"purifier":{"power_detected":true,"mode":3}}` -> `chamber_air_available` 1, `chamber_air_mode` == int(AirMode::Recirculate); a frame without the object leaves all subjects; a present=false frame drops `available` to 0 and fires the edge handler with (false,false); first frame after `set_hardware` fires (true,true); `set_hardware` on a non-provider printer gives available 0 and no handler calls; `merge_status_objects` of `{"purifier":["power_detected"]}` + `{"purifier":["mode","power_detected"]}` == `{"purifier":["power_detected","mode"]}`, and `nullptr` (whole object) wins over a list.
- [ ] **Step 2:** `make t F='[chamber_air_state]'` FAIL.
- [ ] **Step 3:** Implement; subscription builder asks `chamber_air::required_status_objects(hw)` and merges; `PrinterState::update_from_status` calls `chamber_air_.apply(chamber_air::read_status(discovery_, state))` next to the enclosure_top block; `set_hardware` wiring next to `set_enclosure_top_fitted`'s reset. Subjects deinit via `StaticSubjectRegistry`.
- [ ] **Step 4:** `make t F='[chamber_air_state],[enclosure_top],[chamber_air]'` PASS; break the merge (overwrite instead of union), see red, restore.
- [ ] **Step 5:** Commit.

### Task 3: Mock U1 purifier that behaves like firmware 2.x

**Files:** Modify `src/api/moonraker_client_mock.cpp#purifier_status`, its gcode dispatch, `include/moonraker_client_mock.h`, `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`. Test: `tests/unit/test_enclosure_top.cpp` mock case and a new `[chamber_air][mock]` case.

- State held in the mock (mutex-guarded like `zmod_mutex_`): present (from `HELIX_MOCK_ENCLOSURE_TOP`, default fitted), mode, exhaust/inner speed 0..1, thresholds (0.6), desired/critical temp, `inner_fan_work_time` seconds.
- `SET_PURIFIER_MODE MODE=n [FAN_SPEED=] [DESIRE_TEMP=] [ALARM_TEMP=]`: refused (respond_info "purifier not exist", no state change) while absent; mode 1 sets exhaust speed to FAN_SPEED, inner 0; modes 2/3 the reverse; 0 both off.
- `SET_PURIFIER FAN=exhaust|inner [SPEED=0..1] [WORK=n]`: speed refused while absent and >0; WORK sets the counter (allowed absent).
- Work time accrues with sim time while mode is 2 or 3 and inner speed > 0.
- Mock-only `HELIX_MOCK_TOP_COVER PRESENT=0|1` gcode flips presence; on 0 force mode 0 and both speeds 0 (firmware behaviour). Document it.
- [ ] Test: send `SET_PURIFIER_MODE MODE=3 FAN_SPEED=0.5` -> status mode 3, inner speed 0.5; `HELIX_MOCK_TOP_COVER PRESENT=0` -> power_detected false, mode 0, speeds 0; then `SET_PURIFIER FAN=exhaust SPEED=0.5` leaves exhaust at 0. Red-then-green as above. Commit.

### Task 4: Fan overlay - live mode row and owned fan dials with guards

**Files:** Modify `ui_xml/fan_control_overlay.xml`, `src/ui/ui_fan_control_overlay.cpp`, `include/ui_fan_control_overlay.h`. Test: `tests/unit/test_fan_control_overlay*.cpp` or a new `tests/unit/test_fan_control_overlay_chamber_air.cpp` (`[fan_overlay][chamber_air]`), XML fixture with the U1 mock.

- Row: hidden unless `chamber_air_available eq 1 and (printer_has_chamber_air_* ...)`; each button gets `bind_state_if_eq subject="chamber_air_mode" state="checked" ref_value="<int AirMode>"`. Click sends `mode_gcode(hw, mode, targets)` where targets come from Task 5's settings accessor (until Task 5 lands, `ModeTargets{}`).
- Owned fans: in `populate_fans()`, after real fans, for each `owned_fans(hw)` whose `equivalent_object` is not in `fan_state().get_fans()`, add a `FanDial` with id `OwnedFan::id`, name `lv_tr("Exhaust fan")` / `lv_tr("Recirculation fan")`, speed from the Task 2 subject, and send through `fan_speed_gcode`. Real dials whose object name is an `equivalent_object` are tagged as that role. Repopulate on `chamber_air_available` change (it gates all of them, real equivalents included: hidden while 0).
- Guards, one pure function so it is testable without LVGL: `helix::chamber_air::FanGuard guard_fan_change(std::optional<AirMode> mode, FanRole role, int requested_pct, int current_pct, std::optional<int> threshold_pct, int floor_pct);` returning `{Apply(pct) | Confirm(pct) | Blocked}` (`enum class FanGuardAction { Apply, Confirm, Blocked }; struct FanGuard { FanGuardAction action; int pct; };`). Rules: mode nullopt or Off -> Apply requested; role == disabled_fan(mode) and requested > 0 -> Blocked; role == driven_fan(mode) -> pct = max(requested, floor); then if pct < threshold and pct < current -> Confirm. Put it in `chamber_air_modes.h` (Task 1 file, tests in `[chamber_air]`).
- UI on Confirm: `modal_confirm` with "Reducing the fan speed may affect print quality. Do you want to continue?" (exhaust) or "Reducing the fan speed may affect filtration performance. Continue?" (recirculation); cancel restores the dial. Blocked: toast "With internal circulation on, the exhaust fan cannot be enabled" / "With exhaust cooling on, the recirculation fan cannot be enabled", dial restored.
- [ ] Tests: guard table cases (each rule, Off/unknown passthrough, floor clamps 0->10 for the driven fan); overlay with U1 mock: row visible, 2 synthetic dials named as above; with `fan_generic exhaust_fan` in the object list only one synthetic dial; after the mock's `PRESENT=0` the row and owned dials are hidden. Red/green, commit.

### Task 5: Chamber air targets in settings

**Files:** Create `ui_xml/chamber_air_settings_row.xml` (or rows inside the existing fan settings overlay - builder's call after `ctl geom` at 800x480 and 480x320); Modify the fan settings overlay source, `include/chamber_air_settings.h` + `src/system/chamber_air_settings.cpp`.
- Produces: `helix::chamber_air::ModeTargets load_targets(); void save_targets(const ModeTargets&);` per printer at `Config::df() + "chamber_air/targets"` with keys `target_c`, `exhaust_pct`, `recirculate_pct`; defaults from `ModeTargets{}`; clamp target 20..80 °C, pct 10..100.
- Row shown only while `chamber_air_available eq 1 and chamber_air_has_targets eq 1`.
- [ ] Tests (`[chamber_air][settings]`): round-trip, defaults when absent, clamping of out-of-range stored values, per-printer isolation (two printer ids). Task 4's click handler switches to `load_targets()`. Commit.

### Task 6: Filter meter reads the firmware counter

**Files:** Modify `include/maintenance_meters.h`, `src/system/maintenance_meters.cpp`, `src/ui/panel_widgets/maintenance_model.cpp`. Tests `[maintenance]`.
- Produces (pure): `enum class FilterSource { PrintHours, Firmware }; FilterSource filter_source(bool provider_has_counter); std::vector<int> apply_firmware_filter_hours(State& s, double firmware_hours);` - sets Filter `used_h` to `firmware_hours` and returns `{Filter}` once when it first reaches the interval (reuse the `notified` latch); `accrue()` gains `bool skip_filter` so print hours skip Filter while Firmware is the source.
- Model: source = Firmware when `chamber_air::filter_reset_gcode(hw)` is non-empty and a `filter_seconds` reading has arrived; on each new reading call `apply_firmware_filter_hours(s, secs / 3600)`. Reset for Filter under Firmware: send `filter_reset_gcode`, on success `maintenance::reset_meter`, on error leave the meter (normal gcode error path).
- [ ] Tests: firmware hours replace used hours; due notice fires once at the interval and not again; `accrue(..., skip_filter=true)` leaves Filter and moves Nozzle/Rails; source PrintHours when no counter. Red/green, commit.

### Task 7: First-fit notice and removal warning

**Files:** Create `include/ui_chamber_air_notices.h`, `src/ui/ui_chamber_air_notices.cpp`; wire in `Application` startup beside other notifiers via `ChamberAirState::set_presence_edge_handler`. Provider copy lives in `chamber_air_modes.cpp`: `struct AccessoryCopy { const char* name; const char* detected; const char* removed; bool suggests_shaper_recalibration; }; std::optional<AccessoryCopy> accessory_copy(const PrinterDiscovery& hw);` U1: name "Top Cover", detected "New top cover detected. Its fans and air modes are on the Fans screen.", removed "The top cover has been disconnected. To ensure print quality, reconnect the top cover before continuing.", shaper true.
- Pure decision: `enum class NoticeAction { None, ShowFirstFit, ShowRemoved }; NoticeAction decide_notice(bool present, bool at_startup, bool first_fit_shown, bool print_active);` - present && !first_fit_shown && !print_active -> ShowFirstFit (startup included); present && print_active && !shown -> None but pending (re-decide when the print ends: caller re-calls with print_active false); !present && !at_startup -> ShowRemoved (also mid-print); otherwise None.
- First-fit modal: "Open Fans" (opens the fan overlay) and "OK"; on close persist `Config::df() + "chamber_air/first_fit_shown" = true`, then if `suggests_shaper_recalibration`: `modal_confirm` "After connecting the top cover, we recommend recalibrating vibration compensation to ensure print quality." with "Calibrate now" (opens `get_global_input_shaper_panel()` the way the Calibration menu does) and "Later". Pass `on_dismiss` so the persisted flag and pending state never leak.
- [ ] Tests (`[chamber_air][notices]`): `decide_notice` table (all five Review Focus #5 cases); flag persists per printer. Commit.

### Task 8: Resume check

**Files:** Modify `src/ui/print_control_buttons.cpp#request_resume`. Test `[print_control]` or `[chamber_air]`.
- Pure: `bool chamber_too_hot_to_resume(std::optional<AirMode> mode, std::optional<double> chamber_c, std::optional<double> critical_c);` true only for Exhaust with both temps known and chamber > critical.
- `request_resume()`: if true, `modal_confirm` "The chamber temperature is too high. This may cause extruder clogging. Continue printing?" -> confirm runs the existing resume body; cancel does nothing. Chamber temperature = the value HelixScreen already resolves for the chamber sensor (PrinterState chamber temperature subject).
- [ ] Tests: the pure function's table (Off/Recirculate false; unknown temps false; equal false; above true). Commit.

### Task 9: Docs and translations

- `docs/devel/CHAMBER_HEATER.md` § chamber air modes: the provider contract, the U1 row, how to add a printer.
- `docs/devel/printers/SNAPMAKER_U1_SUPPORT.md`: replace the `purifier` NOT SUBSCRIBED section with the 2.x field table, the three layouts, and the stock-screen behaviour (from the design).
- New strings through the translation pipeline; `make t F='[translation]'` if a translation test tag exists.
- Delete both plan files in the final commit of the branch.
- Commit.

## Finish (once, orchestrator)

1. One review of the branch (fresh reviewer, `docs/devel/REVIEW_RUBRIC.md`).
2. Screenshots, U1 mock, 800x480 and 480x320: fan overlay with cover (Recirculate checked), fan overlay after unplug, settings row, Maintenance Filter meter, first-fit modal, removal modal.
3. `make land-gate` on thelio; land on `release/1.1`; port to main; `make mutate-diff` after landing.
4. Comment on #2015 and ask the Discord reporter to verify on the next 1.1.x beta.
