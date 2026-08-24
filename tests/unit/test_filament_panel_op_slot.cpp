// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_panel_op_slot.cpp
 * @brief Integration guard: FilamentPanel::execute_load / execute_unload act on
 *        the DROPDOWN-SELECTED slot, resolved once through selected_op_slot().
 *
 * Run with: ./build/bin/helix-tests "[filament][op_slot][panel]"
 *
 * Unlike test_filament_op_slot_resolver.cpp (which pins the pure resolver and a
 * hand-written mirror of the panel's branching), this test drives the ACTUAL
 * production methods on a real FilamentPanel view built from filament_panel.xml,
 * with a real ToolState topology + a recording AMS backend injected into the real
 * AmsState singleton. It asserts WHICH slot argument reaches the backend.
 *
 * The bug this guards (single-source-of-truth fix): execute_load/execute_unload
 * used to read backend->get_system_info().current_slot and act on THAT, so on a
 * BoxTurtle where lane 4 (slot 3) was loaded to the toolhead but the dropdown
 * defaulted to T0, Load acted on the already-loaded lane 3 instead of the
 * selected lane 0. Both executors now call the private selected_op_slot() — the
 * same resolution the button gating uses — so the op can never diverge.
 *
 * Mutation check: reverting execute_load() to dispatching on sys.current_slot
 * makes "BoxTurtle: Load follows the selected tool" FAIL (the backend is asked
 * for 3, not 0). If it still passes, the test does not reach the callsite.
 */

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "config.h"
#include "post_op_cooldown_manager.h"
#include "printer_state.h"
#include "tool_state.h"

#include <lvgl.h>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ToolState;
using helix::ToolTopology;
using TA = helix::ui::FilamentPanelTestAccess;
// AmsState / AmsBackendMock / AmsSystemInfo / AmsType live in the global namespace.

namespace {

// Recording backend: overrides only what the panel's op path reads, so the test
// controls the system snapshot and observes the slot the executors dispatch.
// Subclasses the production mock (test infrastructure per CLAUDE L065) so all the
// unrelated pure-virtuals are already satisfied.
class RecordingBackend : public AmsBackendMock {
  public:
    RecordingBackend() : AmsBackendMock(4) {}

    AmsSystemInfo sys_{};  ///< Snapshot returned to the panel (test sets fields)
    int loaded_slot_ = -1; ///< Which slot reports "loaded at toolhead"
    PathTopology topology_ = PathTopology::HUB; ///< Simulated path topology (test sets)
    /// Presence the gating reads for every non-loaded lane. AVAILABLE by default
    /// so the button gating's slot_has_filament term never masks what a test is
    /// actually asserting; a test that cares sets it explicitly.
    SlotStatus slot_status_ = SlotStatus::AVAILABLE;
    /// AmsBackend::filament_ops_self_home() — true only on AD5X IFS in
    /// production. Decides whether a PAUSED print still refuses filament ops.
    bool self_homes_ = false;

    // Observed dispatches
    int last_load_slot = -999;
    int load_calls = 0;
    int last_unload_slot = -999;
    int unload_calls = 0;
    int last_change_tool = -999; ///< Tool passed to change_tool()
    int change_tool_calls = 0;   ///< How many times change_tool() dispatched

    [[nodiscard]] AmsSystemInfo get_system_info() const override {
        return sys_;
    }
    [[nodiscard]] PathTopology get_topology() const override {
        return topology_;
    }
    [[nodiscard]] AmsType get_type() const override {
        return sys_.type;
    }
    // Force the AMS load branch in execute_load() (never the raw-macro fallback).
    [[nodiscard]] bool requires_slot_selection_for_load() const override {
        return true;
    }
    [[nodiscard]] bool slot_is_actively_loaded(int slot) const override {
        return slot == loaded_slot_;
    }
    [[nodiscard]] bool slot_has_filament_at_toolhead(int slot) const override {
        return slot == loaded_slot_;
    }
    // Deterministic per-lane presence for the button gating (the base mock owns
    // its own slot table, which this test does not populate).
    [[nodiscard]] SlotInfo get_slot_info(int slot) const override {
        SlotInfo info;
        info.slot_index = slot;
        info.global_index = slot;
        info.mapped_tool = slot;
        info.status = (slot == loaded_slot_) ? SlotStatus::LOADED : slot_status_;
        return info;
    }
    [[nodiscard]] bool filament_ops_self_home() const override {
        return self_homes_;
    }

    AmsError load_filament(int slot) override {
        last_load_slot = slot;
        ++load_calls;
        return AmsErrorHelper::success();
    }
    AmsError unload_filament(int slot) override {
        last_unload_slot = slot;
        ++unload_calls;
        return AmsErrorHelper::success();
    }
    AmsError change_tool(int t) override {
        last_change_tool = t;
        ++change_tool_calls;
        return AmsErrorHelper::success();
    }
};

// Builds a real FilamentPanel view over an injected RecordingBackend + topology,
// and tears the whole thing down in the right order (UI subtree, then panel, then
// the shared singletons) so the next test in the shard starts clean.
struct OpSlotHarness {
    LVGLUITestFixture& fx;
    RecordingBackend* mock = nullptr;
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;

    OpSlotHarness(LVGLUITestFixture& f, const AmsSystemInfo& sys, int loaded_slot,
                  const ToolTopology& topo)
        : fx(f) {
        // The panel wires observers on ToolState + AmsState in its ctor, so their
        // subjects must exist first.
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);

        // Inject the recording backend + topology BEFORE constructing the panel.
        auto owned = std::make_unique<RecordingBackend>();
        owned->sys_ = sys;
        owned->loaded_slot_ = loaded_slot;
        mock = owned.get();
        AmsState::instance().set_backend(std::move(owned));
        AmsState::instance().sync_from_backend(); // publishes ams_type != NONE
        ToolState::instance().set_ams_topology(topo);

        panel = std::make_unique<FilamentPanel>(fx.state(), fx.api());
        panel->init_subjects();

        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());

        // Flush deferred observer callbacks (active-tool sync, gating recompute).
        fx.process_lvgl(30);

        // Ensure the dropdown carries every tool so a T3 selection is reachable
        // (setup() already calls this, but the topology publish may have raced).
        TA::populate_extruder_dropdown(*panel);
    }

    void select_tool(int idx) {
        lv_obj_t* dd = TA::extruder_dropdown(*panel);
        REQUIRE(dd != nullptr);
        REQUIRE(lv_dropdown_get_option_count(dd) >= static_cast<uint32_t>(idx + 1));
        lv_dropdown_set_selected(dd, static_cast<uint32_t>(idx));
    }

    ~OpSlotHarness() {
        if (root) {
            lv_obj_delete(root); // delete UI subtree while panel subjects live
        }
        fx.process_lvgl(10);
        panel.reset(); // dtor deinits subjects + removes observers (subjects valid)
        AmsState::instance().set_backend(nullptr);
        ToolState::instance().clear_ams_topology();
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }
};

// Identity BoxTurtle/AFC snapshot: 4 lanes, tool i -> slot i, lane 4 (slot 3)
// loaded to the toolhead, aggregate current_slot == 3. The unit is populated
// because the load-vs-swap rule reads per-slot mapped_tool off it — a real AFC
// snapshot always carries its lanes.
AmsSystemInfo boxturtle_sys() {
    AmsSystemInfo sys;
    sys.type = AmsType::AFC;
    sys.total_slots = 4;
    sys.current_slot = 3;
    sys.filament_loaded = true;
    sys.tool_to_slot_map = {0, 1, 2, 3};

    AmsUnit unit;
    unit.slot_count = 4;
    unit.connected = true;
    for (int i = 0; i < 4; ++i) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.mapped_tool = i;
        unit.slots.push_back(slot);
    }
    sys.units.push_back(std::move(unit));
    return sys;
}

ToolTopology identity_topo() {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    return topo;
}

} // namespace

// Drive print state through the REAL input. These panels and managers gate on
// print_lifecycle, which PrinterPrintState publishes from update_from_status()
// alongside print_state_enum. Writing the enum subject by hand leaves the
// lifecycle stale, so the code under test never re-gates and the assertion fails
// as if the production guard were missing. Production cannot desync the two:
// printer_print_state.cpp has exactly one writer of print_state_enum_ and
// publish_lifecycle_state() is the next statement.
static void set_wire_state(helix::PrinterState& st, const char* wire) {
    st.update_from_status(nlohmann::json{{"print_stats", {{"state", wire}}}});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "BoxTurtle: execute_load targets the SELECTED tool, not current_slot",
                 "[filament][op_slot][panel]") {
    // Dropdown = T0 while lane 4 (slot 3) is loaded. Filament is seated, so this
    // is a swap, not a fresh load (plan_load's load-vs-swap rule) — but the
    // argument must still come from the SELECTED lane: change_tool(0), never a
    // dispatch derived from current_slot 3. That is the single-source-of-truth
    // bug this guards, and the mutation target.
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.select_tool(0);

    // selected_op_slot() (the real production resolver) resolves T0 -> slot 0.
    REQUIRE(TA::selected_op_slot(*h.panel) == 0);

    TA::execute_load(*h.panel);

    REQUIRE(h.mock->change_tool_calls == 1);
    CHECK(h.mock->last_change_tool == 0); // slot 0's mapped tool, NOT 3
    CHECK(h.mock->load_calls == 0);       // seated machine swaps, never feeds
}

TEST_CASE_METHOD(LVGLUITestFixture, "BoxTurtle: execute_unload targets the selected loaded slot",
                 "[filament][op_slot][panel]") {
    // Dropdown = T3 and slot 3 is loaded -> unload_filament(3).
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.select_tool(3);

    REQUIRE(TA::selected_op_slot(*h.panel) == 3);

    TA::execute_unload(*h.panel);

    REQUIRE(h.mock->unload_calls == 1);
    CHECK(h.mock->last_unload_slot == 3);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "BoxTurtle: execute_unload on an unloaded selected slot makes NO backend call",
                 "[filament][op_slot][panel]") {
    // Dropdown = T0, slot 0 is NOT loaded (only slot 3 is). The "nothing loaded"
    // guard must refuse the unload — the backend is never asked to unload.
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.select_tool(0);

    REQUIRE(TA::selected_op_slot(*h.panel) == 0);

    TA::execute_unload(*h.panel);

    CHECK(h.mock->unload_calls == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Snapmaker U1 shape: selected tool survives a stuck current_slot",
                 "[filament][op_slot][panel]") {
    // Identity tool->slot map, current_slot stuck at -1 (the original U1 root
    // cause), dropdown = T3. selected_op_slot() must yield 3 via the identity map,
    // and execute_load() must dispatch load_filament(3) — never a bare-default 0.
    AmsSystemInfo u1;
    u1.type = AmsType::SNAPMAKER;
    u1.total_slots = 4;
    u1.current_slot = -1; // stuck
    u1.filament_loaded = false;
    u1.tool_to_slot_map = {0, 1, 2, 3};

    OpSlotHarness h(*this, u1, /*loaded_slot=*/-1, identity_topo());
    h.select_tool(3);

    REQUIRE(TA::selected_op_slot(*h.panel) == 3);

    TA::execute_load(*h.panel);

    REQUIRE(h.mock->load_calls == 1);
    CHECK(h.mock->last_load_slot == 3); // NOT 0 (bare default / stuck current_slot)
}

// ============================================================================
// Bug A guard: the dropdown must NOT issue a physical tool change on a
// shared-extruder AMS (HUB / LINEAR). Selecting a tool is selection-only there;
// the explicit Load button performs the swap. Only a true PARALLEL toolchanger
// (each tool = its own toolhead) changes tool on select. active_tool = T0 in
// identity_topo(), so selecting T1 clears the "already active" early-return and
// reaches the topology gate.
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AFC/BoxTurtle (HUB): dropdown selection issues NO physical tool change",
                 "[filament][op_slot][panel]") {
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.mock->topology_ = PathTopology::HUB;
    h.select_tool(1); // != active T0

    TA::handle_extruder_changed(*h.panel);

    CHECK(h.mock->change_tool_calls == 0); // no cut/unload/load swap
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AD5X IFS (LINEAR): dropdown selection issues NO physical tool change",
                 "[filament][op_slot][panel]") {
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.mock->topology_ = PathTopology::LINEAR;
    h.select_tool(2); // != active T0

    TA::handle_extruder_changed(*h.panel);

    CHECK(h.mock->change_tool_calls == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Toolchanger (PARALLEL): dropdown selection changes tool to the selected index",
                 "[filament][op_slot][panel]") {
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.mock->topology_ = PathTopology::PARALLEL;
    h.select_tool(1); // != active T0

    TA::handle_extruder_changed(*h.panel);

    REQUIRE(h.mock->change_tool_calls == 1);
    CHECK(h.mock->last_change_tool == 1);
}

// ============================================================================
// Bug C guard: after a filament op, the extruder must cool back down. Two halves:
//
//   1. restore_heater_after_preheat() schedules the post-op cooldown whenever the
//      printer is IDLE, and skips it while printing/paused (a real print manages
//      its own heat — cooling under it would fight the job).
//   2. Fire-and-forget AMS backend ops (AFC/BoxTurtle) complete via
//      ams_action_observer_ returning to IDLE — NOT the gcode/macro success
//      callback that used to be the only caller of restore_heater_after_preheat().
//      That completion block must reach restore, or the nozzle holds the material
//      temp indefinitely after a swap (the reported bug).
//
// The 120s timer -> M104 S0 machinery itself lives in PostOpCooldownManager and is
// shared/pre-existing; these tests pin only the panel's NEW scheduling behavior,
// observed via PostOpCooldownManager::has_pending_timer().
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Bug C: restore_heater_after_preheat schedules a cooldown when idle",
                 "[filament][op_slot][panel]") {
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());

    auto& cd = PostOpCooldownManager::instance();
    cd.init();
    cd.cancel();
    process_lvgl(10);
    REQUIRE_FALSE(cd.has_pending_timer()); // clean baseline

    set_wire_state(state(), "standby");

    TA::restore_heater_after_preheat(*h.panel);
    process_lvgl(30); // observer/schedule hops -> queued timer creation

    CHECK(cd.has_pending_timer());

    cd.cancel();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Bug C: restore_heater_after_preheat does NOT schedule while printing",
                 "[filament][op_slot][panel]") {
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());

    auto& cd = PostOpCooldownManager::instance();
    cd.init();
    cd.cancel();
    process_lvgl(10);
    REQUIRE_FALSE(cd.has_pending_timer());

    set_wire_state(state(), "printing");

    TA::restore_heater_after_preheat(*h.panel);
    process_lvgl(30);

    CHECK_FALSE(cd.has_pending_timer()); // a print manages its own heat

    cd.cancel();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Bug C: restore_heater_after_preheat does NOT schedule while PREPARING",
                 "[filament][op_slot][panel][print_guard]") {
    // print_stats reads standby for the whole of a host-side pre-start block, so
    // the old PRINTING||PAUSED test scheduled a cooldown that the block's own
    // heating would immediately undo - and then fight.
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());

    auto& cd = PostOpCooldownManager::instance();
    cd.init();
    cd.cancel();
    process_lvgl(10);
    REQUIRE_FALSE(cd.has_pending_timer());

    set_wire_state(state(), "standby");
    state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    process_lvgl(10);

    TA::restore_heater_after_preheat(*h.panel);
    process_lvgl(30);
    CHECK_FALSE(cd.has_pending_timer());

    // The block ending hands the behaviour back, so the guard cannot latch.
    state().set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
    process_lvgl(10);
    TA::restore_heater_after_preheat(*h.panel);
    process_lvgl(30);
    CHECK(cd.has_pending_timer());

    cd.cancel();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Bug C: AFC backend load completion schedules the post-op cooldown",
                 "[filament][op_slot][panel]") {
    // End-to-end: an AFC load is dispatched fire-and-forget, then the AMS action
    // returns to IDLE (backend finished). The ams_action_observer_ completion
    // block must run restore_heater_after_preheat() and leave a cooldown pending.
    // Mutation target: delete the restore_heater_after_preheat() call in that block
    // (ui_panel_filament.cpp) and this fails — no timer pending after completion.
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.select_tool(0); // slot 0 is NOT loaded (only slot 3 is) -> load proceeds

    set_wire_state(state(), "standby");

    auto& cd = PostOpCooldownManager::instance();
    cd.init();
    cd.cancel();
    process_lvgl(10);
    REQUIRE_FALSE(cd.has_pending_timer());

    // Fire-and-forget backend op; op guard now active. Lane 3 is seated, so the
    // dispatch is the swap arm — the completion path is the same either way.
    TA::execute_load(*h.panel);
    REQUIRE(h.mock->change_tool_calls == 1);

    // Backend signals progress, then completion, via the shared AMS action subject.
    lv_subject_t* action = AmsState::instance().get_ams_action_subject();
    lv_subject_set_int(action, static_cast<int>(AmsAction::LOADING));
    process_lvgl(10);
    lv_subject_set_int(action, static_cast<int>(AmsAction::IDLE));
    process_lvgl(30); // deferred observer -> op_succeeded + restore -> schedule

    CHECK(cd.has_pending_timer());

    cd.cancel();
    process_lvgl(10);
}

// ============================================================================
// Opt-out guards: AFC (and other filament systems) run their own post-operation
// cooldown, so ours has to be switchable off — otherwise two timers fight over
// the same heater. Both off-switches are read inside PostOpCooldownManager::
// schedule(), the single choke point every caller funnels through.
//
// Mutation targets: delete either early-return in schedule() and the matching
// test fails (a timer is left pending when the user asked for none).
// ============================================================================

namespace {

/// Sets a per-printer config key for the duration of a test, then puts back what
/// was there. Config has no key-erase, so an absent key restores to `fallback` —
/// pass the same default the production reader uses and the state is equivalent.
template <typename T> class ScopedConfigValue {
  public:
    ScopedConfigValue(std::string key, T value, T fallback) : key_(std::move(key)) {
        auto* cfg = helix::Config::get_instance();
        prev_ = cfg->get<T>(key_, fallback);
        cfg->set<T>(key_, value);
    }
    ~ScopedConfigValue() {
        helix::Config::get_instance()->set<T>(key_, prev_);
    }

  private:
    std::string key_;
    T prev_;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Post-op cooldown: disabled in settings schedules nothing",
                 "[filament][op_slot][panel]") {
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());

    auto* cfg = helix::Config::get_instance();
    ScopedConfigValue<bool> off(cfg->df() + "filament/auto_cooldown", false, true);

    auto& cd = PostOpCooldownManager::instance();
    cd.init();
    cd.cancel();
    process_lvgl(10);
    REQUIRE_FALSE(cd.has_pending_timer());

    set_wire_state(state(), "standby");

    TA::restore_heater_after_preheat(*h.panel);
    process_lvgl(30);

    CHECK_FALSE(cd.has_pending_timer()); // the filament system owns the cooldown

    cd.cancel();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Post-op cooldown: a zero delay means off, not fire-immediately",
                 "[filament][op_slot][panel]") {
    // CONFIGURATION.md documents cooldown_delay_seconds=0 as "disable auto-cooldown".
    // A 0ms lv_timer would instead fire on the next tick and cut the heater at once.
    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());

    auto* cfg = helix::Config::get_instance();
    ScopedConfigValue<bool> on(cfg->df() + "filament/auto_cooldown", true, true);
    ScopedConfigValue<int> zero(cfg->df() + "filament/cooldown_delay_seconds", 0, 120);

    auto& cd = PostOpCooldownManager::instance();
    cd.init();
    cd.cancel();
    process_lvgl(10);
    REQUIRE_FALSE(cd.has_pending_timer());

    set_wire_state(state(), "standby");

    TA::restore_heater_after_preheat(*h.panel);

    // Drain the update queue WITHOUT lv_timer_handler. schedule() creates the timer
    // from a queued lambda, but a 0ms timer would then fire and null itself inside
    // the same process_lvgl() sweep — making "fired instantly" (the bug) read
    // identical to "never scheduled" (the fix). Draining alone lets the timer be
    // created but never run, so its mere existence is the failure signal.
    for (int i = 0; i < 5; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }

    CHECK_FALSE(cd.has_pending_timer());

    cd.cancel();
    process_lvgl(10);
}

// The pure gating rule is pinned in test_filament_op_slot_resolver.cpp. This
// drives the REAL panel: it asserts that update_filament_op_buttons() actually
// consults print state, and that the print-state observer re-runs the gating on
// a panel that is already open — which is how the failure reached the user. The
// AD5X reporter was sitting on the filament screen when the runout pause fired,
// tapped a still-lit Load, and got "Cannot run filament operation while
// printing" from the backend guard (bundle JX2FVRB9).
//
// The panel watches print_lifecycle. It needs PRINTING -> PAUSED as a gating edge
// (a pause UNGATES the buttons on every backend but AD5X, and print_active reads 1
// across both so it never fires there) AND it needs Idle -> Preparing, which the
// raw print_state_enum does not move on at all - the gate refuses during a
// host-side pre-print block, and the enum still says standby for its whole
// duration.
//
// Mutation check: drop `print_blocks_op` from the compute_op_button_gating()
// call in update_filament_op_buttons(), OR delete the print-state observer
// registration, and this test fails at the post-start assertions.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel greys Load/Unload when a print owns the toolhead",
                 "[filament][op_slot][panel][print_guard]") {
    auto read = [](const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    };
    auto set_job_state = [this](helix::PrintJobState st) {
        const char* wire = "standby";
        switch (st) {
        case helix::PrintJobState::PRINTING:
            wire = "printing";
            break;
        case helix::PrintJobState::PAUSED:
            wire = "paused";
            break;
        default:
            break;
        }
        set_wire_state(state(), wire);
        process_lvgl(10);
    };

    // A host-side pre-print block: print_stats still reads standby while the
    // pre-start G-code homes and probes. Raising the phase is what makes the
    // lifecycle Preparing.
    auto set_preprint_phase = [this](helix::PrintStartPhase phase) {
        state().set_print_start_state(phase, "", 0);
        process_lvgl(10);
    };

    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.select_tool(3); // T3 == the loaded lane
    TA::handle_extruder_changed(*h.panel);
    process_lvgl(10);

    // Baseline, no print: slot 3 is loaded, so Load is greyed and Unload is live.
    REQUIRE(read("filament_load_disabled") == 1);
    REQUIRE(read("filament_unload_disabled") == 0);

    // A print starts while the panel is open. Setting the state subject must
    // drive the observer and re-gate BOTH buttons with no other interaction.
    set_job_state(helix::PrintJobState::PRINTING);
    CHECK(read("filament_load_disabled") == 1);
    CHECK(read("filament_unload_disabled") == 1); // the regression: was 0

    // The runout pause. AFC does not self-home, so refuse_if_printing() now
    // ALLOWS the unload here — pause-then-swap is the recovery Klipper asks for,
    // and the panel must hand the button back or the fix is invisible.
    set_job_state(helix::PrintJobState::PAUSED);
    CHECK(read("filament_load_disabled") == 1);   // slot 3 already loaded
    CHECK(read("filament_unload_disabled") == 0); // the paused-swap fix

    // Same pause on a backend whose filament macro homes itself (AD5X IFS):
    // still refused, because the buried _G28 would probe into the part.
    h.mock->self_homes_ = true;
    set_job_state(helix::PrintJobState::PRINTING); // force an observed edge
    set_job_state(helix::PrintJobState::PAUSED);
    CHECK(read("filament_load_disabled") == 1);
    CHECK(read("filament_unload_disabled") == 1);
    h.mock->self_homes_ = false;

    // A host-side pre-print block owns the toolhead just as a running print does,
    // and print_stats cannot say so — this is the window the migration to the
    // lifecycle exists to cover. Unload must be refused here even though the wire
    // still reads standby.
    set_job_state(helix::PrintJobState::STANDBY);
    set_preprint_phase(helix::PrintStartPhase::BED_MESH);
    CHECK(read("filament_load_disabled") == 1);
    CHECK(read("filament_unload_disabled") == 1);
    set_preprint_phase(helix::PrintStartPhase::IDLE);

    // Cancelling/finishing the print hands the buttons back.
    set_job_state(helix::PrintJobState::STANDBY);
    CHECK(read("filament_load_disabled") == 1); // still loaded
    CHECK(read("filament_unload_disabled") == 0);
}

// The panel's Load gate grew a slot_has_filament term (the AMS context menu had
// carried it since it was written). An empty lane cannot be loaded from, and
// dispatching one leaves the button spinning against a backend refusal.
//
// SlotStatus::UNKNOWN is deliberately NOT read as empty: it means the backend
// publishes no per-lane presence, and greying Load there would break loading
// entirely on such a printer.
//
// Mutation check: drop the slot_has_filament assignment in
// update_filament_op_buttons() and the empty-lane section fails; make
// slot_presence() map UNKNOWN to false and the unknown section fails.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel greys Load on an empty lane, not an unknown one",
                 "[filament][op_slot][panel][op_gating]") {
    auto read = [](const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    };

    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());

    SECTION("a lane the backend reports EMPTY cannot be loaded from") {
        h.mock->slot_status_ = SlotStatus::EMPTY;
        h.select_tool(0); // an unloaded lane
        TA::handle_extruder_changed(*h.panel);
        process_lvgl(10);
        CHECK(read("filament_load_disabled") == 1);
    }

    SECTION("a lane with filament stays loadable") {
        h.mock->slot_status_ = SlotStatus::AVAILABLE;
        h.select_tool(0);
        TA::handle_extruder_changed(*h.panel);
        process_lvgl(10);
        CHECK(read("filament_load_disabled") == 0);
    }

    SECTION("UNKNOWN presence does not grey Load") {
        h.mock->slot_status_ = SlotStatus::UNKNOWN;
        h.select_tool(0);
        TA::handle_extruder_changed(*h.panel);
        process_lvgl(10);
        CHECK(read("filament_load_disabled") == 0);
    }
}

// The panel's Load/Unload had no system_busy term, so an AMS op started
// elsewhere (the AMS panel, or the printer itself) left both buttons lit for its
// whole duration — every tap answered by check_preconditions()'s busy refusal.
//
// Mutation check: drop the system_busy assignment in
// update_filament_op_buttons() and this fails.
TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel greys Load/Unload while the AMS is busy",
                 "[filament][op_slot][panel][op_gating]") {
    auto read = [](const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    };

    OpSlotHarness h(*this, boxturtle_sys(), /*loaded_slot=*/3, identity_topo());
    h.select_tool(3);
    TA::handle_extruder_changed(*h.panel);
    process_lvgl(10);
    REQUIRE(read("filament_unload_disabled") == 0);

    // An unload kicked off from the AMS panel. AmsSystemInfo::is_busy() is
    // "action is neither IDLE nor ERROR"; publishing it must re-gate here.
    h.mock->sys_.action = AmsAction::UNLOADING;
    lv_subject_set_int(AmsState::instance().get_ams_action_subject(),
                       static_cast<int>(AmsAction::UNLOADING));
    process_lvgl(10);
    CHECK(read("filament_load_disabled") == 1);
    CHECK(read("filament_unload_disabled") == 1);

    // ...and hands them back when it finishes.
    h.mock->sys_.action = AmsAction::IDLE;
    lv_subject_set_int(AmsState::instance().get_ams_action_subject(),
                       static_cast<int>(AmsAction::IDLE));
    process_lvgl(10);
    CHECK(read("filament_unload_disabled") == 0);
}
